#include "armbat/guest_memory.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iterator>

#include <sys/mman.h>
#include <unistd.h>

#include "armbat/guest_arena.h"

namespace armbat {
namespace {

constexpr std::uint64_t kPageSize = 4096;
constexpr std::uint64_t kAddressSpace = 1ull << 32;

constexpr std::uint32_t kMapShared = 0x01;
constexpr std::uint32_t kMapPrivate = 0x02;
constexpr std::uint32_t kMapFixed = 0x10;
constexpr std::uint32_t kMapAnonymous = 0x20;
constexpr std::uint32_t kMapFixedNoReplace = 0x100000;

// madvise advice values (asm-generic, the same on ARM32 and arm64).
constexpr int kMadvDontNeed = 4;
constexpr int kMadvRemove = 9;

// Purging relies on the arena being anonymous private memory, where
// MADV_DONTNEED leaves pages that read as zero. If the arena is ever backed by
// a file or a memfd, set this to false and every purge becomes a memset.
constexpr bool kArenaIsAnonymous = true;

std::uint64_t PageDown(std::uint64_t value) { return value & ~(kPageSize - 1); }
std::uint64_t PageUp(std::uint64_t value) { return (value + kPageSize - 1) & ~(kPageSize - 1); }

}  // namespace

GuestMemory::GuestMemory(GuestArena& arena, std::uint32_t map_start, std::uint32_t map_limit,
                         std::uint32_t brk_base, std::uint32_t brk_limit)
    : arena_(arena),
      window_start_(PageUp(map_start)),
      window_limit_(PageDown(map_limit)),
      brk_base_(static_cast<std::uint32_t>(PageUp(brk_base))),
      brk_limit_(static_cast<std::uint32_t>(PageDown(brk_limit))),
      brk_current_(brk_base_),
      brk_mapped_end_(brk_base_) {}

// ---- helpers (all expect mutex_ to be held) --------------------------------

bool GuestMemory::Overlaps(std::uint64_t start, std::uint64_t end) const {
    auto it = live_.upper_bound(start);
    if (it != live_.begin() && std::prev(it)->second.end > start) {
        return true;
    }
    return it != live_.end() && it->first < end;
}

bool GuestMemory::AnyMapped(std::uint64_t start, std::uint64_t end) const {
    for (std::uint64_t page = start; page < end; page += kPageSize) {
        if (arena_.IsMapped(static_cast<std::uint32_t>(page), kPageSize)) {
            return true;
        }
    }
    return false;
}

// True when part of [start, end) is committed in the arena by someone other
// than this class: the heap, a loaded image, a thread stack, an HLE page.
bool GuestMemory::HasForeignMapping(std::uint64_t start, std::uint64_t end) const {
    std::uint64_t cursor = start;
    auto it = live_.upper_bound(start);
    if (it != live_.begin()) {
        cursor = std::max(cursor, std::prev(it)->second.end);
    }
    while (cursor < end) {
        const std::uint64_t gap_end = it != live_.end() ? std::min(it->first, end) : end;
        if (cursor < gap_end && AnyMapped(cursor, gap_end)) {
            return true;
        }
        if (it == live_.end()) {
            break;
        }
        cursor = std::max(cursor, it->second.end);
        ++it;
    }
    return false;
}

// Removes [start, end) from the table, keeping what sticks out on either side.
void GuestMemory::Carve(std::uint64_t start, std::uint64_t end, std::vector<Range>* removed) {
    auto it = live_.upper_bound(start);
    if (it != live_.begin()) {
        --it;
    }
    while (it != live_.end() && it->first < end) {
        const std::uint64_t segment_start = it->first;
        const Segment segment = it->second;
        if (segment.end <= start) {
            ++it;
            continue;
        }
        it = live_.erase(it);
        if (segment_start < start) {
            live_.emplace(segment_start, Segment{start, segment.anonymous});
        }
        if (segment.end > end) {
            live_.emplace(end, Segment{segment.end, segment.anonymous});
        }
        if (removed != nullptr) {
            removed->push_back({std::max(segment_start, start), std::min(segment.end, end)});
        }
    }
}

// Lowest gap of `length` bytes inside the mmap window.
std::uint64_t GuestMemory::FindGap(std::uint64_t length) const {
    std::uint64_t cursor = window_start_;
    for (const auto& [start, segment] : live_) {
        if (segment.end <= cursor) {
            continue;
        }
        if (start >= window_limit_) {
            break;
        }
        if (start > cursor && start - cursor >= length) {
            return cursor;
        }
        cursor = std::max(cursor, segment.end);
    }
    if (window_limit_ > cursor && window_limit_ - cursor >= length) {
        return cursor;
    }
    return 0;
}

// Makes the pages read as zero. Whole host pages go through MADV_DONTNEED,
// which also returns the memory to the system; slivers at the edges (the host
// page can be 16 KiB on recent devices while the guest's is 4 KiB) are cleared
// by hand, never touching a host page that reaches outside the range.
void GuestMemory::PurgeToZero(std::uint64_t start, std::uint64_t length) {
    if (length == 0) {
        return;
    }
    auto* const base = reinterpret_cast<unsigned char*>(
        arena_.Ptr(static_cast<std::uint32_t>(start)));
    if (kArenaIsAnonymous) {
        static const std::uintptr_t host_page =
            static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
        const std::uintptr_t low = reinterpret_cast<std::uintptr_t>(base);
        const std::uintptr_t high = low + static_cast<std::uintptr_t>(length);
        if (host_page != 0) {
            const std::uintptr_t first = (low + host_page - 1) & ~(host_page - 1);
            const std::uintptr_t last = high & ~(host_page - 1);
            if (first < last &&
                ::madvise(reinterpret_cast<void*>(first), last - first, MADV_DONTNEED) == 0) {
                std::memset(base, 0, static_cast<std::size_t>(first - low));
                std::memset(reinterpret_cast<void*>(last), 0,
                            static_cast<std::size_t>(high - last));
                return;
            }
        }
    }
    std::memset(base, 0, static_cast<std::size_t>(length));
}

// ---- mmap ------------------------------------------------------------------

std::uint32_t GuestMemory::Map(std::uint32_t address, std::uint32_t length, int flags, int fd,
                               std::uint32_t page_offset, std::int32_t* error) {
    const auto fail = [error](int code) -> std::uint32_t {
        if (error != nullptr) {
            *error = -code;
        }
        return 0;
    };
    if (length == 0) {
        return fail(EINVAL);
    }
    const bool fixed = (flags & kMapFixed) != 0;
    const bool anonymous = (flags & kMapAnonymous) != 0;
    if (!anonymous && fd < 0) {
        return fail(EBADF);
    }
    if ((flags & (kMapShared | kMapPrivate)) == 0) {
        return fail(EINVAL);
    }
    const std::uint64_t size = PageUp(length);
    if (size >= kAddressSpace) {
        return fail(ENOMEM);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::uint64_t start = 0;
    if (fixed) {
        if ((address & (kPageSize - 1)) != 0) {
            return fail(EINVAL);
        }
        start = address;
        const std::uint64_t end = start + size;
        if (end > kAddressSpace || end > arena_.Size()) {
            return fail(ENOMEM);
        }
        const bool foreign = HasForeignMapping(start, end);
        if ((flags & kMapFixedNoReplace) != 0 && (foreign || Overlaps(start, end))) {
            return fail(EEXIST);
        }
        if (foreign) {
            // Replacing the heap, an image or a stack would be the guest
            // corrupting the container that runs it; refuse instead of obeying.
            return fail(ENOMEM);
        }
        // The bionic linker reserves a range with PROT_NONE and maps every
        // segment over it with MAP_FIXED, so replacing our own ranges is normal.
        std::vector<Range> removed;
        Carve(start, end, &removed);
        for (const Range& piece : removed) {
            arena_.Unmap(static_cast<std::uint32_t>(piece.start),
                         piece.end - piece.start);
        }
    } else {
        // A hint is honoured when the range is free, like Linux does: jemalloc
        // checks whether it got the address it asked for and backs off if not.
        if (address != 0 && (address & (kPageSize - 1)) == 0 &&
            address >= window_start_ && address + size <= window_limit_ &&
            !Overlaps(address, address + size)) {
            start = address;
        } else {
            start = FindGap(size);
            if (start == 0) {
                return fail(ENOMEM);
            }
        }
        // Only this class's own ranges are in live_, so a gap it found can still
        // be inside the heap, an image or a thread stack. Mapping there would
        // hand the guest memory that already belongs to something else.
        if (HasForeignMapping(start, start + size)) {
            return fail(ENOMEM);
        }
    }

    if (!arena_.Map(static_cast<std::uint32_t>(start), size)) {
        return fail(ENOMEM);
    }
    // Fresh pages must read as zero even when the arena recycled them.
    PurgeToZero(start, size);

    if (!anonymous) {
        const off_t offset = static_cast<off_t>(page_offset) * static_cast<off_t>(kPageSize);
        std::size_t filled = 0;
        while (filled < size) {
            const ssize_t count = ::pread(fd, arena_.Ptr(static_cast<std::uint32_t>(start + filled)),
                                          static_cast<std::size_t>(size - filled),
                                          offset + static_cast<off_t>(filled));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const int code = errno;
                arena_.Unmap(static_cast<std::uint32_t>(start), size);
                return fail(code);
            }
            if (count == 0) {
                break;  // short file: the rest stays zero, like Linux
            }
            filled += static_cast<std::size_t>(count);
        }
    }

    live_.emplace(start, Segment{start + size, anonymous});
    return static_cast<std::uint32_t>(start);
}

// ---- munmap ----------------------------------------------------------------

bool GuestMemory::Unmap(std::uint32_t address, std::uint32_t length) {
    if (length == 0) {
        return true;
    }
    if ((address & (kPageSize - 1)) != 0) {
        return false;
    }
    const std::uint64_t start = address;
    const std::uint64_t end = std::min<std::uint64_t>(PageUp(start + length), kAddressSpace);

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Range> removed;
    Carve(start, end, &removed);
    // Only what this class handed out is released: a munmap that reaches the
    // heap or an image by mistake removes nothing of theirs.
    for (const Range& piece : removed) {
        arena_.Unmap(static_cast<std::uint32_t>(piece.start), piece.end - piece.start);
    }
    return true;
}

// ---- madvise ---------------------------------------------------------------

std::int32_t GuestMemory::Advise(std::uint32_t address, std::uint32_t length, int advice) {
    // Same order as the kernel: validate the advice, then the arguments.
    switch (advice) {
        case 0:
        case 1:
        case 2:
        case 3:  // NORMAL RANDOM SEQUENTIAL WILLNEED
        case kMadvDontNeed:
        case kMadvRemove:
        case 10:
        case 11:
        case 12:
        case 13:  // DONTFORK DOFORK MERGEABLE UNMERGEABLE
        case 14:
        case 15:
        case 16:
        case 17:  // HUGEPAGE NOHUGEPAGE DONTDUMP DODUMP
            break;
        default:
            // Includes MADV_FREE (8): the 3.18 kernel the container reports has
            // no such advice, so callers take the MADV_DONTNEED path instead.
            return -EINVAL;
    }
    if ((address & (kPageSize - 1)) != 0) {
        return -EINVAL;
    }
    if (length == 0) {
        return 0;
    }
    if (advice == kMadvRemove) {
        return -EINVAL;  // only for shared/tmpfs mappings
    }

    const std::uint64_t start = address;
    const std::uint64_t end = std::min<std::uint64_t>(PageUp(start + length), kAddressSpace);

    std::lock_guard<std::mutex> lock(mutex_);
    bool gap = false;
    std::uint64_t cursor = start;
    auto it = live_.upper_bound(start);
    if (it != live_.begin() && std::prev(it)->second.end > start) {
        it = std::prev(it);
    }
    for (; it != live_.end() && it->first < end; ++it) {
        const std::uint64_t piece_start = std::max(it->first, start);
        const std::uint64_t piece_end = std::min(it->second.end, end);
        if (piece_start > cursor) {
            gap = true;
        }
        // DONTNEED on anonymous memory means "zero-fill on the next touch", and
        // an allocator that is told so stops clearing the pages itself: the
        // zeroing is not optional. File-backed private pages would be re-read
        // from the file, which the container does not track, so they are left
        // alone -- and their segment says so.
        if (advice == kMadvDontNeed && it->second.anonymous) {
            PurgeToZero(piece_start, piece_end - piece_start);
        }
        cursor = std::max(cursor, piece_end);
    }
    if (cursor < end) {
        gap = true;
    }
    return gap ? -ENOMEM : 0;
}

// ---- brk -------------------------------------------------------------------

std::uint32_t GuestMemory::Break(std::uint32_t requested) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (requested == 0 || requested < brk_base_ || requested > brk_limit_) {
        return brk_current_;  // like Linux: failure answers with the current break
    }
    const std::uint32_t wanted = static_cast<std::uint32_t>(PageUp(requested));
    if (wanted > brk_mapped_end_) {
        if (!arena_.Map(brk_mapped_end_, wanted - brk_mapped_end_)) {
            return brk_current_;
        }
        PurgeToZero(brk_mapped_end_, wanted - brk_mapped_end_);
        brk_mapped_end_ = wanted;
    } else if (requested < brk_current_) {
        // Memory given back has to read as zero when the break grows again.
        const std::uint32_t from = static_cast<std::uint32_t>(PageUp(requested));
        if (from < brk_mapped_end_) {
            PurgeToZero(from, brk_mapped_end_ - from);
        }
    }
    brk_current_ = requested;
    return requested;
}

}  // namespace armbat
