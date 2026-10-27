// The core the JIT emulates, described in one place.
//
// Three places tell a guest what the CPU can do: the initial stack's AT_HWCAP,
// the getauxval HLE call, and /proc/cpuinfo. A library that reads one of them
// and gets a different answer than from another takes a code path the core does
// not implement, and the disagreement only shows up when that path runs -- an
// OpenSSL routine selected by AT_HWCAP2, or a NEON kernel selected by a missing
// "neon" token, faults thousands of instructions away from the cause.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace armbat {

// ARMv7-A with VFPv3-D16 and Advanced SIMD, which is what the A32 frontend
// decodes and the arm64 backend emits. No ARMv8 crypto, no integer divide: the
// A32 side of this JIT does not have those instructions.
constexpr std::uint32_t kGuestHwcap = (1u << 0)   // SWP
                                      | (1u << 1)  // HALF
                                      | (1u << 2)  // THUMB
                                      | (1u << 4)  // FAST_MULT
                                      | (1u << 6)  // VFP
                                      | (1u << 7)  // EDSP
                                      | (1u << 11)  // THUMBEE
                                      | (1u << 12)  // NEON
                                      | (1u << 13)  // VFPv3
                                      | (1u << 14)  // VFPv3D16
                                      | (1u << 15);  // TLS

// The same set in the vocabulary the kernel prints on the Features line. The
// order is the one an ARMv7 kernel uses, so a parser that looks for a token
// finds it wherever it looks in the list.
constexpr const char* kGuestCpuFeatures =
    "swp half thumb fastmult vfp edsp thumbee neon vfpv3 vfpv3d16 tls";

// A Cortex-A9: implementer 0x41, part 0xc09, no integer divide. The values are
// the ones the kernel reports, and cpufeatures matches on them to decide which
// optional features to add on top of the HWCAP.
inline std::string GuestCpuInfo(int cores) {
    // An emulated device with a hundred processors is not a device any
    // application was written for; the container's clone() has a fixed number
    // of thread slots anyway.
    cores = std::clamp(cores, 1, 8);
    std::string text;
    text.reserve(static_cast<std::size_t>(cores) * 192 + 96);
    for (int cpu = 0; cpu < cores; ++cpu) {
        text += "processor\t: " + std::to_string(cpu) + "\n";
        text += "Processor\t: ARMv7 Processor rev 0 (v7l)\n";
        text += "BogoMIPS\t: 38.40\n";
        text += "Features\t: ";
        text += kGuestCpuFeatures;
        text += "\nCPU implementer\t: 0x41\n";
        text += "CPU architecture: 7\n";
        text += "CPU variant\t: 0x0\n";
        text += "CPU part\t: 0xc09\n";
        text += "CPU revision\t: 0\n\n";
    }
    text += "Hardware\t: armbat\n";
    text += "Revision\t: 0000\n";
    text += "Serial\t\t: 0000000000000000\n";
    return text;
}

}  // namespace armbat
