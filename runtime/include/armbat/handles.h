#pragma once

#include <cstdint>

namespace armbat {

// Guest code is 32-bit; the platform's objects (a window, an EGL display, a
// context) are host pointers. These tables are the only place where the two
// meet: the guest sees a small integer, the container keeps the pointer.
std::uint32_t RegisterHostHandle(void* host);
void* LookupHostHandle(std::uint32_t guest);

}  // namespace armbat
