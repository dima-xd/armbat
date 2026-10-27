#include "armbat/guest_heap.h"

#include <android/log.h>

#include "armbat/log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "armbat/guest_arena.h"
#include "armbat/a32_machine.h"
#include "armbat/hle.h"

namespace armbat {
namespace {

constexpr std::uint32_t kHeaderSize = 8;
constexpr std::uint32_t kMinBlock = 16;  // header plus the two free-list links
// 16-byte blocks. malloc() therefore hands out payloads on an 8-byte boundary
// (block + kHeaderSize), which is what bionic guarantees on ARM32. Anything that
// needs more asks for it through memalign and gets a shifted pointer that
// `aligned_` remembers.
constexpr std::uint32_t kAlign = 16;
constexpr std::uint32_t kUsedBit = 1;
constexpr std::uint32_t kSizeMask = ~std::uint32_t{15};

// CheckHeap walks every block header and the whole free list. That is a debugging
// tool, not something a game can afford on every malloc: with a busy heap it
// turns a constant-time allocation into a scan of hundreds of thousands of blocks.
constexpr bool kVerifyHeap = false;

std::uint32_t Align(std::uint32_t value) {
    return (value + kAlign - 1) & ~(kAlign - 1);
}

}  // namespace

GuestHeap::GuestHeap(GuestArena& arena, std::uint32_t base, std::uint32_t size)
    : arena_(arena), base_(base), size_(size) {}

bool GuestHeap::Init(std::string* error) {
    if (size_ < kMinBlock) {
        if (error) *error = "guest heap is too small";
        return false;
    }
    if (!arena_.Map(base_, size_)) {
        if (error) *error = "cannot map the guest heap";
        return false;
    }

    // Map() commits fresh anonymous pages, so the heap is already zero. Zeroing
    // it again would touch every page and make the whole heap resident at startup.
    size_ &= ~(kAlign - 1);
    SetBlockSize(base_, size_, false);
    SetPreviousSize(base_, 0);
    free_head_ = base_;
    in_use_ = 0;
    ready_ = true;
    return true;
}

std::uint32_t GuestHeap::BlockAt(std::uint32_t address) const {
    return address;
}

std::uint32_t GuestHeap::BlockSize(std::uint32_t address) const {
    std::uint32_t value = 0;
    std::memcpy(&value, arena_.Ptr(address), sizeof(value));
    return value & kSizeMask;
}

bool GuestHeap::BlockUsed(std::uint32_t address) const {
    std::uint32_t value = 0;
    std::memcpy(&value, arena_.Ptr(address), sizeof(value));
    return (value & kUsedBit) != 0;
}

void GuestHeap::SetBlockSize(std::uint32_t address, std::uint32_t size, bool used) {
    const std::uint32_t value = (size & kSizeMask) | (used ? kUsedBit : 0);
    std::memcpy(arena_.Ptr(address), &value, sizeof(value));
}

void GuestHeap::SetPreviousSize(std::uint32_t address, std::uint32_t previous_size) {
    std::memcpy(arena_.Ptr(address + 4), &previous_size, sizeof(previous_size));
}

std::uint32_t GuestHeap::NextFree(std::uint32_t address) const {
    std::uint32_t value = 0;
    std::memcpy(&value, arena_.Ptr(address + kHeaderSize), sizeof(value));
    return value;
}

void GuestHeap::LinkFree(std::uint32_t address) {
    const std::uint32_t head = free_head_;
    std::memcpy(arena_.Ptr(address + kHeaderSize), &head, sizeof(head));
    const std::uint32_t zero = 0;
    std::memcpy(arena_.Ptr(address + kHeaderSize + 4), &zero, sizeof(zero));
    if (head != 0) {
        std::memcpy(arena_.Ptr(head + kHeaderSize + 4), &address, sizeof(address));
    }
    free_head_ = address;
}

void GuestHeap::UnlinkFree(std::uint32_t address) {
    std::uint32_t next = 0;
    std::uint32_t previous = 0;
    std::memcpy(&next, arena_.Ptr(address + kHeaderSize), sizeof(next));
    std::memcpy(&previous, arena_.Ptr(address + kHeaderSize + 4), sizeof(previous));

    if (next != 0 && (next < base_ || next >= base_ + size_)) {
        ARMBAT_LOG_ERROR("Armbat",
                            "heap: corrupt free list block=0x%08x next=0x%08x prev=0x%08x",
                            address, next, previous);
        next = 0;
    }
    if (previous != 0 && (previous < base_ || previous >= base_ + size_)) {
        ARMBAT_LOG_ERROR("Armbat",
                            "heap: corrupt free list block=0x%08x next=0x%08x prev=0x%08x",
                            address, next, previous);
        previous = 0;
    }

    if (previous != 0) {
        std::memcpy(arena_.Ptr(previous + kHeaderSize), &next, sizeof(next));
    } else {
        free_head_ = next;
    }
    if (next != 0) {
        std::memcpy(arena_.Ptr(next + kHeaderSize + 4), &previous, sizeof(previous));
    }
}

bool GuestHeap::CheckHeap(const char* operation) {
    if (!ready_ || check_reported_) {
        return true;
    }
    const std::uint32_t end = base_ + size_;
    struct HistoryEntry {
        std::uint32_t block;
        std::uint32_t raw;
        std::uint32_t stored_previous;
    };
    HistoryEntry history[8]{};
    int history_count = 0;
    std::uint32_t block = base_;
    std::uint32_t previous_size = 0;
    int blocks = 0;
    while (block < end) {
        std::uint32_t raw = 0;
        std::memcpy(&raw, arena_.Ptr(block), sizeof(raw));
        const std::uint32_t block_size = raw & kSizeMask;
        std::uint32_t stored_previous = 0;
        std::memcpy(&stored_previous, arena_.Ptr(block + 4), sizeof(stored_previous));
        history[history_count % 8] = HistoryEntry{block, raw, stored_previous};
        ++history_count;
        if (block_size < kMinBlock || (block_size % kAlign) != 0 || block + block_size > end) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap check after %s: block 0x%08x raw=0x%08x previous=%u free=0x%08x",
                                operation, block, raw, previous_size, free_head_);
            for (int i = 0; i < 8 && i < history_count; ++i) {
                const HistoryEntry& entry = history[(history_count - 1 - i) % 8];
                ARMBAT_LOG_ERROR("Armbat",
                                    "  block 0x%08x raw=0x%08x stored_previous=%u", entry.block,
                                    entry.raw, entry.stored_previous);
            }
            const std::uint32_t anchor =
                history_count >= 2 ? history[(history_count - 2) % 8].block : block;
            const std::uint32_t dump_start = anchor > base_ + 160 ? anchor - 160 : base_;
            for (std::uint32_t row = dump_start; row < anchor + 16; row += 32) {
                std::uint32_t words[8] = {};
                for (int i = 0; i < 8; ++i) {
                    std::memcpy(&words[i], arena_.Ptr(row + static_cast<std::uint32_t>(i) * 4),
                                sizeof(std::uint32_t));
                }
                ARMBAT_LOG_ERROR("Armbat",
                                    "  mem 0x%08x: %08x %08x %08x %08x %08x %08x %08x %08x", row,
                                    words[0], words[1], words[2], words[3], words[4], words[5],
                                    words[6], words[7]);
            }
            check_reported_ = true;
            return false;
        }
        if (stored_previous != previous_size) {
            ARMBAT_LOG_ERROR("Armbat",
                "heap check after %s: block 0x%08x stored_previous=%u expected=%u used=%d",
                operation, block, stored_previous, previous_size, (raw & kUsedBit) != 0);
            for (int i = 0; i < 8 && i < history_count; ++i) {
                const HistoryEntry& entry = history[(history_count - 1 - i) % 8];
                ARMBAT_LOG_ERROR("Armbat",
                                    "  block 0x%08x raw=0x%08x stored_previous=%u", entry.block,
                                    entry.raw, entry.stored_previous);
            }
            check_reported_ = true;
            return false;
        }
        previous_size = block_size;
        block += block_size;
        if (++blocks > 200000) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap check after %s: no end after %d blocks", operation, blocks);
            check_reported_ = true;
            return false;
        }
    }
    if (block != end) {
        ARMBAT_LOG_ERROR("Armbat",
                            "heap check after %s: blocks end at 0x%08x, heap ends at 0x%08x",
                            operation, block, end);
        check_reported_ = true;
        return false;
    }

    std::uint32_t link_previous = 0;
    std::uint32_t free_block = free_head_;
    int links = 0;
    while (free_block != 0) {
        if (free_block < base_ || free_block >= end ||
            ((free_block - base_) % kAlign) != 0) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap check after %s: free list entry 0x%08x is out of range",
                                operation, free_block);
            check_reported_ = true;
            return false;
        }
        std::uint32_t raw = 0;
        std::memcpy(&raw, arena_.Ptr(free_block), sizeof(raw));
        if ((raw & kUsedBit) != 0) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap check after %s: free list block 0x%08x is marked used",
                                operation, free_block);
            check_reported_ = true;
            return false;
        }
        std::uint32_t next = 0;
        std::uint32_t previous = 0;
        std::memcpy(&next, arena_.Ptr(free_block + kHeaderSize), sizeof(next));
        std::memcpy(&previous, arena_.Ptr(free_block + kHeaderSize + 4), sizeof(previous));
        if (previous != link_previous) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap check after %s: free list block 0x%08x previous=0x%08x "
                                "expected=0x%08x",
                                operation, free_block, previous, link_previous);
            check_reported_ = true;
            return false;
        }
        link_previous = free_block;
        free_block = next;
        if (++links > 200000) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap check after %s: free list does not end", operation);
            check_reported_ = true;
            return false;
        }
    }
    return true;
}

