#include "armbat/guest_kernel.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include <android/fdsan.h>
#include <android/log.h>

#include "armbat/log.h"
#include <fcntl.h>
#include <linux/futex.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "armbat/a32_machine.h"
#include "armbat/guest_abi.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_modules.h"
#include "armbat/guest_pthread.h"
#include "armbat/guest_signal.h"
#include "armbat/guest_stdio.h"

namespace armbat {
namespace {

constexpr std::uint32_t kSyscallSvc = 0;
constexpr std::uint32_t kMaxPath = 4096;
constexpr std::uint32_t kOLargeFile = 0x8000;  // 32-bit-only flag
constexpr std::uint32_t kAtEmptyPath = 0x1000;

// clone() flags used by bionic's pthreads and by everything that wraps it.
constexpr std::uint32_t kCloneVm = 0x00000100;
constexpr std::uint32_t kCloneSetTls = 0x00080000;
constexpr std::uint32_t kCloneParentSettid = 0x00100000;
constexpr std::uint32_t kCloneChildCleartid = 0x00200000;
constexpr std::uint32_t kCloneChildSettid = 0x01000000;

// ARM private system calls share `svc #0` and live above the ordinary table.
constexpr std::uint32_t kArmNrBase = 0x0f0000;
constexpr std::uint32_t kArmNrCacheflush = kArmNrBase + 2;
constexpr std::uint32_t kArmNrSetTls = kArmNrBase + 5;
constexpr std::uint32_t kArmNrGetTls = kArmNrBase + 6;
// Armbat's own handshake with the bionic boot program.
constexpr std::uint32_t kArmbatBootReady = kArmNrBase + 0x100;
// A name lookup the guest cannot make itself: bionic asks netd, and a container
// has none. The guest's libarmbat_net.so carries the question out through this
// trap and receives the addresses packed by the host's resolver.
constexpr std::uint32_t kArmbatResolve = kArmNrBase + 0x101;

// ARM uses the asm-generic syscall table, so these numbers are stable.
enum SyscallNumber : std::uint32_t {
    kSysExit = 1,
    kSysRead = 3,
    kSysWrite = 4,
    kSysOpen = 5,
    kSysClose = 6,
    kSysLseek = 19,
    kSysGetpid = 20,
    kSysGetuid = 24,
    kSysAccess = 33,
    kSysKill = 37,
    kSysDup = 41,
    kSysBrk = 45,
    kSysGetgid = 47,
    kSysGeteuid = 49,
    kSysGetegid = 50,
    kSysIoctl = 54,
    kSysFcntl = 55,
    kSysDup2 = 63,
    kSysGetppid = 64,
    kSysSigaction = 67,
    kSysSetrlimit = 75,
    // ARM kept the 32-bit `ugetrlimit` and bionic calls it directly, so a
    // guest that asks for a limit through `getrlimit` never reaches prlimit64.
    kSysUgetrlimit = 191,
    kSysGettimeofday = 78,
    kSysReadlink = 85,
    kSysMunmap = 91,
    kSysFtruncate = 93,
    kSysStat = 106,
    kSysLstat = 107,
    kSysFstat = 108,
    kSysUname = 122,
    kSysMprotect = 125,
    kSysSigprocmask = 126,
    kSysClone = 120,
    kSysReadv = 145,
    kSysWritev = 146,
    kSysPersonality = 136,
    kSysLlseek = 140,
    kSysSchedYield = 158,
    kSysNanosleep = 162,
    kSysPrctl = 172,
    kSysRtSigreturn = 173,
    kSysRtSigaction = 174,
    kSysRtSigprocmask = 175,
    kSysPread64 = 180,
    kSysPwrite64 = 181,
    kSysGetcwd = 183,
    kSysSigaltstack = 186,
    kSysTkill = 238,
    kSysMmap2 = 192,
    kSysStat64 = 195,
    kSysLstat64 = 196,
    kSysFstat64 = 197,
    kSysFcntl64 = 221,
    kSysGettid = 224,
    kSysFutex = 240,
    kSysSchedGetaffinity = 242,
    kSysExitGroup = 248,
    kSysEpollCreate = 250,
    kSysEpollCtl = 251,
    kSysEpollWait = 252,
    kSysSetTidAddress = 256,
    kSysClockGettime = 263,
    kSysClockGetres = 264,
    kSysTgkill = 268,
    kSysSocket = 281,
    kSysConnect = 283,
    kSysOpenat = 322,
    kSysFstatat64 = 327,
    kSysReadlinkat = 332,
    kSysFaccessat = 334,
    kSysSetRobustList = 338,
    kSysEventfd2 = 356,
    kSysEpollCreate1 = 357,
    kSysDup3 = 358,
    kSysPipe2 = 359,
    kSysPrlimit64 = 369,
    kSysGetrandom = 384,

    // The rest of the file tree a game builds for itself -- its own directories,
    // saved worlds, options -- plus the 32-bit uid calls an old bionic asks for
    // and two scheduler queries whose answers are known. ARM borrows the unified
    // numbers for the *at family.
    kSysUnlink = 10,
    kSysChmod = 15,
    kSysRename = 38,
    kSysMkdir = 39,
    kSysRmdir = 40,
    kSysTruncate = 92,
    kSysFchmod = 94,
    kSysTruncate64 = 193,
    kSysFtruncate64 = 194,
    // leveldb makes a database durable by flushing the file it wrote and the
    // directory that holds it; a world cannot be created or saved without them.
    kSysSync = 36,
    kSysFsync = 118,
    kSysFdatasync = 148,
    kSysSchedPolicy = 157,
    kSysGetuid32 = 199,
    kSysGetgid32 = 200,
    kSysGeteuid32 = 201,
    kSysGetegid32 = 202,
    kSysSchedSetaffinity = 241,
    kSysMkdirat = 323,
    kSysFchownat = 325,
    kSysUnlinkat = 328,
    kSysRenameat = 329,
    kSysLinkat = 330,
    kSysSymlinkat = 331,
    kSysFchmodat = 333,

    // The socket calls beside socket and connect: a game opens one even in
    // single player, to look for a LAN server while the menu loads.
    kSysBind = 282,
    kSysListen = 284,
    kSysAccept = 285,
    kSysGetsockname = 286,
    kSysGetpeername = 287,
    kSysSocketpair = 288,
    kSysSend = 289,
    kSysSendto = 290,
    kSysRecv = 291,
    kSysRecvfrom = 292,
    // An engine's network layer builds an iovec for everything it writes, and
    // bionic's own interface enumeration reads netlink replies through msghdr.
    // Without them the guest never learns its own address, which is the first
    // thing a LAN game asks for.
    kSysSendmsg = 296,
    kSysRecvmsg = 297,
    kSysShutdown = 293,
    kSysSetsockopt = 294,
    kSysGetsockopt = 295,
    kSysAccept4 = 366,

