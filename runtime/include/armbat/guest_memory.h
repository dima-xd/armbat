#pragma once

#include <cstdint>
#include <map>
#include <memory>
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

    // The arena behind the guest address space. A guest address is an offset into
    // it, so a platform call that needs a host pointer for a guest buffer -- the
    // NDK asset reader is the one -- gets one from here instead of a copy through
    // a staging buffer.
    GuestArena& Arena() const { return arena_; }

    // Returns the mapped address, or 0 with *error = -errno.
    std::uint32_t Map(std::uint32_t address, std::uint32_t length, int flags, int fd,
                      std::uint32_t page_offset, std::int32_t* error);

    // Anonymous guest memory for a buffer the container itself has to hand the
    // guest and keep alive: an APK asset a game reads through AAsset_getBuffer,
    // not a mapping the guest asked for. It lives in the mmap window, which is
    // the only region sized for whole packs, and never overlaps a mapping the
    // guest owns. ReleaseAnonymous is the matching free.
    std::uint32_t Anonymous(std::uint32_t length);
    void ReleaseAnonymous(std::uint32_t address, std::uint32_t length);

    // false only for EINVAL (unaligned address). Unmapping nothing succeeds,
    // like Linux.
    bool Unmap(std::uint32_t address, std::uint32_t length);

    // 0 or -errno, following the kernel: an unknown advice is EINVAL and a
    // range that is partly unmapped is ENOMEM.
    std::int32_t Advise(std::uint32_t address, std::uint32_t length, int advice);

    // msync: writes the shared file mappings in the range back to their files,
    // and with MS_INVALIDATE re-reads them instead. Private mappings are left
    // alone: their pages are the guest's private copies and were never meant to
    // reach the file. 0 or -errno.
    std::int32_t Sync(std::uint32_t address, std::uint32_t length, int flags);

    // Returns the program break after the call, following Linux brk semantics.
    std::uint32_t Break(std::uint32_t requested);

private:
    // A descriptor kept open for a shared file mapping. The guest is free to
    // close its own copy right after mmap, and the kernel keeps the mapping
    // writable through the file anyway, so the container has to hold a
    // descriptor of its own until the last piece of the mapping is gone.
    struct FileHandle {
        int fd{-1};
        ~FileHandle();
    };
    struct Segment {
        std::uint64_t end;
        bool anonymous;
        // MAP_SHARED from a file: what the guest writes has to reach the file.
        bool shared{false};
        std::shared_ptr<FileHandle> file;
        // Byte offset of this segment's first byte in the file.
        std::uint64_t file_offset{0};
    };
    // A piece of a segment that a carve took out, with everything needed to
    // write it back: the piece's own address range and where the piece starts
    // inside the segment (which is where its file offset comes from).
    struct Piece {
        std::uint64_t start;
        std::uint64_t end;
        std::uint64_t segment_start;
        Segment segment;
    };

    bool Overlaps(std::uint64_t start, std::uint64_t end) const;
    bool HasForeignMapping(std::uint64_t start, std::uint64_t end) const;
    bool AnyMapped(std::uint64_t start, std::uint64_t end) const;
    void Carve(std::uint64_t start, std::uint64_t end, std::vector<Piece>* removed);
    std::uint64_t FindGap(std::uint64_t length) const;
    void PurgeToZero(std::uint64_t start, std::uint64_t length);
    // Pushes a shared piece out to its file. Must run while the pages are still
    // accessible: the flush reads guest memory through the arena.
    void Flush(const Piece& piece);
    // Drops a shared piece's pages and reads the file's current contents back.
    void Invalidate(const Piece& piece);

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
