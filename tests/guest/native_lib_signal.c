// Guest signal delivery: a fault handler that is itself interrupted by a second
// signal.
//
// This lives in its own library on purpose. The container runs guest code
// through two libc paths, and only one of them implements signals:
//
//   * the Bionic path, where the guest's own sigaction/sigprocmask/pthread_kill
//     from the sysroot run on top of the kernel's signal syscalls;
//   * the HLE path used by the loader selftest, where pthread_kill is absent and
//     sigaction/sigprocmask are stubs that return 0.
//
// Keeping the test in the pthread library meant one unresolved import,
// pthread_kill, stopped that library from loading at all, so the atomics suite
// next to it was never executed. A separate ELF lets the linker resolve what
// each file actually needs: the pthread library loads under HLE, and this one is
// reported as SKIP there instead of pretending the delivery worked.
//
// Runs through the sysroot Bionic path only.

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>

// ---- a signal handled inside a signal -------------------------------------
//
// A handler the container runs for the guest is guest code, so it reaches a safe
// point like any other code: a syscall from inside it lets a signal that is
// pending at that moment be delivered on top of it. The frame the outer handler
// is still reading must survive that. When it does not, the outer handler's
// context comes back zeroed, the thread resumes at pc=0, and from the outside
// that looks like a crash somewhere else entirely.
//
// One fault, one pending signal on the faulting thread, one syscall from inside
// the handler, and a sender that signals only after the handler asked for it:
// nothing here depends on timing.
//
// Returns 1 on success and 0 otherwise on purpose. A fault the container cannot
// take ends the run and leaves r0 at zero, and that has to read as a failure.

static volatile int g_frame_handler_ran;
static volatile int g_frame_sender_done;
static volatile int g_frame_inner_ran;
static volatile int g_frame_zeroed;
static pthread_t g_frame_victim;

static void frame_inner_handler(int signal) {
    (void)signal;
    g_frame_inner_ran = 1;
}

static void frame_segv_handler(int signal, struct siginfo* info, void* ucontext) {
    (void)signal;
    (void)info;
    // ucontext_t: flags, link, stack, then uc_mcontext at 0x14; the kernel's
    // sigcontext keeps r0 at its third word and pc at index 18.
    unsigned* const context = (unsigned*)ucontext + 5;
    g_frame_handler_ran = 1;
    // The sender waits for this flag, so the signal is pending by the time the
    // syscall below reaches the safe point. The spin itself must not make one,
    // or the inner delivery would land before the handler is under way.
    while (!g_frame_sender_done) {
    }
    sched_yield();
    if (context[18] < 0x1000u) {
        // The frame was wiped under this handler: resuming from it is guessing.
        // Leaving pc alone lets the container's retry budget end the run.
        g_frame_zeroed = 1;
        return;
    }
    // Resume after the faulting load instead of retrying it, which is what a
    // runtime that turns a fault into an exception does. The instruction is
    // whatever length the state says: four bytes in ARM, two or four in Thumb.
    unsigned length = 4;
    if ((context[19] & 0x20u) != 0) {
        const unsigned halfword = *(const unsigned short*)context[18];
        length = ((halfword >> 11) >= 0x1Du) ? 4u : 2u;
    }
    context[18] += length;
}

static void* frame_sender(void* argument) {
    (void)argument;
    while (!g_frame_handler_ran) {
    }
    // Returns only after the target machine has the signal pending.
    pthread_kill(g_frame_victim, SIGUSR1);
    g_frame_sender_done = 1;
    return NULL;
}

int lib_signal_frame_nested(void) {
    struct sigaction segv;
    struct sigaction usr;
    memset(&segv, 0, sizeof(segv));
    memset(&usr, 0, sizeof(usr));
    segv.sa_sigaction = frame_segv_handler;
    sigemptyset(&segv.sa_mask);
    segv.sa_flags = SA_SIGINFO;
    usr.sa_handler = frame_inner_handler;
    sigemptyset(&usr.sa_mask);
    usr.sa_flags = 0;

    g_frame_handler_ran = 0;
    g_frame_sender_done = 0;
    g_frame_inner_ran = 0;
    g_frame_zeroed = 0;
    g_frame_victim = pthread_self();

    if (sigaction(SIGSEGV, &segv, NULL) != 0 || sigaction(SIGUSR1, &usr, NULL) != 0) {
        return 0;
    }
    pthread_t sender;
    if (pthread_create(&sender, NULL, frame_sender, NULL) != 0) {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGUSR1, SIG_DFL);
        return 0;
    }
    // Between the image area and the mmap window: nothing maps here, so the
    // fault is the same on every run.
    const volatile unsigned* const unmapped = (const volatile unsigned*)0x10000000u;
    (void)*unmapped;

    const int result = (g_frame_zeroed == 0 && g_frame_inner_ran != 0) ? 1 : 0;
    pthread_join(sender, NULL);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    return result;
}
