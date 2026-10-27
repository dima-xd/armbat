#include "armbat/crash_handler.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>
#include <unwind.h>
#include <dirent.h>
#include <dlfcn.h>

#include <android/log.h>

#include "armbat/log.h"

#include "armbat/a32_machine.h"

namespace armbat {
namespace {

A32Machine* g_machine = nullptr;
thread_local A32Machine* t_machine = nullptr;
A32Machine* CurrentMachine() { return t_machine != nullptr ? t_machine : g_machine; }

char* AppendText(char* out, const char* text) {
    while (*text != '\0') {
        *out++ = *text++;
    }
    return out;
}

char* AppendHex(char* out, std::uintptr_t value) {
    static const char kDigits[] = "0123456789abcdef";
    *out++ = '0';
    *out++ = 'x';
    char digits[16];
    int count = 0;
    do {
        digits[count++] = kDigits[value & 0xF];
        value >>= 4;
    } while (value != 0);
    while (count > 0) {
        *out++ = digits[--count];
    }
    return out;
}

char* AppendDec(char* out, std::uintptr_t value) {
    char digits[20];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0) {
        *out++ = digits[--count];
    }
    return out;
}

void Handler(int signal, siginfo_t* info, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    std::uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#endif

    char buffer[640];
    char* end = buffer;
    end = AppendText(end, "fatal: host signal ");
    end = AppendDec(end, static_cast<std::uintptr_t>(signal));
    end = AppendText(end, " at host pc=");
    end = AppendHex(end, pc);
    end = AppendText(end, " addr=");
    end = AppendHex(end, reinterpret_cast<std::uintptr_t>(info != nullptr ? info->si_addr : nullptr));

    if (A32Machine* machine = CurrentMachine(); machine != nullptr) {
        end = AppendText(end, "\nRegs:");
        for (std::size_t i = 0; i < 16; ++i) {
            const std::uint32_t value = machine->Reg(i);
            end = AppendText(end, " r");
            end = AppendDec(end, i);
            end = AppendText(end, "=");
            end = AppendHex(end, value);
        }
        const std::uint32_t guest_pc = machine->Reg(15);
        std::uint32_t instruction = 0;
        if (machine->ReadGuestWord(guest_pc, &instruction)) {
            end = AppendText(end, "\nguest insn at ");
            end = AppendHex(end, guest_pc);
            end = AppendText(end, " = ");
            end = AppendHex(end, instruction);
            std::uint32_t second = 0;
            if (machine->ReadGuestWord(guest_pc + 4, &second)) {
                *end++ = ' ';
                end = AppendHex(end, second);
            }
        }
    }
    *end = '\0';

    (void)!::write(2, buffer, static_cast<std::size_t>(end - buffer));
    armbat::LogPanic(buffer);

    // The report is out; let the default action kill the process the way the
    // platform expects, so a tombstone is still produced.
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

}  // namespace


namespace {

// SIGUSR1 belongs to the runtime (it forces a GC), so the dump uses a
// real-time signal the platform leaves alone.
const int kDumpSignal = SIGRTMIN + 6;

struct BacktraceState {
    int depth = 0;
};

_Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void* argument) {
    auto* state = static_cast<BacktraceState*>(argument);
    if (state->depth >= 24) {
        return _URC_END_OF_STACK;
    }
    const std::uintptr_t pc = static_cast<std::uintptr_t>(_Unwind_GetIP(context));
    Dl_info info{};
    if (pc != 0 && ::dladdr(reinterpret_cast<void*>(pc), &info) != 0 && info.dli_sname != nullptr) {
        ARMBAT_LOG_INFO("Armbat", "  #%02d %s+0x%lx (%s)", state->depth,
                            info.dli_fname != nullptr ? info.dli_fname : "?", 
                            static_cast<unsigned long>(pc - reinterpret_cast<std::uintptr_t>(info.dli_fbase)),
                            info.dli_sname);
    } else if (pc != 0) {
        ARMBAT_LOG_INFO("Armbat", "  #%02d 0x%lx", state->depth,
                            static_cast<unsigned long>(pc));
    }
    ++state->depth;
    return _URC_NO_REASON;
}

void StackHandler(int, siginfo_t*, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    std::uintptr_t pc = 0;
    std::uintptr_t sp = 0;
#if defined(__aarch64__)
    pc = static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
    sp = static_cast<std::uintptr_t>(uc->uc_mcontext.sp);
#endif
    Dl_info where{};
    const char* image = "anonymous";
    if (pc != 0 && ::dladdr(reinterpret_cast<void*>(pc), &where) != 0 && where.dli_fname != nullptr) {
        image = where.dli_fname;
    }
    ARMBAT_LOG_INFO("Armbat", "thread %d: pc=%p sp=%p in %s", ::gettid(),
                        reinterpret_cast<void*>(pc), reinterpret_cast<void*>(sp), image);
    BacktraceState state;
    _Unwind_Backtrace(&CollectFrame, &state);
}

void InstallStackHandler() {
    struct sigaction action {};
    action.sa_sigaction = &StackHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigaction(kDumpSignal, &action, nullptr);
}

}  // namespace

void DumpThreadStacks() {
    static bool installed = false;
    if (!installed) {
        InstallStackHandler();
        installed = true;
    }
    const int pid = ::getpid();
    DIR* tasks = ::opendir("/proc/self/task");
    if (tasks == nullptr) {
        return;
    }
    std::vector<int> tids;
    while (dirent* entry = ::readdir(tasks)) {
        const int tid = std::atoi(entry->d_name);
        if (tid > 0 && tid != static_cast<int>(::gettid())) {
            tids.push_back(tid);
        }
    }
    ::closedir(tasks);
    ARMBAT_LOG_INFO("Armbat", "stacks of %zu threads:", tids.size() + 1);
    for (int tid : tids) {
        ::syscall(SYS_tgkill, pid, tid, kDumpSignal);
        ::usleep(20000);
    }
}

void SetCrashMachine(A32Machine* machine) {
    t_machine = machine;
}

namespace {
std::atomic<bool> g_sample{false};
}

void RequestGuestSample() { g_sample.store(true); }
bool TakeGuestSample() {
    // The JIT asks once per instruction it translates, which is millions of
    // times while a game loads. The exchange only has to happen when a sample
    // was actually requested: a relaxed load is a plain move.
    if (!g_sample.load(std::memory_order_relaxed)) {
        return false;
    }
    return g_sample.exchange(false);
}

void InstallCrashHandler(A32Machine* machine) {
    g_machine = machine;
    struct sigaction action {};
    action.sa_sigaction = &Handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int signal : {SIGILL, SIGSEGV, SIGBUS, SIGFPE}) {
        ::sigaction(signal, &action, nullptr);
    }
}

}  // namespace armbat
