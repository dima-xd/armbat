#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace armbat {

class GuestArena;

// Guest address-space manager behind mmap2 / munmap / madvise / brk.
//
// There is no demand paging: a mapping is committed immediately, anonymous
// pages read as zero, and file-backed pages are filled from the file at map
// time. What is tracked is *which ranges belong to the guest's allocator*, and
// the invariant that matters to a guest is the one a real kernel keeps:
//
//   every range handed out by Map() is a node of live_, ranges never overlap,
//   and nothing outside live_ (the heap, loaded images, thread stacks, the HLE
//   pages) can be unmapped, replaced or purged by the guest.
//
// bionic's allocator is the demanding caller. jemalloc purges pages with
// MADV_DONTNEED and remembers which pages it may hand out again without
// clearing them: an madvise that answers 0 without zeroing turns its calloc()
// into a source of uninitialized memory, which in this container showed up as a
// garbage thread-local resolver state and then as free(0xffffffff). Purge
// therefore has to mean purge, and the range table has to be exact, because a
// range that is both free and live is memory handed out twice.
//
// All entry points take mutex_: guest threads are real host threads and call
// these concurrently.
class GuestMemory {
public:
    GuestMemory(GuestArena& arena, std::uint32_t map_start, std::uint32_t map_limit,
                std::uint32_t brk_base, std::uint32_t brk_limit);
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Returns the mapped address, or 0 with *error = -errno.
    std::uint32_t Map(std::uint32_t address, std::uint32_t length, int flags, int fd,
                      std::uint32_t page_offset, std::int32_t* error);

    // false only for EINVAL (unaligned address). Unmapping nothing succeeds,
    // like Linux.
    bool Unmap(std::uint32_t address, std::uint32_t length);

    // 0 or -errno, following the kernel: an unknown advice is EINVAL and a
    // range that is partly unmapped is ENOMEM.
    std::int32_t Advise(std::uint32_t address, std::uint32_t length, int advice);

    // Returns the program break after the call, following Linux brk semantics.
    std::uint32_t Break(std::uint32_t requested);

private:
    struct Segment {
        std::uint64_t end;
        bool anonymous;
    };
    struct Range {
        std::uint64_t start;
        std::uint64_t end;
    };

    bool Overlaps(std::uint64_t start, std::uint64_t end) const;
    bool HasForeignMapping(std::uint64_t start, std::uint64_t end) const;
    bool AnyMapped(std::uint64_t start, std::uint64_t end) const;
    void Carve(std::uint64_t start, std::uint64_t end, std::vector<Range>* removed);
    std::uint64_t FindGap(std::uint64_t length) const;
    void PurgeToZero(std::uint64_t start, std::uint64_t length);

    GuestArena& arena_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, Segment> live_;
    std::uint64_t window_start_;
    std::uint64_t window_limit_;
    std::uint32_t brk_base_;
    std::uint32_t brk_limit_;
    std::uint32_t brk_current_;
    std::uint32_t brk_mapped_end_;
};

}  // namespace armbat