std::uint32_t GuestHeap::Allocate(std::uint32_t size) {
    const std::lock_guard lock(mutex_);
    if constexpr (kVerifyHeap) {
        CheckHeap("malloc");
    }
    if (!ready_ || size > size_ - kMinBlock) {
        return 0;
    }
    const std::uint32_t need = std::max(Align(size + kHeaderSize), kMinBlock);
    const std::uint32_t end = base_ + size_;

    std::uint32_t steps = 0;
    for (std::uint32_t block = free_head_; block != 0; block = NextFree(block)) {
        // The free list lives inside freed blocks: any use-after-free in the guest
        // can replace `next` with arbitrary bytes. Walking it without checking
        // would read host memory at an address the guest chose.
        if (block < base_ || block >= end || ((block - base_) % kAlign) != 0 ||
            ++steps > 200000) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap: corrupt free list at 0x%08x (step %u)", block, steps);
            break;
        }
        const std::uint32_t block_size = BlockSize(block);
        if (block_size < kMinBlock || block_size % kAlign != 0 ||
            std::uint64_t(block) + block_size > end) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap: corrupt free block 0x%08x size=%u", block, block_size);
            break;
        }
        if (block_size < need) {
            continue;
        }

        UnlinkFree(block);
        const std::uint32_t remainder = block_size - need;
        if (remainder >= kMinBlock) {
            const std::uint32_t rest = block + need;
            SetBlockSize(block, need, true);
            SetBlockSize(rest, remainder, false);
            SetPreviousSize(rest, need);
            const std::uint32_t after = rest + remainder;
            if (after < base_ + size_) {
                SetPreviousSize(after, remainder);
            }
            LinkFree(rest);
            in_use_ += need - kHeaderSize;
        } else {
            SetBlockSize(block, block_size, true);
            in_use_ += block_size - kHeaderSize;
        }
        return block + kHeaderSize;
    }

    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) < 12) {
        ARMBAT_LOG_ERROR("Armbat",
                            "heap: no block for %u bytes (need %u, in_use %u of %u)", size, need,
                            in_use_, size_);
    }
    return 0;
}

