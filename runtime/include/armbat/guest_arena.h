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

    // Applies the protection the guest asked for to a range it has mapped.
    //
    // The arena is host memory, so this is the same mprotect the guest called,
    // and it has to be done: bionic puts a guard page at the bottom of every
    // thread stack with mprotect(PROT_NONE), and with nothing behind the call an
    // overflowing thread does not die on its guard -- it writes straight into the
    // stack of the thread mapped below it, which is a corrupted thread instead of
    // a crash in the one that was wrong. A collector's write barrier is the same
    // bargain: it protects a page, takes the fault, and marks it dirty.
    bool Protect(std::uint32_t addr, std::uint64_t len, int prot);

    // End of the contiguous range that contains `addr` and that the container
    // itself may copy into or out of: the kernel writes a guest buffer with a
    // host memcpy, so a page the guest protected must end the range here even
    // though the mapping continues past it.
    std::uint64_t AccessibleEnd(std::uint32_t addr) const;

    // Whether the guest has mapped the range, whatever protection it put on it.
    // mprotect validates against this: a page the guest itself made inaccessible
    // is still its page, and it must be able to make it accessible again.
    bool IsCommitted(std::uint32_t addr, std::uint64_t len = 1) const;

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
            const std::uint8_t state = pages_[index].load(std::memory_order_acquire);
            if (state == kPageFree || state == kPageGuard) {
                return false;
            }
        }
        return true;
    }

    // True when any page of the range is mapped. Asking IsMapped() once per
    // page cost a call and a bounds check per 4 KiB, which a guest's large
    // mmap pays for every time it probes a range before taking it.
    bool AnyMapped(std::uint32_t addr, std::uint64_t len) const {
        const std::uint64_t start = addr;
        const std::uint64_t end = static_cast<std::uint64_t>(addr) + len;
        if (len == 0 || end > size_ || pages_ == nullptr) {
            return false;
        }
        const std::uint64_t first = start >> page_shift_;
        const std::uint64_t last = (end - 1) >> page_shift_;
        for (std::uint64_t index = first; index <= last; ++index) {
            const std::uint8_t state = pages_[index].load(std::memory_order_acquire);
            // Same answer IsMapped() gives for one page: a guard page is
            // refused to the guest, so it is not a mapping either.
            if (state != kPageFree && state != kPageGuard) {
                return true;
            }
        }
        return false;
    }

    // End of the contiguous mapped range that contains `addr`, or 0 when the
    // address is not mapped. String scans use this to validate a whole run of
    // pages at once instead of asking about every byte.
    std::uint64_t MappedEnd(std::uint32_t addr) const;

    std::byte* Ptr(std::uint32_t addr) { return base_ + addr; }
    const std::byte* Ptr(std::uint32_t addr) const { return base_ + addr; }

private:
    void Normalize();

    // `pages_` is one byte per guest page, and the value is the answer a guest
    // access gets. A page the guest protected still belongs to it (mprotect has
    // to be able to give the access back), so "inaccessible" is a state of its
    // own and not the same byte as "never mapped".
    static constexpr std::uint8_t kPageFree = 0;
    static constexpr std::uint8_t kPageReadWrite = 1;
    static constexpr std::uint8_t kPageGuard = 2;
    static constexpr std::uint8_t kPageReadOnly = 3;

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