    // The calls a game's runtime reaches for next: the ones an engine needs to
    // sleep, wait on a descriptor, grow the allocator's arena, or query the
    // host's clock and memory.
    kSysGetrusage = 77,
    kSysSysinfo = 116,
    kSysSelect = 142,
    kSysPoll = 168,
    kSysGetdents64 = 217,
    kSysMadvise = 220,
    kSysClockNanosleep = 265,
    kSysStatfs64 = 266,
    kSysFstatfs64 = 267,
    kSysSchedGetscheduler = 155,
    kSysSchedSetscheduler = 156,
    kSysSchedGetPriorityMax = 159,
    kSysSchedGetPriorityMin = 160,
    kSysPpoll = 336,
    kSysTimerfdCreate = 350,
    kSysTimerfdSettime = 351,
    kSysTimerfdGettime = 352,
    kSysPselect6 = 400,
};

std::int32_t ClampSize(ssize_t value) {
    return static_cast<std::int32_t>(std::min<ssize_t>(value, INT32_MAX));
}

GuestStat64 ToGuestStat64(const struct stat& host) {
    GuestStat64 guest{};
    guest.st_dev = static_cast<std::uint64_t>(host.st_dev);
    guest.st_ino_old = static_cast<std::uint32_t>(host.st_ino);
    guest.st_mode = static_cast<std::uint32_t>(host.st_mode);
    guest.st_nlink = static_cast<std::uint32_t>(host.st_nlink);
    guest.st_uid = static_cast<std::uint32_t>(host.st_uid);
    guest.st_gid = static_cast<std::uint32_t>(host.st_gid);
    guest.st_rdev = static_cast<std::uint64_t>(host.st_rdev);
    guest.st_size = static_cast<std::int64_t>(host.st_size);
    guest.st_blksize = static_cast<std::uint32_t>(host.st_blksize);
    guest.st_blocks = static_cast<std::uint64_t>(host.st_blocks);
    guest.atime = static_cast<std::int32_t>(host.st_atim.tv_sec);
    guest.atime_nsec = static_cast<std::uint32_t>(host.st_atim.tv_nsec);
    guest.mtime = static_cast<std::int32_t>(host.st_mtim.tv_sec);
    guest.mtime_nsec = static_cast<std::uint32_t>(host.st_mtim.tv_nsec);
    guest.ctime = static_cast<std::int32_t>(host.st_ctim.tv_sec);
    guest.ctime_nsec = static_cast<std::uint32_t>(host.st_ctim.tv_nsec);
    guest.st_ino = static_cast<std::uint64_t>(host.st_ino);
    return guest;
}

std::uint64_t Split64(std::uint32_t low, std::uint32_t high) {
    return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
}

// The guest's 32-bit `struct epoll_event`: 4 bytes of events and an 8-byte
// union aligned to 8, so 16 bytes with padding.
struct GuestEpollEvent {
    std::uint32_t events;
    std::uint32_t pad;
    std::uint64_t data;
};

static_assert(sizeof(GuestEpollEvent) == 16, "32-bit epoll_event must be 16 bytes");

// The guest's 32-bit struct msghdr: every pointer is 32 bits and every length is
// a 32-bit size_t, so the structure cannot be handed to the host as it stands.
struct GuestMsghdr {
    std::uint32_t name;
    std::uint32_t name_length;
    std::uint32_t iov;
    std::uint32_t iov_length;
    std::uint32_t control;
    std::uint32_t control_length;
    std::uint32_t flags;
};

static_assert(sizeof(GuestMsghdr) == 28, "32-bit msghdr must be 28 bytes");

// Ancillary data framing differs between the two ABIs: the guest's cmsghdr is
// three words with a 4-byte aligned payload, the host's is a 64-bit length, two
// ints and an 8-byte aligned payload. The payload itself (SO_TIMESTAMP,
// IP_PKTINFO) is byte-identical, so only the framing is rebuilt.
constexpr std::size_t kGuestCmsgHeader = 12;
constexpr std::size_t kHostCmsgHeader = 16;
// A message with more segments than this is not a game's write path but a broken
// length; refusing keeps the allocation bounded.
constexpr std::uint32_t kMaxIoVecs = 1024;

std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool TranslateGuestIoVecs(GuestArena& arena, std::uint32_t iov, std::uint32_t count,
                          std::vector<iovec>& host) {
    host.clear();
    if (count == 0) {
        return true;
    }
    if (count > kMaxIoVecs || !arena.IsMapped(iov, count * sizeof(GuestIoVec))) {
        return false;
    }
    host.resize(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        GuestIoVec guest{};
        std::memcpy(&guest, arena.Ptr(iov + index * sizeof(GuestIoVec)), sizeof(guest));
        if (guest.length != 0 && !arena.IsMapped(guest.base, guest.length)) {
            host.clear();
            return false;
        }
        host[index].iov_base = guest.length != 0 ? arena.Ptr(guest.base) : nullptr;
        host[index].iov_len = guest.length;
    }
    return true;
}

bool BuildHostControl(GuestArena& arena, std::uint32_t control, std::uint32_t length,
                      std::vector<char>& host) {
    host.clear();
    if (control == 0 || length == 0) {
        return true;
    }
    if (!arena.IsMapped(control, length)) {
        return false;
    }
    const char* guest = reinterpret_cast<const char*>(arena.Ptr(control));
    std::size_t offset = 0;
    while (offset + kGuestCmsgHeader <= length) {
        std::uint32_t guest_length = 0;
        std::int32_t level = 0;
        std::int32_t type = 0;
        std::memcpy(&guest_length, guest + offset, sizeof(guest_length));
        if (guest_length < kGuestCmsgHeader || offset + guest_length > length) {
            break;
        }
        std::memcpy(&level, guest + offset + 4, sizeof(level));
        std::memcpy(&type, guest + offset + 8, sizeof(type));
        const std::size_t data_length = guest_length - kGuestCmsgHeader;
        const std::size_t start = AlignUp(host.size(), 8);
        host.resize(start + kHostCmsgHeader + data_length);
        auto* header = reinterpret_cast<cmsghdr*>(host.data() + start);
        header->cmsg_len = kHostCmsgHeader + data_length;
        header->cmsg_level = level;
        header->cmsg_type = type;
        std::memcpy(host.data() + start + kHostCmsgHeader, guest + offset + kGuestCmsgHeader,
                    data_length);
        offset += AlignUp(guest_length, 4);
    }
    return true;
}

// Turns the host's ancillary data back into the guest's framing. Returns the
// number of bytes written into the guest buffer; a short buffer is the guest's
// MSG_CTRUNC, never a fault.
std::uint32_t BuildGuestControl(GuestArena& arena, const void* control, std::size_t length,
                                std::uint32_t destination, std::uint32_t capacity, bool* truncated) {
    *truncated = false;
    if (control == nullptr || length < kHostCmsgHeader || destination == 0 || capacity == 0) {
        return 0;
    }
    const char* host = static_cast<const char*>(control);
    char* guest = reinterpret_cast<char*>(arena.Ptr(destination));
    std::size_t offset = 0;
    std::size_t written = 0;
    while (offset + kHostCmsgHeader <= length) {
        const auto* header = reinterpret_cast<const cmsghdr*>(host + offset);
        if (header->cmsg_len < kHostCmsgHeader || offset + header->cmsg_len > length) {
            break;
        }
        const std::size_t data_length = header->cmsg_len - kHostCmsgHeader;
        const std::size_t start = AlignUp(written, 4);
        const std::size_t needed = start + kGuestCmsgHeader + data_length;
        if (needed > capacity) {
            *truncated = true;
            break;
        }
        std::uint32_t guest_length = static_cast<std::uint32_t>(kGuestCmsgHeader + data_length);
        std::int32_t level = header->cmsg_level;
        std::int32_t type = header->cmsg_type;
        std::memcpy(guest + start, &guest_length, sizeof(guest_length));
        std::memcpy(guest + start + 4, &level, sizeof(level));
        std::memcpy(guest + start + 8, &type, sizeof(type));
        std::memcpy(guest + start + kGuestCmsgHeader, host + offset + kHostCmsgHeader,
                    data_length);
        written = needed;
        offset += AlignUp(static_cast<std::size_t>(header->cmsg_len), 8);
    }
    return static_cast<std::uint32_t>(written);
}


// The machine whose thread is inside a syscall right now. Handlers that act on
// thread state (clone, TLS, cache flush) must use this one, not the machine the
// guest was started with.
thread_local A32Machine* t_calling_machine = nullptr;

// The syscall running on this host thread has asked its machine to stop. It has
// to be per thread: all guest threads share one kernel object, so a shared flag
// is cleared by the next thread that enters a syscall. When that happens to an
// exit(), the guest walks on past its own exit into bionic's stack-teardown
// code and unmaps a thread struct another thread is joining -- the world-load
// hang. bionic's _exit has no code after it that is meant to run.
thread_local bool t_halt_requested = false;

A32Machine& CallingMachine(A32Machine& fallback) {
    return t_calling_machine != nullptr ? *t_calling_machine : fallback;
}

// Threads parked in futex right now. A wait that outlives a few seconds is
// either a deadlock or a wakeup the container lost, and the table says who else
// is parked and on what.
//
// The table is a fixed array of slots that a waiting thread claims for itself,
// not a locked list: a condition variable wait is not rare in a game (every
// queued task on a worker pool is one), and the previous push_back/erase under a
// process-wide mutex made every one of them serialise against every other
// thread's wait and wakeup. A slot carries only what the dump prints; the stack
// of the parked thread is looked up by tid when the dump is taken, so nothing
// here points at a machine that could go away.
constexpr std::size_t kFutexSlots = 256;

struct FutexSlot {
    std::atomic<std::uint32_t> tid{0};  // 0 while the slot is free
    std::atomic<std::uint32_t> address{0};
    std::atomic<std::uint32_t> expected{0};
    std::atomic<std::uint32_t> operation{0};
    std::atomic<std::uint32_t> sp{0};
    std::atomic<std::uint64_t> since{0};  // steady-clock nanoseconds, 0 while unfilled
    // A park is reported once. The log quota is per process, and the guest's own
    // output shares it: repeating the same idle-pool line every few seconds
    // pushes the engine's log out of the buffer, which is worse than silence.
    std::atomic<bool> reported{false};
};

FutexSlot g_futex_slots[kFutexSlots];
thread_local int t_futex_slot = -1;

std::uint64_t SteadyNanos() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void FutexWaitBegin(std::uint32_t address, std::int32_t expected, int operation, A32Machine& machine) {
    if (t_futex_slot >= 0) {
        return;  // the outer wait is the one worth reporting
    }
    const std::uint32_t tid = static_cast<std::uint32_t>(::gettid());
    const std::size_t start = tid % kFutexSlots;
    for (std::size_t probe = 0; probe < kFutexSlots; ++probe) {
        const std::size_t index = (start + probe) % kFutexSlots;
        FutexSlot& slot = g_futex_slots[index];
        std::uint32_t empty = 0;
        if (!slot.tid.compare_exchange_strong(empty, tid, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            continue;
        }
        slot.address.store(address, std::memory_order_relaxed);
        slot.expected.store(static_cast<std::uint32_t>(expected), std::memory_order_relaxed);
        slot.operation.store(static_cast<std::uint32_t>(operation), std::memory_order_relaxed);
        slot.sp.store(machine.Reg(13), std::memory_order_relaxed);
        slot.reported.store(false, std::memory_order_relaxed);
        // Last: a slot without a start time is skipped by the dump, so a reader
        // never sees a half-filled record.
        slot.since.store(SteadyNanos(), std::memory_order_release);
        t_futex_slot = static_cast<int>(index);
        return;
    }
    // A full table costs the dump one thread, never the wait.
}

void FutexWaitEnd() {
    if (t_futex_slot < 0) {
        return;
    }
    FutexSlot& slot = g_futex_slots[static_cast<std::size_t>(t_futex_slot)];
    t_futex_slot = -1;
    slot.since.store(0, std::memory_order_relaxed);
    slot.address.store(0, std::memory_order_relaxed);
    slot.expected.store(0, std::memory_order_relaxed);
    slot.operation.store(0, std::memory_order_relaxed);
    slot.sp.store(0, std::memory_order_relaxed);
    slot.reported.store(false, std::memory_order_relaxed);
    // Release the slot only after everything else is cleared, so the next thread
    // to claim it cannot race with the clears.
    slot.tid.store(0, std::memory_order_release);
}

}  // namespace

GuestKernel::GuestKernel(A32Machine& machine, GuestArena& arena, KernelConfig config)
    : machine_(machine),
      arena_(arena),
      memory_(arena, config.mmap_start, config.mmap_limit, config.brk_base, config.brk_limit) {}

bool GuestKernel::Prepare(std::string* error) {
    (void)error;
    machine_.AddSvcHandler([this](std::uint32_t svc, A32Machine& machine) {
        return Handle(svc, machine);
    });
    return true;
}

bool GuestKernel::Handle(std::uint32_t svc, A32Machine& machine) {
    if (svc != kSyscallSvc) {
        return false;
    }

    // Every syscall has to act on the machine of the thread that made it: a
    // clone, a cache flush or a TLS update issued by a guest thread belongs to
    // that thread, not to the one that started the guest.
    A32Machine* const previous_caller = t_calling_machine;
    t_calling_machine = &machine;
    struct CallerRestore {
        A32Machine* previous;
        ~CallerRestore() { t_calling_machine = previous; }
    } restore{previous_caller};

    const std::uint32_t args[6] = {machine.Reg(0), machine.Reg(1), machine.Reg(2),
                                   machine.Reg(3), machine.Reg(4), machine.Reg(5)};
    const std::uint32_t number = machine.Reg(7);

    ++calls_;
    last_number_ = number;
    machine.NoteSyscall(number);

    t_halt_requested = false;
    const std::int32_t result = number >= kArmNrBase ? DispatchPrivate(number, args)
                                                     : Dispatch(number, args);
    machine.SetReg(0, static_cast<std::uint32_t>(result));

    // The first use of each syscall, once: a startup that stops making progress
    // is nearly always a call that got an answer the guest did not expect, and
    // the set of calls it made is what tells one apart from the other.
    const std::size_t trace_slot =
        number < kArmNrBase
            ? (number < kTraceNumbers ? number : kTraceNumbers + kTracePrivate)
            : (number - kArmNrBase < kTracePrivate ? kTraceNumbers + (number - kArmNrBase)
                                                   : kTraceNumbers + kTracePrivate);
    if (trace_slot < kTraceNumbers + kTracePrivate &&
        !traced_syscalls_[trace_slot].exchange(true, std::memory_order_relaxed)) {
        ARMBAT_LOG_INFO("Armbat",
                            "syscall %u first: r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x -> %d",
                            number, args[0], args[1], args[2], args[3], result);
    }

    if (t_halt_requested) {
        t_halt_requested = false;
        machine.Stop();
    }
    return true;
}

bool GuestKernel::ReadString(std::uint32_t address, std::string& out) const {
    out.clear();
    for (std::uint32_t offset = 0; offset < kMaxPath; ++offset) {
        if (!arena_.IsMapped(address + offset, 1)) {
            return false;
        }
        const char ch = static_cast<char>(
            *reinterpret_cast<const std::uint8_t*>(arena_.Ptr(address + offset)));
        if (ch == '\0') {
            return true;
        }
        out.push_back(ch);
    }
    return false;
}

bool GuestKernel::WriteGuest(std::uint32_t address, const void* data, std::size_t size) {
    if (size != 0 && !arena_.IsMapped(address, size)) {
        return false;
    }
    std::memcpy(arena_.Ptr(address), data, size);
    return true;
}

std::string GuestKernel::MapPath(const std::string& guest_path) const {
    if (path_mapper_) {
        return path_mapper_(guest_path);
    }
    return guest_path;
}

std::string GuestKernel::ResolveGuestPath(std::string guest_path) {
    // The guest has no working directory of its own: `chdir` is not part of the
    // emulated kernel and `getcwd` answers "/", so a relative name always means
    // a name under the guest's root. Answer that explicitly instead of handing
    // the name to the host, whose working directory is not part of the contract
    // and a library such as Mortar strips the leading slash of a Java-provided
    // path before it opens a save.
    if (!guest_path.empty() && guest_path[0] != '/') {
        guest_path.insert(guest_path.begin(), '/');
    }
    return guest_path;
}

void GuestKernel::TrackGuestFd(int fd) {
    if (fd < 0) {
        return;
    }
    const std::lock_guard lock(fds_mutex_);
    guest_fds_.insert(fd);
}

void GuestKernel::ForgetGuestFd(int fd) {
    const std::lock_guard lock(fds_mutex_);
    guest_fds_.erase(fd);
}

bool GuestKernel::IsGuestFd(int fd) const {
    const std::lock_guard lock(fds_mutex_);
    return guest_fds_.count(fd) != 0;
}

std::string GuestKernel::RecentPaths() const {
    std::string text;
    for (const std::string& trace : path_traces_) {
        text += "\n  path: ";
        text += trace;
    }
    return text;
}

void GuestKernel::TracePath(const std::string& guest_path, const std::string& host_path,
                            int result) {    if (path_traces_.size() >= 32) {
        path_traces_.erase(path_traces_.begin());
    }
    path_traces_.push_back(guest_path + " -> " + host_path + " = " + std::to_string(result));
    // TEMP: follows the Bricknet save path that ends in a null resource.
    if (guest_path.find("bricknet") != std::string::npos ||
        guest_path.find("saveDir") != std::string::npos) {
        ARMBAT_LOG_INFO("Armbat", "path: %s -> %s = %d",
                            guest_path.c_str(), host_path.c_str(), result);
    }
}

void GuestKernel::ForwardLogRecord(const void* data, std::size_t size) {
    if (size < 8) {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    // A logd record starts with u16 len and u16 hdr_size, followed by a
    // logger_entry (28 bytes on this platform). The text is everything after
    // the header; the exact convention differs between liblog versions, so the
    // header size only narrows the guess and a non-printable guess is discarded.
    std::uint16_t header = 0;
    std::memcpy(&header, bytes + 2, sizeof(header));
    std::size_t offset = 0;
    if (header >= 20 && header <= 28) {
        offset = 28;
    } else if (header > 28 && static_cast<std::size_t>(header) + 4 <= size) {
        offset = static_cast<std::size_t>(header) + 4;
    }
    auto collect = [&](std::size_t from) {
        std::string text;
        for (std::size_t index = from; index < size; ++index) {
            const std::uint8_t byte = bytes[index];
            if (byte == '\n' || byte == '\t' || (byte >= 0x20 && byte < 0x7f)) {
                text.push_back(static_cast<char>(byte));
            }
        }
        return text;
    };
    std::string text = collect(offset);
    if (text.size() < 4 && offset != 0) {
        text = collect(0);
    }
    if (!text.empty()) {
        ForwardStdio(2, text);
    }
}

std::int32_t GuestKernel::DispatchPrivate(std::uint32_t number, const std::uint32_t* args) {
    switch (number) {
    case kArmNrSetTls: {
        A32Machine& caller = CallingMachine(machine_);
        caller.SetTlsBase(args[0]);
        return 0;
    }
    case kArmNrGetTls:
        return static_cast<std::int32_t>(CallingMachine(machine_).TlsBase());
    case kArmNrCacheflush: {
        // dynarmic keeps its own code cache; guest writes to code are already
        // visible to it through the arena, so only an invalidation is needed.
        const std::uint32_t begin = args[0];
        const std::uint32_t end = args[1];
        if (end > begin) {
            CallingMachine(machine_).InvalidateCacheRange(begin, end - begin);
        }
        return 0;
    }
    case kArmbatBootReady:
        if (boot_ready_) {
            boot_ready_(args[0]);
        }
        t_halt_requested = true;
        return 0;
    case kArmbatResolve: {
        // The answer is packed as a count followed by one entry per address:
        // family, length, and the raw address. The guest shim turns it into
        // addrinfo with the guest's own allocator.
        constexpr std::size_t kEntries = 8;
        constexpr std::size_t kEntryMax = 8 + 16;
        std::string name;
        if (!ReadString(args[0], name) || name.empty()) {
            return -EINVAL;
        }
        if (args[2] < 4 || !arena_.IsMapped(args[1], args[2])) {
            return -EFAULT;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(name.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
            return -EHOSTUNREACH;
        }
        std::vector<std::uint8_t> packed(4, 0);
        std::uint32_t count = 0;
        for (const addrinfo* node = found;
             node != nullptr && count < kEntries;
             node = node->ai_next) {
            if (node->ai_addr == nullptr) {
                continue;
            }
            const bool ipv4 = node->ai_family == AF_INET;
            if (!ipv4 && node->ai_family != AF_INET6) {
                continue;
            }
            std::uint8_t bytes[16] = {};
            if (ipv4) {
                std::memcpy(bytes, &reinterpret_cast<const sockaddr_in*>(node->ai_addr)->sin_addr,
                            sizeof(in_addr));
            } else {
                std::memcpy(bytes,
                            &reinterpret_cast<const sockaddr_in6*>(node->ai_addr)->sin6_addr,
                            sizeof(in6_addr));
            }
            const std::uint32_t length = ipv4 ? sizeof(in_addr) : sizeof(in6_addr);
            const std::size_t start = packed.size();
            packed.resize(start + kEntryMax, 0);
            const std::uint32_t family = static_cast<std::uint32_t>(node->ai_family);
            std::memcpy(packed.data() + start, &family, sizeof(family));
            std::memcpy(packed.data() + start + 4, &length, sizeof(length));
            std::memcpy(packed.data() + start + 8, bytes, length);
            ++count;
        }
        ::freeaddrinfo(found);
        if (count == 0) {
            return -EHOSTUNREACH;
        }
        std::memcpy(packed.data(), &count, sizeof(count));
        if (packed.size() > args[2]) {
            return -ENOSPC;
        }
        std::memcpy(arena_.Ptr(args[1]), packed.data(), packed.size());
        return static_cast<std::int32_t>(packed.size());
    }
    default:
        last_unimplemented_ = number;
        return -ENOSYS;
    }
}

std::int32_t GuestKernel::Dispatch(std::uint32_t number, const std::uint32_t* args) {
    switch (number) {
    case kSysRead: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        const std::uint32_t count = std::min<std::uint32_t>(args[2], INT32_MAX);
        if (count != 0 && !arena_.IsMapped(buffer, count)) {
            return -EFAULT;
        }
        const ssize_t result = ::read(fd, arena_.Ptr(buffer), count);
        return result < 0 ? -errno : ClampSize(result);
    }

    case kSysWrite: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        const std::uint32_t count = std::min<std::uint32_t>(args[2], INT32_MAX);
        if (count != 0 && !arena_.IsMapped(buffer, count)) {
            return -EFAULT;
        }
        if (IsLogSocket(fd)) {
            ForwardLogRecord(arena_.Ptr(buffer), count);
            return ClampSize(count);
        }
        if ((fd == 1 || fd == 2) && count != 0) {
            ForwardStdio(fd,
                         std::string_view(reinterpret_cast<const char*>(arena_.Ptr(buffer)), count));
        }
        const ssize_t result = ::write(fd, arena_.Ptr(buffer), count);
        return result < 0 ? -errno : ClampSize(result);
    }

    case kSysWritev:
    case kSysReadv: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t vector_address = args[1];
        const std::uint32_t vector_count = std::min<std::uint32_t>(args[2], 1024);
        if (vector_count != 0 &&
            !arena_.IsMapped(vector_address, vector_count * sizeof(GuestIoVec))) {
            return -EFAULT;
        }
        // Guest iovecs are 8 bytes; the host's iovec is 16. Gather/scatter through
        // one contiguous host buffer instead of rebuilding the vector array.
        std::vector<GuestIoVec> guest_vectors(vector_count);
        if (vector_count != 0) {
            std::memcpy(guest_vectors.data(), arena_.Ptr(vector_address),
                        vector_count * sizeof(GuestIoVec));
        }
        std::size_t total = 0;
        for (const GuestIoVec& vector : guest_vectors) {
            if (!arena_.IsMapped(vector.base, vector.length)) {
                return -EFAULT;
            }
            total += vector.length;
        }
        std::vector<char> buffer(total);
        std::size_t offset = 0;
        if (number == kSysWritev) {
            for (const GuestIoVec& vector : guest_vectors) {
                if (vector.length != 0) {
                    std::memcpy(buffer.data() + offset, arena_.Ptr(vector.base), vector.length);
                }
                offset += vector.length;
            }
            if (IsLogSocket(fd)) {
                ForwardLogRecord(buffer.data(), buffer.size());
                return ClampSize(static_cast<ssize_t>(buffer.size()));
            }
            if ((fd == 1 || fd == 2) && !buffer.empty()) {
                ForwardStdio(fd, std::string_view(buffer.data(), buffer.size()));
            }
            const ssize_t result = ::write(fd, buffer.data(), buffer.size());
            return result < 0 ? -errno : ClampSize(result);
        }
        const ssize_t result = ::read(fd, buffer.data(), buffer.size());
        if (result < 0) {
            return -errno;
        }
        for (const GuestIoVec& vector : guest_vectors) {
            const std::size_t chunk =
                std::min<std::size_t>(vector.length, static_cast<std::size_t>(result) - offset);
            if (chunk == 0) {
                break;
            }
            std::memcpy(arena_.Ptr(vector.base), buffer.data() + offset, chunk);
            offset += chunk;
        }
        return ClampSize(result);
    }

    case kSysPread64:
    case kSysPwrite64: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        const std::uint32_t count = std::min<std::uint32_t>(args[2], INT32_MAX);
        // ARM EABI keeps 64-bit arguments in an even register pair, so after
        // (fd, buf, count) the padding lands in r3 and the offset occupies r4:r5.
        // bionic's pread64 stub loads exactly those two registers from the
        // caller's stack; reading r3:r4 instead asks the kernel for an offset of
        // (garbage << 0), which happily returns zero bytes at end of file.
        const std::uint64_t offset = Split64(args[4], args[5]);
        if (count != 0 && !arena_.IsMapped(buffer, count)) {
            return -EFAULT;
        }
        const ssize_t result = number == kSysPread64
                                   ? ::pread(fd, arena_.Ptr(buffer), count,
                                             static_cast<off_t>(offset))
                                   : ::pwrite(fd, arena_.Ptr(buffer), count,
                                              static_cast<off_t>(offset));
        return result < 0 ? -errno : ClampSize(result);
    }