std::uint32_t GuestHeap::AllocateAligned(std::uint32_t alignment, std::uint32_t size) {
    if (alignment <= kAlign) {
        return Allocate(size);
    }
    if ((alignment & (alignment - 1)) != 0) {
        return 0;
    }
    const std::lock_guard lock(mutex_);
    if (!ready_ || size > size_ - kMinBlock || alignment > 0x10000) {
        return 0;
    }
    const std::uint32_t payload = Allocate(size + alignment + 4);
    if (payload == 0) {
        return 0;
    }
    const std::uint32_t aligned = (payload + alignment - 1) & ~(alignment - 1);
    if (aligned == payload) {
        return payload;
    }
    // The shifted pointer no longer maps back to a header by subtracting
    // kHeaderSize, so free()/realloc() look it up here instead.
    aligned_.emplace(aligned, payload);
    return aligned;
}

std::uint32_t GuestHeap::Reallocate(std::uint32_t address, std::uint32_t size) {
    const std::lock_guard lock(mutex_);
    if (address == 0) {
        return Allocate(size);
    }
    if (size == 0) {
        Release(address);
        return 0;
    }

    std::uint32_t payload = address;
    const auto aligned_entry = aligned_.find(address);
    if (aligned_entry != aligned_.end()) {
        payload = aligned_entry->second;
    }
    if (payload < base_ + kHeaderSize || payload >= base_ + size_ ||
        ((payload - kHeaderSize) - base_) % kAlign != 0) {
        return 0;
    }
    const std::uint32_t block = payload - kHeaderSize;
    if (!BlockUsed(block)) {
        return 0;
    }

    const std::uint32_t old_size = BlockSize(block);
    const std::uint32_t old_payload = old_size - kHeaderSize;
    const std::uint32_t need = std::max(Align(size + kHeaderSize), kMinBlock);
    const std::uint32_t end = base_ + size_;

    if (aligned_entry != aligned_.end()) {
        // A shifted allocation cannot be grown in place: move it and let the map
        // entry go away with the old payload.
        const std::uint32_t fresh = Allocate(size);
        if (fresh == 0) {
            return 0;
        }
        std::memcpy(arena_.Ptr(fresh), arena_.Ptr(address), std::min(old_payload, size));
        Release(address);
        return fresh;
    }

    if (need <= old_size) {
        const std::uint32_t remainder = old_size - need;
        if (remainder >= kMinBlock) {
            SetBlockSize(block, need, true);
            const std::uint32_t rest = block + need;
            std::uint32_t rest_size = remainder;
            const std::uint32_t after = rest + remainder;
            // The tail just cut off may be followed by a free block (a previous
            // shrinking realloc). Two adjacent free blocks are something the
            // "first fit" walk never produces, so merge them here.
            if (after < end) {
                const std::uint32_t after_size = BlockSize(after);
                if (!BlockUsed(after) && after_size >= kMinBlock &&
                    after_size % kAlign == 0 && std::uint64_t(after) + after_size <= end) {
                    UnlinkFree(after);
                    rest_size += after_size;
                }
            }
            SetBlockSize(rest, rest_size, false);
            SetPreviousSize(rest, need);
            const std::uint32_t new_after = rest + rest_size;
            if (new_after < end) {
                SetPreviousSize(new_after, rest_size);
            }
            LinkFree(rest);
            in_use_ -= old_payload - (need - kHeaderSize);
        }
        return address;
    }

    // Grow into the next block when it is free and large enough. Without this a
    // std::vector doubling its storage copies the whole buffer on every step.
    const std::uint32_t next = block + old_size;
    if (next < end && !BlockUsed(next)) {
        const std::uint32_t next_size = BlockSize(next);
        if (next_size >= kMinBlock && next_size % kAlign == 0 &&
            std::uint64_t(next) + next_size <= end && old_size + next_size >= need) {
            UnlinkFree(next);
            const std::uint32_t merged = old_size + next_size;
            const std::uint32_t remainder = merged - need;
            if (remainder >= kMinBlock) {
                SetBlockSize(block, need, true);
                const std::uint32_t rest = block + need;
                SetBlockSize(rest, remainder, false);
                SetPreviousSize(rest, need);
                const std::uint32_t after = rest + remainder;
                if (after < end) {
                    SetPreviousSize(after, remainder);
                }
                LinkFree(rest);
                in_use_ += need - old_size;
            } else {
                SetBlockSize(block, merged, true);
                const std::uint32_t after = block + merged;
                if (after < end) {
                    SetPreviousSize(after, merged);
                }
                in_use_ += merged - old_size;
            }
            return address;
        }
    }

    const std::uint32_t fresh = Allocate(size);
    if (fresh == 0) {
        return 0;
    }
    std::memcpy(arena_.Ptr(fresh), arena_.Ptr(payload), std::min(old_payload, size));
    Release(address);
    return fresh;
}

