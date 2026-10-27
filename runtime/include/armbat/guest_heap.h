#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace armbat {

class GuestArena;
class HleRegistry;

// A 32-bit allocator living inside the guest arena.
//
// Blocks carry boundary tags (size plus the size of the physically previous
// block), so free() coalesces with both neighbours. Without that, a game that
// allocates and frees in a loop turns the heap into unusable holes.
class GuestHeap {
public:
    GuestHeap(GuestArena& arena, std::uint32_t base, std::uint32_t size);

    bool Init(std::string* error);

    // Returns a guest address aligned to 16 bytes, or 0.
    std::uint32_t Allocate(std::uint32_t size);
    // memalign/aligned_alloc/posix_memalign. `alignment` must be a power of two.
    std::uint32_t AllocateAligned(std::uint32_t alignment, std::uint32_t size);
    std::uint32_t Reallocate(std::uint32_t address, std::uint32_t size);
    bool Release(std::uint32_t address);
    // Payload capacity of an allocation, or 0 when the address is not ours.
    std::uint32_t UsableSize(std::uint32_t address) const;

    // malloc, free, calloc, realloc.
    void RegisterFunctions(HleRegistry& registry);

    std::uint32_t Base() const { return base_; }
    std::uint32_t InUse() const {
        const std::lock_guard lock(mutex_);
        return in_use_;
    }

private:
    std::uint32_t BlockAt(std::uint32_t address) const;
    std::uint32_t BlockSize(std::uint32_t address) const;
    bool BlockUsed(std::uint32_t address) const;
    void SetBlockSize(std::uint32_t address, std::uint32_t size, bool used);
    std::uint32_t NextFree(std::uint32_t address) const;
    void LinkFree(std::uint32_t address);
    void UnlinkFree(std::uint32_t address);
    void SetPreviousSize(std::uint32_t address, std::uint32_t previous_size);

    // Walks every block header and both free-list directions. A stray write or a
    // wrong split shows up as a block that no longer tiles the heap; the first
    // report names the operation that noticed it.
    bool CheckHeap(const char* operation);

    GuestArena& arena_;
    std::uint32_t base_ = 0;
    std::uint32_t size_ = 0;
    std::uint32_t free_head_ = 0;
    std::uint32_t in_use_ = 0;
    bool ready_ = false;
    bool check_reported_ = false;
    // Payload address -> real payload, for allocations that had to be shifted to
    // satisfy memalign. Only entries here are ambiguous, so free() can tell an
    // aligned pointer from an ordinary one without guessing.
    std::unordered_map<std::uint32_t, std::uint32_t> aligned_;
    // Guest threads allocate concurrently; Reallocate calls Allocate and Release,
    // so the lock has to be re-entrant.
    mutable std::recursive_mutex mutex_;
};

}  // namespace armbat
