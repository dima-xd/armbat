#include "armbat/kernel_helpers.h"

#include <cstring>
#include <initializer_list>
#include <vector>

#include "armbat/guest_arena.h"
#include "armbat/log.h"

namespace armbat {
namespace {

void Put(std::vector<std::uint32_t>& page, std::uint32_t address,
         std::initializer_list<std::uint32_t> words) {
    std::uint32_t offset = (address - kKernelHelpersAddress) / 4;
    for (const std::uint32_t word : words) {
        if (offset < page.size()) {
            page[offset++] = word;
        }
    }
}

}  // namespace

bool InstallKernelHelpers(GuestArena& arena) {
    if (!arena.Map(kKernelHelpersAddress, 0x1000)) {
        return false;
    }
    std::vector<std::uint32_t> page(0x1000 / 4, 0);
    // __kuser_memory_barrier: the barrier, then the return.
    Put(page, 0xffff0fa0, {0xF57FF05F /* dmb sy */, 0xE12FFF1E /* bx lr */});
    // __kuser_cmpxchg(oldval = r0, newval = r1, ptr = r2), 0 on success and 1
    // when the value had changed. The arguments are the kernel's, not an
    // invented order: libgcc and old bionic call this page as
    // `__kernel_cmpxchg(oldval, newval, ptr)`, so a helper that reads through r0
    // dereferences the old value as a pointer -- address zero for every
    // compare-against-null, which is exactly how a 2012 game touches this page.
    Put(page, 0xffff0fc0, {
        0xE1923F9F,  // ldrex r3, [r2]
        0xE1530000,  // cmp   r3, r0
        0x1A000002,  // bne   0xffff0fd8
        0xE1823F91,  // strex r3, r1, [r2]
        0xE3530000,  // cmp   r3, #0
        0x03A00000,  // moveq r0, #0
        0x13A00001,  // movne r0, #1
        0xE12FFF1E,  // bx    lr
    });
    // __kuser_get_tls: the thread pointer, the register bionic's own __get_tls
    // reads.
    Put(page, 0xffff0fe0, {0xEE1D0F70 /* mrc p15, 0, r0, c13, c0, 3 */, 0xE12FFF1E /* bx lr */});
    // __kuser_helper_version: the highest helper this page implements.
    Put(page, 0xffff0ff0, {5});
    std::memcpy(arena.Ptr(kKernelHelpersAddress), page.data(), page.size() * sizeof(std::uint32_t));
    ARMBAT_LOG_INFO("Armbat", "kernel helpers: barrier, cmpxchg and tls at 0x%08x",
                    kKernelHelpersAddress);
    return true;
}

}  // namespace armbat
