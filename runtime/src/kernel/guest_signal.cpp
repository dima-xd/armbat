#include "armbat/guest_signal.h"

#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <android/log.h>

#include "armbat/log.h"

#include "armbat/a32_machine.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_pthread.h"

namespace armbat {
namespace {

// A signal frame per thread. The page sits between the page the platform lended
// contexts use (0x00d00000) and the HLE stub region, which nothing else claims.
constexpr std::uint32_t kFrameBase = 0x00d80000;
constexpr std::uint32_t kFrameSlotSize = 0x800;
// One slot per live thread, not one per tid hash: two threads that hash to the
// same slot overwrite the siginfo and the ucontext the other thread's handler is
// still reading, and a handler that resumes from a clobbered context resumes
// nowhere. The range is half a megabyte, so slots are free.
constexpr std::uint32_t kFrameSlots = 64;
constexpr std::uint32_t kUcontextOffset = 0x100;
constexpr std::uint32_t kUcontextSize = 0x400;

// The host signal that breaks a guest thread out of a blocking syscall. It has
// to be one the platform does not use for anything else and whose handler may
// return: SIGUSR2 is free on Android, SIGSEGV belongs to the JIT's fault path.
constexpr int kWakeupSignal = SIGUSR2;

constexpr std::uint32_t kSaSiginfo = 0x00000004u;
// SA_NODEFER: the handler runs with its own signal still deliverable. Without
// it a device blocks the signal being handled for the lifetime of the handler.
constexpr std::uint32_t kSaNodefer = 0x40000000u;
constexpr std::uint32_t kSigDfl = 0;
constexpr std::uint32_t kSigIgn = 1;
// si_code for a thread-directed signal sent with tkill; the guest's handlers
// only compare it against SI_USER/SI_TKILL.
constexpr std::uint32_t kSiTkill = static_cast<std::uint32_t>(-6);

struct Action {
    std::uint32_t handler = kSigDfl;
    std::uint32_t flags = 0;
    std::uint32_t restorer = 0;
    // sa_mask: signals the handler blocks while it runs. libmono's Boehm suspend
    // handler is installed with every signal in it, which is what keeps the
    // restart signal queued instead of running it inside the suspend handler.
    std::uint64_t mask = 0;
};

GuestArena* g_arena = nullptr;
std::mutex g_actions_mutex;
Action g_actions[65];
std::atomic<bool> g_ready{false};

void WakeupHandler(int) {
    // The syscall that was interrupted returns EINTR by itself. The guest signal
    // handler runs later, on the guest thread, at a safe point.
}

// The ARM32 siginfo_t and ucontext_t the guest's own C library and its runtime
// read. sigcontext is the kernel's, unchanged since ARMv7: three bookkeeping
// words, then r0-r10, fp, ip, sp, lr, pc, cpsr and the address that faulted.
// ucontext keeps it at 0x14, after the flags, the link and the stack.
constexpr std::uint32_t kUcMcontext = 0x14;
// siginfo_t: si_signo, si_errno, si_code, then the union; SIGSEGV fills
// _sigfault with the address that faulted.
constexpr std::uint32_t kSiSegv = 11;
constexpr std::uint32_t kSegvMaperr = 1;  // the page is not mapped at all
constexpr std::uint32_t kThumbCpsr = 0x20;

std::uint32_t FrameForThisThread() {
    // The slot is taken once and kept for the life of the thread: the handler is
    // handed this address, and the thread that is still inside a handler must
    // find its own frame untouched.
    static thread_local std::uint32_t slot = kFrameSlots;
    if (slot == kFrameSlots) {
        static std::atomic<std::uint32_t> next{0};
        slot = next.fetch_add(1, std::memory_order_relaxed) % kFrameSlots;
    }
    return kFrameBase + slot * kFrameSlotSize;
}

}  // namespace

void InitGuestSignals(GuestArena& arena) {
    g_arena = &arena;
    if (!arena.IsMapped(kFrameBase, kFrameSlots * kFrameSlotSize) &&
        !arena.Map(kFrameBase, kFrameSlots * kFrameSlotSize)) {
        ARMBAT_LOG_ERROR("Armbat",
                            "signals: cannot map the signal frame page");
        return;
    }

    struct sigaction action {};
    action.sa_handler = WakeupHandler;
    sigemptyset(&action.sa_mask);
    // No SA_RESTART: a guest thread parked in a syscall must wake up with EINTR
    // so it can reach the safe point that runs the handler.
    action.sa_flags = 0;
    if (::sigaction(kWakeupSignal, &action, nullptr) != 0) {
        ARMBAT_LOG_ERROR("Armbat",
                            "signals: cannot install the wakeup handler");
        return;
    }
    UnblockGuestSignalWakeup();
    g_ready.store(true, std::memory_order_release);
}

void UnblockGuestSignalWakeup() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, kWakeupSignal);
    ::pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
}

