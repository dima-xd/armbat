#pragma once

#include <cstdint>

namespace armbat {

class GuestArena;

// The vector page: the 32-bit ARM kernel maps it at the very top of the address
// space so that code compiled for ARMv5/v6 has a place to call for a barrier, a
// compare-and-swap or the thread pointer. It is not a library -- the kernel
// mapped one page at 0xffff0000 and put a handful of routines at its top -- and
// code reaches it by address, not by import: a memory barrier is a
// `blx 0xffff0fa0`, a compare-and-swap is `blx 0xffff0fc0`. The archive calls
// them: creating a world runs MC's leveldb environment setup, which barriers
// through that page and then fetches from an address this process never maps.
//
// The kernel here is 64-bit and maps no such page, so the container provides it,
// at the same addresses and with the same contract, as ARM code the JIT runs
// like any other guest code.
inline constexpr std::uint32_t kKernelHelpersAddress = 0xffff0000;

// Commits the page and fills it with the four helpers an old image can call by
// address: __kuser_memory_barrier (0xffff0fa0), __kuser_cmpxchg (0xffff0fc0),
// __kuser_get_tls (0xffff0fe0) and __kuser_helper_version (0xffff0ff0).
//
// Every path that runs ARMv7 code needs the same page: the container itself and
// the loader selftest, whose atomics suite calls 0xffff0fc0 directly. Requires
// an arena that covers the whole 32-bit space; a smaller one refuses the mapping
// and this returns false.
bool InstallKernelHelpers(GuestArena& arena);

}  // namespace armbat
