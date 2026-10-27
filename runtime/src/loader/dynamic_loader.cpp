#include "armbat/dynamic_loader.h"

#include <android/log.h>

#include "armbat/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "armbat/a32_machine.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_modules.h"
#include "armbat/hle.h"

namespace armbat {
namespace {

constexpr std::uint32_t kBaseStep = 0x00100000;  // 1 MiB between modules
constexpr std::size_t kMaxImage = 256u << 20;
constexpr std::uint32_t kPageSize = 4096;

// Handles that are not guest addresses. dlsym accepts both, and the virtual one
// stands for the libraries the host provides (libc.so, libm.so, ...) rather than
// for one loaded image.
constexpr std::uint32_t kGlobalHandle = 0xFFFFFFFEu;   // dlopen(NULL)
constexpr std::uint32_t kVirtualHandle = 0xFFFFFFFDu;  // libc.so and friends

std::uint32_t AlignUp(std::uint32_t value) {
    return (value + kPageSize - 1) & ~(kPageSize - 1);
}

std::string Basename(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// dl_unwind_find_exidx is a process-wide service in bionic, not a method of one
// loader: the guest's unwinder asks for it from whatever thread throws. Keeping
// the answer in a table of its own means the unwinder keeps working no matter
// what happens to the loader's library list, and it can never be asked for a
// module whose exidx has not been published yet.
struct ExidxEntry {
    std::uint32_t base;
    std::uint32_t size;
    std::uint32_t exidx;
    std::uint32_t count;
};

constexpr std::size_t kMaxExidx = 64;
ExidxEntry g_exidx[kMaxExidx];
std::atomic<std::size_t> g_exidx_count{0};
std::mutex g_exidx_mutex;

void PublishExidx(const LoadedLibrary& library) {
    if (library.exidx == 0 || library.exidx_count == 0) {
        ARMBAT_LOG_WARN("Armbat",
                            "exidx: '%s' base=0x%08x has no .ARM.exidx, exceptions "
                            "cannot cross it",
                            library.soname.c_str(), library.base);
        return;
    }
    const std::lock_guard lock(g_exidx_mutex);
    const std::size_t index = g_exidx_count.load(std::memory_order_relaxed);
    if (index >= kMaxExidx) {
        return;
    }
    g_exidx[index] = ExidxEntry{library.base, library.image_size, library.exidx,
                                library.exidx_count};
    g_exidx_count.store(index + 1, std::memory_order_release);
    ARMBAT_LOG_INFO("Armbat",
                        "exidx: '%s' base=0x%08x size=0x%08x table=0x%08x entries=%u (n=%zu)",
                        library.soname.c_str(), library.base, library.image_size, library.exidx,
                        library.exidx_count, index + 1);
}

}  // namespace

DynamicLoader::DynamicLoader(A32Machine& machine, GuestArena& arena, Elf32Loader& loader,
                             std::uint32_t first_base)
    : machine_(machine), arena_(arena), loader_(loader), next_base_(first_base) {}

std::uint32_t DynamicLoader::Open(const std::string& host_path, A32Machine& machine,
                                  std::string* error) {
    const std::string basename = Basename(host_path);
    std::vector<std::uint8_t> image;
    std::uint32_t handle = 0;
    std::uint32_t image_size = 0;
    long long map_ms = 0;
    long long init_ms = 0;

    {
        // The whole lookup-and-map sequence is serialised: two threads opening a
        // library at the same time would otherwise be handed the same base. The
        // mutex is recursive because Load() resolves imports through Resolve().
        const std::lock_guard lock(mutex_);
        for (const LoadedLibrary& existing : libraries_) {
            if (existing.path == host_path ||
                (!basename.empty() && existing.soname == basename)) {
                return existing.base;  // dlopen of the same module maps it once
            }
        }
        if (loader_.IsHostLibrary(host_path) || loader_.IsHostLibrary(basename)) {
            return kVirtualHandle;
        }

        std::ifstream file(host_path, std::ios::binary | std::ios::ate);
        if (!file) {
            if (error) *error = "cannot open '" + host_path + "'";
            return 0;
        }
        // Ask for the size first. Reading through istreambuf_iterator would pull
        // the entire file into memory before the size check could reject it.
        const std::streamoff length = file.tellg();
        if (length <= 0 || static_cast<std::uint64_t>(length) > kMaxImage) {
            if (error) *error = "unusable image size for '" + host_path + "'";
            return 0;
        }
        file.seekg(0, std::ios::beg);
        image.resize(static_cast<std::size_t>(length));
        if (!file.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(length))) {
            if (error) *error = "cannot read '" + host_path + "'";
            return 0;
        }

        const auto map_started = std::chrono::steady_clock::now();
        auto library = loader_.Load(image.data(), image.size(), next_base_, error);
        if (!library) {
            return 0;
        }
        map_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - map_started)
                     .count();        library->path = host_path;

