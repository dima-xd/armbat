// Native arm64 counterparts of the guest benchmarks.
//
// Compiled with the same -O2 and -fno-vectorize as the guest source so the only
// difference is the ISA: this measures JIT overhead, not auto-vectorization.

#include <arm_neon.h>

typedef unsigned int u32;
typedef short s16;

unsigned native_fib(unsigned n) {
    unsigned a = 0;
    unsigned b = 1;
    while (n-- != 0) {
        const unsigned next = a + b;
        a = b;
        b = next;
    }
    return a;
}

unsigned native_bench_int(unsigned iterations) {
    u32 acc = 1;
    for (u32 i = 0; i < iterations; ++i) {
        acc = acc * 1664525u + 1013904223u;
        acc ^= acc >> 13;
    }
    return acc;
}

void native_bench_neon(s16* dst, const s16* a, const s16* b, u32 count, u32 repeat) {
    for (u32 r = 0; r < repeat; ++r) {
        for (u32 i = 0; i + 8 <= count; i += 8) {
            const int16x8_t va = vld1q_s16(a + i);
            const int16x8_t vb = vld1q_s16(b + i);
            vst1q_s16(dst + i, vqaddq_s16(va, vb));
        }
    }
}

unsigned native_bench_mem(u32* dst, const u32* src, u32 count, u32 repeat) {
    u32 sum = 0;
    for (u32 r = 0; r < repeat; ++r) {
        for (u32 i = 0; i < count; ++i) {
            const u32 value = src[i] + r;
            dst[i] = value;
            sum += value;
        }
    }
    return sum;
}

void native_bench_neon_reg(unsigned iterations) {
    short tmp[8];
    int16x8_t acc = vdupq_n_s16(1);
    int16x8_t step = vdupq_n_s16(3);
    const int16x8_t one = vdupq_n_s16(1);
    for (unsigned i = 0; i < iterations; ++i) {
        acc = vqaddq_s16(acc, step);
        step = vaddq_s16(step, one);
    }
    vst1q_s16(tmp, acc);
}

