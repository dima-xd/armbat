#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace armbat {

// A window over the bytes a read-only descriptor has been given. One host read
// covers the request and the neighbourhood behind it, and the reads that follow
// into that neighbourhood are answered from memory: a guest that seeks a byte
// further and reads the same block again -- how a text or archive indexer walks a
// file -- asks FUSE once per window instead of once per read.
//
// The window owns the file position as well, because prefetching leaves the host
// descriptor past the bytes the guest has consumed. Only a descriptor the guest
// cannot write is served this way: a write through any descriptor changes bytes a
// window already holds, so the caller passes the generation it was filled at and
// the window is dropped when that generation moved.
class ReadWindow {
public:
    // Bytes kept from one host read.
    static constexpr std::uint32_t kBytes = 64 * 1024;

    void SetSize(std::uint64_t size) {
        size_ = size;
        size_known_ = true;
    }

    std::uint64_t position() const { return position_; }
    void SetPosition(std::uint64_t position) { position_ = position; }
    bool size_known() const { return size_known_; }
    std::uint64_t size() const { return size_; }

    // True when a read of `length` at the current position is already in memory.
    bool Holds(std::uint64_t generation, std::uint32_t length) const {
        return window_length_ != 0 && window_generation_ == generation &&
               position_ >= window_offset_ &&
               position_ + length <= window_offset_ + window_length_;
    }

    // Answers a read the window holds. `Holds` has to be true.
    void CopyHeld(std::uint8_t* out, std::uint32_t length) {
        std::memcpy(out, window_.data() + (position_ - window_offset_), length);
        position_ += length;
    }

    // Fills the window at the current position from the host and copies the
    // request out of it. Returns the bytes the host answered (0 at end of file)
    // or -1 with errno set.
    std::int64_t Fill(int fd, std::uint8_t* out, std::uint32_t length, std::uint64_t generation);

    // Moves the position the way lseek does. False when the answer needs the host
    // (an unknown size, a whence this does not model, a position below zero).
    bool Seek(std::int64_t offset, int whence, std::int64_t* result);

    // The next read has to come from the host.
    void Drop() { window_length_ = 0; }

private:
    std::uint64_t position_ = 0;
    std::uint64_t size_ = 0;
    bool size_known_ = false;
    std::vector<std::uint8_t> window_;
    std::uint64_t window_offset_ = 0;
    std::uint32_t window_length_ = 0;
    std::uint64_t window_generation_ = 0;
};

}  // namespace armbat