        ARMBAT_LOG_INFO("Armbat",
                            "map %s base=0x%08x size=0x%x init=0x%08x", library->soname.c_str(),
                            library->base, library->image_size, library->init_array);

        handle = library->base;
        image_size = library->image_size;
        // The image is visible before its initializers run: a constructor that
        // throws asks the unwinder for its own .ARM.exidx, and the unwinder can
        // only answer for libraries it already knows.
        next_base_ += ((library->image_size + kBaseStep - 1) / kBaseStep) * kBaseStep;
        libraries_.push_back(std::move(*library));
        ARMBAT_LOG_INFO("Armbat",
                            "loader %p: pushed '%s' base=0x%08x n=%zu", static_cast<void*>(this),
                            libraries_.back().soname.c_str(), libraries_.back().base,
                            libraries_.size());
        PublishExidx(libraries_.back());
        // A library that a later DT_NEEDED names has to be recognised by name.
        loader_.AddVirtualLibrary(libraries_.back().soname);
    }

    // Initializers run outside the lock: a constructor may call dlopen itself,
    // and the nested load would deadlock on a non-recursive mutex held for the
    // whole call. Work on a copy: the container may grow while they run.
    LoadedLibrary snapshot;
    {
        const std::lock_guard lock(mutex_);
        for (const LoadedLibrary& loaded : libraries_) {
            if (loaded.base == handle) {
                snapshot = loaded;
                break;
            }
        }
    }
    //
    // dlopen arrives as an HLE call, i.e. while the guest is executing. A nested
    // call is mandatory there: dynarmic refuses to re-enter a running Jit.
    A32Machine& target = machine.Executing() ? machine : machine_;
    std::string init_error;
    const auto init_started = std::chrono::steady_clock::now();
    const bool initializers_ok = loader_.RunInitializers(target, snapshot, &init_error);
    init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - init_started)
                  .count();
    ARMBAT_LOG_INFO("Armbat", "load: '%s' map=%lld ms init=%lld ms",
                        snapshot.soname.c_str(), map_ms, init_ms);
    if (!initializers_ok) {
        // Remove the image that failed, not "whatever is last": a constructor may
        // have opened another module before it faulted.
        {
            const std::lock_guard lock(mutex_);
            ARMBAT_LOG_WARN("Armbat", "loader %p: erasing failed image base=0x%08x",
                                static_cast<void*>(this), handle);
            for (auto it = libraries_.begin(); it != libraries_.end(); ++it) {
                if (it->base == handle) {
                    libraries_.erase(it);
                    break;
                }
            }
        }
        arena_.Unmap(handle, AlignUp(image_size));
        if (error) *error = init_error;
        return 0;
    }

    return handle;
}

std::uint32_t DynamicLoader::OpenEmbedded(const void* image, std::size_t size,
                                          const std::string& soname, std::string* error) {
    const std::lock_guard lock(mutex_);
    auto library = loader_.Load(image, size, next_base_, error);
    if (!library) {
        return 0;
    }
    library->soname = soname;
    next_base_ += ((library->image_size + kBaseStep - 1) / kBaseStep) * kBaseStep;
    const std::uint32_t handle = library->base;
    libraries_.push_back(std::move(*library));
    ARMBAT_LOG_INFO("Armbat",
                        "loader %p: pushed '%s' base=0x%08x n=%zu", static_cast<void*>(this),
                        libraries_.back().soname.c_str(), libraries_.back().base,
                        libraries_.size());
    PublishExidx(libraries_.back());
    // These images never go through the kernel's mmap, so nothing else would
    // register them: without this a thread parked in a lent context reports a
    // bare `0x061013cc` instead of `libarmbat_attach.so+0x13cc`.
    RegisterGuestModuleMapping(libraries_.back().base, libraries_.back().image_size, soname);
    loader_.AddVirtualLibrary(soname);
    return handle;
}

