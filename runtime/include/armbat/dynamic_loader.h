#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "armbat/elf32_loader.h"

namespace armbat {

class A32Machine;
class GuestArena;
class HleRegistry;

// Runtime loading for guest libraries: dlopen/dlsym on top of Elf32Loader.
//
// Every module gets its own base inside the arena; the base doubles as the
// dlopen handle. Symbols are searched in the module itself, then across all
// modules opened so far.
class DynamicLoader {
public:
    DynamicLoader(A32Machine& machine, GuestArena& arena, Elf32Loader& loader,
                  std::uint32_t first_base);

    // Loads a host file and runs its initializers. `machine` is the machine of
    // the calling guest thread: dlopen runs inside an HLE call, and a constructor
    // may arrive from any thread. Returns the handle, or 0.
    std::uint32_t Open(const std::string& host_path, A32Machine& machine, std::string* error);

    // Loads without an active guest thread (container startup, dependency
    // ordering). Uses the loader's own machine.
    std::uint32_t Open(const std::string& host_path, std::string* error) {
        return Open(host_path, machine_, error);
    }

    // Maps an image the container carries itself (the arm32 unwinder). It has
    // no initializers and its symbols are visible to every later import.
    std::uint32_t OpenEmbedded(const void* image, std::size_t size, const std::string& soname,
                               std::string* error);

    std::uint32_t Symbol(std::uint32_t handle, std::string_view name) const;

    // Exported by any module opened so far; used to satisfy imports.
    std::uint32_t Resolve(std::string_view name) const;

    // dlopen, dlsym, dlclose, dlerror.
    void RegisterFunctions(HleRegistry& registry);

    std::size_t Count() const { return libraries_.size(); }

    // The images opened so far, in load order. The JNI bridge walks their
    // exports to find the Java_* entry points of the archive.
    //
    // A deque, not a vector: a constructor loaded by dlopen may itself call
    // dlopen, so the container has to keep its elements in place.
    const std::deque<LoadedLibrary>& Libraries() const { return libraries_; }

private:
    void RememberError(std::string message);
    void ForgetError();

    A32Machine& machine_;
    GuestArena& arena_;
    Elf32Loader& loader_;
    std::uint32_t next_base_;
    // Recursive because a constructor may call dlopen while the loader is
    // already inside its own list update. Never held across RunInitializers.
    mutable std::recursive_mutex mutex_;
    std::deque<LoadedLibrary> libraries_;
    std::string last_error_;
    std::uint32_t error_address_ = 0;
};

}  // namespace armbat
