#pragma once

#include <cstdint>

namespace armbat {

// Guest code is 32-bit; the platform's objects (a window, an EGL display, a
// context) are host pointers. These tables are the only place where the two
// meet: the guest sees a small integer, the container keeps the pointer.
std::uint32_t RegisterHostHandle(void* host);
void* LookupHostHandle(std::uint32_t guest);
// The handle a host pointer was registered under, or 0. Lets a caller tell
// "the platform gave me this object again" from "this is a new one".
std::uint32_t FindHostHandle(void* host);

}  // namespace armbat