std::uint32_t DynamicLoader::Symbol(std::uint32_t handle, std::string_view name) const {
    if (handle == 0 || handle == kGlobalHandle || handle == kVirtualHandle ||
        handle == 0xFFFFFFFFu) {
        return Resolve(name);
    }
    const std::lock_guard lock(mutex_);
    for (const LoadedLibrary& library : libraries_) {
        if (library.base == handle) {
            return loader_.Sym(library, name);
        }
    }
    return 0;
}

std::uint32_t DynamicLoader::Resolve(std::string_view name) const {
    const std::lock_guard lock(mutex_);
    for (const LoadedLibrary& library : libraries_) {
        const std::uint32_t address = loader_.Sym(library, name);
        if (address != 0) {
            return address;
        }
    }
    return 0;
}

void DynamicLoader::RememberError(std::string message) {
    const std::lock_guard lock(mutex_);
    last_error_ = std::move(message);
}

void DynamicLoader::ForgetError() {
    const std::lock_guard lock(mutex_);
    last_error_.clear();
}

void DynamicLoader::RegisterFunctions(HleRegistry& registry) {
    registry.Add("dlopen", [this](HleCall& call) {
        if (call.Arg(0) == 0) {
            // dlopen(NULL): the global scope, not a file called "0".
            ForgetError();
            call.Return(kGlobalHandle);
            return;
        }
        std::string path;
        if (!call.ReadCString(call.Arg(0), path, 4096)) {
            RememberError("dlopen: path outside the guest image");
            call.Return(0);
            return;
        }
        std::string error;
        const std::uint32_t handle = Open(path, call.Machine(), &error);
        if (handle == 0) {
            const std::string message = error.empty() ? "dlopen failed" : error;
            RememberError(message);
            ARMBAT_LOG_ERROR("Armbat", "dlopen %s failed: %s", path.c_str(),
                                message.c_str());
        } else {
            ForgetError();
        }
        call.Return(handle);
    });

    registry.Add("dlsym", [this, &registry](HleCall& call) {
        std::string name;
        if (!call.ReadCString(call.Arg(1), name, 1024)) {
            RememberError("dlsym: name outside the guest image");
            call.Return(0);
            return;
        }
        std::uint32_t address = Symbol(call.Arg(0), name);
        if (address == 0) {
            // RTLD_DEFAULT and the virtual handles (libc.so, libm.so, ...) are
            // served by HLE, not by a loaded image.
            address = registry.Resolve(name);
        }
        if (address == 0) {
            RememberError("undefined symbol '" + name + "'");
        } else {
            ForgetError();
        }
        call.Return(address);
    });

    registry.Add("dlclose", [this](HleCall& call) {
        // Modules stay mapped: real programs occasionally keep pointers into a
        // "closed" library, and unmapping guest code from a live JIT cache is
        // not something we support yet.
        ForgetError();
        call.Return(0);
    });

    registry.Add("dlerror", [this, &registry](HleCall& call) {
        std::string message;
        {
            const std::lock_guard lock(mutex_);
            message.swap(last_error_);
        }
        if (message.empty()) {
            call.Return(0);
            return;
        }
        const std::uint32_t size = static_cast<std::uint32_t>(message.size() + 1);
        const std::uint32_t address = registry.AllocateData(size);
        if (address == 0) {
            call.Return(0);
            return;
        }
        if (!call.CopyOut(address, message.c_str(), size)) {
            registry.ReleaseData(address);
            call.Return(0);
            return;
        }
        if (error_address_ != 0) {
            registry.ReleaseData(error_address_);
        }
        error_address_ = address;
        call.Return(address);
    });

    // The ARM unwinder asks which module owns a program counter and gets back
    // that module's .ARM.exidx table. Without this answer no C++ exception can
    // cross a frame, because libunwind has nowhere to read the unwind opcodes
    // from.
    registry.Add("dl_unwind_find_exidx", [](HleCall& call) {
        const std::uint32_t pc = call.Arg(0);
        const std::lock_guard lock(g_exidx_mutex);
        const std::size_t count = g_exidx_count.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < count; ++index) {
            const ExidxEntry& entry = g_exidx[index];
            if (pc < entry.base || pc - entry.base >= entry.size) {
                continue;
            }
            if (call.Arg(1) != 0) {
                const std::uint32_t entries = entry.count;
                call.CopyOut(call.Arg(1), &entries, sizeof(entries));
            }
            call.Return(entry.exidx);
            return;
        }
        if (call.Arg(1) != 0) {
            const std::uint32_t zero = 0;
            call.CopyOut(call.Arg(1), &zero, sizeof(zero));
        }
        call.Return(0);
    });
}

}  // namespace armbat
