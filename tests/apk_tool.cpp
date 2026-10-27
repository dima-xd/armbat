#include <cstdio>
#include <cstdlib>
#include <string>

#include "armbat/guest_container.h"

// Headless container: maps an APK's ARMv7 libraries and prints the same report
// the shell shows. This is how a real application is inspected without a UI, and
// how the runtime is exercised on an emulator or over adb.
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: armbat-apk <application.apk> [cache-directory] [target-sdk]\n");
        return 2;
    }

    std::string cache = argc > 2 ? argv[2] : "";
    if (cache.empty()) {
        const char* directory = std::getenv("TMPDIR");
        cache = directory != nullptr ? directory : "/data/local/tmp";
    }

    armbat::GuestContainer container;
    std::string error;
    if (!container.Start(&error)) {
        std::fprintf(stderr, "container: %s\n", error.c_str());
        return 1;
    }

    // The CLI has no manifest reader: the caller can pass the target SDK it read
    // itself, and zero leaves the guest linker at its own default.
    const std::uint32_t target_sdk = argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 0;
    const std::string report = container.LoadApk(argv[1], cache, target_sdk, &error);
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
