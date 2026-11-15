#include "armbat/crash_handler.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>
#include <unwind.h>
#include <dirent.h>
#include <dlfcn.h>

#include <android/log.h>

#include "armbat/log.h"

#include "armbat/a32_machine.h"
#include "armbat/guest_modules.h"

namespace armbat {
namespace {

// The platform installs its own handlers for these before the container exists
// and some of them are not crash reporting at all: ART turns an implicit null
// check in compiled Java into a NullPointerException, and a stack overflow into
// a StackOverflowError, by repairing the context this handler is given. Keeping
// the previous action and handing the fault back is what keeps that working --
// an unhandled one ends in a tombstone either way.
struct SignalSlot {
    int signal = 0;
    struct sigaction previous {};
    bool valid = false;
};

constexpr int kSignals[] = {SIGILL, SIGSEGV, SIGBUS, SIGFPE};
constexpr std::size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
SignalSlot g_slots[kSignalCount];

thread_local A32Machine* t_machine = nullptr;
A32Machine* CurrentMachine() { return t_machine; }

// A report is built in place: a crash handler may not allocate, and a handler
// that needs a std::string is a handler that may not survive the fault it is
// reporting.
class Report {
public:
    Report(char* buffer, std::size_t size)
        : begin_(buffer), out_(buffer), end_(buffer + size) {
        if (size != 0) {
            *out_ = '\0';
        }
    }

    Report& Text(const char* text) {
        while (*text != '\0' && out_ < end_) {
            *out_++ = *text++;
        }
        if (out_ < end_) {
            *out_ = '\0';
        }
        return *this;
    }

    Report& Hex(std::uintptr_t value) {
        static const char kDigits[] = "0123456789abcdef";
        Text("0x");
        char digits[16];
        int count = 0;
        do {
            digits[count++] = kDigits[value & 0xF];
            value >>= 4;
        } while (value != 0);
        while (count > 0) {
            Char(digits[--count]);
        }
        return *this;
    }

    Report& Dec(std::uintptr_t value) {
        char digits[20];
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0) {
            Char(digits[--count]);
        }
        return *this;
    }

    Report& Char(char value) {
        if (out_ < end_) {
            *out_++ = value;
            *out_ = '\0';
        }
        return *this;
    }

