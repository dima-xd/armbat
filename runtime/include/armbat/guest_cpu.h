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
#include <cstdio>
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

// The same emulated device, in the shape /proc/stat has. Boehm's collector --
// the heap every Mono and Unity guest links -- asks this file how many
// processors there are and sizes its marker threads from the answer; with the
// file missing it reports -1 and configures the heap for an unknown machine.
// The host's own file cannot be forwarded instead: it describes the phone's
// cores, and a guest that counts them would size its thread pools for a
// different machine than the one HWCAP and cpuinfo describe.
inline std::string GuestProcStat(int cores) {
    cores = std::clamp(cores, 1, 8);
    std::uint64_t total[8] = {};
    std::string text;
    text.reserve(static_cast<std::size_t>(cores) * 64 + 192);
    constexpr const char* kFormat = "cpu%s %llu %llu %llu %llu %llu %llu %llu %llu\n";
    for (int cpu = 0; cpu < cores; ++cpu) {
        const std::uint64_t values[8] = {
            552000ull + 1379ull * static_cast<std::uint64_t>(cpu),  // user
            1900ull,                                                // nice
            123000ull + 419ull * static_cast<std::uint64_t>(cpu),   // system
            2230000ull + 811ull * static_cast<std::uint64_t>(cpu),  // idle
            5150ull,                                                // iowait
            0ull,                                                   // irq
            3390ull,                                                // softirq
            0ull,                                                   // steal
        };
        for (int field = 0; field < 8; ++field) {
            total[field] += values[field];
        }
        const std::string suffix = std::to_string(cpu);
        char line[160];
        std::snprintf(line, sizeof(line), kFormat, suffix.c_str(),
                      static_cast<unsigned long long>(values[0]),
                      static_cast<unsigned long long>(values[1]),
                      static_cast<unsigned long long>(values[2]),
                      static_cast<unsigned long long>(values[3]),
                      static_cast<unsigned long long>(values[4]),
                      static_cast<unsigned long long>(values[5]),
                      static_cast<unsigned long long>(values[6]),
                      static_cast<unsigned long long>(values[7]));
        text += line;
    }
    char aggregate[192];
    std::snprintf(aggregate, sizeof(aggregate), kFormat, "",
                  static_cast<unsigned long long>(total[0]),
                  static_cast<unsigned long long>(total[1]),
                  static_cast<unsigned long long>(total[2]),
                  static_cast<unsigned long long>(total[3]),
                  static_cast<unsigned long long>(total[4]),
                  static_cast<unsigned long long>(total[5]),
                  static_cast<unsigned long long>(total[6]),
                  static_cast<unsigned long long>(total[7]));
    text = aggregate + text;
    text += "intr 0 0 0 0 0 0 0 0 0 0 0\n";
    text += "ctxt 8123456\n";
    text += "btime 1546300800\n";
    text += "processes 51234\n";
    text += "procs_running 1\n";
    text += "procs_blocked 0\n";
    text += "softirq 1234567 0 0 0 0 0 0 0 0 0 0\n";
    return text;
}

}  // namespace armbat
