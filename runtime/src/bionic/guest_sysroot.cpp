#include "armbat/guest_sysroot.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/log.h>

#include "armbat/log.h"

#include "guest_boot.h"
#include "shim_libandroid.h"
#include "shim_libegl.h"
#include "shim_libglesv1.h"
#include "shim_libglesv2.h"
#include "shim_libjnigraphics.h"
#include "shim_liblog.h"
#include "shim_libopensles.h"
#include "sysroot_libc.h"
#include "sysroot_libdl.h"
#include "sysroot_libm.h"
#include "sysroot_linker.h"
#include "sysroot_libstdcpp.h"
#include "sysroot_libz.h"

namespace armbat {
namespace {

struct Image {
    const char* relative_path;  // inside the sysroot directory
    const char* guest_path;     // the path the guest uses
    const unsigned char* data;
    std::uint32_t size;
};

// Order matters only for diagnostics; the linker opens its dependencies itself.
// The last entries are not AOSP binaries: they are the platform shims the build
// generates from the NDK's API-24 stubs, exporting the Android API as traps into
// the HLE registry (see tools/gen_platform_shims.py).
const Image kImages[] = {
    {"armbat_boot", "/system/bin/armbat_boot", guest_boot::kBootImage, guest_boot::kBootSize},
    {"linker", "/system/bin/linker", sysroot_linker::kLinkerImage, sysroot_linker::kLinkerSize},
    {"libc.so", "/system/lib/libc.so", sysroot_libc::kLibcImage, sysroot_libc::kLibcSize},
    {"libm.so", "/system/lib/libm.so", sysroot_libm::kLibmImage, sysroot_libm::kLibmSize},
    {"libdl.so", "/system/lib/libdl.so", sysroot_libdl::kLibdlImage, sysroot_libdl::kLibdlSize},
    {"libstdc++.so", "/system/lib/libstdc++.so", sysroot_libstdcpp::kLibstdcppImage,
     sysroot_libstdcpp::kLibstdcppSize},
    {"libz.so", "/system/lib/libz.so", sysroot_libz::kLibzImage, sysroot_libz::kLibzSize},
    {"libEGL.so", "/system/lib/libEGL.so", shims::kEglImage, shims::kEglSize},
    {"libGLESv1_CM.so", "/system/lib/libGLESv1_CM.so", shims::kGlesv1Image, shims::kGlesv1Size},
    {"libGLESv2.so", "/system/lib/libGLESv2.so", shims::kGlesv2Image, shims::kGlesv2Size},
    {"libandroid.so", "/system/lib/libandroid.so", shims::kAndroidImage, shims::kAndroidSize},
    {"libjnigraphics.so", "/system/lib/libjnigraphics.so", shims::kJnigraphicsImage,
     shims::kJnigraphicsSize},
    {"liblog.so", "/system/lib/liblog.so", shims::kLogImage, shims::kLogSize},
    {"libOpenSLES.so", "/system/lib/libOpenSLES.so", shims::kOpenslesImage, shims::kOpenslesSize},
};

bool FileSize(const std::string& path, std::uint32_t* size) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }
    *size = static_cast<std::uint32_t>(st.st_size);
    return true;
}

bool WriteFile(const std::string& path, const void* data, std::uint32_t size, std::string* error) {
    // A partially written file from an interrupted run must never be treated as
    // the real library: write to a temporary name and rename over it.
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        *error = "cannot create " + temporary + ": " + std::strerror(errno);
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t written = 0;
    while (written < size) {
        const ssize_t count = ::write(fd, bytes + written, size - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = "cannot write " + temporary + ": " + std::strerror(errno);
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::uint32_t>(count);
    }
    if (::close(fd) != 0) {
        *error = "cannot close " + temporary + ": " + std::strerror(errno);
        ::unlink(temporary.c_str());
        return false;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        *error = "cannot rename " + temporary + ": " + std::strerror(errno);
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

}  // namespace

GuestSysroot::GuestSysroot(std::string directory) : directory_(std::move(directory)) {}

std::string GuestSysroot::DefaultDirectory(const std::string& code_cache_dir) {
    return code_cache_dir + "/sysroot";
}

bool GuestSysroot::Materialize(std::string* error) {
    if (ready_) {
        return true;
    }
    if (directory_.empty()) {
        *error = "no sysroot directory";
        return false;
    }
    if (::mkdir(directory_.c_str(), 0755) != 0 && errno != EEXIST) {
        *error = "cannot create " + directory_ + ": " + std::strerror(errno);
        return false;
    }

    for (const Image& image : kImages) {
        const std::string path = directory_ + "/" + image.relative_path;
        std::uint32_t existing = 0;
        if (FileSize(path, &existing) && existing == image.size) {
            continue;
        }
        if (!WriteFile(path, image.data, image.size, error)) {
            return false;
        }
    }

    ready_ = true;
    std::string disk;
    for (const Image& image : kImages) {
        std::uint32_t actual = 0;
        FileSize(directory_ + "/" + image.relative_path, &actual);
        disk += std::string(disk.empty() ? "" : ", ") + image.relative_path + "=" +
                std::to_string(actual);
    }
    ARMBAT_LOG_INFO("Armbat", "sysroot: %s (%s) disk: %s",
                        directory_.c_str(), Inventory().c_str(), disk.c_str());
    return true;
}

std::optional<std::string> GuestSysroot::HostPath(std::string_view guest_path) const {
    if (!ready_) {
        return std::nullopt;
    }
    for (const Image& image : kImages) {
        if (image.guest_path == guest_path) {
            return directory_ + "/" + image.relative_path;
        }
    }
    // A path the sysroot does not carry: let the real filesystem answer, which
    // is what a missing library or file should look like anyway.
    return std::nullopt;
}

std::string GuestSysroot::Inventory() const {
    std::string text;
    for (const Image& image : kImages) {
        if (!text.empty()) {
            text += ", ";
        }
        text += image.guest_path;
        text += "=";
        text += std::to_string(image.size);
    }
    return text;
}

}  // namespace armbat
