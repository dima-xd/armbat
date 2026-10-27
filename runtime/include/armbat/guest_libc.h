#pragma once

#include <memory>
#include <string>

namespace armbat {

class GuestHeap;
class HleRegistry;

// The rest of bionic that real applications and libc++ import: number parsing,
// locale, wide characters, environment, directory streams, file metadata and
// process queries.
//
// Everything here runs on the host libc. Guest pointers are translated through
// the arena, and the few structures whose layout differs between 32-bit and
// 64-bit processes (stat, tm, dirent) are converted field by field.
class GuestLibc {
public:
    GuestLibc(HleRegistry& registry, GuestHeap& heap);
    ~GuestLibc();

    GuestLibc(const GuestLibc&) = delete;
    GuestLibc& operator=(const GuestLibc&) = delete;

    bool Install(std::string* error);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace armbat
