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
constexpr std::uint32_t kFrameSlots = 32;
constexpr std::uint32_t kUcontextOffset = 0x100;
constexpr std::uint32_t kUcontextSize = 0x400;

// The host signal that breaks a guest thread out of a blocking syscall. It has
// to be one the platform does not use for anything else and whose handler may
// return: SIGUSR2 is free on Android, SIGSEGV belongs to the JIT's fault path.
constexpr int kWakeupSignal = SIGUSR2;

constexpr std::uint32_t kSaSiginfo = 0x00000004u;
constexpr std::uint32_t kSigDfl = 0;
constexpr std::uint32_t kSigIgn = 1;
// si_code for a thread-directed signal sent with tkill; the guest's handlers
// only compare it against SI_USER/SI_TKILL.
constexpr std::uint32_t kSiTkill = static_cast<std::uint32_t>(-6);

struct Action {
    std::uint32_t handler = kSigDfl;
    std::uint32_t flags = 0;
    std::uint32_t restorer = 0;
};

GuestArena* g_arena = nullptr;
std::mutex g_actions_mutex;
Action g_actions[65];
std::atomic<bool> g_ready{false};

void WakeupHandler(int) {
    // The syscall that was interrupted returns EINTR by itself. The guest signal
    // handler runs later, on the guest thread, at a safe point.
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
                          std::uint32_t restorer) {
    if (signal <= 0 || signal >= 65) {
        return;
    }
    const std::lock_guard lock(g_actions_mutex);
    g_actions[signal].handler = handler;
    g_actions[signal].flags = flags;
    g_actions[signal].restorer = restorer;
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

        const std::uint32_t frame =
            kFrameBase + (static_cast<std::uint32_t>(::gettid()) & (kFrameSlots - 1)) *
                             kFrameSlotSize;
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
        machine.CallNestedPreservingRegisters(action.handler, arguments);
    }
}

}  // namespace armbat
