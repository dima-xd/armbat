#include "armbat/guest_signal.h"

#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <android/log.h>

#include "armbat/log.h"

#include "armbat/a32_machine.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_modules.h"
#include "armbat/guest_pthread.h"

namespace armbat {
namespace {

// A signal frame per thread. The page sits between the page the platform lended
// contexts use (0x00d00000) and the HLE stub region, which nothing else claims.
constexpr std::uint32_t kFrameBase = 0x00d80000;
constexpr std::uint32_t kFrameSlotSize = 0x800;
// A frame is taken per *delivery*, not per thread. A guest handler is guest
// code, so it reaches the safe point, and a pending signal -- a collector's
// suspend -- is delivered on top of it; the second delivery would memset the
// frame the first handler is still reading. That is not hypothetical: a fault
// handler that ran while a zeroed suspend frame landed on its slot resumed at
// pc=0 with every register and cpsr zeroed, which reads as a crash somewhere
// else entirely. One frame per live delivery removes the overlap whatever shape
// it takes, and a thread that exits can never leave a frame owned.
constexpr std::uint32_t kFrameSlots = A32Machine::Config::kProcessorLimit;
constexpr std::uint32_t kFrameWords = (kFrameSlots + 63) / 64;
constexpr std::uint32_t kFrameRegionEnd = 0x00e00000;  // HLE stubs start here
static_assert(kFrameBase + kFrameSlots * kFrameSlotSize <= kFrameRegionEnd,
              "signal frames must end before the HLE stub region");
constexpr std::uint32_t kUcontextOffset = 0x100;
constexpr std::uint32_t kUcontextSize = 0x400;

// The host signal that breaks a guest thread out of a blocking syscall. It has
// to be one the platform does not use for anything else and whose handler may
// return: SIGUSR2 is free on Android, SIGSEGV belongs to the JIT's fault path.
constexpr int kWakeupSignal = SIGUSR2;

constexpr std::uint32_t kSaSiginfo = 0x00000004u;
// SA_RESTART: the kernel restarts a slow syscall the handler interrupted instead
// of failing it with EINTR. It decides whether a parked wait may be ended.
constexpr std::uint32_t kSaRestart = 0x10000000u;
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
constexpr std::uint32_t kSegvAccerr = 2;  // the page exists, the access does not
constexpr std::uint32_t kThumbCpsr = 0x20;

std::atomic<std::uint64_t> g_frame_used[kFrameWords];

// Zero when every frame is in flight, which the caller reports instead of
// writing somewhere it does not own.
std::uint32_t AcquireFrame() {
    for (std::uint32_t word = 0; word < kFrameWords; ++word) {
        std::uint64_t current = g_frame_used[word].load(std::memory_order_relaxed);
        for (;;) {
            if (current == ~std::uint64_t{0}) {
                break;  // full: try the next word
            }
            const std::uint32_t bit = static_cast<std::uint32_t>(__builtin_ctzll(~current));
            if (g_frame_used[word].compare_exchange_weak(current, current | (1ull << bit),
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_relaxed)) {
                return kFrameBase + (word * 64 + bit) * kFrameSlotSize;
            }
            // `current` now holds the value the exchange lost to; retry with it.
        }
    }
    return 0;
}

void ReleaseFrame(std::uint32_t frame) {
    const std::uint32_t slot = (frame - kFrameBase) / kFrameSlotSize;
    g_frame_used[slot / 64].fetch_and(~(1ull << (slot % 64)), std::memory_order_release);
}

// The register file of the thread the signal interrupted, in the layout of the
// kernel's struct sigcontext. Written for a fault and for an asynchronous
// signal alike: a handler that reads the stack pointer -- a collector's suspend
// handler does -- has to find a real one, not zero.
void WriteSigcontext(GuestArena& arena, std::uint32_t ucontext,
                     const std::uint32_t registers[16], std::uint32_t cpsr,
                     std::uint32_t fault_address) {
    std::uint32_t* const context =
        reinterpret_cast<std::uint32_t*>(arena.Ptr(ucontext + kUcMcontext));
    for (std::uint32_t index = 0; index <= 10; ++index) {
        context[3 + index] = registers[index];
    }
    context[14] = registers[11];
    context[15] = registers[12];
    context[16] = registers[13];
    context[17] = registers[14];
    context[18] = registers[15];
    context[19] = cpsr;
    context[20] = fault_address;
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
    // A guest that installs a handler for a signal expects that signal to reach
    // its own code; one line per signal at boot names them, which is the
    // difference between "the runtime took the fault" and "nobody was there",
    // and it is the only way a stop-the-world signal becomes visible at all.
    if (handler != kSigDfl && handler != kSigIgn) {
        static std::atomic<std::uint64_t> reported{0};
        const std::uint64_t bit = 1ull << (signal - 1);
        if ((reported.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
            if (signal == kSiSegv) {
                ARMBAT_LOG_INFO("Armbat", "signals: SIGSEGV handler at 0x%08x flags=0x%x",
                                handler, flags);
            } else {
                ARMBAT_LOG_INFO("Armbat", "signals: handler %d at 0x%08x flags=0x%x", signal,
                                handler, flags);
            }
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

// Whether every signal in `deliverable` was installed with SA_RESTART, i.e.
// whether Linux would run the handler and then restart the wait instead of
// handing the guest an EINTR. A wait that answers EINTR to a restarted call
// invents a wakeup the guest never got on a device.
bool GuestSignalsRestart(std::uint64_t deliverable) {
    const std::lock_guard lock(g_actions_mutex);
    for (int signal = 1; signal < 65 && deliverable != 0; ++signal) {
        if ((deliverable & (1ull << (signal - 1))) == 0) {
            continue;
        }
        if ((g_actions[signal].flags & kSaRestart) == 0) {
            return false;
        }
    }
    return true;
}

bool SendGuestSignal(std::uint32_t tid, int signal) {
    if (signal <= 0 || signal >= 65) {
        return false;
    }
    // A guest thread running on a borrowed context carries the lender's
    // identity: bionic answers pthread_self() with the lender's pthread, and the
    // tid cached in it is what raise(), pthread_kill() and a collector's
    // stop-the-world address. The signal belongs to whoever runs on that
    // context, not to the lender parked in the container's stub.
    tid = BorrowingThreadForLender(tid);
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

        const std::uint32_t frame = AcquireFrame();
        if (frame == 0) {
            // Every frame is with a handler that is still running. Leaving the
            // signal pending offers it again at the next safe point.
            machine.RequestSignal(signal);
            return;
        }
        // siginfo_t on ARM32: int si_signo, si_errno, si_code, then the union.
        std::uint32_t info[32] = {};
        info[0] = signal;
        info[2] = kSiTkill;
        std::memcpy(g_arena->Ptr(frame), info, sizeof(info));
        // The handler gets the context the thread was interrupted with. It used
        // to be zeroed: a handler that only read si_signo was fine, and a
        // collector's suspend handler -- which walks the stack the context names
        // -- was not.
        std::memset(g_arena->Ptr(frame + kUcontextOffset), 0, kUcontextSize);
        std::uint32_t registers[16] = {};
        for (std::uint32_t index = 0; index < 16; ++index) {
            registers[index] = machine.Reg(index);
        }
        WriteSigcontext(*g_arena, frame + kUcontextOffset, registers, machine.Cpsr(), 0);

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
        ReleaseFrame(frame);
    }
}

bool DeliverGuestFault(A32Machine& machine, std::uint32_t address) {
    // One fault at a time on this thread: a handler that faults again is a guest
    // that cannot recover, and running a second handler on top of the first one
    // would only bury the first fault.
    static thread_local int depth = 0;
    // Retrying a fault the handler did not repair is what a device does, but it
    // has to end: the same pc faulting again and again is a run that would never
    // report anything.
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

    const std::uint32_t frame = AcquireFrame();
    if (frame == 0) {
        return false;
    }
    const std::uint32_t ucontext = frame + kUcontextOffset;
    std::memset(g_arena->Ptr(frame), 0, kFrameSlotSize);
    // A marker outside everything the guest reads. If it is gone when the
    // handler returns, a frame that was not ours overwrote this delivery.
    const std::uint32_t guard = 0x5a5a0000u | ((frame - kFrameBase) / kFrameSlotSize);
    const std::uint32_t guard_address = frame + kFrameSlotSize - 4;
    std::memcpy(g_arena->Ptr(guard_address), &guard, sizeof(guard));

    // siginfo_t, ARM32: si_signo, si_errno, si_code, then _sigfault.si_addr.
    // A page that exists but refuses the access is SEGV_ACCERR on a device, and
    // a handler that distinguishes the two reads si_code for it.
    std::uint32_t* const info = reinterpret_cast<std::uint32_t*>(g_arena->Ptr(frame));
    info[0] = kSiSegv;
    info[2] = g_arena->IsMapped(address, 1) ? kSegvAccerr : kSegvMaperr;
    info[3] = address;

    std::uint32_t* const context =
        reinterpret_cast<std::uint32_t*>(g_arena->Ptr(ucontext + kUcMcontext));
    std::uint32_t interrupted[16] = {};
    for (std::uint32_t index = 0; index < 16; ++index) {
        interrupted[index] = machine.Reg(index);
    }
    const std::uint32_t interrupted_cpsr = machine.Cpsr();
    WriteSigcontext(*g_arena, ucontext, interrupted, interrupted_cpsr, address);

    std::vector<std::uint32_t> arguments{kSiSegv};
    if ((action.flags & kSaSiginfo) != 0) {
        arguments.push_back(frame);
        arguments.push_back(ucontext);
    }

    // A fault is delivered like any other signal: the thread's own mask and the
    // action's sa_mask apply, and SIGSEGV is blocked inside its own handler
    // unless SA_NODEFER asked otherwise. A runtime that nests fault handlers
    // reads the mask to decide whether it may fault again on this thread.
    const std::uint64_t previous = machine.BlockedSignals();
    std::uint64_t during = previous | action.mask;
    if ((action.flags & kSaNodefer) == 0) {
        during |= 1ull << (kSiSegv - 1);
    }
    machine.SetBlockedSignals(during);
    depth = 1;
    machine.CallNestedPreservingRegisters(action.handler, arguments);
    depth = 0;
    machine.SetBlockedSignals(previous);

    // The handler ran with the interrupted file saved and restored around it, so
    // the only way to say "continue elsewhere" is the context it was given.
    // One line for the first fault a thread takes: a runtime that converts
    // faults into exceptions does it often, and each one is normal there.
    static thread_local bool logged = false;
    if (!logged) {
        logged = true;
        ARMBAT_LOG_INFO("Armbat",
                        "fault: sigsegv at 0x%08x taken by the guest, pc=%s lr=%s "
                        "r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x sp=0x%08x",
                        address, armbat::DescribeGuestAddress(interrupted[15]).c_str(),
                        armbat::DescribeGuestAddress(interrupted[14]).c_str(), interrupted[0],
                        interrupted[1], interrupted[2], interrupted[3], interrupted[13]);
    }
    std::uint32_t guard_after = 0;
    std::memcpy(&guard_after, g_arena->Ptr(guard_address), sizeof(guard_after));
    if (guard_after != guard) {
        ARMBAT_LOG_WARN("Armbat",
                        "signal: frame 0x%08x overwritten while the handler ran", frame);
    }

    // A device restores the whole context on rt_sigreturn and resumes at the
    // saved pc, so a handler that repairs r0 (or any register) without moving pc
    // is honoured too. Comparing only pc and cpsr threw such a repair away and
    // retried the instruction with the old values.
    const std::uint32_t resume_pc = context[18];
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
    ReleaseFrame(frame);

    // Retrying is what a device does when a handler returns to the faulting
    // instruction, and it has to end: the same pc faulting again and again is a
    // run that would never report anything.
    if (retry_pc == interrupted[15]) {
        if (++retries > 4) {
            return false;
        }
    } else {
        retry_pc = interrupted[15];
        retries = 0;
    }
    return true;
}

}  // namespace armbat