    const char* Begin() const { return begin_; }
    const char* End() const { return out_; }

private:
    char* begin_;
    char* out_;
    char* end_;
};

int HexDigit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

// Parses one line of /proc/self/maps and keeps it when it covers `address`.
// Layout: start-end perms offset dev inode path.
bool ParseMapsLine(const char* line, std::uintptr_t address, std::uintptr_t* start,
                   char* perms, std::size_t perms_size, const char** name) {
    const char* cursor = line;
    std::uintptr_t begin = 0;
    int digit = HexDigit(*cursor);
    if (digit < 0) {
        return false;
    }
    while ((digit = HexDigit(*cursor)) >= 0) {
        begin = (begin << 4) | static_cast<std::uintptr_t>(digit);
        ++cursor;
    }
    if (*cursor != '-') {
        return false;
    }
    ++cursor;
    std::uintptr_t finish = 0;
    while ((digit = HexDigit(*cursor)) >= 0) {
        finish = (finish << 4) | static_cast<std::uintptr_t>(digit);
        ++cursor;
    }
    if (address < begin || address >= finish) {
        return false;
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    for (std::size_t i = 0; i + 1 < perms_size && cursor[i] != ' ' && cursor[i] != '\0'; ++i) {
        perms[i] = cursor[i];
        perms[i + 1] = '\0';
    }
    while (*cursor != ' ' && *cursor != '\0') {
        ++cursor;
    }
    // Skip the offset, the device and the inode: the path is what is left, and
    // it may contain spaces of its own.
    for (int field = 0; field < 3; ++field) {
        while (*cursor == ' ') {
            ++cursor;
        }
        while (*cursor != ' ' && *cursor != '\0') {
            ++cursor;
        }
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    *start = begin;
    *name = cursor;
    return true;
}

// What the host has mapped at `address`: a guest crash and a host crash need
// completely different answers, and a bare pc cannot tell them apart. The maps
// file is read with open/read, which a signal handler may do; dladdr is not
// used because it takes locks the faulting thread may already hold.
void AppendMapping(Report& report, std::uintptr_t address) {
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        report.Text("<no maps>");
        return;
    }
    char chunk[1024];
    char line[512] = {};
    std::size_t used = 0;
    bool found = false;
    std::uintptr_t start = 0;
    char perms[8] = {};
    const char* name = nullptr;
    for (;;) {
        const ssize_t count = ::read(fd, chunk, sizeof(chunk));
        if (count <= 0) {
            break;
        }
        for (ssize_t index = 0; index < count && !found; ++index) {
            if (chunk[index] != '\n') {
                if (used + 1 < sizeof(line)) {
                    line[used++] = chunk[index];
                    line[used] = '\0';
                }
                continue;
            }
            used = 0;
            found = ParseMapsLine(line, address, &start, perms, sizeof(perms), &name);
        }
        if (found) {
            break;
        }
    }
    ::close(fd);
    if (!found) {
        report.Text("<unmapped>");
        return;
    }
    std::size_t length = 0;
    while (name[length] != ' ' && name[length] != '\0' && length < 192) {
        ++length;
    }
    if (length == 0) {
        // Anonymous memory has no path; the range is all there is to say.
        report.Text("<anonymous+");
    } else {
        for (std::size_t index = 0; index < length; ++index) {
            report.Char(name[index]);
        }
        report.Char('+');
    }
    report.Hex(address - start).Text(" ").Text(perms).Text(">");
}

void AppendThread(Report& report) {
    char name[32] = {};
    const int tid = ::gettid();
    if (::prctl(PR_GET_NAME, name, 0, 0, 0) != 0) {
        name[0] = '\0';
    }
    report.Text(" tid=").Dec(static_cast<std::uintptr_t>(tid)).Text(" comm=");
    if (name[0] != '\0') {
        report.Text(name);
    } else {
        report.Char('-');
    }
}

void AppendGuestRegisters(Report& report) {
    A32Machine* machine = CurrentMachine();
    if (machine == nullptr) {
        // A host thread that never entered the JIT (an ART compiler thread, a
        // Java thread between calls) has no guest state to print, and printing
        // another thread's registers would be worse than printing none.
        report.Text("\nguest: no machine on this thread");
        return;
    }
    report.Text("\nRegs:");
    for (std::size_t index = 0; index < 16; ++index) {
        report.Text(" r").Dec(index).Text("=").Hex(machine->Reg(index));
    }
    const std::uint32_t guest_pc = machine->Reg(15);
    // The image and offset are what names the guest code the host thread was
    // running; a bare 32-bit number leaves the reader to find the module base
    // first, which is exactly what nobody can do from a single log line.
    char described[96] = {};
    if (DescribeGuestAddressInline(guest_pc, described, sizeof(described))) {
        report.Text("\nguest pc: ").Text(described);
    }
    if (DescribeGuestAddressInline(machine->Reg(14), described, sizeof(described))) {
        report.Text(" lr: ").Text(described);
    }
    std::uint32_t instruction = 0;
    if (machine->ReadGuestWord(guest_pc, &instruction)) {
        report.Text("\nguest insn at ").Hex(guest_pc).Text(" = ").Hex(instruction);
        std::uint32_t second = 0;
        if (machine->ReadGuestWord(guest_pc + 4, &second)) {
            report.Char(' ').Hex(second);
        }
    }
}

// Hands the fault to whoever handled it before the container existed. A handler
// that repairs the context (ART) makes this a recovery and never returns here;
// one that does nothing at all would fault again on the same instruction, which
// the caller turns into a default action instead of a loop.
void ChainToPlatform(int signal, siginfo_t* info, void* context) {
    for (std::size_t index = 0; index < kSignalCount; ++index) {
        if (g_slots[index].signal != signal || !g_slots[index].valid) {
            continue;
        }
        const struct sigaction& previous = g_slots[index].previous;
        if ((previous.sa_flags & SA_SIGINFO) != 0) {
            if (previous.sa_sigaction != nullptr) {
                previous.sa_sigaction(signal, info, context);
            }
            return;
        }
        if (previous.sa_handler == SIG_IGN) {
            return;
        }
        if (previous.sa_handler != SIG_DFL && previous.sa_handler != nullptr) {
            previous.sa_handler(signal);
            return;
        }
        break;
    }
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(signal, &action, nullptr);
    // Delivered the moment this handler returns, which is the tombstone the
    // platform expects for a fault nobody claimed.
    ::raise(signal);
}

void Handler(int signal, siginfo_t* info, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    std::uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#endif
    const auto address =
        reinterpret_cast<std::uintptr_t>(info != nullptr ? info->si_addr : nullptr);

    // A handler that neither repairs the context nor terminates turns the same
    // instruction into the same fault forever. Three is already one more than a
    // real report needs.
    static thread_local std::uintptr_t last_pc = 0;
    static thread_local int repeats = 0;
    if (pc != 0 && pc == last_pc) {
        ++repeats;
    } else {
        last_pc = pc;
        repeats = 0;
    }

    char buffer[2048];
    Report report(buffer, sizeof(buffer));
    report.Text(repeats > 2 ? "fatal: repeating host signal " : "fatal: host signal ")
        .Dec(static_cast<std::uintptr_t>(signal))
        .Text(" si_code=")
        .Dec(static_cast<std::uintptr_t>(info != nullptr ? info->si_code : 0))
        .Text(" at host pc=")
        .Hex(pc)
        .Text(" <");
    AppendMapping(report, pc);
    report.Text(" addr=").Hex(address).Text(" <");
    AppendMapping(report, address);
    AppendThread(report);
    AppendGuestRegisters(report);
    if (repeats > 2) {
        report.Text("\nthe platform handler neither repaired nor ended this fault");
    }

    (void)!::write(2, report.Begin(), static_cast<std::size_t>(report.End() - report.Begin()));
    armbat::LogPanic(report.Begin());

    if (repeats > 2) {
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(signal, &action, nullptr);
        ::raise(signal);
        return;
    }
    ChainToPlatform(signal, info, context);
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

void InstallCrashHandler() {
    // The container starts once per process, but a second start in the same
    // process would save this handler as its own predecessor and chain to
    // itself.
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    struct sigaction action {};
    action.sa_sigaction = &Handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset(&action.sa_mask);
    for (std::size_t index = 0; index < kSignalCount; ++index) {
        struct sigaction previous {};
        if (::sigaction(kSignals[index], &action, &previous) != 0) {
            continue;
        }
        g_slots[index].signal = kSignals[index];
        g_slots[index].previous = previous;
        g_slots[index].valid = true;
    }
}

}  // namespace armbat