    case kSysOpen:
    case kSysOpenat: {
        const bool with_directory = number == kSysOpenat;
        const std::uint32_t path_address = args[with_directory ? 1 : 0];
        std::string path;
        if (!ReadString(path_address, path)) {
            return -EFAULT;
        }

        int flags = static_cast<int>(args[with_directory ? 2 : 1]);
        flags &= ~static_cast<int>(kOLargeFile);
        const int mode = static_cast<int>(args[with_directory ? 3 : 2]);
        const int directory = with_directory ? static_cast<int>(args[0]) : AT_FDCWD;
        // A relative name is relative to the guest's working directory, which is
        // always the guest root; an explicit descriptor is resolved by the host
        // against that descriptor, as on a real kernel.
        if (directory == AT_FDCWD) {
            path = ResolveGuestPath(std::move(path));
        }
        const std::string host_path = MapPath(path);

        const int fd = with_directory
                           ? ::openat(static_cast<int>(args[0]), host_path.c_str(), flags, mode)
                           : ::open(host_path.c_str(), flags, mode);
        TracePath(path, host_path, fd < 0 ? -errno : 0);
        if (fd >= 0) {
            TrackGuestFd(fd);
        }
        return fd < 0 ? -errno : fd;
    }

    case kSysClose: {
        const int fd = static_cast<int>(args[0]);
        // A guest descriptor number is a host descriptor number, and the two
        // spaces overlap: a guest that closes a number it was never handed --
        // liblog's socket, a Java stream, a surface -- either takes the
        // container's logging away for the rest of the run or dies in fdsan with
        // nothing of the guest left in the record. Only the numbers the guest
        // opened belong to it, plus the three standard ones the platform hands a
        // program; anything else is EBADF, as a real kernel answers.
        if (fd < 0) {
            return -EBADF;
        }
        if (fd > 2) {
            if (!IsGuestFd(fd) || android_fdsan_get_owner_tag(fd) != 0) {
                static std::atomic<unsigned> refusals{0};
                if (refusals.fetch_add(1) < 8) {
                    ARMBAT_LOG_WARN("Armbat",
                                        "close: fd=%d is not the guest's (owner tag %lld)", fd,
                                        static_cast<long long>(android_fdsan_get_owner_tag(fd)));
                }
                return -EBADF;
            }
            ForgetGuestFd(fd);
        }
        log_sockets_.erase(fd);
        const int result = ::close(fd);
        return result < 0 ? -errno : 0;
    }

    // `_llseek` is the 32-bit ARM way to move a file descriptor to a 64-bit
    // offset: the offset is split into two registers and the result comes back
    // through a pointer, not through r0.
    case kSysLlseek: {
        const int fd = static_cast<int>(args[0]);
        const std::uint64_t offset = Split64(args[2], args[1]);
        const std::uint32_t result_pointer = args[3];
        const int whence = static_cast<int>(args[4]);
        if (!arena_.IsMapped(result_pointer, 8)) {
            return -EFAULT;
        }
        const off_t result = ::lseek(fd, static_cast<off_t>(offset), whence);
        if (result < 0) {
            return -errno;
        }
        const std::uint64_t guest = static_cast<std::uint64_t>(result);
        std::memcpy(arena_.Ptr(result_pointer), &guest, sizeof(guest));
        return 0;
    }

    case kSysLseek: {
        const off_t result = ::lseek(static_cast<int>(args[0]),
                                     static_cast<off_t>(static_cast<std::int32_t>(args[1])),
                                     static_cast<int>(args[2]));
        return result < 0 ? -errno : static_cast<std::int32_t>(result);
    }