void SetGuestSignalAction(int signal, std::uint32_t handler, std::uint32_t flags,
                          std::uint32_t restorer, std::uint64_t mask) {
    if (signal <= 0 || signal >= 65) {
        return;
    }
    // A guest that installs a handler for a fault signal is a guest that expects
    // faults to become its exceptions; one line names it at boot, which is the
    // difference between "the runtime took the fault" and "nobody was there".
    if (signal == kSiSegv && handler != kSigDfl && handler != kSigIgn) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            ARMBAT_LOG_INFO("Armbat", "signals: SIGSEGV handler at 0x%08x flags=0x%x", handler,
                            flags);
        }
    }
    const std::lock_guard lock(g_actions_mutex);
    g_actions[signal].handler = handler;
    g_actions[signal].flags = flags;
    g_actions[signal].restorer = restorer;
    g_actions[signal].mask = mask;
}

bool HasGuestSignalHandler(int signal) {
    if (signal <= 0 || signal >= 65) {
        return false;
    }
    const std::lock_guard lock(g_actions_mutex);
    return g_actions[signal].handler != kSigDfl && g_actions[signal].handler != kSigIgn;
}

bool SendGuestSignal(std::uint32_t tid, int signal) {
    if (signal <= 0 || signal >= 65) {
        return false;
    }
    A32Machine* target = FindLiveGuestThread(tid);
    if (target == nullptr) {
        return false;
    }
    target->RequestSignal(static_cast<std::uint32_t>(signal));
    // A thread blocked in a syscall has no instruction stream to interrupt: the
    // host signal makes that syscall fail with EINTR, and the guest's retry runs
    // through an SVC, which is the safe point where its handler is delivered.
    if (::syscall(__NR_tgkill, ::getpid(), static_cast<int>(tid), kWakeupSignal) != 0) {
        return false;
    }
    return true;
}

void DeliverGuestSignals(A32Machine& machine) {
    if (!g_ready.load(std::memory_order_acquire) || g_arena == nullptr) {
        return;
    }
    for (;;) {
        const std::uint32_t signal = machine.TakePendingSignal();
        if (signal == 0) {
            return;
        }
        Action action;
        {
            const std::lock_guard lock(g_actions_mutex);
            action = g_actions[signal];
        }
        if (action.handler == kSigDfl || action.handler == kSigIgn) {
            // SIG_DFL termination is not emulated: the container never had
            // signal semantics, and killing the whole guest for a signal it
            // ignored would be a regression, not a feature.
            continue;
        }

        const std::uint32_t frame = FrameForThisThread();
        // siginfo_t on ARM32: int si_signo, si_errno, si_code, then the union.
        std::uint32_t info[32] = {};
        info[0] = signal;
        info[2] = kSiTkill;
        std::memcpy(g_arena->Ptr(frame), info, sizeof(info));
        // A zeroed ucontext is a placeholder: bionic's handlers read it through
        // a pointer, and an unmapped one would fault. The register set inside it
        // is not filled in.
        std::memset(g_arena->Ptr(frame + kUcontextOffset), 0, kUcontextSize);

        std::vector<std::uint32_t> arguments{signal};
        if ((action.flags & kSaSiginfo) != 0) {
            arguments.push_back(frame);
            arguments.push_back(frame + kUcontextOffset);
        }
        // A device runs a handler with the thread's own mask, the action's
        // sa_mask and (unless SA_NODEFER) the signal being handled all blocked,
        // and restores the previous mask when the handler returns. That is the
        // whole stop-the-world protocol: a suspend handler is installed with
        // sa_mask = everything, so the resume signal stays queued until the
        // handler itself drops into sigsuspend with a mask that lets it through.
        const std::uint64_t previous = machine.BlockedSignals();
        std::uint64_t during = previous | action.mask;
        if ((action.flags & kSaNodefer) == 0) {
            during |= 1ull << (signal - 1);
        }
        machine.SetBlockedSignals(during);
        machine.CallNestedPreservingRegisters(action.handler, arguments);
        machine.SetBlockedSignals(previous);
    }
}

