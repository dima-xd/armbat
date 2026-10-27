#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "armbat/guest_arena.h"

namespace armbat {

class A32Machine;

// A loaded ET_DYN image. All addresses are guest addresses (32-bit offsets).
struct LoadedLibrary {
    std::string soname;
    std::string path;  // host path, empty for embedded images
    std::vector<std::string> needed;
    std::uint32_t base = 0;
    std::uint32_t image_size = 0;
    std::uint32_t entry = 0;
    std::uint32_t init_array = 0;
    std::uint32_t init_array_size = 0;
    std::uint32_t symtab = 0;
    std::uint32_t strtab = 0;
    std::uint32_t strtab_size = 0;
    std::uint32_t symbol_count = 0;
    std::uint32_t syment = 16;
    // DT_HASH (System V): nbucket, nchain, then buckets and chains. It is the
    // image's own index of its exported names, so a lookup is a hash probe
    // instead of a scan of .dynsym -- which for a game library means 77k
    // entries compared per import instead of one.
    std::uint32_t hash_table = 0;
    std::uint32_t relocation_count = 0;
    // .ARM.exidx (PT_ARM_EXIDX): the unwinder table C++ exceptions need,
    // as a guest address plus a count of 8-byte entries.
    std::uint32_t exidx = 0;
    std::uint32_t exidx_count = 0;
};

// Loads 32-bit ARM shared objects into the guest arena. Supports the
// relocation subset produced by -fPIC -shared -nostdlib code; anything else is
// reported by name instead of being silently ignored.
class Elf32Loader {
public:
    explicit Elf32Loader(GuestArena& arena);

    std::optional<LoadedLibrary> Load(const void* image, std::size_t size, std::uint32_t base,
                                      std::string* error);

    // Returns the guest address of an exported symbol, or 0.
    std::uint32_t Sym(const LoadedLibrary& lib, std::string_view name) const;

    // Every defined function the image exports, with its guest address. The JNI
    // bridge uses it to find Java_* entry points without having to parse a dex.
    std::vector<std::pair<std::string, std::uint32_t>> Exports(const LoadedLibrary& lib) const;

    // Called for every undefined symbol. Return a guest address (an HLE stub)
    // or 0 to reject the load with the symbol's name.
    using ImportResolver = std::function<std::uint32_t(std::string_view)>;
    void SetImportResolver(ImportResolver resolver);

    // Libraries whose DT_NEEDED entries are satisfied by the host (libc.so,
    // libm.so, liblog.so, ...). Any other dependency rejects the load.
    void AddVirtualLibrary(std::string soname);

    // A library the host itself serves. Unlike AddVirtualLibrary (which the APK
    // loader also uses for images it is about to map), this is what dlopen must
    // answer with a handle instead of looking for a file.
    void AddHostLibrary(std::string soname) {
        virtual_libraries_.insert(soname);
        host_libraries_.insert(std::move(soname));
    }
    bool IsHostLibrary(std::string_view soname) const {
        return host_libraries_.contains(soname);
    }

    // Calls DT_INIT and every DT_INIT_ARRAY entry with (argc, argv, envp), the
    // way bionic does. Constructors that only take no arguments ignore the
    // registers; the ones written for __libc_init do not.
    bool RunInitializers(A32Machine& machine, const LoadedLibrary& lib, std::string* error);

    // The initializer the machine is running right now. A failure during static
    // construction is only diagnosable with the constructor that was executing.
    std::uint32_t LastInitializer() const { return last_initializer_; }
    std::uint32_t LastInitializerIndex() const { return last_initializer_index_; }
    std::uint32_t LastInitializerCount() const { return last_initializer_count_; }

private:
    // A pointer into the dynamic string table, or nullptr when the offset is
    // outside DT_STRSZ or the string's NUL is not inside the mapped image.
    // Returned pointers are only valid while the image stays loaded.
    const char* StrtabCString(const LoadedLibrary& lib, std::uint32_t offset) const;
    std::optional<std::string> ReadStrtabString(const LoadedLibrary& lib,
                                                std::uint32_t offset) const;
    // Looks a name up through the image's own DT_HASH table. Returns 0 when the
    // name is not exported.
    std::uint32_t SymThroughHash(const LoadedLibrary& lib, std::string_view name) const;

    std::uint32_t last_initializer_ = 0;
    std::uint32_t last_initializer_index_ = 0;
    std::uint32_t last_initializer_count_ = 0;

    GuestArena& arena_;
    ImportResolver import_resolver_;
    std::set<std::string, std::less<>> virtual_libraries_;
    std::set<std::string, std::less<>> host_libraries_;
};

// Marks the libraries every Android app links against as host-provided, so
// DT_NEEDED entries for them are accepted and their symbols come from HLE.
void AddSystemLibraries(Elf32Loader& loader);

// What an unloaded image says about itself: its soname and the libraries it
// wants. The container needs this before loading anything, because an
// application library may depend on another library from the same APK, and the
// order has to be right.
struct ImageIdentity {
    std::string soname;
    std::vector<std::string> needed;
};

bool ReadImageIdentity(const void* image, std::size_t size, ImageIdentity* out);

// Every function the image declares as defined and exported, without loading or
// mapping it. The bionic path needs this to find a library's Java_* entry points:
// the guest's linker owns the mappings, so the file is the only place a name can
// be read from.
std::vector<std::string> ReadDynamicFunctionNames(const void* image, std::size_t size);

}  // namespace armbat
