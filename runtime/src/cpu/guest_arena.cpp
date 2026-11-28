#include "armbat/guest_arena.h"
#include "armbat/trace.h"

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
        pages_[index].store(kPageReadWrite, std::memory_order_release);
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
        pages_[index].store(kPageFree, std::memory_order_release);
    }
    if (mprotect(base_ + start, static_cast<std::size_t>(end - start), PROT_NONE) != 0) {
        return false;
    }
    // Handing the pages back is not bookkeeping. Unmapping a few hundred
    // megabytes of unpacked assets otherwise leaves them resident, because
    // PROT_NONE still keeps the physical pages, and the host's low-memory killer
    // settles on the container. MADV_DONTNEED leaves them reading as zero, which
    // is what Map() promises for a recycled range.
    ::madvise(base_ + start, static_cast<std::size_t>(end - start), MADV_DONTNEED);

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

bool GuestArena::Protect(std::uint32_t addr, std::uint64_t len, int prot) {
    if (!Ok() || len == 0) {
        return false;
    }
    // Not traced: a startup calls this tens of thousands of times at a few
    // microseconds each, and that many zones drown the trace without saying
    // anything a total would not.
    const std::uint64_t page = page_size_;
    const std::uint64_t start = addr & ~(page - 1);
    const std::uint64_t end = (static_cast<std::uint64_t>(addr) + len + page - 1) & ~(page - 1);
    if (end > size_) {
        return false;
    }
    // Host protection is the guest's, minus execute: guest instructions are
    // fetched through the JIT's callbacks and never executed by the host, so a
    // page the guest calls executable only has to stay readable.
    constexpr int kProtWrite = 0x2;
    constexpr int kProtExec = 0x4;
    const bool writable = (prot & kProtWrite) != 0;
    const bool accessible = (prot & (0x1 | kProtWrite | kProtExec)) != 0;
    int host = PROT_NONE;
    if (writable) {
        host = PROT_READ | PROT_WRITE;
    } else if (accessible) {
        host = PROT_READ;
    }
    const std::uint8_t state = !accessible ? kPageGuard
                                           : (writable ? kPageReadWrite : kPageReadOnly);
    // How much a state lets a guest do. The numeric order of the states means
    // nothing: read-only is 3 and read-write is 1.
    const auto rank = [](std::uint8_t value) {
        switch (value) {
            case kPageReadWrite:
                return 2;
            case kPageReadOnly:
                return 1;
            default:
                return 0;
        }
    };

    // The kernel answers ENOMEM for a range with a hole in it, and a caller that
    // got zero instead would go on to rely on protection that is not there.
    const std::uint64_t first_index = start / page;
    const std::uint64_t last_index = end / page;
    bool downgrade = false;
    for (std::uint64_t index = first_index; index < last_index; ++index) {
        const std::uint8_t current = pages_[index].load(std::memory_order_relaxed);
        if (current == kPageFree) {
            return false;
        }
        downgrade = downgrade || rank(current) > rank(state);
    }
    // Publication order follows the direction of the change. A reader that sees
    // a page as accessible must be able to touch it, so a range that gains
    // access is announced after the host protection grew. A range that loses it
    // is the other way round: announcing afterwards leaves a window in which the
    // page is already read-only or gone while the table still says a write will
    // work, and a guest thread writing in that window faults in the host instead
    // of the guest.
    std::vector<std::uint8_t> previous;
    if (downgrade) {
        previous.resize(static_cast<std::size_t>(last_index - first_index));
        for (std::uint64_t index = first_index; index < last_index; ++index) {
            const std::uint8_t current = pages_[index].load(std::memory_order_relaxed);
            previous[static_cast<std::size_t>(index - first_index)] = current;
            if (rank(current) > rank(state)) {
                pages_[index].store(state, std::memory_order_relaxed);
            }
        }
    }
    if (mprotect(base_ + start, static_cast<std::size_t>(end - start), host) != 0) {
        // Nothing was actually changed, so the table must not claim it was.
        for (std::size_t index = 0; index < previous.size(); ++index) {
            pages_[first_index + index].store(previous[index], std::memory_order_relaxed);
        }
        return false;
    }
    // Announced after the protection changed: a reader that sees a page as
    // accessible must be able to touch it.
    for (std::uint64_t index = first_index; index < last_index; ++index) {
        pages_[index].store(state, std::memory_order_release);
    }
    return true;
}

std::uint64_t GuestArena::AccessibleEnd(std::uint32_t addr, std::uint64_t limit) const {
    if (pages_ == nullptr || addr >= size_) {
        return 0;
    }
    const std::uint64_t count = size_ / page_size_;
    std::uint64_t index = static_cast<std::uint64_t>(addr) >> page_shift_;
    if (index >= count || pages_[index].load(std::memory_order_acquire) == kPageFree) {
        return 0;
    }
    // A caller that needs one request's worth of bytes must not walk the run of
    // writable pages past it. The guest heap is one such run -- tens of thousands
    // of pages -- and a reader that asked for 4 KiB at a time paid for the whole
    // heap on every read.
    std::uint64_t last = count - 1;
    if (limit != 0 && limit <= size_) {
        const std::uint64_t asked = (limit - 1) >> page_shift_;
        if (asked < last) {
            last = asked;
        }
    }
    while (index <= last) {
        const std::uint8_t state = pages_[index].load(std::memory_order_acquire);
        if (state != kPageReadWrite) {
            break;
        }
        ++index;
    }
    return index * page_size_;
}

bool GuestArena::IsCommitted(std::uint32_t addr, std::uint64_t len) const {
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + len;
    if (len == 0 || end > size_ || pages_ == nullptr) {
        return false;
    }
    const std::uint64_t first = static_cast<std::uint64_t>(addr) >> page_shift_;
    const std::uint64_t last = (end - 1) >> page_shift_;
    for (std::uint64_t index = first; index <= last; ++index) {
        if (pages_[index].load(std::memory_order_acquire) == kPageFree) {
            return false;
        }
    }
    return true;
}

std::uint64_t GuestArena::MappedEnd(std::uint32_t addr) const {    const std::uint64_t point = addr;
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
