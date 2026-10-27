#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace armbat {

class A32Machine;
class Elf32Loader;
class GuestArena;
class GuestHeap;
class HleRegistry;
class DynamicLoader;
class JniBridge;

// One guest world for one application: the arena, the machine and the HLE
// subsystems that its libraries can reach while being mapped and initialized.
//
// The shell keeps one instance for the life of the process; the headless tool
// creates one per run. Nothing here is global, so both are honest users of the
// same class.
class GuestContainer {
public:
    GuestContainer();
    ~GuestContainer();

    GuestContainer(const GuestContainer&) = delete;
    GuestContainer& operator=(const GuestContainer&) = delete;

    // Reserves the arena and wires the heap, libc, stdio, pthreads, the kernel
    // and the dynamic loader. Idempotent.
    bool Start(std::string* error);
    bool Started() const;

    // Maps every ARMv7 library of the APK, runs its initializers and returns the
    // report. `target_sdk` is the archive's targetSdkVersion, which the guest's
    // linker needs to enable the legacy behaviour the application was built
    // against. `error` explains the first library that could not be completed.
    std::string LoadApk(const std::string& apk_path, const std::string& cache_dir,
                        std::uint32_t target_sdk, std::string* error);

    // The guest world, for the JNI bridge: it has to call into the mapped
    // libraries, resolve their exported JNI entry points and use the same HLE
    // table and heap the guest does.
    A32Machine& Machine();
    GuestArena& Arena();
    GuestHeap& Heap();
    HleRegistry& Imports();
    DynamicLoader& Modules();
    // Runs the JNI_OnLoad of every mapped library. It needs the archive's
    // class loader, so it must follow Jni().RegisterExports.
    std::string CallOnLoad();

    Elf32Loader& Loader();
    JniBridge& Jni();

    // A Java_* entry point of a library the guest's own linker loaded. On the
    // bionic path the HLE loader's module list stays empty, so the JNI bridge
    // reads the entry points from here instead.
    struct GuestExport {
        std::string symbol;
        std::uint32_t address;
    };
    const std::vector<GuestExport>& BionicExports() const;

    // Builds the guest's ANativeActivity and calls the archive's
    // ANativeActivity_onCreate. The activity's paths point into `data_path`.
    std::string StartNativeActivity(const std::string& data_path);

    // The guest's own folders on the shared storage: the stand-ins for
    // /data/data/<package> and /sdcard/Android/obb/<package>, plus the package
    // the archive was built as. Set before LoadApk; both the kernel's path
    // mapper and the native activity's paths are built from them.
    void SetGuestStorage(const std::string& data_root, const std::string& obb_root,
                         const std::string& package_name);

    // Delivers one ANativeActivity callback. `has_argument` says whether the
    // callback takes a second parameter (a window, an input queue, a rect).
    std::string NotifyNative(int index, std::uint32_t argument, bool has_argument);

    // The host window behind the guest's surface: registered, then announced to
    // the archive as onNativeWindowCreated. Null announces its destruction.
    std::string SetNativeWindow(void* window);

    // "libminecraftpe.so+0x12345 in SomeSymbol", for crash reports.
    std::string DescribeAddress(std::uint32_t address) const;

    // Runs a guest entry point that a platform thread reached, on a guest
    // context attached to that very thread, and returns its answer in `result`
    // (r0) and `result_high` (r1, the high half of a long). The thread is the
    // same one, so Java code the guest calls back into still runs on the thread
    // Android expects; what is lent to it is a guest stack and a guest TLS
    // block. False when no context is available, which leaves the call to the
    // caller.
    bool RunOnGuestThread(std::uint32_t entry, const std::vector<std::uint32_t>& args,
                          std::uint32_t* result, std::uint32_t* result_high);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace armbat
