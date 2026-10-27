#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace armbat {

// Flat guest address space: guest address is a 32-bit offset into the arena, so
// host_pointer = Base() + guest_address and no pointer translation is needed.
//
// The full 4 GiB is reserved PROT_NONE up front; guest pages are committed
// lazily with Map(). A guest access to an uncommitted page faults, which is
// exactly the signal dynarmic's fastmem needs in order to fall back to the
// MemoryRead*/MemoryWrite* callbacks.
class GuestArena {
public:
    static constexpr std::uint64_t kDefaultSize = 4ull << 30;

    explicit GuestArena(std::uint64_t size = kDefaultSize);
    ~GuestArena();

    GuestArena(const GuestArena&) = delete;
    GuestArena& operator=(const GuestArena&) = delete;

    bool Ok() const { return base_ != nullptr; }
    const char* Error() const { return error_; }

    std::byte* Base() const { return base_; }
    std::uint64_t Size() const { return size_; }

    // Commits [addr, addr+len) as readable and writable.
    bool Map(std::uint32_t addr, std::uint64_t len);

    // Returns [addr, addr+len) to PROT_NONE and forgets the mapping, so a later
    // Map() at the same address is a fresh allocation.
    bool Unmap(std::uint32_t addr, std::uint64_t len);

    // The hot path of the whole emulator: asked in front of every guest memory
    // access that did not go through fastmem, in front of every pointer argument
    // of an HLE call, and for every instruction the JIT fetches. Defined here so
    // that it inlines into its callers, and it shifts instead of dividing: the
    // page size is a run-time value, and a divide on this path cost more than
    // the page lookup itself.
    bool IsMapped(std::uint32_t addr, std::uint64_t len = 1) const {
        const std::uint64_t start = addr;
        const std::uint64_t end = static_cast<std::uint64_t>(addr) + len;
        if (len == 0 || end > size_ || pages_ == nullptr) {
            return false;
        }
        const std::uint64_t first = start >> page_shift_;
        const std::uint64_t last = (end - 1) >> page_shift_;
        for (std::uint64_t index = first; index <= last; ++index) {
            if (pages_[index].load(std::memory_order_acquire) == 0) {
                return false;
            }
        }
        return true;
    }

    // End of the contiguous mapped range that contains `addr`, or 0 when the
    // address is not mapped. String scans use this to validate a whole run of
    // pages at once instead of asking about every byte.
    std::uint64_t MappedEnd(std::uint32_t addr) const;

    std::byte* Ptr(std::uint32_t addr) { return base_ + addr; }
    const std::byte* Ptr(std::uint32_t addr) const { return base_ + addr; }

private:
    void Normalize();

    std::byte* base_{nullptr};
    std::uint64_t size_{0};
    std::uint64_t page_size_{4096};
    std::uint64_t page_shift_{12};
    const char* error_{"not initialised"};
    // One byte per guest page: the answer every guest load and store needs.
    // The JIT asks whether an address is mapped in front of each memory access,
    // so the answer cannot be a lock and a binary search -- that put a shared
    // mutex on the hottest path in the emulator. Mapping still goes through the
    // range list below, which is what MappedEnd answers from.
    std::unique_ptr<std::atomic<std::uint8_t>[]> pages_;
    // Guest threads map and query ranges concurrently: mmap from one thread while
    // another faults into the callbacks. Readers are the common case.
    mutable std::shared_mutex ranges_mutex_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> mapped_;
};

}  // namespace armbat