    // Since Lollipop bionic's `struct stat` is the 64-bit-inode structure, and
    // `fstat` is an alias of `fstat64`, so every one of these writes the same
    // 104-byte layout.
    case kSysFstat:
    case kSysFstat64: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        if (!arena_.IsMapped(buffer, sizeof(GuestStat64))) {
            return -EFAULT;
        }
        struct stat host {};
        if (::fstat(fd, &host) != 0) {
            return -errno;
        }
        const GuestStat64 guest = ToGuestStat64(host);
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysStat:
    case kSysLstat:
    case kSysStat64:
    case kSysLstat64: {
        std::string path;
        if (!ReadString(args[0], path)) {
            return -EFAULT;
        }
        const std::uint32_t buffer = args[1];
        if (!arena_.IsMapped(buffer, sizeof(GuestStat64))) {
            return -EFAULT;
        }
        const std::string host_path = MapPath(path);
        struct stat host {};
        const bool follow = number == kSysStat || number == kSysStat64;
        const int result = follow ? ::stat(host_path.c_str(), &host)
                                  : ::lstat(host_path.c_str(), &host);
        if (result != 0) {
            TracePath(path, host_path, -errno);
            return -errno;
        }
        const GuestStat64 guest = ToGuestStat64(host);
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysFstatat64: {
        const int directory = static_cast<int>(args[0]);
        std::string path;
        if (!ReadString(args[1], path)) {
            return -EFAULT;
        }
        const std::uint32_t buffer = args[2];
        const int flags = static_cast<int>(args[3]);
        if (!arena_.IsMapped(buffer, sizeof(GuestStat64))) {
            return -EFAULT;
        }
        const std::string host_path =
            (path.empty() && (flags & kAtEmptyPath) != 0) ? std::string() : MapPath(path);
        struct stat host {};
        if (::fstatat(directory, host_path.c_str(), &host, flags) != 0) {
            return -errno;
        }
        const GuestStat64 guest = ToGuestStat64(host);
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysAccess:
    case kSysFaccessat: {
        const bool at = number == kSysFaccessat;
        const int directory = at ? static_cast<int>(args[0]) : AT_FDCWD;
        std::string path;
        if (!ReadString(args[at ? 1 : 0], path)) {
            return -EFAULT;
        }
        const int mode = static_cast<int>(args[at ? 2 : 1]);
        const int flags = at ? static_cast<int>(args[3]) : 0;
        const std::string host_path = MapPath(path);
        return ::faccessat(directory, host_path.c_str(), mode, flags) == 0 ? 0 : -errno;
    }

    // A game builds its own data tree -- directories, world saves, options --
    // under its files directory. These calls cross the same path mapping as the
    // reads, so whatever the guest names is the host path of the same name, and
    // the answer is the host's. Guest descriptors are host descriptors, so the
    // *at forms take the directory descriptor straight through.
    case kSysMkdir:
    case kSysMkdirat: {
        const bool at = number == kSysMkdirat;
        const int directory = at ? static_cast<int>(args[0]) : AT_FDCWD;
        std::string path;
        if (!ReadString(args[at ? 1 : 0], path)) {
            return -EFAULT;
        }
        const int mode = static_cast<int>(args[at ? 2 : 1]) & 07777;
        const std::string host_path = MapPath(path);
        const int result = at ? ::mkdirat(directory, host_path.c_str(), mode)
                              : ::mkdir(host_path.c_str(), mode);
        TracePath(path, host_path, result == 0 ? 0 : -errno);
        return result == 0 ? 0 : -errno;
    }

    case kSysRmdir: {
        std::string path;
        if (!ReadString(args[0], path)) {
            return -EFAULT;
        }
        const std::string host_path = MapPath(path);
        return ::rmdir(host_path.c_str()) == 0 ? 0 : -errno;
    }

    case kSysUnlink:
    case kSysUnlinkat: {
        const bool at = number == kSysUnlinkat;
        const int directory = at ? static_cast<int>(args[0]) : AT_FDCWD;
        std::string path;
        if (!ReadString(args[at ? 1 : 0], path)) {
            return -EFAULT;
        }
        const int flags = at ? static_cast<int>(args[2]) : 0;
        const std::string host_path = MapPath(path);
        const int result = at ? ::unlinkat(directory, host_path.c_str(), flags)
                              : ::unlink(host_path.c_str());
        TracePath(path, host_path, result == 0 ? 0 : -errno);
        return result == 0 ? 0 : -errno;
    }

    case kSysRename:
    case kSysRenameat: {
        const bool at = number == kSysRenameat;
        const int from_directory = at ? static_cast<int>(args[0]) : AT_FDCWD;
        const int to_directory = at ? static_cast<int>(args[2]) : AT_FDCWD;
        std::string from;
        std::string to;
        if (!ReadString(args[at ? 1 : 0], from) || !ReadString(args[at ? 3 : 1], to)) {
            return -EFAULT;
        }
        const std::string host_from = MapPath(from);
        const std::string host_to = MapPath(to);
        const int result =
            at ? ::renameat(from_directory, host_from.c_str(), to_directory, host_to.c_str())
               : ::rename(host_from.c_str(), host_to.c_str());
        TracePath(from, host_from, result == 0 ? 0 : -errno);
        return result == 0 ? 0 : -errno;
    }

    case kSysChmod:
    case kSysFchmodat: {
        const bool at = number == kSysFchmodat;
        const int directory = at ? static_cast<int>(args[0]) : AT_FDCWD;
        std::string path;
        if (!ReadString(args[at ? 1 : 0], path)) {
            return -EFAULT;
        }
        const int mode = static_cast<int>(args[at ? 2 : 1]) & 07777;
        const std::string host_path = MapPath(path);
        const int result = at ? ::fchmodat(directory, host_path.c_str(), mode, 0)
                              : ::chmod(host_path.c_str(), mode);
        return result == 0 ? 0 : -errno;
    }

    case kSysFchmod: {
        return ::fchmod(static_cast<int>(args[0]), static_cast<int>(args[1]) & 07777) == 0
                   ? 0
                   : -errno;
    }

    case kSysFchownat: {
        std::string path;
        if (!ReadString(args[1], path)) {
            return -EFAULT;
        }
        const std::string host_path = MapPath(path);
        return ::fchownat(static_cast<int>(args[0]), host_path.c_str(),
                          static_cast<uid_t>(args[2]), static_cast<gid_t>(args[3]),
                          static_cast<int>(args[4])) == 0
                   ? 0
                   : -errno;
    }

    // ARM's `truncate` takes a 32-bit length; bionic reaches for the 64-bit form
    // for anything longer, which carries its length in the first two registers
    // the way `ftruncate` does.
    case kSysTruncate: {
        std::string path;
        if (!ReadString(args[0], path)) {
            return -EFAULT;
        }
        const std::string host_path = MapPath(path);
        return ::truncate(host_path.c_str(), static_cast<off_t>(args[1])) == 0 ? 0 : -errno;
    }

    case kSysTruncate64: {
        std::string path;
        if (!ReadString(args[0], path)) {
            return -EFAULT;
        }
        const std::string host_path = MapPath(path);
        const std::uint64_t length = Split64(args[1], args[2]);
        return ::truncate(host_path.c_str(), static_cast<off_t>(length)) == 0 ? 0 : -errno;
    }

    case kSysFtruncate64: {
        const std::uint64_t length = Split64(args[1], args[2]);
        return ::ftruncate(static_cast<int>(args[0]), static_cast<off_t>(length)) == 0 ? 0
                                                                                        : -errno;
    }

    case kSysSync:
        ::sync();
        return 0;

    case kSysFsync:
    case kSysFdatasync: {
        const int fd = static_cast<int>(args[0]);
        const int result = number == kSysFsync ? ::fsync(fd) : ::fdatasync(fd);
        return result < 0 ? -errno : 0;
    }

    case kSysLinkat: {
        std::string target;
        std::string link;
        if (!ReadString(args[1], target) || !ReadString(args[3], link)) {
            return -EFAULT;
        }
        const std::string host_target = MapPath(target);
        const std::string host_link = MapPath(link);
        return ::linkat(static_cast<int>(args[0]), host_target.c_str(),
                        static_cast<int>(args[2]), host_link.c_str(),
                        static_cast<int>(args[4])) == 0
                   ? 0
                   : -errno;
    }

    case kSysSymlinkat: {
        std::string target;
        std::string link;
        if (!ReadString(args[0], target) || !ReadString(args[2], link)) {
            return -EFAULT;
        }
        // The target stays a guest path -- whatever follows the link maps it
        // again -- while the link itself has to exist on the host.
        const std::string host_link = MapPath(link);
        return ::symlinkat(target.c_str(), static_cast<int>(args[1]), host_link.c_str()) == 0
                   ? 0
                   : -errno;
    }

    case kSysSchedPolicy:
        return 0;  // SCHED_OTHER, the only policy the container has
    case kSysSchedSetaffinity:
        return 0;

    case kSysReadlink:
    case kSysReadlinkat: {
        const bool at = number == kSysReadlinkat;
        const int directory = at ? static_cast<int>(args[0]) : AT_FDCWD;
        std::string path;
        if (!ReadString(args[at ? 1 : 0], path)) {
            return -EFAULT;
        }
        const std::uint32_t buffer = args[at ? 2 : 1];
        const std::uint32_t size = args[at ? 3 : 2];
        if (size != 0 && !arena_.IsMapped(buffer, size)) {
            return -EFAULT;
        }
        // The guest's own executable is not the host process image. bionic's
        // linker takes the name (and the stat) of /proc/self/exe as the main
        // executable, so answer with the guest path the boot program was
        // started as.
        if (!executable_path_.empty() && path == "/proc/self/exe") {
            const std::uint32_t length =
                std::min<std::uint32_t>(static_cast<std::uint32_t>(executable_path_.size()), size);
            std::memcpy(arena_.Ptr(buffer), executable_path_.data(), length);
            return static_cast<std::int32_t>(length);
        }
        const std::string host_path = MapPath(path);
        const ssize_t result = at ? ::readlinkat(directory, host_path.c_str(),
                                                 reinterpret_cast<char*>(arena_.Ptr(buffer)), size)
                                  : ::readlink(host_path.c_str(),
                                               reinterpret_cast<char*>(arena_.Ptr(buffer)), size);
        TracePath(path, host_path, result < 0 ? -errno : static_cast<int>(result));
        return result < 0 ? -errno : ClampSize(result);
    }

    case kSysGetcwd: {
        const std::uint32_t buffer = args[0];
        const std::uint32_t size = args[1];
        if (size < 2) {
            return -ERANGE;
        }
        if (!arena_.IsMapped(buffer, 2)) {
            return -EFAULT;
        }
        std::memcpy(arena_.Ptr(buffer), "/", 2);
        return 1;
    }

    case kSysGetpid:
        return static_cast<std::int32_t>(::getpid());

    case kSysGettid:
        return static_cast<std::int32_t>(::gettid());

    case kSysGetuid:
    case kSysGetuid32:
        return 10000;
    case kSysGeteuid:
    case kSysGeteuid32:
        return 10000;
    case kSysGetgid:
    case kSysGetgid32:
        return 10000;
    case kSysGetegid:
    case kSysGetegid32:
        return 10000;
    case kSysGetppid:
        return 1;

    case kSysMmap2: {
        std::int32_t error = 0;
        const std::uint32_t mapped = memory_.Map(args[0], args[1], static_cast<int>(args[3]),
                                                 static_cast<int>(args[4]), args[5], &error);
        if (args[1] >= 0x40000) {
            char origin[256] = "-";
            if (static_cast<std::int32_t>(args[4]) >= 0) {
                char link[64];
                std::snprintf(link, sizeof(link), "/proc/self/fd/%d", static_cast<int>(args[4]));
                const ssize_t read = ::readlink(link, origin, sizeof(origin) - 1);
                if (read > 0) {
                    origin[read] = '\0';
                }
            }
            static std::atomic<unsigned> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 24) {
                ARMBAT_LOG_INFO("Armbat",
                                    "mmap: length=0x%08x at=0x%08x offset=0x%08x file=%s", args[1],
                                    mapped != 0 ? mapped : static_cast<std::uint32_t>(error), args[5],
                                    origin);
            }
            if (mapped != 0 && std::strcmp(origin, "-") != 0) {
                RegisterGuestModuleMapping(mapped, args[1], origin);
            }
        }
        return mapped != 0 ? static_cast<std::int32_t>(mapped) : error;
    }

    case kSysMunmap:
        if (memory_.Unmap(args[0], args[1])) {
            armbat::UnregisterGuestMappings(args[0], args[1]);
            return 0;
        }        return -EINVAL;

    case kSysMprotect: {
        if (args[1] == 0) {
            return 0;
        }
        const std::uint32_t address = args[0] & ~0xfffu;
        const std::uint32_t length = (args[1] + (args[0] & 0xfff) + 0xfff) & ~0xfffu;
        if (!arena_.IsMapped(address, length)) {
            return -ENOMEM;
        }
        // The arena is always committed read/write; the JIT fetches code through
        // callbacks, so protection is advisory and only has to be accepted.
        return 0;
    }

    case kSysBrk:
        return static_cast<std::int32_t>(memory_.Break(args[0]));

    case kSysClockGettime: {
        const std::uint32_t buffer = args[1];
        if (!arena_.IsMapped(buffer, sizeof(GuestTimespec))) {
            return -EFAULT;
        }
        const int clock = static_cast<int>(args[0]);
        if (clock < 0 || clock > 7) {
            return -EINVAL;
        }
        timespec host{};
        if (::clock_gettime(static_cast<clockid_t>(clock), &host) != 0) {
            return -errno;
        }
        const GuestTimespec guest{static_cast<std::int32_t>(host.tv_sec),
                                  static_cast<std::int32_t>(host.tv_nsec)};
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysClockGetres: {
        const std::uint32_t buffer = args[1];
        if (!arena_.IsMapped(buffer, sizeof(GuestTimespec))) {
            return -EFAULT;
        }
        timespec host{};
        if (::clock_getres(static_cast<clockid_t>(args[0]), &host) != 0) {
            return -errno;
        }
        const GuestTimespec guest{static_cast<std::int32_t>(host.tv_sec),
                                  static_cast<std::int32_t>(host.tv_nsec)};
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysGettimeofday: {
        const std::uint32_t buffer = args[0];
        if (buffer != 0 && !arena_.IsMapped(buffer, sizeof(GuestTimeval))) {
            return -EFAULT;
        }
        timeval host{};
        if (::gettimeofday(&host, nullptr) != 0) {
            return -errno;
        }
        if (buffer != 0) {
            const GuestTimeval guest{static_cast<std::int32_t>(host.tv_sec),
                                     static_cast<std::int32_t>(host.tv_usec)};
            std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        }
        return 0;
    }

    case kSysNanosleep: {
        const std::uint32_t request = args[0];
        const std::uint32_t remaining = args[1];
        if (!arena_.IsMapped(request, sizeof(GuestTimespec))) {
            return -EFAULT;
        }
        GuestTimespec guest{};
        std::memcpy(&guest, arena_.Ptr(request), sizeof(guest));
        timespec host{guest.seconds, guest.nanoseconds};
        if (::nanosleep(&host, &host) != 0) {
            if (errno == EINTR && arena_.IsMapped(remaining, sizeof(GuestTimespec))) {
                const GuestTimespec left{static_cast<std::int32_t>(host.tv_sec),
                                         static_cast<std::int32_t>(host.tv_nsec)};
                std::memcpy(arena_.Ptr(remaining), &left, sizeof(left));
            }
            return -errno;
        }
        return 0;
    }

    // futex: the guest's own lock words live in the arena, so the host kernel can
    // wait on them directly. Guest threads then sleep in the host instead of
    // spinning under the JIT.
    case kSysFutex: {
        const std::uint32_t address = args[0];
        const int operation = static_cast<int>(args[1]);
        const std::uint32_t value = args[2];
        const std::uint32_t timeout_address = args[3];
        const std::uint32_t address2 = args[4];

        if (!arena_.IsMapped(address, sizeof(std::uint32_t))) {
            return -EFAULT;
        }
        int* target = reinterpret_cast<int*>(arena_.Ptr(address));
        int* target2 = nullptr;
        if (address2 != 0) {
            if (!arena_.IsMapped(address2, sizeof(std::uint32_t))) {
                return -EFAULT;
            }
            target2 = reinterpret_cast<int*>(arena_.Ptr(address2));
        }

        timespec timeout{};
        const timespec* timeout_pointer = nullptr;
        if (timeout_address != 0) {
            if (!arena_.IsMapped(timeout_address, sizeof(GuestTimespec))) {
                return -EFAULT;
            }
            GuestTimespec guest{};
            std::memcpy(&guest, arena_.Ptr(timeout_address), sizeof(guest));
            timeout.tv_sec = guest.seconds;
            timeout.tv_nsec = guest.nanoseconds;
            timeout_pointer = &timeout;
        }

        // A parked thread is the usual shape of a guest hang. Record it so the
        // watchdog can report who is parked on what, and otherwise behave
        // exactly like a futex: a wait without a timeout waits forever.
        const int base_operation = operation & 0x7f;
        const bool waiting =
            base_operation == FUTEX_WAIT || base_operation == FUTEX_WAIT_BITSET;
        if (waiting && timeout_pointer == nullptr) {
            FutexWaitBegin(address, static_cast<std::int32_t>(value), operation, CallingMachine(machine_));
        }
        const long result =
            ::syscall(SYS_futex, target, operation, value, timeout_pointer, target2, args[5]);
        const int error = errno;
        if (waiting && timeout_pointer == nullptr) {
            FutexWaitEnd();
        }
        return result < 0 ? -error : static_cast<std::int32_t>(result);
    }

    case kSysSetTidAddress:
        return static_cast<std::int32_t>(::gettid());

    case kSysSetRobustList:
        return 0;

    case kSysSchedGetaffinity: {
        const std::uint32_t size = args[1];
        const std::uint32_t mask = args[2];
        if (size != 0 && !arena_.IsMapped(mask, size)) {
            return -EFAULT;
        }
        std::vector<unsigned char> buffer(std::max<std::uint32_t>(size, sizeof(cpu_set_t)), 0);
        if (::sched_getaffinity(0, buffer.size(), reinterpret_cast<cpu_set_t*>(buffer.data())) !=
            0) {
            return -errno;
        }
        if (size != 0) {
            std::memcpy(arena_.Ptr(mask), buffer.data(), size);
        }
        return 0;
    }

    case kSysSchedYield:
        ::sched_yield();
        return 0;

    case kSysPrctl:
        return 0;

    case kSysUname: {
        const std::uint32_t buffer = args[0];
        if (!arena_.IsMapped(buffer, sizeof(GuestUtsname))) {
            return -EFAULT;
        }
        GuestUtsname guest{};
        std::snprintf(guest.sysname, sizeof(guest.sysname), "Linux");
        std::snprintf(guest.nodename, sizeof(guest.nodename), "localhost");
        std::snprintf(guest.release, sizeof(guest.release), "3.18.0-armbat");
        std::snprintf(guest.version, sizeof(guest.version), "#1 SMP PREEMPT");
        std::snprintf(guest.machine, sizeof(guest.machine), "armv7l");
        std::snprintf(guest.domainname, sizeof(guest.domainname), "(none)");
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysPrlimit64: {
        const std::uint32_t resource = args[1];
        const std::uint32_t new_limit = args[2];
        const std::uint32_t old_limit = args[3];
        if (new_limit != 0 && !arena_.IsMapped(new_limit, sizeof(GuestRlimit64))) {
            return -EFAULT;
        }
        if (old_limit != 0 && !arena_.IsMapped(old_limit, sizeof(GuestRlimit64))) {
            return -EFAULT;
        }
        struct rlimit host {};
        if (::getrlimit(static_cast<int>(resource), &host) != 0) {
            return -errno;
        }
        if (new_limit != 0) {
            // Limits are advisory inside a container: refusing the change keeps
            // the guest running instead of failing its libc initialization.
        }
        if (old_limit != 0) {
            GuestRlimit64 guest{};
            guest.current = host.rlim_cur == RLIM_INFINITY
                                ? ~std::uint64_t{0}
                                : static_cast<std::uint64_t>(host.rlim_cur);
            guest.maximum = host.rlim_max == RLIM_INFINITY
                                ? ~std::uint64_t{0}
                                : static_cast<std::uint64_t>(host.rlim_max);
            std::memcpy(arena_.Ptr(old_limit), &guest, sizeof(guest));
        }
        return 0;
    }

    case kSysSetrlimit:
        return 0;

    case kSysUgetrlimit: {
        // The kernel's 32-bit layout: two rlim_t words, not two 64-bit ones.
        // A guest that asks for RLIMIT_NOFILE sizes its descriptor table from
        // this answer, and ENOSYS here leaves that size to whatever the caller
        // had on its stack: Mono read its file table through a pointer that
        // came from an unfilled rlimit.
        const std::uint32_t resource = args[0];
        const std::uint32_t limit = args[1];
        if (!arena_.IsMapped(limit, sizeof(GuestRlimit32))) {
            return -EFAULT;
        }
        struct rlimit host {};
        if (::getrlimit(static_cast<int>(resource), &host) != 0) {
            return -errno;
        }
        GuestRlimit32 guest{};
        guest.current = host.rlim_cur == RLIM_INFINITY
                            ? ~std::uint32_t{0}
                            : static_cast<std::uint32_t>(host.rlim_cur);
        guest.maximum = host.rlim_max == RLIM_INFINITY
                            ? ~std::uint32_t{0}
                            : static_cast<std::uint32_t>(host.rlim_max);
        std::memcpy(arena_.Ptr(limit), &guest, sizeof(guest));
        return 0;
    }

    case kSysPersonality:
        // bionic's early init queries the persona and then sets a flag on it.
        // There is one persona in a container, and refusing the change makes
        // the linker call __libc_fatal, so both directions succeed.
        return 0;

    case kSysSigaction:
    case kSysRtSigaction: {
        // Signal handlers are recorded here; delivery happens at a safe point on
        // the target thread (see guest_signal.cpp). The action is read for the
        // handler and the SA_SIGINFO bit only, which is all the delivery path
        // needs; masks and alternate stacks are not emulated.
        //
        // The old action is answered with the one field a caller reads back --
        // the handler -- and nothing else. bionic's struct is a union of
        // function pointers followed by a mask, flags and restorer whose sizes
        // differ between the bionic versions these guests were built against,
        // and signal() puts its stack canary immediately after its local copy:
        // a write sized for one bionic quietly zeroes the canary of another and
        // the guest dies of "stack corruption detected" before it draws a frame.
        const std::uint32_t signal = args[0];
        const std::uint32_t new_action = args[1];
        if (new_action != 0 && arena_.IsMapped(new_action, 4 * sizeof(std::uint32_t))) {
            // bionic's own `sigaction` struct keeps the handler first, then the
            // mask; the raw rt_sigaction struct follows the kernel and puts the
            // flags second. Both are four words, only the order differs.
            const bool rt = number == kSysRtSigaction;
            const std::uint32_t* words =
                reinterpret_cast<const std::uint32_t*>(arena_.Ptr(new_action));
            SetGuestSignalAction(static_cast<int>(signal), words[0], rt ? words[1] : words[2],
                                 rt ? words[2] : words[3]);
        }
        const std::uint32_t old_action = args[2];
        if (old_action != 0 && arena_.IsMapped(old_action, sizeof(std::uint32_t))) {
            std::memset(arena_.Ptr(old_action), 0, sizeof(std::uint32_t));
        }
        return 0;
    }

    case kSysSigprocmask: {
        // The pre-RT variant has no sigset size argument.
        const std::uint32_t old_set = args[2];
        if (old_set != 0 && arena_.IsMapped(old_set, 4)) {
            std::memset(arena_.Ptr(old_set), 0, 4);
        }
        return 0;
    }

    case kSysRtSigprocmask: {
        const std::uint32_t old_set = args[2];
        const std::uint32_t size = args[3];
        if (old_set != 0 && size != 0 && arena_.IsMapped(old_set, size)) {
            std::memset(arena_.Ptr(old_set), 0, size);
        }
        return 0;
    }

    case kSysRtSigreturn:
        return 0;

    case kSysSigaltstack: {
        const std::uint32_t old_stack = args[1];
        if (old_stack != 0 && arena_.IsMapped(old_stack, 12)) {
            std::memset(arena_.Ptr(old_stack), 0, 12);
        }
        return 0;
    }

    case kSysSocket: {
        const int fd = ::socket(static_cast<int>(args[0]), static_cast<int>(args[1]),
                                static_cast<int>(args[2]));
        if (fd >= 0) {
            TrackGuestFd(fd);
        }
        return fd < 0 ? -errno : fd;
    }

    case kSysConnect: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t address = args[1];
        const std::uint32_t length = std::min<std::uint32_t>(args[2], sizeof(sockaddr_un));
        if (length < 2 || !arena_.IsMapped(address, length)) {
            return -EFAULT;
        }
        sockaddr_un guest{};
        std::memcpy(&guest, arena_.Ptr(address), length);
        // bionic's async-safe logger (the linker, libc fatal paths) connects a
        // datagram socket to logd and writev()s a binary record. The container
        // accepts that connection and decodes the records itself, because a
        // guest that cannot log is a guest that fails silently.
        if (guest.sun_family == AF_UNIX) {
            const bool filesystem_name =
                std::strncmp(guest.sun_path, "/dev/socket/logdw", sizeof(guest.sun_path)) == 0;
            const bool abstract_name =
                guest.sun_path[0] == '\0' &&
                std::strncmp(guest.sun_path + 1, "logdw", sizeof(guest.sun_path) - 1) == 0;
            if (filesystem_name || abstract_name) {
                log_sockets_.insert(fd);
                return 0;
            }
        }
        return ::connect(fd, reinterpret_cast<const sockaddr*>(&guest), length) == 0 ? 0 : -errno;
    }

    // The rest of the descriptor's socket life. A game's multi-player and LAN
    // code paths open sockets even in single player -- the server locator pings
    // the local network while the menu loads -- and a layer that answers ENOSYS
    // leaves the engine holding a half-built object it then dereferences. Host
    // descriptors and guest descriptors are the same numbers, and a socket
    // address is the same bytes on both sides, so these pass through.
    case kSysBind: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t address = args[1];
        const std::uint32_t length = std::min<std::uint32_t>(args[2], sizeof(sockaddr_storage));
        if (length < 2 || !arena_.IsMapped(address, length)) {
            return -EFAULT;
        }
        sockaddr_storage guest{};
        std::memcpy(&guest, arena_.Ptr(address), length);
        return ::bind(fd, reinterpret_cast<const sockaddr*>(&guest), length) == 0 ? 0 : -errno;
    }

    case kSysListen: {
        return ::listen(static_cast<int>(args[0]), static_cast<int>(args[1])) == 0 ? 0 : -errno;
    }

    case kSysAccept:
    case kSysAccept4: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t address = args[1];
        const std::uint32_t length_address = args[2];
        std::uint32_t length = 0;
        if (length_address != 0) {
            if (!arena_.IsMapped(length_address, sizeof(std::uint32_t))) {
                return -EFAULT;
            }
            std::memcpy(&length, arena_.Ptr(length_address), sizeof(length));
            if (length > sizeof(sockaddr_storage) ||
                (length != 0 && address != 0 && !arena_.IsMapped(address, length))) {
                return -EFAULT;
            }
        }
        sockaddr_storage peer{};
        socklen_t peer_length = static_cast<socklen_t>(length);
        const int accepted =
            number == kSysAccept4
                ? ::accept4(fd, length != 0 ? reinterpret_cast<sockaddr*>(&peer) : nullptr,
                            length != 0 ? &peer_length : nullptr, static_cast<int>(args[3]))
                : ::accept(fd, length != 0 ? reinterpret_cast<sockaddr*>(&peer) : nullptr,
                           length != 0 ? &peer_length : nullptr);
        if (accepted < 0) {
            return -errno;
        }
        TrackGuestFd(accepted);
        if (length != 0) {
            std::memcpy(arena_.Ptr(address), &peer, peer_length);
            std::memcpy(arena_.Ptr(length_address), &peer_length, sizeof(peer_length));
        }
        return accepted;
    }

    case kSysGetsockname:
    case kSysGetpeername: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t address = args[1];
        const std::uint32_t length_address = args[2];
        std::uint32_t length = 0;
        if (!arena_.IsMapped(length_address, sizeof(std::uint32_t))) {
            return -EFAULT;
        }
        std::memcpy(&length, arena_.Ptr(length_address), sizeof(length));
        if (length > sizeof(sockaddr_storage) ||
            (length != 0 && !arena_.IsMapped(address, length))) {
            return -EFAULT;
        }
        sockaddr_storage local{};
        socklen_t local_length = static_cast<socklen_t>(length);
        const int result =
            number == kSysGetsockname
                ? ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length)
                : ::getpeername(fd, reinterpret_cast<sockaddr*>(&local), &local_length);
        if (result != 0) {
            return -errno;
        }
        std::memcpy(arena_.Ptr(address), &local, local_length);
        std::memcpy(arena_.Ptr(length_address), &local_length, sizeof(local_length));
        return 0;
    }