bool GuestHeap::Release(std::uint32_t address) {
    const std::lock_guard lock(mutex_);
    if constexpr (kVerifyHeap) {
        CheckHeap("free");
    }
    if (!ready_ || address == 0) {
        return false;
    }

    // A memalign pointer was shifted inside a larger block; the map remembers
    // where the block's payload really is.
    std::uint32_t payload = address;
    const auto aligned_entry = aligned_.find(address);
    if (aligned_entry != aligned_.end()) {
        payload = aligned_entry->second;
        aligned_.erase(aligned_entry);
    }
    if (payload < base_ + kHeaderSize || payload >= base_ + size_) {
        return false;
    }
    if (((payload - kHeaderSize) - base_) % kAlign != 0) {
        return false;
    }

    std::uint32_t block = BlockAt(payload - kHeaderSize);
    if (!BlockUsed(block)) {
        return false;
    }

    std::uint32_t size = BlockSize(block);
    const std::uint32_t end = base_ + size_;
    if (size < kMinBlock || size % kAlign != 0 || block + size > end) {
        ARMBAT_LOG_ERROR("Armbat",
                            "heap: free 0x%08x has corrupt header 0x%08x size=%u", address, block,
                            size);
        return false;
    }
    in_use_ -= size - kHeaderSize;

    std::uint32_t after = block + size;
    if (after < end && !BlockUsed(after)) {
        const std::uint32_t after_size = BlockSize(after);
        if (after_size < kMinBlock || after_size % kAlign != 0 || after + after_size > end) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap: free 0x%08x next block 0x%08x has corrupt header size=%u",
                                address, after, after_size);
        } else {
            UnlinkFree(after);
            size += after_size;
        }
    }

    const std::uint32_t previous_size = [&] {
        std::uint32_t value = 0;
        std::memcpy(&value, arena_.Ptr(block + 4), sizeof(value));
        return value;
    }();
    if (previous_size != 0) {
        if (previous_size % kAlign != 0 || previous_size > block - base_) {
            ARMBAT_LOG_ERROR("Armbat",
                                "heap: free 0x%08x block 0x%08x has corrupt previous_size=%u",
                                address, block, previous_size);
        } else {
            const std::uint32_t previous = block - previous_size;
            if (previous >= base_ && !BlockUsed(previous)) {
                const std::uint32_t previous_block_size = BlockSize(previous);
                if (previous_block_size < kMinBlock || previous_block_size % kAlign != 0 ||
                    previous + previous_block_size > end) {
                    ARMBAT_LOG_ERROR("Armbat",
                        "heap: free 0x%08x previous block 0x%08x has corrupt header size=%u",
                        address, previous, previous_block_size);
                } else {
                    UnlinkFree(previous);
                    size += previous_size;
                    block = previous;
                }
            }
        }
    }

    SetBlockSize(block, size, false);
    after = block + size;
    if (after < base_ + size_) {
        SetPreviousSize(after, size);
    }
    LinkFree(block);
    return true;
}

