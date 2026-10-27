/*
 * Guest contexts for platform threads.
 *
 * A guest native is reached from whichever platform thread calls it, and on
 * this container those threads are ART threads: they hold host registers, a
 * host stack and host TLS, and an ARM frame can live in none of them. Running
 * the call on a shared guest thread is not an option either -- Java code the
 * native calls back into must run on the ART thread that reached it, because
 * Android's rules are about that thread (a Handler, a View, a Looper) -- and
 * the container's single machine is no help once the engine thread has taken it
 * and stopped returning.
 *
 * So the container runs such a call on the ART thread itself and lends it the
 * two things it lacks: a guest stack and a guest TLS block. Both belong to a
 * real guest thread created here -- one that has done nothing but park since
 * bionic made it, and therefore owns a TLS block and a stack nobody else uses.
 *
 * A lender publishes the thread pointer and a stack pointer far enough below
 * its own parked frames that the two can share one stack, then parks. It must
 * never return: the stack and the TLS block are unlinked with the thread.
 */

#include <stdint.h>

/* Layout shared with the container (runtime/src/runtime/guest_container.cpp). */
struct armbat_context {
    /* 0 until the fields below are valid; the container waits for it. */
    uint32_t ready;
    uint32_t tls;
    uint32_t sp;
    /* Stays 0 for the life of the thread and is what the park waits on. */
    uint32_t park;
};

/* Keep the park's frames clear of the stack a borrowed call starts on. */
#define ARMBAT_CONTEXT_MARGIN 0x8000u

/*
 * The thread pointer, read the way a 32-bit ARM process reads it. The compiler
 * would call __aeabi_read_tp for `__builtin_thread_pointer()`, and the loader
 * here resolves imports against the platform table first: an ARM register read
 * has neither problem.
 */
static uint32_t armbat_thread_pointer(void) {
    uint32_t pointer;
    __asm__ __volatile__("mrc p15, 0, %0, c13, c0, 3" : "=r"(pointer));
    return pointer;
}

/*
 * Futex wait, inline. The container loads this image itself, and an import
 * named `syscall` would be answered by the platform table -- where it returns
 * ENOSYS -- instead of by bionic's libc.
 */
static void armbat_park(uint32_t *address) {
    register long r0 __asm__("r0") = (long)address;
    register long r1 __asm__("r1") = 0;   /* FUTEX_WAIT */
    register long r2 __asm__("r2") = 0;   /* wait while *address == 0 */
    register long r3 __asm__("r3") = 0;   /* no timeout */
    register long r7 __asm__("r7") = 240; /* __NR_futex */
    __asm__ __volatile__("svc #0"
                         : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3), "r"(r7)
                         : "memory");
}

/* Thread entry point. The container creates it through bionic's pthread_create. */
void *armbat_lender(void *argument) {
    struct armbat_context *context = (struct armbat_context *)argument;
    context->tls = armbat_thread_pointer();
    /* The frames of this loop sit at the top of the stack; a call that starts
     * 32 KiB lower grows down from there and can never reach them. */
    context->sp = ((uint32_t)__builtin_frame_address(0) - ARMBAT_CONTEXT_MARGIN) & ~7u;
    __atomic_store_n(&context->ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        armbat_park(&context->park);
    }
    return 0;
}