    case kSysSetsockopt: {
        const int fd = static_cast<int>(args[0]);
        const int level = static_cast<int>(args[1]);
        const int option = static_cast<int>(args[2]);
        const std::uint32_t value = args[3];
        const std::uint32_t length = std::min<std::uint32_t>(args[4], 256);
        if (length != 0 && !arena_.IsMapped(value, length)) {
            return -EFAULT;
        }
        return ::setsockopt(fd, level, option, arena_.Ptr(value), length) == 0 ? 0 : -errno;
    }

    case kSysGetsockopt: {
        const int fd = static_cast<int>(args[0]);
        const int level = static_cast<int>(args[1]);
        const int option = static_cast<int>(args[2]);
        const std::uint32_t value = args[3];
        const std::uint32_t length_address = args[4];
        std::uint32_t length = 0;
        if (!arena_.IsMapped(length_address, sizeof(std::uint32_t))) {
            return -EFAULT;
        }
        std::memcpy(&length, arena_.Ptr(length_address), sizeof(length));
        if (length > 256 || (length != 0 && !arena_.IsMapped(value, length))) {
            return -EFAULT;
        }
        socklen_t local_length = static_cast<socklen_t>(length);
        if (::getsockopt(fd, level, option, arena_.Ptr(value), &local_length) != 0) {
            return -errno;
        }
        std::memcpy(arena_.Ptr(length_address), &local_length, sizeof(local_length));
        return 0;
    }

