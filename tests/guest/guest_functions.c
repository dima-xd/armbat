// Guest routines compiled as ARMv7 A32 and executed through the JIT.
//
// Contract: every exported function ends with SVC #0xFFFFFF after placing its
// result in r0. The host halts the JIT in CallSVC and reads r0. SVC #0 is
// reserved for the Linux syscall ABI (r7 = syscall number). No libc, no helper
// calls, no 64-bit division: the object must link with --no-undefined.
//
// Pointers below are 32-bit guest addresses; the host maps buffers inside the
// arena and passes their offsets.

#include <arm_neon.h>

typedef unsigned int u32;
typedef unsigned short u16;
typedef short s16;

#define GUEST_EXPORT __attribute__((noinline, used, visibility("default")))

static inline __attribute__((always_inline)) void guest_exit(u32 value) {
    __asm__ volatile("mov r0, %0\n\tsvc #0xFFFFFF" : : "r"(value) : "r0", "memory");
    __builtin_unreachable();
}

GUEST_EXPORT void add(u32 a, u32 b) {
    guest_exit(a + b);
}

// Iterative Fibonacci; exercises conditional execution and tight loops.
GUEST_EXPORT void fib(u32 n) {
    u32 a = 0;
    u32 b = 1;
    while (n-- != 0) {
        const u32 next = a + b;
        a = b;
        b = next;
    }
    guest_exit(a);
}

// Scalar word loads; the host deliberately passes unaligned buffers too.
GUEST_EXPORT void mem_sum(const u32* words, u32 count) {
    u32 sum = 0;
    for (u32 i = 0; i < count; ++i) {
        sum += words[i];
    }
    guest_exit(sum);
}

GUEST_EXPORT void neon_vadd_i16(s16* dst, const s16* a, const s16* b, u32 count) {
    for (u32 i = 0; i + 8 <= count; i += 8) {
        const int16x8_t va = vld1q_s16(a + i);
        const int16x8_t vb = vld1q_s16(b + i);
        vst1q_s16(dst + i, vaddq_s16(va, vb));
    }
    guest_exit(0);
}

// Saturating add: fixes the ARMv7 semantics the host must reproduce.
GUEST_EXPORT void neon_qadd_i16(s16* dst, const s16* a, const s16* b, u32 count) {
    for (u32 i = 0; i + 8 <= count; i += 8) {
        const int16x8_t va = vld1q_s16(a + i);
        const int16x8_t vb = vld1q_s16(b + i);
        vst1q_s16(dst + i, vqaddq_s16(va, vb));
    }
    guest_exit(0);
}

// Narrowing add/sub: the high halves of the double-width sums, eight inputs to
// four outputs. The fork used to lack these encodings entirely, which halted a
// Unity worker thread in the middle of its work.
GUEST_EXPORT void neon_addhn_i16(s16* dst, const s16* a, const s16* b, u32 count) {
    for (u32 i = 0; i + 8 <= count; i += 8) {
        vst1_s16(dst + i / 2, vaddhn_s16(vld1q_s16(a + i), vld1q_s16(b + i)));
    }
    guest_exit(0);
}

GUEST_EXPORT void neon_raddhn_i16(s16* dst, const s16* a, const s16* b, u32 count) {
    for (u32 i = 0; i + 8 <= count; i += 8) {
        vst1_s16(dst + i / 2, vraddhn_s16(vld1q_s16(a + i), vld1q_s16(b + i)));
    }
    guest_exit(0);
}

GUEST_EXPORT void neon_subhn_i16(s16* dst, const s16* a, const s16* b, u32 count) {
    for (u32 i = 0; i + 8 <= count; i += 8) {
        vst1_s16(dst + i / 2, vsubhn_s16(vld1q_s16(a + i), vld1q_s16(b + i)));
    }
    guest_exit(0);
}

GUEST_EXPORT void neon_vadd_f32(float* dst, const float* a, const float* b, u32 count) {
    for (u32 i = 0; i + 4 <= count; i += 4) {
        const float32x4_t va = vld1q_f32(a + i);
        const float32x4_t vb = vld1q_f32(b + i);
        vst1q_f32(dst + i, vaddq_f32(va, vb));
    }
    guest_exit(0);
}

// Scalar VFP add. Unlike Advanced SIMD, VFP honours FPSCR.FZ, so with the
// default FPSCR denormals survive; this is the case the JIT must not flush.
GUEST_EXPORT void vfp_add_f32(float* dst, const float* a, const float* b, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        dst[i] = a[i] + b[i];
    }
    guest_exit(0);
}
// Integer throughput: LCG plus xor-shift, one dependent chain.
GUEST_EXPORT void bench_int(u32 iterations) {
    u32 acc = 1;
    for (u32 i = 0; i < iterations; ++i) {
        acc = acc * 1664525u + 1013904223u;
        acc ^= acc >> 13;
    }
    guest_exit(acc);
}