bool DeliverGuestFault(A32Machine& machine, std::uint32_t address) {
    // One fault at a time on this thread: a handler that faults again would
    // rewrite the frame the outer handler is still reading.
    static thread_local int depth = 0;
    // A handler that returns the context unchanged is a handler that only
    // watched (or repaired the page). Retrying is what a device does, but it
    // has to end: the same pc faulting again and again is a run that would
    // never report anything.
    static thread_local std::uint32_t retry_pc = 0;
    static thread_local int retries = 0;

    if (depth != 0 || !g_ready.load(std::memory_order_acquire) || g_arena == nullptr) {
        return false;
    }
    Action action;
    {
        const std::lock_guard lock(g_actions_mutex);
        action = g_actions[kSiSegv];
    }
    if (action.handler == kSigDfl || action.handler == kSigIgn) {
        return false;
    }

    const std::uint32_t frame = FrameForThisThread();
    const std::uint32_t ucontext = frame + kUcontextOffset;
    std::memset(g_arena->Ptr(frame), 0, kFrameSlotSize);

    // siginfo_t, ARM32: si_signo, si_errno, si_code, then _sigfault.si_addr.
    std::uint32_t* const info = reinterpret_cast<std::uint32_t*>(g_arena->Ptr(frame));
    info[0] = kSiSegv;
    info[2] = kSegvMaperr;
    info[3] = address;

    // The register file of the code that faulted, in the layout of the kernel's
    // struct sigcontext. A handler that wants the thread to continue elsewhere
    // writes pc here; the register file is read back after it returns, which is
    // what rt_sigreturn does on a device.
    std::uint32_t* const context =
        reinterpret_cast<std::uint32_t*>(g_arena->Ptr(ucontext + kUcMcontext));
    std::uint32_t interrupted[16] = {};
    for (std::uint32_t index = 0; index < 16; ++index) {
        interrupted[index] = machine.Reg(index);
    }
    const std::uint32_t interrupted_cpsr = machine.Cpsr();
    for (std::uint32_t index = 0; index <= 10; ++index) {
        context[3 + index] = interrupted[index];
    }
    context[14] = interrupted[11];
    context[15] = interrupted[12];
    context[16] = interrupted[13];
    context[17] = interrupted[14];
    context[18] = interrupted[15];
    context[19] = interrupted_cpsr;
    context[20] = address;

    std::vector<std::uint32_t> arguments{kSiSegv};
    if ((action.flags & kSaSiginfo) != 0) {
        arguments.push_back(frame);
        arguments.push_back(ucontext);
    }

    depth = 1;
    machine.CallNestedPreservingRegisters(action.handler, arguments);
    depth = 0;

    // The handler ran with the interrupted file saved and restored around it, so
    // the only way to say "continue elsewhere" is the context it was given.
    // One line for the first fault a thread takes: a runtime that converts
    // faults into exceptions does it often, and each one is normal there.
    static thread_local bool logged = false;
    if (!logged) {
        logged = true;
        ARMBAT_LOG_INFO("Armbat", "fault: sigsegv at 0x%08x taken by the guest, pc=0x%08x",
                        address, interrupted[15]);
    }
    const std::uint32_t resume_pc = context[18];
    if (resume_pc == interrupted[15] && context[19] == interrupted_cpsr) {
        if (retry_pc == interrupted[15]) {
            if (++retries > 4) {
                return false;
            }
        } else {
            retry_pc = interrupted[15];
            retries = 0;
        }
        return true;  // retry the instruction: the handler left the context alone
    }
    retry_pc = 0;
    retries = 0;

    std::uint32_t pc = resume_pc;
    std::uint32_t cpsr = context[19];
    if ((pc & 1u) != 0) {
        // An exception handler is often a Thumb pointer with the bit still in it.
        pc &= ~1u;
        cpsr |= kThumbCpsr;
    }
    for (std::uint32_t index = 0; index <= 10; ++index) {
        machine.SetReg(index, context[3 + index]);
    }
    machine.SetReg(11, context[14]);
    machine.SetReg(12, context[15]);
    machine.SetReg(13, context[16]);
    machine.SetReg(14, context[17]);
    machine.SetReg(15, pc);
    machine.SetCpsr(cpsr);
    return true;
}

}  // namespace armbat