    case kSysSend:
    case kSysRecv: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        const std::uint32_t length = std::min<std::uint32_t>(args[2], INT32_MAX);
        const int flags = static_cast<int>(args[3]);
        if (length != 0 && !arena_.IsMapped(buffer, length)) {
            return -EFAULT;
        }
        const ssize_t result = number == kSysSend
                                   ? ::send(fd, arena_.Ptr(buffer), length, flags)
                                   : ::recv(fd, arena_.Ptr(buffer), length, flags);
        return result < 0 ? -errno : static_cast<std::int32_t>(result);
    }

    case kSysSendto:
    case kSysRecvfrom: {
        const bool sending = number == kSysSendto;
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        const std::uint32_t length = std::min<std::uint32_t>(args[2], INT32_MAX);
        const int flags = static_cast<int>(args[3]);
        const std::uint32_t address = args[4];
        if (length != 0 && !arena_.IsMapped(buffer, length)) {
            return -EFAULT;
        }
        sockaddr_storage peer{};
        socklen_t peer_length = 0;
        if (sending) {
            peer_length = static_cast<socklen_t>(
                std::min<std::uint32_t>(args[5], sizeof(sockaddr_storage)));
            // A netlink request goes out unaddressed: the family is already in
            // the header, and the kernel routes it to itself. Treating the empty
            // address as a fault left getifaddrs() with no answer at all.
            if (address == 0 && peer_length == 0) {
                const ssize_t result =
                    ::sendto(fd, arena_.Ptr(buffer), length, flags, nullptr, 0);
                return result < 0 ? -errno : static_cast<std::int32_t>(result);
            }
            if (peer_length < 2 || !arena_.IsMapped(address, peer_length)) {
                return -EFAULT;
            }
            std::memcpy(&peer, arena_.Ptr(address), peer_length);
            const ssize_t result = ::sendto(fd, arena_.Ptr(buffer), length, flags,
                                            reinterpret_cast<const sockaddr*>(&peer), peer_length);
            return result < 0 ? -errno : static_cast<std::int32_t>(result);
        }
        const std::uint32_t length_address = args[5];
        if (length_address != 0) {
            if (!arena_.IsMapped(length_address, sizeof(std::uint32_t))) {
                return -EFAULT;
            }
            std::memcpy(&peer_length, arena_.Ptr(length_address), sizeof(peer_length));
            if (peer_length > sizeof(sockaddr_storage) ||
                (peer_length != 0 && address != 0 && !arena_.IsMapped(address, peer_length))) {
                return -EFAULT;
            }
        }
        const ssize_t result =
            ::recvfrom(fd, arena_.Ptr(buffer), length, flags,
                       length_address != 0 ? reinterpret_cast<sockaddr*>(&peer) : nullptr,
                       length_address != 0 ? &peer_length : nullptr);
        if (result < 0) {
            return -errno;
        }
        if (length_address != 0) {
            std::memcpy(arena_.Ptr(address), &peer, peer_length);
            std::memcpy(arena_.Ptr(length_address), &peer_length, sizeof(peer_length));
        }
        return static_cast<std::int32_t>(result);
    }

    case kSysSendmsg:
    case kSysRecvmsg: {
        const bool sending = number == kSysSendmsg;
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t message_address = args[1];
        const int flags = static_cast<int>(args[2]);
        if (!arena_.IsMapped(message_address, sizeof(GuestMsghdr))) {
            return -EFAULT;
        }
        GuestMsghdr guest{};
        std::memcpy(&guest, arena_.Ptr(message_address), sizeof(guest));

        std::vector<iovec> iov;
        if (!TranslateGuestIoVecs(arena_, guest.iov, guest.iov_length, iov)) {
            return -EFAULT;
        }
        // On a send the address is the peer; on a receive it is the buffer the
        // peer is copied into. The guest's copy is read only when sending, and
        // only its capacity matters when receiving.
        sockaddr_storage address{};
        socklen_t address_length = 0;
        if (guest.name != 0 && guest.name_length != 0) {
            address_length =
                static_cast<socklen_t>(std::min<std::uint32_t>(guest.name_length, sizeof(address)));
            if (!arena_.IsMapped(guest.name, address_length)) {
                return -EFAULT;
            }
            if (sending) {
                std::memcpy(&address, arena_.Ptr(guest.name), address_length);
            }
        }
        std::vector<char> control;
        if (!BuildHostControl(arena_, guest.control, guest.control_length, control)) {
            return -EFAULT;
        }

        msghdr host{};
        host.msg_name = guest.name != 0 ? &address : nullptr;
        host.msg_namelen = address_length;
        host.msg_iov = iov.empty() ? nullptr : iov.data();
        host.msg_iovlen = iov.size();
        host.msg_control = control.empty() ? nullptr : control.data();
        host.msg_controllen = control.size();
        host.msg_flags = 0;

        const ssize_t result = sending ? ::sendmsg(fd, &host, flags) : ::recvmsg(fd, &host, flags);
        if (result < 0) {
            return -errno;
        }
        if (sending) {
            return static_cast<std::int32_t>(result);
        }

        std::uint32_t guest_flags = static_cast<std::uint32_t>(host.msg_flags);
        if (guest.name != 0) {
            const socklen_t copied = static_cast<socklen_t>(
                std::min<std::size_t>(host.msg_namelen, guest.name_length));
            if (copied != 0) {
                std::memcpy(arena_.Ptr(guest.name), &address, copied);
            }
            std::memcpy(arena_.Ptr(message_address + offsetof(GuestMsghdr, name_length)), &copied,
                        sizeof(copied));
        }
        if (guest.control != 0 && guest.control_length != 0) {
            bool truncated = false;
            const std::uint32_t written =
                BuildGuestControl(arena_, host.msg_control, host.msg_controllen, guest.control,
                                  guest.control_length, &truncated);
            std::memcpy(arena_.Ptr(message_address + offsetof(GuestMsghdr, control_length)),
                        &written, sizeof(written));
            if (truncated) {
                guest_flags |= MSG_CTRUNC;
            }
        }
        std::memcpy(arena_.Ptr(message_address + offsetof(GuestMsghdr, flags)), &guest_flags,
                    sizeof(guest_flags));
        return static_cast<std::int32_t>(result);
    }

    case kSysShutdown: {
        return ::shutdown(static_cast<int>(args[0]), static_cast<int>(args[1])) == 0 ? 0 : -errno;
    }

    case kSysSocketpair: {
        int pair[2] = {-1, -1};
        if (::socketpair(static_cast<int>(args[0]), static_cast<int>(args[1]),
                         static_cast<int>(args[2]), pair) != 0) {
            return -errno;
        }
        const std::uint32_t destination = args[3];
        if (!arena_.IsMapped(destination, sizeof(pair))) {
            ::close(pair[0]);
            ::close(pair[1]);
            return -EFAULT;
        }
        std::memcpy(arena_.Ptr(destination), pair, sizeof(pair));
        TrackGuestFd(pair[0]);
        TrackGuestFd(pair[1]);
        return 0;
    }

    case kSysTgkill:
    case kSysTkill:
    case kSysKill: {
        // Thread-directed delivery. The guest's tids are host tids -- clone()
        // returns ::gettid() -- so a signal names a host thread directly. kill()
        // addresses a process, and a container has exactly one, so it lands on
        // the caller.
        std::uint32_t target = static_cast<std::uint32_t>(::gettid());
        int signal = 0;
        if (number == kSysTgkill) {
            target = args[1];
            signal = static_cast<int>(args[2]);
        } else if (number == kSysTkill) {
            target = args[0];
            signal = static_cast<int>(args[1]);
        } else {
            signal = static_cast<int>(args[1]);
        }
        if (signal == 0) {
            // kill(pid, 0) is an existence probe.
            return 0;
        }
        if (signal < 0 || signal >= 65) {
            return -EINVAL;
        }
        if (HasGuestSignalHandler(signal)) {
            // A conservative collector stops threads with exactly this call, so
            // a dropped signal is not a missing feature: it is a hang.
            return SendGuestSignal(target, signal) ? 0 : -ESRCH;
        }
        // No handler installed: the default action is not emulated, and the
        // signal is dropped as it always was. A fatal one still leaves the only
        // record of why the thread gave up.
        if (signal == SIGABRT || signal == SIGSEGV || signal == SIGILL || signal == SIGBUS ||
            signal == SIGFPE) {
            A32Machine& caller = CallingMachine(machine_);
            ARMBAT_LOG_WARN("Armbat",
                                "signal: guest raised %d at %s (tid=%u, not delivered)", signal,
                                armbat::DescribeGuestAddress(caller.Reg(15)).c_str(),
                                static_cast<unsigned>(::gettid()));
            // The frames of an aborting thread are the only record of where it
            // gave up: nothing else is logged between the fault and the exit.
            const std::uint32_t sp = caller.Reg(13);
            std::string frames;
            for (std::uint32_t address : caller.GuestReturnAddresses(sp, 0x800)) {
                frames += " " + armbat::DescribeGuestAddress(address);
            }
            ARMBAT_LOG_WARN("Armbat", "signal: frames:%s", frames.c_str());
            // The frame that failed its check is just above the abort frames, so
            // dump enough of it to see what was written over the guard.
            for (int row = 0; row < 24; ++row) {
                std::uint32_t words[8];
                bool ok = true;
                for (int word = 0; word < 8 && ok; ++word) {
                    ok = caller.ReadGuestWord(sp + static_cast<std::uint32_t>(row * 32 + word * 4),
                                              &words[word]);
                }
                if (!ok) {
                    break;
                }
                ARMBAT_LOG_WARN("Armbat",
                                    "signal stack +%03x: %08x %08x %08x %08x %08x %08x %08x %08x",
                                    row * 32, words[0], words[1], words[2], words[3], words[4],
                                    words[5], words[6], words[7]);
            }
        }
        return 0;
    }

    case kSysEpollCreate:
    case kSysEpollCreate1: {
        const int fd = number == kSysEpollCreate1 ? ::epoll_create1(static_cast<int>(args[0]))
                                                  : ::epoll_create(static_cast<int>(args[0]));
        if (fd >= 0) {
            TrackGuestFd(fd);
        }
        return fd < 0 ? -errno : fd;
    }

    // 32-bit bionic's struct epoll_event is 16 bytes (events, then the 8-byte
    // data at offset 8); the 64-bit host packs it into 12. Both directions need
    // an explicit conversion or every event lands in the wrong field.
    case kSysEpollCtl: {
        const std::uint32_t address = args[3];
        if (!arena_.IsMapped(address, sizeof(GuestEpollEvent))) {
            return -EFAULT;
        }
        GuestEpollEvent guest{};
        std::memcpy(&guest, arena_.Ptr(address), sizeof(guest));
        epoll_event host{};
        host.events = guest.events;
        host.data.u64 = guest.data;
        return ::epoll_ctl(static_cast<int>(args[0]), static_cast<int>(args[1]),
                           static_cast<int>(args[2]), &host) == 0
                   ? 0
                   : -errno;
    }

    case kSysEpollWait: {
        const int max_events = static_cast<int>(args[2]);
        const std::uint32_t events = args[1];
        if (max_events <= 0 || max_events > 4096 ||
            !arena_.IsMapped(events, sizeof(GuestEpollEvent) * max_events)) {
            return -EINVAL;
        }
        std::vector<epoll_event> host_events(static_cast<std::size_t>(max_events));
        const int result = ::epoll_wait(static_cast<int>(args[0]), host_events.data(), max_events,
                                        static_cast<int>(args[3]));
        if (result < 0) {
            return -errno;
        }
        for (int index = 0; index < result; ++index) {
            GuestEpollEvent guest{};
            guest.events = host_events[index].events;
            guest.data = host_events[index].data.u64;
            std::memcpy(arena_.Ptr(events + static_cast<std::uint32_t>(index) *
                                                sizeof(GuestEpollEvent)),
                        &guest, sizeof(guest));
        }
        return result;
    }

    case kSysEventfd2: {
        const int fd = ::eventfd(static_cast<unsigned int>(args[0]), static_cast<int>(args[1]));
        if (fd >= 0) {
            TrackGuestFd(fd);
        }
        return fd < 0 ? -errno : fd;
    }

    case kSysDup: {
        const int result = ::dup(static_cast<int>(args[0]));
        if (result >= 0) {
            TrackGuestFd(result);
        }
        return result < 0 ? -errno : result;
    }

    // Redirection into a descriptor the guest does not own would take a host
    // object's place: the platform answers EBADF for a bad target, and so does
    // the container, except for the standard streams every program may redirect.
    case kSysDup2:
    case kSysDup3: {
        const int target = static_cast<int>(args[1]);
        if (target < 0) {
            return -EBADF;
        }
        if (target > 2) {
            if (!IsGuestFd(target)) {
                return -EBADF;
            }
            ForgetGuestFd(target);
        }
        const int result = number == kSysDup2
                               ? ::dup2(static_cast<int>(args[0]), target)
                               : ::dup3(static_cast<int>(args[0]), target,
                                        static_cast<int>(args[2]));
        if (result < 0) {
            return -errno;
        }
        TrackGuestFd(result);
        return 0;
    }

    case kSysPipe2: {
        int fds[2] = {-1, -1};
        if (::pipe2(fds, static_cast<int>(args[1])) != 0) {
            return -errno;
        }
        if (!arena_.IsMapped(args[0], sizeof(fds))) {
            ::close(fds[0]);
            ::close(fds[1]);
            return -EFAULT;
        }
        TrackGuestFd(fds[0]);
        TrackGuestFd(fds[1]);
        std::memcpy(arena_.Ptr(args[0]), fds, sizeof(fds));
        return 0;
    }

    // File locks arrive as a pointer to the guest's `struct flock`, and the guest
    // layout is not the host's: 32-bit offsets (or a 64-bit struct at the 32-bit
    // ABI's alignment) against a 64-bit `off_t`, and the address means nothing in
    // the host address space. Handing that pointer to the host fcntl() locks
    // nothing and answers EFAULT, which is fatal for leveldb: DB::Open() locks
    // LOCK before it reads a single key, so a refused lock leaves every world's
    // database closed and the load waiting on it forever.
    case kSysFcntl:
    case kSysFcntl64: {
        const int fd = static_cast<int>(args[0]);
        const int command = static_cast<int>(args[1]);
        const std::uint32_t pointer = args[2];
        // The guest kernel's numbers: F_GETLK/F_SETLK/F_SETLKW are 5/6/7 with
        // 32-bit offsets, and the arm `*64` variants are 12/13/14 with 64-bit
        // offsets. bionic on a 32-bit ABI upgrades leveldb's F_SETLK to F_SETLK64,
        // so both shapes have to be understood.
        const bool wide = command >= 12 && command <= 14;
        const bool locking = (command >= 5 && command <= 7) || wide;
        if (locking) {
            const std::uint32_t size = wide ? 32 : 16;
            if (pointer == 0 || !arena_.IsMapped(pointer, size)) {
                return -EFAULT;
            }
            auto* bytes = reinterpret_cast<std::uint8_t*>(arena_.Ptr(pointer));
            struct flock lock {};
            std::int16_t type = 0;
            std::int16_t whence = 0;
            std::memcpy(&type, bytes + 0, sizeof(type));
            std::memcpy(&whence, bytes + 2, sizeof(whence));
            lock.l_type = type;
            lock.l_whence = whence;
            std::int32_t pid = 0;
            if (wide) {
                std::int64_t start = 0;
                std::int64_t length = 0;
                std::memcpy(&start, bytes + 8, sizeof(start));
                std::memcpy(&length, bytes + 16, sizeof(length));
                std::memcpy(&pid, bytes + 24, sizeof(pid));
                lock.l_start = start;
                lock.l_len = length;
            } else {
                std::int32_t start = 0;
                std::int32_t length = 0;
                std::memcpy(&start, bytes + 4, sizeof(start));
                std::memcpy(&length, bytes + 8, sizeof(length));
                std::memcpy(&pid, bytes + 12, sizeof(pid));
                lock.l_start = start;
                lock.l_len = length;
            }
            lock.l_pid = pid;
            const int request = command == 5 || command == 12
                                    ? F_GETLK
                                    : (command == 7 || command == 14 ? F_SETLKW : F_SETLK);
            if (::fcntl(fd, request, &lock) < 0) {
                return -errno;
            }
            if (request == F_GETLK) {
                // The kernel answers with the lock that blocks the request, or
                // F_UNLCK; the guest reads it back through the same structure.
                const std::int16_t answer_type = lock.l_type;
                const std::int16_t answer_whence = lock.l_whence;
                std::memcpy(bytes + 0, &answer_type, sizeof(answer_type));
                std::memcpy(bytes + 2, &answer_whence, sizeof(answer_whence));
                if (wide) {
                    const std::int64_t start = lock.l_start;
                    const std::int64_t length = lock.l_len;
                    const std::int32_t owner = lock.l_pid;
                    std::memcpy(bytes + 8, &start, sizeof(start));
                    std::memcpy(bytes + 16, &length, sizeof(length));
                    std::memcpy(bytes + 24, &owner, sizeof(owner));
                } else {
                    const std::int32_t start = static_cast<std::int32_t>(lock.l_start);
                    const std::int32_t length = static_cast<std::int32_t>(lock.l_len);
                    const std::int32_t owner = lock.l_pid;
                    std::memcpy(bytes + 4, &start, sizeof(start));
                    std::memcpy(bytes + 8, &length, sizeof(length));
                    std::memcpy(bytes + 12, &owner, sizeof(owner));
                }
            }
            return 0;
        }
        const int result = ::fcntl(fd, command, static_cast<int>(pointer));
        if (result < 0) {
            return -errno;
        }
        // A duplicated descriptor is a new guest descriptor, and one the guest
        // never learns about cannot be closed.
        if (command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
            TrackGuestFd(result);
        }
        return result;
    }

    case kSysIoctl: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t request = args[1];
        const std::uint32_t argument = args[2];
        // What a socket asks its driver: how many bytes are waiting, whether it
        // is non-blocking, and the addresses of the interfaces it can broadcast
        // on. A LAN game asks all three before it opens its world to other
        // players, and the guest's struct ifreq is 32 bytes where the host's is
        // 40 -- the name comes first and the union starts at 16 on both.
        constexpr std::uint32_t kFionread = 0x541B;
        constexpr std::uint32_t kFionbio = 0x5421;
        constexpr std::uint32_t kSiocgifaddr = 0x8915;
        constexpr std::uint32_t kSiocgifbrdaddr = 0x8919;
        constexpr std::uint32_t kSiocgifnetmask = 0x891B;
        constexpr std::uint32_t kSiocgifmtu = 0x8921;
        constexpr std::uint32_t kSiocgifhwaddr = 0x8927;
        constexpr std::uint32_t kSiocgifindex = 0x8933;
        constexpr std::uint32_t kSiocgiflags = 0x8913;
        constexpr std::uint32_t kGuestIfreqSize = 32;
        switch (request) {
        case kFionread:
        case kFionbio: {
            if (!arena_.IsMapped(argument, sizeof(int))) {
                return -EFAULT;
            }
            int value = 0;
            std::memcpy(&value, arena_.Ptr(argument), sizeof(value));
            if (::ioctl(fd, static_cast<unsigned long>(request), &value) != 0) {
                return -errno;
            }
            std::memcpy(arena_.Ptr(argument), &value, sizeof(value));
            return 0;
        }
        case kSiocgifaddr:
        case kSiocgifbrdaddr:
        case kSiocgifnetmask:
        case kSiocgifmtu:
        case kSiocgifhwaddr:
        case kSiocgifindex:
        case kSiocgiflags: {
            if (!arena_.IsMapped(argument, kGuestIfreqSize)) {
                return -EFAULT;
            }
            ifreq host{};
            std::memcpy(host.ifr_name, arena_.Ptr(argument), IFNAMSIZ);
            host.ifr_name[IFNAMSIZ - 1] = '\0';
            if (::ioctl(fd, static_cast<unsigned long>(request), &host) != 0) {
                return -errno;
            }
            std::memcpy(arena_.Ptr(argument + IFNAMSIZ),
                        reinterpret_cast<const char*>(&host) + IFNAMSIZ, 16);
            return 0;
        }
        default:
            return -ENOTTY;
        }
    }

    case kSysFtruncate: {
        const std::uint64_t length = Split64(args[1], args[2]);
        return ::ftruncate(static_cast<int>(args[0]), static_cast<off_t>(length)) == 0 ? 0
                                                                                       : -errno;
    }

    case kSysGetrandom: {
        const std::uint32_t buffer = args[0];
        const std::uint32_t length = std::min<std::uint32_t>(args[1], INT32_MAX);
        if (length != 0 && !arena_.IsMapped(buffer, length)) {
            return -EFAULT;
        }
        const ssize_t result = ::getrandom(arena_.Ptr(buffer), length, args[2] & 0x7u);
        return result < 0 ? -errno : ClampSize(result);
    }

    case kSysMadvise:
        // MADV_DONTNEED is not advice here: an allocator that is told the pages
        // now read as zero stops clearing them itself, and jemalloc's calloc()
        // then hands out whatever the pages held. The purge has to actually
        // purge; every other advice is only advice.
        return memory_.Advise(args[0], args[1], static_cast<int>(args[2]));

    case kSysClockNanosleep: {
        const int clock = static_cast<int>(args[0]);
        const int flags = static_cast<int>(args[1]);
        const std::uint32_t request = args[2];
        const std::uint32_t remain = args[3];
        if (!arena_.IsMapped(request, sizeof(GuestTimespec))) {
            return -EFAULT;
        }
        GuestTimespec guest{};
        std::memcpy(&guest, arena_.Ptr(request), sizeof(guest));
        const struct timespec host = ToHostTimespec(guest);
        struct timespec left{};
        const int result = ::clock_nanosleep(static_cast<clockid_t>(clock), flags, &host, &left);
        if (result != 0) {
            // clock_nanosleep answers with the error itself, not -1/errno.
            return -result;
        }
        if (remain != 0 && arena_.IsMapped(remain, sizeof(GuestTimespec))) {
            const GuestTimespec out = FromHostTimespec(left);
            std::memcpy(arena_.Ptr(remain), &out, sizeof(out));
        }
        return 0;
    }

    case kSysPoll: {
        const std::uint32_t fds = args[0];
        const std::uint32_t count = args[1];
        const int timeout = static_cast<int>(args[2]);
        if (count != 0 && !arena_.IsMapped(fds, count * sizeof(struct pollfd))) {
            return -EFAULT;
        }
        const int result = ::poll(reinterpret_cast<struct pollfd*>(arena_.Ptr(fds)), count, timeout);
        return result < 0 ? -errno : result;
    }

    case kSysPpoll: {
        const std::uint32_t fds = args[0];
        const std::uint32_t count = args[1];
        const std::uint32_t timeout = args[2];
        if (count != 0 && !arena_.IsMapped(fds, count * sizeof(struct pollfd))) {
            return -EFAULT;
        }
        int milliseconds = -1;
        if (timeout != 0) {
            if (!arena_.IsMapped(timeout, sizeof(GuestTimespec))) {
                return -EFAULT;
            }
            GuestTimespec guest{};
            std::memcpy(&guest, arena_.Ptr(timeout), sizeof(guest));
            const std::int64_t total = static_cast<std::int64_t>(guest.seconds) * 1000 +
                                       (static_cast<std::int64_t>(guest.nanoseconds) + 999999) / 1000000;
            milliseconds = total > INT32_MAX ? INT32_MAX : static_cast<int>(total);
        }
        // The mask is not applied: no asynchronous signal is ever delivered
        // into guest code, so waiting with an empty one is the same wait.
        const int result = ::poll(reinterpret_cast<struct pollfd*>(arena_.Ptr(fds)), count, milliseconds);
        return result < 0 ? -errno : result;
    }

    case kSysSelect: {
        const int count = static_cast<int>(args[0]);
        const std::uint32_t sets[3] = {args[1], args[2], args[3]};
        const std::uint32_t timeout = args[4];
        // bionic's fd_set is 128 bytes wide, and so is the host's, so the set
        // is passed through untouched.
        constexpr std::uint32_t kSetBytes = 128;
        for (const std::uint32_t set : sets) {
            if (set != 0 && !arena_.IsMapped(set, kSetBytes)) {
                return -EFAULT;
            }
        }
        struct timeval host_timeout{};
        struct timeval* host_timeout_pointer = nullptr;
        if (timeout != 0) {
            if (!arena_.IsMapped(timeout, sizeof(GuestTimeval))) {
                return -EFAULT;
            }
            GuestTimeval guest{};
            std::memcpy(&guest, arena_.Ptr(timeout), sizeof(guest));
            host_timeout.tv_sec = guest.seconds;
            host_timeout.tv_usec = guest.microseconds;
            host_timeout_pointer = &host_timeout;
        }
        const int result = ::select(
            count, sets[0] != 0 ? reinterpret_cast<fd_set*>(arena_.Ptr(sets[0])) : nullptr,
            sets[1] != 0 ? reinterpret_cast<fd_set*>(arena_.Ptr(sets[1])) : nullptr,
            sets[2] != 0 ? reinterpret_cast<fd_set*>(arena_.Ptr(sets[2])) : nullptr,
            host_timeout_pointer);
        return result < 0 ? -errno : result;
    }

    case kSysPselect6: {
        const int count = static_cast<int>(args[0]);
        const std::uint32_t sets[3] = {args[1], args[2], args[3]};
        const std::uint32_t timeout = args[4];
        constexpr std::uint32_t kSetBytes = 128;
        for (const std::uint32_t set : sets) {
            if (set != 0 && !arena_.IsMapped(set, kSetBytes)) {
                return -EFAULT;
            }
        }
        struct timespec host_timeout{};
        struct timespec* host_timeout_pointer = nullptr;
        if (timeout != 0) {
            if (!arena_.IsMapped(timeout, sizeof(GuestTimespec))) {
                return -EFAULT;
            }
            GuestTimespec guest{};
            std::memcpy(&guest, arena_.Ptr(timeout), sizeof(guest));
            host_timeout = ToHostTimespec(guest);
            host_timeout_pointer = &host_timeout;
        }
        const int result = ::pselect(
            count, sets[0] != 0 ? reinterpret_cast<fd_set*>(arena_.Ptr(sets[0])) : nullptr,
            sets[1] != 0 ? reinterpret_cast<fd_set*>(arena_.Ptr(sets[1])) : nullptr,
            sets[2] != 0 ? reinterpret_cast<fd_set*>(arena_.Ptr(sets[2])) : nullptr,
            host_timeout_pointer, nullptr);
        return result < 0 ? -errno : result;
    }

    case kSysGetrusage: {
        const int who = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        if (!arena_.IsMapped(buffer, sizeof(GuestRusage))) {
            return -EFAULT;
        }
        struct rusage host{};
        if (::getrusage(who, &host) != 0) {
            return -errno;
        }
        auto narrow = [](long value) {
            return static_cast<std::int32_t>(std::clamp<long>(value, INT32_MIN, INT32_MAX));
        };
        GuestRusage guest{};
        guest.utime = {static_cast<std::int32_t>(host.ru_utime.tv_sec),
                       static_cast<std::int32_t>(host.ru_utime.tv_usec)};
        guest.stime = {static_cast<std::int32_t>(host.ru_stime.tv_sec),
                       static_cast<std::int32_t>(host.ru_stime.tv_usec)};
        guest.maxrss = narrow(host.ru_maxrss);
        guest.ixrss = narrow(host.ru_ixrss);
        guest.idrss = narrow(host.ru_idrss);
        guest.isrss = narrow(host.ru_isrss);
        guest.minflt = narrow(host.ru_minflt);
        guest.majflt = narrow(host.ru_majflt);
        guest.nswap = narrow(host.ru_nswap);
        guest.inblock = narrow(host.ru_inblock);
        guest.oublock = narrow(host.ru_oublock);
        guest.msgsnd = narrow(host.ru_msgsnd);
        guest.msgrcv = narrow(host.ru_msgrcv);
        guest.nsignals = narrow(host.ru_nsignals);
        guest.nvcsw = narrow(host.ru_nvcsw);
        guest.nivcsw = narrow(host.ru_nivcsw);
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysSysinfo: {
        const std::uint32_t buffer = args[0];
        if (!arena_.IsMapped(buffer, sizeof(GuestSysinfo))) {
            return -EFAULT;
        }
        struct sysinfo host{};
        if (::sysinfo(&host) != 0) {
            return -errno;
        }
        auto narrow = [](unsigned long value) {
            return static_cast<std::uint32_t>(std::min<unsigned long>(value, UINT32_MAX));
        };
        GuestSysinfo guest{};
        guest.uptime = static_cast<std::int32_t>(
            std::clamp<long>(static_cast<long>(host.uptime), INT32_MIN, INT32_MAX));
        for (int index = 0; index < 3; ++index) {
            guest.loads[index] = narrow(host.loads[index]);
        }
        guest.totalram = narrow(host.totalram);
        guest.freeram = narrow(host.freeram);
        guest.sharedram = narrow(host.sharedram);
        guest.bufferram = narrow(host.bufferram);
        guest.totalswap = narrow(host.totalswap);
        guest.freeswap = narrow(host.freeswap);
        guest.procs = static_cast<std::uint16_t>(std::min<unsigned long>(host.procs, 0xffffu));
        guest.totalhigh = narrow(host.totalhigh);
        guest.freehigh = narrow(host.freehigh);
        guest.mem_unit = host.mem_unit;
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysSchedGetscheduler:
    case kSysSchedSetscheduler:
    case kSysSchedGetPriorityMax:
    case kSysSchedGetPriorityMin:
        // The container does not let guest code change its own scheduling: it
        // shares the host's threads. Answering SCHED_OTHER with a zero-priority
        // range tells audio and render threads the truth instead of a number
        // they would try to raise to.
        return 0;

    case kSysGetdents64: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        const std::uint32_t length = std::min<std::uint32_t>(args[2], 1u << 20);
        if (length != 0 && !arena_.IsMapped(buffer, length)) {
            return -EFAULT;
        }
        // linux_dirent64 is ABI-identical on 32-bit and 64-bit Linux, and the
        // descriptor is the host's own, so the call passes straight through.
        const long result = ::syscall(SYS_getdents64, fd, arena_.Ptr(buffer), length);
        return result < 0 ? -errno : ClampSize(result);
    }

    case kSysStatfs64:
    case kSysFstatfs64: {
        const bool from_fd = number == kSysFstatfs64;
        const std::uint32_t buffer = args[1];
        if (!arena_.IsMapped(buffer, sizeof(GuestStatfs))) {
            return -EFAULT;
        }
        struct statvfs host{};
        if (from_fd) {
            if (::fstatvfs(static_cast<int>(args[0]), &host) != 0) {
                return -errno;
            }
        } else {
            std::string path;
            if (!ReadString(args[0], path)) {
                return -EFAULT;
            }
            if (::statvfs(MapPath(path).c_str(), &host) != 0) {
                return -errno;
            }
        }
        GuestStatfs guest{};
        // TMPFS_MAGIC: what the container actually presents to the guest.
        guest.type = 0x01021994u;
        guest.block_size = static_cast<std::uint32_t>(host.f_bsize);
        guest.blocks = host.f_blocks;
        guest.blocks_free = host.f_bfree;
        guest.blocks_available = host.f_bavail;
        guest.files = host.f_files;
        guest.files_free = host.f_ffree;
        guest.name_length = static_cast<std::uint32_t>(host.f_namemax);
        guest.fragment_size = static_cast<std::uint32_t>(host.f_frsize);
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysTimerfdCreate: {
        const int fd = ::timerfd_create(static_cast<int>(args[0]), static_cast<int>(args[1]));
        return fd < 0 ? -errno : fd;
    }

    case kSysTimerfdSettime: {
        const int fd = static_cast<int>(args[0]);
        const int flags = static_cast<int>(args[1]);
        const std::uint32_t new_value = args[2];
        const std::uint32_t old_value = args[3];
        if (!arena_.IsMapped(new_value, sizeof(GuestItimerspec))) {
            return -EFAULT;
        }
        if (old_value != 0 && !arena_.IsMapped(old_value, sizeof(GuestItimerspec))) {
            return -EFAULT;
        }
        GuestItimerspec guest{};
        std::memcpy(&guest, arena_.Ptr(new_value), sizeof(guest));
        struct itimerspec host{};
        host.it_interval = ToHostTimespec(guest.interval);
        host.it_value = ToHostTimespec(guest.value);
        struct itimerspec previous{};
        if (::timerfd_settime(fd, flags, &host, &previous) != 0) {
            return -errno;
        }
        if (old_value != 0) {
            const GuestItimerspec out{FromHostTimespec(previous.it_interval),
                                      FromHostTimespec(previous.it_value)};
            std::memcpy(arena_.Ptr(old_value), &out, sizeof(out));
        }
        return 0;
    }

    case kSysTimerfdGettime: {
        const int fd = static_cast<int>(args[0]);
        const std::uint32_t buffer = args[1];
        if (!arena_.IsMapped(buffer, sizeof(GuestItimerspec))) {
            return -EFAULT;
        }
        struct itimerspec host{};
        if (::timerfd_gettime(fd, &host) != 0) {
            return -errno;
        }
        const GuestItimerspec guest{FromHostTimespec(host.it_interval),
                                    FromHostTimespec(host.it_value)};
        std::memcpy(arena_.Ptr(buffer), &guest, sizeof(guest));
        return 0;
    }

    case kSysClone: {
        // clone(flags, child_stack, parent_tid, tls, child_tid): what bionic's
        // pthread_create turns into. Only CLONE_VM is supported - a forked
        // process would need a second address space, which a container that
        // runs in one process cannot honestly provide.
        const std::uint32_t flags = args[0];
        const std::uint32_t child_stack = args[1];
        const std::uint32_t parent_tid = args[2];
        const std::uint32_t tls = args[3];
        const std::uint32_t child_tid = args[4];
        if (clone_logs_ < 4) {
            ++clone_logs_;
            ARMBAT_LOG_INFO("Armbat",
                                "clone: flags=0x%08x stack=0x%08x tls=0x%08x ptid=0x%08x "
                                "ctid=0x%08x",
                                flags, child_stack, tls, parent_tid, child_tid);
        }
        if ((flags & kCloneVm) == 0 || !thread_spawner_) {
            ARMBAT_LOG_WARN("Armbat",
                                "clone: refused, no CLONE_VM (flags=0x%08x) or no thread support",
                                flags);
            return -ENOSYS;
        }
        if (child_stack == 0 || !arena_.IsMapped(child_stack - 4, 4)) {
            ARMBAT_LOG_WARN("Armbat",
                                "clone: refused, child stack 0x%08x is not mapped", child_stack);
            return -EFAULT;
        }
        CloneStart start;
        for (std::size_t index = 0; index < 16; ++index) {
            start.regs[index] = CallingMachine(machine_).Reg(index);
        }
        // The supervisor callback runs after the PC has already been advanced
        // past the svc, so PC is where the child has to resume.
        start.entry = CallingMachine(machine_).Reg(15);
        start.sp = child_stack;
        start.tls = (flags & kCloneSetTls) != 0 ? tls : CallingMachine(machine_).TlsBase();
        start.regs[0] = 0;  // the child of clone() sees a return value of zero
        start.regs[13] = child_stack;
        start.clear_tid = (flags & kCloneChildCleartid) != 0 ? child_tid : 0;
        start.thumb = CallingMachine(machine_).Thumb();
        const std::uint32_t tid = thread_spawner_(start);
        if (tid == 0) {
            return -EAGAIN;
        }
        if ((flags & kCloneParentSettid) != 0 && parent_tid != 0 &&
            arena_.IsMapped(parent_tid, sizeof(tid))) {
            std::memcpy(arena_.Ptr(parent_tid), &tid, sizeof(tid));
        }
        if ((flags & kCloneChildSettid) != 0 && child_tid != 0 &&
            arena_.IsMapped(child_tid, sizeof(tid))) {
            std::memcpy(arena_.Ptr(child_tid), &tid, sizeof(tid));
        }
        return static_cast<std::int32_t>(tid);
    }

    case kSysExit:
    case kSysExitGroup: {
        // The last thing a guest does is the only record of why it stopped, and
        // the frames above the exit call name the code that decided to.
        A32Machine& caller = CallingMachine(machine_);
        std::string frames;
        for (std::uint32_t address : caller.GuestReturnAddresses(caller.Reg(13), 0x1000)) {
            frames += " " + armbat::DescribeGuestAddress(address);
            if (frames.size() > 400) {
                break;
            }
        }
        ARMBAT_LOG_INFO("Armbat", "exit: tid=%u code=%u frames:%s",
                            static_cast<unsigned>(::gettid()), args[0], frames.c_str());
        t_halt_requested = true;
        return 0;
    }

    default:
        last_unimplemented_ = number;
        return -ENOSYS;
    }
}

