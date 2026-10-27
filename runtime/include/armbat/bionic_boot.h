#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace armbat {

class A32Machine;
class GuestArena;

// Boots the real Android userland inside the guest arena: maps the bionic
// linker, maps the boot program the linker will initialize, builds the initial
// stack (argc/argv/envp/auxv) and runs the linker's entry point. Control comes
// back from the boot program's own `svc` once libc has initialized and its
// constructors have run.
//
// Everything afterwards goes through the API the boot program publishes: the
// guest's own dlopen/dlsym, so libraries are loaded, relocated and unwound by
// bionic instead of by the container.
class BionicBoot {
public:
    BionicBoot(A32Machine& machine, GuestArena& arena);
    ~BionicBoot();

    BionicBoot(const BionicBoot&) = delete;
    BionicBoot& operator=(const BionicBoot&) = delete;

    // Called by the kernel when the boot program traps with its ready call.
    void OnReady(std::uint32_t api_address);

    bool Boot(std::string* error);
    bool Booted() const { return booted_; }

    // Guest-side operations. A zero result from Dlopen/Dlsym is a failure and
    // LastError() then carries bionic's own message.
    std::uint32_t Dlopen(const std::string& guest_path, int flags);
    std::uint32_t Dlsym(std::uint32_t handle, const std::string& name);
    std::uint32_t Call(std::uint32_t function, std::initializer_list<std::uint32_t> args);

    std::uint32_t Malloc(std::uint32_t size);
    void Free(std::uint32_t address);

    // Tells the linker which SDK the application targets, the way the platform
    // does for a real process. bionic gates legacy compatibility on it: text
    // relocations and DT_NEEDED entries that carry a path only work for
    // applications targeting API 22 or older. Zero means "unknown" and leaves
    // the linker at its own default.
    void SetTargetSdk(std::uint32_t target_sdk);

    // Writes a NUL-terminated string into the guest's scratch page and returns
    // its guest address, or 0 when it does not fit.
    std::uint32_t WriteString(const std::string& text);

    std::string LastError() const;
    std::string Describe() const;

    std::uint32_t LinkerBase() const { return linker_base_; }
    std::uint32_t BootBase() const { return boot_base_; }

private:
    struct Api {
        std::uint32_t dlopen = 0;
        std::uint32_t dlsym = 0;
        std::uint32_t dlclose = 0;
        std::uint32_t dlerror = 0;
        std::uint32_t exit = 0;
        std::uint32_t abort = 0;
        std::uint32_t atexit = 0;
        std::uint32_t malloc = 0;
        std::uint32_t free = 0;
    };

    bool MapImage(const unsigned char* image, std::uint32_t size, std::uint32_t base,
                  std::uint32_t* entry, std::uint32_t* phdr, std::uint32_t* phnum,
                  std::string* error);
    bool BuildStack(std::uint32_t top, const std::string& exec_path, std::uint32_t phdr,
                    std::uint32_t phnum, std::uint32_t entry, std::uint32_t* sp,
                    std::string* error);
    bool ReadApi(std::uint32_t address, std::string* error);

    A32Machine& machine_;
    GuestArena& arena_;

    std::uint32_t linker_base_ = 0;
    std::uint32_t boot_base_ = 0;
    std::uint32_t scratch_ = 0;
    std::uint32_t ready_api_ = 0;
    Api api_;
    bool booted_ = false;
    std::string boot_error_;
};

}  // namespace armbat
