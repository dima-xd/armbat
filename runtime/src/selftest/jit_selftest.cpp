#include "armbat/selftest.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "armbat/a32_machine.h"
#include "armbat/detail/report.h"
#include "armbat/guest_arena.h"
#include "armbat/version.h"
#include "guest_blob.h"

extern "C" {
unsigned native_fib(unsigned n);
unsigned native_bench_int(unsigned iterations);
void native_bench_neon(short* dst, const short* a, const short* b, unsigned count, unsigned repeat);
unsigned native_bench_mem(unsigned* dst, const unsigned* src, unsigned count, unsigned repeat);
void native_bench_neon_reg(unsigned iterations);
}

namespace armbat {
namespace {

constexpr std::uint32_t kCodeAddr = 0x00001000;
constexpr std::uint32_t kCodeSize = 0x00010000;
constexpr std::uint32_t kStackAddr = 0x00100000;
constexpr std::uint32_t kStackSize = 0x00040000;
constexpr std::uint32_t kBufferA = 0x00200000;
constexpr std::uint32_t kBufferB = 0x00300000;
constexpr std::uint32_t kBufferC = 0x00400000;
constexpr std::uint32_t kBufferSize = 0x00100000;

std::uint32_t GuestSym(std::uint32_t offset) {
    return generated::kGuestLoadAddress + offset;
}

using detail::Format;
using detail::Reporter;

template <typename Fn>
double MeasureNs(Fn&& fn, int runs = 3) {
    double best = 1e30;
    for (int i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto end = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(end - start).count();
        if (ns < best) {
            best = ns;
        }
    }
    return best;
}

}  // namespace

SuiteReport RunJitSelftest() {
    Reporter report;
    std::string header = std::string("armbat ") + VersionString() + " JIT selftest\n";

    GuestArena arena;
    if (!arena.Ok()) {
        report.Fail("arena.reserve",
                    Format("4 GiB PROT_NONE reservation failed: %s", arena.Error() ? arena.Error() : "?"));
        SuiteReport result;
        result.text = report.Text(header);
        result.failures = report.Failures();
        return result;
    }
    report.Pass("arena.reserve", Format("4 GiB reserved at %p", static_cast<void*>(arena.Base())));

    const bool mapped = arena.Map(kCodeAddr, kCodeSize) && arena.Map(kStackAddr, kStackSize) &&
                        arena.Map(kBufferA, kBufferSize) && arena.Map(kBufferB, kBufferSize) &&
                        arena.Map(kBufferC, kBufferSize);
    report.Check("arena.map", mapped, "code, stack and three 1 MiB buffers");
    if (!mapped) {
        SuiteReport result;
        result.text = report.Text(header);
        result.failures = report.Failures();
        return result;
    }

    A32Machine machine(arena);
    const std::uint32_t sp = kStackAddr + kStackSize - 64;
    machine.SetSp(sp);

    const bool code_loaded = machine.LoadCode(kCodeAddr, generated::kGuestBlob, generated::kGuestBlobSize);
    report.Check("jit.load", code_loaded,
                 Format("%u bytes at 0x%08x", generated::kGuestBlobSize, kCodeAddr));

    // --- Integer -------------------------------------------------------------
    const std::uint32_t add = machine.Call(GuestSym(generated::sym::add), {2, 3});
    report.Check("jit.add", add == 5, Format("add(2,3)=%u, halt=%s", add, machine.LastHaltReasonName().c_str()));
    if (!machine.LastError().empty()) {
        report.Fail("jit.add.error", machine.LastError());
    }

    const std::uint32_t fib20 = machine.Call(GuestSym(generated::sym::fib), {20});
    report.Check("jit.fib20", fib20 == 6765, Format("fib(20)=%u", fib20));

    const std::uint32_t fib30 = machine.Call(GuestSym(generated::sym::fib), {30});
    report.Check("jit.fib30", fib30 == 832040, Format("fib(30)=%u", fib30));

    // --- Scalar memory -------------------------------------------------------
    constexpr std::uint32_t kWords = 16384;
    auto* words = reinterpret_cast<std::uint32_t*>(arena.Ptr(kBufferA));
    std::uint32_t host_sum = 0;
    for (std::uint32_t i = 0; i < kWords; ++i) {
        words[i] = i * 2654435761u;
        host_sum += words[i];
    }

    const std::uint32_t guest_sum = machine.Call(GuestSym(generated::sym::mem_sum), {kBufferA, kWords});
    report.Check("mem.sum_aligned", guest_sum == host_sum,
                 Format("guest=%u host=%u", guest_sum, host_sum));

    // Unaligned base: ARMv7 permits unaligned LDR, so the sum must be identical.
    auto* unaligned = reinterpret_cast<std::uint32_t*>(arena.Ptr(kBufferA + 0x1001));
    std::uint32_t unaligned_host = 0;
    for (std::uint32_t i = 0; i < kWords; ++i) {
        unaligned[i] = i ^ 0xA5A5A5A5u;
        unaligned_host += unaligned[i];
    }
    const std::uint32_t unaligned_guest =
        machine.Call(GuestSym(generated::sym::mem_sum), {kBufferA + 0x1001, kWords});
    report.Check("mem.sum_unaligned", unaligned_guest == unaligned_host,
                 Format("guest=%u host=%u", unaligned_guest, unaligned_host));

    // --- NEON ----------------------------------------------------------------
    constexpr std::uint32_t kSamples = 8192;
    auto* src_a = reinterpret_cast<std::int16_t*>(arena.Ptr(kBufferA));
    auto* src_b = reinterpret_cast<std::int16_t*>(arena.Ptr(kBufferB));
    auto* dst = reinterpret_cast<std::int16_t*>(arena.Ptr(kBufferC));
    for (std::uint32_t i = 0; i < kSamples; ++i) {
        src_a[i] = static_cast<std::int16_t>((i * 37) % 20000 - 10000);
        src_b[i] = static_cast<std::int16_t>(32767 - static_cast<int>(i % 3));
    }
    std::memset(dst, 0, kSamples * sizeof(std::int16_t));

    const std::uint32_t neon_halt = machine.Call(GuestSym(generated::sym::neon_vadd_i16), {kBufferC, kBufferA, kBufferB, kSamples});
    (void)neon_halt;
    bool neon_ok = true;
    for (std::uint32_t i = 0; i < kSamples; ++i) {
        const int expected = src_a[i] + src_b[i];
        if (dst[i] != static_cast<std::int16_t>(static_cast<std::uint16_t>(expected))) {
            neon_ok = false;
            report.Fail("neon.vadd_i16", Format("lane %u: got %d want %d", i, dst[i], expected));
            break;
        }
    }
    if (neon_ok) {
        report.Pass("neon.vadd_i16", Format("%u lanes", kSamples));
    }

    // Saturating add must clamp, not wrap.
    std::memset(dst, 0, kSamples * sizeof(std::int16_t));
    machine.Call(GuestSym(generated::sym::neon_qadd_i16), {kBufferC, kBufferA, kBufferB, kSamples});
    bool sat_ok = true;
    for (std::uint32_t i = 0; i < kSamples && sat_ok; ++i) {
        int expected = static_cast<int>(src_a[i]) + static_cast<int>(src_b[i]);
        if (expected > 32767) {
            expected = 32767;
        } else if (expected < -32768) {
            expected = -32768;
        }
        if (dst[i] != static_cast<std::int16_t>(expected)) {
            sat_ok = false;
            report.Fail("neon.qadd_i16", Format("lane %u: got %d want %d", i, dst[i], expected));
        }
    }
    if (sat_ok && neon_ok) {
        report.Pass("neon.qadd_i16", Format("%u lanes saturated", kSamples));
    }

    // Narrowing add/sub: the operands are read as double-width elements made of
    // adjacent lane pairs, and the high half of each sum lands in the D result.
    // Values straddle zero and the rounding midpoint so that a missing shift or
    // a missing round constant cannot pass.
    for (std::uint32_t i = 0; i < kSamples; ++i) {
        src_a[i] = static_cast<std::int16_t>((i % 2) ? 32767 : -32768);
        src_b[i] = static_cast<std::int16_t>((i % 5) * 9000 - 18000);
    }
    const auto pack_pair = [](std::int16_t lo, std::int16_t hi) {
        return (static_cast<std::uint32_t>(static_cast<std::uint16_t>(hi)) << 16) |
               static_cast<std::uint16_t>(lo);
    };
    const struct {
        const char* name;
        std::uint32_t entry;
        bool subtract;
        bool round;
    } narrow_cases[] = {
        {"neon.addhn_i16", GuestSym(generated::sym::neon_addhn_i16), false, false},
        {"neon.raddhn_i16", GuestSym(generated::sym::neon_raddhn_i16), false, true},
        {"neon.subhn_i16", GuestSym(generated::sym::neon_subhn_i16), true, false},
    };
    for (const auto& narrow : narrow_cases) {
        std::memset(dst, 0, kSamples * sizeof(std::int16_t));
        machine.Call(narrow.entry, {kBufferC, kBufferA, kBufferB, kSamples});
        bool narrow_ok = true;
        for (std::uint32_t i = 0; i + 1 < kSamples && narrow_ok; i += 2) {
            const std::uint32_t wide_n = pack_pair(src_a[i], src_a[i + 1]);
            const std::uint32_t wide_m = pack_pair(src_b[i], src_b[i + 1]);
            std::uint32_t sum = narrow.subtract ? wide_n - wide_m : wide_n + wide_m;
            if (narrow.round) {
                sum += 0x8000u;
            }
            const auto want = static_cast<std::int16_t>(static_cast<std::uint16_t>(sum >> 16));
            if (dst[i / 2] != want) {
                narrow_ok = false;
                report.Fail(narrow.name, Format("lane %u: got %d want %d", i / 2, dst[i / 2], want));
            }
        }
        if (narrow_ok) {
            report.Pass(narrow.name, Format("%u lanes", kSamples / 2));
        }
    }

    // FPSCR read-back: the guest must see the sticky QC flag it just set.
    for (std::uint32_t i = 0; i < 8; ++i) {
        src_a[i] = 32767;
        src_b[i] = 32767;
    }
    const std::uint32_t fpscr = machine.Call(GuestSym(generated::sym::read_fpscr_after_qadd),
                                             {kBufferA, kBufferB, 8});
    report.Check("fpsr.qc_sticky", (fpscr & (1u << 27)) != 0, Format("FPSCR=0x%08x", fpscr));

    const std::uint32_t cleared = machine.Call(GuestSym(generated::sym::fpscr_clear_after_saturation),
                                               {kBufferA, kBufferB});
    report.Check("fpsr.clear", (cleared & (1u << 27)) == 0, Format("FPSCR=0x%08x", cleared));

    // Float add, including denormals and infinities, compared bit-for-bit.
    // The host FPCR decides denormal handling unless the JIT masks it, so report
    // it explicitly; a guest must see ARMv7 defaults (FZ=0), not the host mode.
    std::uint64_t fpcr = 0;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
#endif
    const bool host_fz = (fpcr & (1ull << 24)) != 0;
    report.Pass("host.fpcr", Format("FPCR=0x%llx FZ=%d", static_cast<unsigned long long>(fpcr), host_fz ? 1 : 0));
    if (host_fz) {
        const std::uint64_t relaxed = fpcr & ~(1ull << 24);
        __asm__ volatile("msr fpcr, %0" ::"r"(relaxed));
    }
    auto* fa = reinterpret_cast<float*>(arena.Ptr(kBufferA));
    auto* fb = reinterpret_cast<float*>(arena.Ptr(kBufferB));
    auto* fdst = reinterpret_cast<float*>(arena.Ptr(kBufferC));
    // ARMv7 Advanced SIMD always flushes denormals, regardless of FPSCR.FZ.
    const auto asimd = [](float value) {
        return std::fabs(value) < 1.17549435e-38f ? 0.0f : value;
    };
    std::vector<float> expected_neon(kSamples);
    for (std::uint32_t i = 0; i < kSamples; ++i) {
        fa[i] = static_cast<float>(i) * 0.125f;
        fb[i] = -(static_cast<float>(i) * 0.0625f);
        expected_neon[i] = asimd(asimd(fa[i]) + asimd(fb[i]));
    }
    fa[0] = 1e-40f;  // denormal input: ASIMD flushes it to zero
    fb[0] = 1e-40f;
    expected_neon[0] = 0.0f;
    fa[1] = 3.402823466e+38f;
    fb[1] = 3.402823466e+38f;
    expected_neon[1] = fa[1] + fb[1];  // +inf

    std::memset(fdst, 0, kSamples * sizeof(float));
    machine.Call(GuestSym(generated::sym::neon_vadd_f32), {kBufferC, kBufferA, kBufferB, kSamples});
    bool float_ok = true;
    for (std::uint32_t i = 0; i < kSamples && float_ok; ++i) {
        if (std::memcmp(&fdst[i], &expected_neon[i], sizeof(float)) != 0) {
            float_ok = false;
            report.Fail("neon.vadd_f32_ftz",
                        Format("lane %u: got %g want %g", i, fdst[i], expected_neon[i]));
        }
    }
    if (float_ok) {
        report.Pass("neon.vadd_f32_ftz", Format("%u lanes, denormals flushed", kSamples));
    }

    // Scalar VFP honours FPSCR.FZ; the reset value is 0, so denormals survive.
    std::vector<float> expected_vfp(kSamples);
    for (std::uint32_t i = 0; i < kSamples; ++i) {
        expected_vfp[i] = fa[i] + fb[i];
    }
    std::memset(fdst, 0, kSamples * sizeof(float));
    machine.Call(GuestSym(generated::sym::vfp_add_f32), {kBufferC, kBufferA, kBufferB, kSamples});
    bool vfp_ok = true;
    for (std::uint32_t i = 0; i < kSamples && vfp_ok; ++i) {
        if (std::memcmp(&fdst[i], &expected_vfp[i], sizeof(float)) != 0) {
            vfp_ok = false;
            report.Fail("vfp.vadd_f32_denormal",
                        Format("lane %u: got %g want %g", i, fdst[i], expected_vfp[i]));
        }
    }
    if (vfp_ok) {
        report.Pass("vfp.vadd_f32_denormal", Format("%u lanes bit-exact", kSamples));
    }
    if (host_fz) {
        __asm__ volatile("msr fpcr, %0" ::"r"(fpcr));
    }

    // --- Benchmarks ----------------------------------------------------------
    constexpr std::uint32_t kIntIterations = 2000000;
    const std::uint32_t guest_int_warm = machine.Call(GuestSym(generated::sym::bench_int), {kIntIterations});
    const std::uint32_t native_int = native_bench_int(kIntIterations);
    report.Check("bench.int_correct", guest_int_warm == native_int,
                 Format("guest=%u native=%u", guest_int_warm, native_int));

    machine.ResetStats();
    const double guest_int_ns = MeasureNs([&] {
        machine.Call(GuestSym(generated::sym::bench_int), {kIntIterations});
    });
    const std::uint64_t guest_int_ticks = machine.RetiredTicks();
    const double native_int_ns = MeasureNs([&] {
        volatile unsigned sink = native_bench_int(kIntIterations);
        (void)sink;
    });

    const double guest_ns_per_iter = guest_int_ns / kIntIterations;
    const double native_ns_per_iter = native_int_ns / kIntIterations;
    report.Pass("bench.int",
                Format("guest %.3f ns/iter, native %.3f ns/iter, ratio %.2fx, %.2f GIPS",
                       guest_ns_per_iter, native_ns_per_iter,
                       guest_ns_per_iter / native_ns_per_iter,
                       guest_int_ticks / guest_int_ns));

    // Cold translation cost: a fresh block for fib(200000) is compiled once.
    machine.ClearCache();
    const auto cold_start = std::chrono::steady_clock::now();
    machine.Call(GuestSym(generated::sym::fib), {200000});
    const auto cold_end = std::chrono::steady_clock::now();
    const double cold_ms = std::chrono::duration<double, std::milli>(cold_end - cold_start).count();
    report.Pass("bench.cold_fib", Format("%.3f ms including translation of %u bytes",
                                         cold_ms, generated::kGuestBlobSize));

    // NEON throughput
    constexpr std::uint32_t kNeonCount = 65536;
    constexpr std::uint32_t kNeonRepeat = 200;
    auto* neon_a = reinterpret_cast<std::int16_t*>(arena.Ptr(kBufferA));
    auto* neon_b = reinterpret_cast<std::int16_t*>(arena.Ptr(kBufferB));
    auto* neon_dst = reinterpret_cast<std::int16_t*>(arena.Ptr(kBufferC));
    for (std::uint32_t i = 0; i < kNeonCount; ++i) {
        neon_a[i] = static_cast<std::int16_t>(i % 1000);
        neon_b[i] = static_cast<std::int16_t>((i * 7) % 1000);
    }

    const double guest_neon_ns = MeasureNs([&] {
        machine.Call(GuestSym(generated::sym::bench_neon), {kBufferC, kBufferA, kBufferB, kNeonCount, kNeonRepeat});
    });
    const double native_neon_ns = MeasureNs([&] {
        native_bench_neon(neon_dst, neon_a, neon_b, kNeonCount, kNeonRepeat);
    });
    const double neon_items = static_cast<double>(kNeonCount) * kNeonRepeat;
    report.Pass("bench.neon",
                Format("%.2f Gop/s guest, %.2f Gop/s native, ratio %.2fx",
                        neon_items / guest_neon_ns, neon_items / native_neon_ns,
                        guest_neon_ns / native_neon_ns));

    // Register-only NEON, to separate ALU throughput from vector load/store cost.
    constexpr std::uint32_t kNeonRegIters = 1000000;
    machine.Call(GuestSym(generated::sym::bench_neon_reg), {kNeonRegIters});
    const double guest_neon_reg_ns = MeasureNs([&] {
        machine.Call(GuestSym(generated::sym::bench_neon_reg), {kNeonRegIters});
    });
    report.Pass("bench.neon_reg",
                Format("guest %.3f ns/iter for %u vops (native loop is optimised away)",
                       guest_neon_reg_ns / kNeonRegIters, 2u));

    // Same loop with a 9x larger vector body: flat ns/iter means dispatch-bound.
    constexpr std::uint32_t kNeonHeavyIters = 500000;
    machine.Call(GuestSym(generated::sym::bench_neon_heavy), {kNeonHeavyIters});
    const double guest_heavy_ns = MeasureNs([&] {
        machine.Call(GuestSym(generated::sym::bench_neon_heavy), {kNeonHeavyIters});
    });
    report.Pass("bench.neon_heavy",
                Format("guest %.3f ns/iter, %.2f ns per vector op (9 vops/iter)",
                       guest_heavy_ns / kNeonHeavyIters,
                       guest_heavy_ns / kNeonHeavyIters / 9.0));

    constexpr std::uint32_t kVfpIters = 1000000;
    machine.Call(GuestSym(generated::sym::bench_vfp_reg), {kVfpIters});
    const double guest_vfp_ns = MeasureNs([&] {
        machine.Call(GuestSym(generated::sym::bench_vfp_reg), {kVfpIters});
    });
    report.Pass("bench.vfp_reg",
                Format("guest %.3f ns/iter (4 scalar VFP ops/iter)", guest_vfp_ns / kVfpIters));

    // Does block linking explain the FP gap? If the FP loop is dispatch-bound,
    // disabling linking leaves it unchanged while the integer loop collapses.
    for (const bool linking : {true, false}) {
        A32Machine::Config link_config;
        link_config.block_linking = linking;
        A32Machine link_probe(arena, link_config);
        link_probe.SetSp(sp);
        if (!link_probe.LoadCode(kCodeAddr, generated::kGuestBlob, generated::kGuestBlobSize)) {
            report.Fail("bench.linking", "probe could not load code");
            continue;
        }
        constexpr std::uint32_t kLinkIters = 500000;
        link_probe.Call(GuestSym(generated::sym::bench_int), {kLinkIters});
        const double int_ns = MeasureNs([&] {
            link_probe.Call(GuestSym(generated::sym::bench_int), {kLinkIters});
        });
        link_probe.Call(GuestSym(generated::sym::bench_vfp_reg), {kLinkIters});
        const double vfp_ns = MeasureNs([&] {
            link_probe.Call(GuestSym(generated::sym::bench_vfp_reg), {kLinkIters});
        });
        report.Pass("bench.linking",
                    Format("block_linking=%d: int %.3f ns/iter, vfp %.3f ns/iter",
                           linking ? 1 : 0, int_ns / kLinkIters, vfp_ns / kLinkIters));
    }

    // Memory throughput
    constexpr std::uint32_t kMemWords = 262144;
    constexpr std::uint32_t kMemRepeat = 8;
    auto* mem_src = reinterpret_cast<std::uint32_t*>(arena.Ptr(kBufferA));
    auto* mem_dst = reinterpret_cast<std::uint32_t*>(arena.Ptr(kBufferC));
    for (std::uint32_t i = 0; i < kMemWords; ++i) {
        mem_src[i] = i;
    }

    const double guest_mem_ns = MeasureNs([&] {
        machine.Call(GuestSym(generated::sym::bench_mem), {kBufferC, kBufferA, kMemWords, kMemRepeat});
    });
    const std::uint64_t mem_callback_reads = machine.CallbackReads();
    const std::uint64_t mem_callback_writes = machine.CallbackWrites();
    const double native_mem_ns = MeasureNs([&] {
        native_bench_mem(mem_dst, mem_src, kMemWords, kMemRepeat);
    });
    const double mem_bytes = static_cast<double>(kMemWords) * kMemRepeat * 8.0;
    report.Pass("bench.mem",
                Format("%.1f GiB/s guest, %.1f GiB/s native, host callbacks r=%llu w=%llu",
                        mem_bytes / guest_mem_ns, mem_bytes / native_mem_ns,
                        static_cast<unsigned long long>(mem_callback_reads),
                        static_cast<unsigned long long>(mem_callback_writes)));

    // Configuration matrix: quantifies what fastmem and cycle counting cost.
    struct Variant {
        bool fastmem;
        bool ticks;
    };
    const Variant variants[] = {{true, true}, {true, false}, {false, true}, {false, false}};
    constexpr std::uint32_t kProbeWords = 65536;
    constexpr std::uint32_t kProbeRepeat = 4;
    for (const Variant& variant : variants) {
        A32Machine::Config config;
        config.fastmem = variant.fastmem;
        config.cycle_counting = variant.ticks;
        A32Machine probe(arena, config);
        probe.SetSp(sp);
        if (!probe.LoadCode(kCodeAddr, generated::kGuestBlob, generated::kGuestBlobSize)) {
            report.Fail("bench.matrix", "probe could not load code");
            continue;
        }
        probe.Call(GuestSym(generated::sym::bench_mem), {kBufferC, kBufferA, kProbeWords, kProbeRepeat});
        probe.ResetStats();
        const double ns = MeasureNs([&] {
            probe.Call(GuestSym(generated::sym::bench_mem), {kBufferC, kBufferA, kProbeWords, kProbeRepeat});
        });
        const double gib = static_cast<double>(kProbeWords) * kProbeRepeat * 8.0 / ns;
        report.Pass("bench.matrix",
                    Format("fastmem=%d cycle_counting=%d: %.1f GiB/s, callbacks r=%llu w=%llu",
                           variant.fastmem ? 1 : 0, variant.ticks ? 1 : 0, gib,
                           static_cast<unsigned long long>(probe.CallbackReads()),
                           static_cast<unsigned long long>(probe.CallbackWrites())));
    }

    if (!machine.LastError().empty()) {
        report.Fail("jit.error", machine.LastError());
    }

    SuiteReport result;
    result.text = report.Text(header);
    result.failures = report.Failures();
    return result;
}

}  // namespace armbat