// NEON throughput with explicit dependency on the destination buffer.
GUEST_EXPORT void bench_neon(s16* dst, const s16* a, const s16* b, u32 count, u32 repeat) {
    for (u32 r = 0; r < repeat; ++r) {
        for (u32 i = 0; i + 8 <= count; i += 8) {
            const int16x8_t va = vld1q_s16(a + i);
            const int16x8_t vb = vld1q_s16(b + i);
            vst1q_s16(dst + i, vqaddq_s16(va, vb));
        }
    }
    guest_exit(0);
}

// Memory throughput over a flat buffer.
GUEST_EXPORT void bench_mem(u32* dst, const u32* src, u32 count, u32 repeat) {
    u32 sum = 0;
    for (u32 r = 0; r < repeat; ++r) {
        for (u32 i = 0; i < count; ++i) {
            const u32 value = src[i] + r;
            dst[i] = value;
            sum += value;
        }
    }
    guest_exit(sum);
}


// Register-only NEON: no memory traffic, isolates ALU throughput.
GUEST_EXPORT void bench_neon_reg(u32 iterations) {
    int16x8_t acc = vdupq_n_s16(1);
    int16x8_t step = vdupq_n_s16(3);
    const int16x8_t one = vdupq_n_s16(1);
    for (u32 i = 0; i < iterations; ++i) {
        acc = vqaddq_s16(acc, step);
        step = vaddq_s16(step, one);
    }
    s16 tmp[8];
    vst1q_s16(tmp, acc);
    guest_exit((u32)tmp[0]);
}


// Heavy register-only NEON body: if per-iteration cost stays flat while the
// body grows, the overhead is block dispatch, not the vector ops themselves.
GUEST_EXPORT void bench_neon_heavy(u32 iterations) {
    int16x8_t a0 = vdupq_n_s16(1), a1 = vdupq_n_s16(2), a2 = vdupq_n_s16(3), a3 = vdupq_n_s16(4);
    int16x8_t step = vdupq_n_s16(1);
    const int16x8_t one = vdupq_n_s16(1);
    for (u32 i = 0; i < iterations; ++i) {
        a0 = vqaddq_s16(a0, step);
        a1 = vqsubq_s16(a1, step);
        a2 = vqaddq_s16(a2, step);
        a3 = vqsubq_s16(a3, step);
        a0 = vqaddq_s16(a0, one);
        a1 = vqsubq_s16(a1, one);
        a2 = vqaddq_s16(a2, one);
        a3 = vqsubq_s16(a3, one);
        step = vaddq_s16(step, one);
    }
    s16 tmp[8];
    vst1q_s16(tmp, vqaddq_s16(vqaddq_s16(a0, a1), vqaddq_s16(a2, a3)));
    guest_exit((u32)tmp[0]);
}

// Scalar VFP loop with the same shape: separates "any FP state" from "NEON".
GUEST_EXPORT void bench_vfp_reg(u32 iterations) {
    float a0 = 1.0f, a1 = 2.0f, a2 = 3.0f, a3 = 4.0f;
    float step = 0.001f;
    for (u32 i = 0; i < iterations; ++i) {
        a0 += step;
        a1 -= step;
        a2 += step;
        a3 -= step;
        step *= 1.0000001f;
    }
    guest_exit((u32)(a0 + a1 + a2 + a3));
}

// Reads FPSCR back from the guest after a saturating add. QC (bit 27) must be
// set; this is what proves the FPSR handling is still correct after Armbat's
// block-entry MSR removal.
GUEST_EXPORT void read_fpscr_after_qadd(const s16* a, const s16* b, u32 count) {
    s16 tmp[8];
    for (u32 i = 0; i + 8 <= count; i += 8) {
        vst1q_s16(tmp, vqaddq_s16(vld1q_s16(a + i), vld1q_s16(b + i)));
    }
    u32 fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    guest_exit(fpscr);
}

// Saturates (sets QC), then clears FPSCR: the read-back must be zero, proving
// that no stale host flags are resurrected by the deferred spill.
GUEST_EXPORT void fpscr_clear_after_saturation(const s16* a, const s16* b) {
    s16 tmp[8];
    vst1q_s16(tmp, vqaddq_s16(vld1q_s16(a), vld1q_s16(b)));
    u32 zero = 0;
    __asm__ volatile("vmsr fpscr, %0" ::"r"(zero) : "memory");
    u32 fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    guest_exit(fpscr);
}
