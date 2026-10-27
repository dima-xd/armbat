#include "armbat/guest_arena.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

namespace armbat {
namespace {

std::uint64_t PageSize() {
    static const std::uint64_t page_size = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    return page_size;
}

std::uint64_t PageShift(std::uint64_t page_size) {
    std::uint64_t shift = 0;
    while ((std::uint64_t{1} << shift) < page_size) {
        ++shift;
    }
    return shift;
}

}  // namespace

GuestArena::GuestArena(std::uint64_t size)
    : size_(size), page_size_(PageSize()), page_shift_(PageShift(page_size_)) {
    void* p = mmap(nullptr, static_cast<std::size_t>(size), PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        error_ = strerror(errno);
        return;
    }
    base_ = static_cast<std::byte*>(p);
    // One byte per guest page, all clear: nothing is committed yet.
    pages_ = std::make_unique<std::atomic<std::uint8_t>[]>(
        static_cast<std::size_t>(size_ / page_size_));
    for (std::size_t i = 0; i < size_ / page_size_; ++i) {
        pages_[i].store(0, std::memory_order_relaxed);
    }
    error_ = nullptr;
}

GuestArena::~GuestArena() {
    if (base_ != nullptr) {
        munmap(base_, static_cast<std::size_t>(size_));
    }
}

bool GuestArena::Map(std::uint32_t addr, std::uint64_t len) {
    if (!Ok() || len == 0) {
        return false;
    }

    const std::uint64_t page = page_size_;
    const std::uint64_t start = addr & ~(page - 1);
    const std::uint64_t end = (static_cast<std::uint64_t>(addr) + len + page - 1) & ~(page - 1);
    if (end > size_) {
        return false;
    }

    void* p = base_ + start;
    if (mprotect(p, static_cast<std::size_t>(end - start), PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    // Announced after the protection changed: a reader that sees a page as
    // mapped must be able to touch it.
    for (std::uint64_t index = start / page; index < end / page; ++index) {
        pages_[index].store(1, std::memory_order_release);
    }
    {
        const std::unique_lock lock(ranges_mutex_);
        // Ranges almost always arrive in increasing order (an image is mapped
        // front to back), so the common case is an append or an extension of the
        // last range. Sorting every range on every map made a loader's many
        // small mappings quadratic.
        const std::uint64_t last_end =
            mapped_.empty() ? 0 : mapped_.back().first + mapped_.back().second;
        if (mapped_.empty() || start > last_end) {
            mapped_.emplace_back(start, end - start);
        } else if (start >= mapped_.back().first) {
            if (end > last_end) {
                mapped_.back().second = end - mapped_.back().first;
            }
        } else {
            mapped_.emplace_back(start, end - start);
            Normalize();
        }
    }
    return true;
}

bool GuestArena::Unmap(std::uint32_t addr, std::uint64_t len) {
    if (!Ok() || len == 0) {
        return false;
    }

    const std::uint64_t page = page_size_;
    const std::uint64_t start = addr & ~(page - 1);
    const std::uint64_t end = (static_cast<std::uint64_t>(addr) + len + page - 1) & ~(page - 1);
    if (end > size_) {
        return false;
    }

    // Withdrawn before the protection changes, so no reader is told a page is
    // usable while it is already PROT_NONE.
    for (std::uint64_t index = start / page; index < end / page; ++index) {
        pages_[index].store(0, std::memory_order_release);
    }
    if (mprotect(base_ + start, static_cast<std::size_t>(end - start), PROT_NONE) != 0) {
        return false;
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> kept;
    kept.reserve(mapped_.size() + 1);
    {
        const std::unique_lock lock(ranges_mutex_);
        for (const auto& [range_start, range_len] : mapped_) {
            const std::uint64_t range_end = range_start + range_len;
            if (range_end <= start || range_start >= end) {
                kept.emplace_back(range_start, range_len);
                continue;
            }
            if (range_start < start) {
                kept.emplace_back(range_start, start - range_start);
            }
            if (range_end > end) {
                kept.emplace_back(end, range_end - end);
            }
        }
        mapped_.swap(kept);
    }
    return true;
}

std::uint64_t GuestArena::MappedEnd(std::uint32_t addr) const {
    const std::uint64_t point = addr;
    if (point >= size_) {
        return 0;
    }
    const std::shared_lock lock(ranges_mutex_);
    auto range = std::upper_bound(mapped_.begin(), mapped_.end(), point,
                                  [](std::uint64_t value,
                                     const std::pair<std::uint64_t, std::uint64_t>& candidate) {
                                      return value < candidate.first;
                                  });
    if (range == mapped_.begin()) {
        return 0;
    }
    --range;
    const std::uint64_t end = range->first + range->second;
    return point < end ? end : 0;
}

void GuestArena::Normalize() {
    if (mapped_.size() < 2) {
        return;
    }

    std::sort(mapped_.begin(), mapped_.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    merged.reserve(mapped_.size());
    for (const auto& [start, length] : mapped_) {
        if (!merged.empty() && start <= merged.back().first + merged.back().second) {
            const std::uint64_t end = start + length;
            const std::uint64_t merged_end = merged.back().first + merged.back().second;
            if (end > merged_end) {
                merged.back().second = end - merged.back().first;
            }
        } else {
            merged.emplace_back(start, length);
        }
    }
    mapped_.swap(merged);
}

}  // namespace armbat