std::uint32_t GuestHeap::UsableSize(std::uint32_t address) const {
    const std::lock_guard lock(mutex_);
    if (!ready_ || address == 0) {
        return 0;
    }
    std::uint32_t payload = address;
    const auto entry = aligned_.find(address);
    if (entry != aligned_.end()) {
        payload = entry->second;
    }
    if (payload < base_ + kHeaderSize || payload >= base_ + size_ ||
        ((payload - kHeaderSize) - base_) % kAlign != 0) {
        return 0;
    }
    const std::uint32_t block = payload - kHeaderSize;
    if (!BlockUsed(block)) {
        return 0;
    }
    const std::uint32_t size = BlockSize(block);
    if (size < kMinBlock || size % kAlign != 0 ||
        std::uint64_t(block) + size > std::uint64_t(base_) + size_) {
        return 0;
    }
    return size - kHeaderSize;
}

void GuestHeap::RegisterFunctions(HleRegistry& registry) {
    registry.Add("malloc", [this](HleCall& call) { call.Return(Allocate(call.Arg(0))); });

    registry.Add("free", [this](HleCall& call) {
        const std::uint32_t address = call.Arg(0);
        if (address != 0 && !Release(address)) {
            const std::uint32_t caller = call.Machine().Reg(14);
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1) < 24) {
                ARMBAT_LOG_ERROR("Armbat",
                                    "heap: free(0x%08x) rejected, caller lr=0x%08x", address,
                                    caller);
            }
        }
        call.Return(0);
    });

    registry.Add("calloc", [this](HleCall& call) {
        const std::uint64_t total = static_cast<std::uint64_t>(call.Arg(0)) * call.Arg(1);
        if (total > 0xFFFFFFFFull) {
            call.Return(0);
            return;
        }
        const std::uint32_t address = Allocate(static_cast<std::uint32_t>(total));
        if (address != 0) {
            call.Zero(address, static_cast<std::uint32_t>(total));
        }
        call.Return(address);
    });

    registry.Add("realloc", [this](HleCall& call) {
        call.Return(Reallocate(call.Arg(0), call.Arg(1)));
    });

    registry.Add("reallocarray", [this](HleCall& call) {
        const std::uint64_t total = static_cast<std::uint64_t>(call.Arg(1)) * call.Arg(2);
        if (total > 0xFFFFFFFFull) {
            call.Return(0);
            return;
        }
        call.Return(Reallocate(call.Arg(0), static_cast<std::uint32_t>(total)));
    });

    registry.Add("memalign", [this](HleCall& call) {
        call.Return(AllocateAligned(call.Arg(0), call.Arg(1)));
    });
    registry.Add("aligned_alloc", [this](HleCall& call) {
        call.Return(AllocateAligned(call.Arg(0), call.Arg(1)));
    });
    registry.Add("posix_memalign", [this](HleCall& call) {
        const std::uint32_t alignment = call.Arg(0);
        const std::uint32_t size = call.Arg(1);
        const std::uint32_t output = call.Arg(2);
        if (alignment < sizeof(std::uint32_t) || (alignment & (alignment - 1)) != 0) {
            call.Return(22);  // EINVAL
            return;
        }
        const std::uint32_t address = AllocateAligned(alignment, size);
        if (address == 0) {
            call.Return(12);  // ENOMEM
            return;
        }
        if (output == 0 || !call.CopyOut(output, &address, sizeof(address))) {
            Release(address);
            call.Return(14);  // EFAULT
            return;
        }
        call.Return(0);
    });
    registry.Add("malloc_usable_size", [this](HleCall& call) {
        call.Return(UsableSize(call.Arg(0)));
    });

    // C++ allocation operators. We do not run libc++ itself; these are the only
    // symbols a game's own C++ code needs before it pulls in the standard
    // library (std::string, containers, iostreams are not covered yet).
    registry.Add("_Znwj", [this](HleCall& call) {
        const std::uint32_t address = Allocate(call.Arg(0));
        if (address == 0) {
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1) < 8) {
                ARMBAT_LOG_ERROR("Armbat",
                                    "heap: operator new(%u) failed, caller lr=0x%08x",
                                    call.Arg(0), call.Machine().Reg(14));
            }
        }
        call.Return(address);
    });
    registry.Add("_Znaj", [this](HleCall& call) { call.Return(Allocate(call.Arg(0))); });
    registry.Add("_ZdlPv", [this](HleCall& call) {
        Release(call.Arg(0));
        call.Return(0);
    });
    registry.Add("_ZdaPv", [this](HleCall& call) {
        Release(call.Arg(0));
        call.Return(0);
    });
    // Not-throwing forms: a null result is the contract, not a fault.
    registry.Add("_ZnwjRKSt9nothrow_t", [this](HleCall& call) { call.Return(Allocate(call.Arg(0))); });
    registry.Add("_ZnajRKSt9nothrow_t", [this](HleCall& call) { call.Return(Allocate(call.Arg(0))); });
    registry.Add("_ZdlPvRKSt9nothrow_t", [this](HleCall& call) {
        Release(call.Arg(0));
        call.Return(0);
    });
    registry.Add("_ZdaPvRKSt9nothrow_t", [this](HleCall& call) {
        Release(call.Arg(0));
        call.Return(0);
    });
    // Sized delete: clang emits these by default from C++14 on. The size is
    // ignored, but the symbol has to resolve.
    registry.Add("_ZdlPvj", [this](HleCall& call) {
        Release(call.Arg(0));
        call.Return(0);
    });
    registry.Add("_ZdaPvj", [this](HleCall& call) {
        Release(call.Arg(0));
        call.Return(0);
    });
}

}  // namespace armbat
