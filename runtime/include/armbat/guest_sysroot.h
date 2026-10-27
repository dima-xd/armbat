#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace armbat {

// The real ARMv7 Android userland the guest runs on: the bionic linker and the
// libc family from the pinned system image.
//
// The images are embedded in the runtime at build time, which keeps the APK
// self-contained and makes the guest's /system paths independent of the host's
// asset pipeline. They are written to a directory once and then opened by the
// guest through GuestKernel's path translation.
class GuestSysroot {
public:
    explicit GuestSysroot(std::string directory);

    // The directory a container uses for a given host code-cache dir.
    static std::string DefaultDirectory(const std::string& code_cache_dir);

    // Writes every image that is missing or has the wrong size. Idempotent.
    bool Materialize(std::string* error);

    const std::string& Directory() const { return directory_; }

    // Maps /system/bin/linker and /system/lib/<name> to the host file. Returns
    // nullopt for a path this sysroot does not serve, so callers can fall back
    // to the real filesystem.
    std::optional<std::string> HostPath(std::string_view guest_path) const;

    // For diagnostics: "linker 621708, libc.so 771876, ...".
    std::string Inventory() const;

private:
    std::string directory_;
    bool ready_ = false;
};

}  // namespace armbat