namespace {

// The CLONE_CHILD_CLEARTID word of the thread running on this host thread. The
// Linux kernel clears it when a thread exits and wakes its futex; bionic's
// pthread_join is waiting for exactly that.
thread_local std::uint32_t t_thread_clear_tid = 0;

}  // namespace

void GuestKernel::PrepareThread(std::uint32_t clear_tid) {
    t_thread_clear_tid = clear_tid;
}

void GuestKernel::FinishThread() {
    const std::uint32_t address = t_thread_clear_tid;
    t_thread_clear_tid = 0;
    if (address == 0) {
        return;
    }
    const bool mapped = arena_.IsMapped(address, sizeof(std::uint32_t));
    // bionic's detached exit unmaps the thread stack, TCB included, before it
    // calls exit, so the word can already be gone here. Linux still wakes the
    // futex at this point (exit_clear_child_tid ignores a failing put_user), and
    // that wake is the only thing that can release a pthread_join that raced
    // with the detach. Skipping it parks the joiner for good: a frozen render
    // thread in WorkerPool::~WorkerPool -> BackgroundWorker::stop was exactly
    // that, a join parked on a worker whose stack had been torn down.
    if (mapped) {
        const std::uint32_t zero = 0;
        std::memcpy(arena_.Ptr(address), &zero, sizeof(zero));
    }
    ::syscall(SYS_futex, arena_.Ptr(address), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

void GuestKernel::DumpParkedFutexes() {
    const std::uint64_t now = SteadyNanos();
    for (std::size_t index = 0; index < kFutexSlots; ++index) {
        FutexSlot& slot = g_futex_slots[index];
        const std::uint32_t tid = slot.tid.load(std::memory_order_relaxed);
        const std::uint64_t since = slot.since.load(std::memory_order_acquire);
        if (tid == 0 || since == 0 || now <= since) {
            continue;
        }
        const std::uint64_t seconds = (now - since) / 1'000'000'000ull;
        if (seconds < 4) {
            continue;
        }
        const std::uint32_t address = slot.address.load(std::memory_order_relaxed);
        if (slot.reported.exchange(true, std::memory_order_relaxed)) {
            continue;  // already reported in this park
        }
        // The expected value says whether the waker ever ran: a word that still
        // holds it was never signalled, a word that changed means the wakeup
        // was lost between the guest and the container.
        std::uint32_t current = 0;
        if (arena_.IsMapped(address, sizeof(current))) {
            std::memcpy(&current, arena_.Ptr(address), sizeof(current));
        } else {
            current = 0xFFFFFFFFu;
        }
        std::string frames =
            armbat::GuestStackFramesForTid(tid, slot.sp.load(std::memory_order_relaxed), 0x800);
        ARMBAT_LOG_WARN("Armbat",
                            "futex: tid=%u parked %lld s on 0x%08x op=0x%x expected=0x%08x now=0x%08x%s",
                            tid, static_cast<long long>(seconds), address,
                            slot.operation.load(std::memory_order_relaxed),
                            slot.expected.load(std::memory_order_relaxed), current,
                            frames.empty() ? "" : (" frames:" + frames).c_str());
    }
}

}  // namespace armbat