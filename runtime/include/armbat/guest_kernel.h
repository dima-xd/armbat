#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "armbat/guest_memory.h"

namespace armbat {

class A32Machine;
class GuestArena;

// Address ranges the kernel hands out. The defaults fit the 4 GiB runtime arena;
// the selftest uses a smaller arena and passes its own ranges.
struct KernelConfig {
    std::uint32_t mmap_start = 0x50000000;
    std::uint32_t mmap_limit = 0x70000000;
    std::uint32_t brk_base = 0x70000000;
    std::uint32_t brk_limit = 0x78000000;
};

// The Linux ARM EABI syscall surface Armbat implements.
//
// Guest code traps with `SVC #0`, the syscall number is in r7, arguments in
// r0-r5, and the result comes back in r0 with the kernel convention: a negative
// errno on failure. File descriptors are real host descriptors, which works
// because the guest runs inside our process.
class GuestKernel {
public:
    GuestKernel(A32Machine& machine, GuestArena& arena, KernelConfig config = {});

    bool Prepare(std::string* error);

    // Translates a guest path to the host file it names. The bionic runtime uses
    // it to serve /system from the container's own sysroot; without a mapper the
    // guest sees the host filesystem unchanged.
    using PathMapper = std::function<std::string(const std::string&)>;
    void SetPathMapper(PathMapper mapper) { path_mapper_ = std::move(mapper); }

    // The path the guest should see as its own executable. The linker reads
    // /proc/self/exe to build the main-executable soinfo, and without this it
    // would find the *host* process image (app_process64 on a device), whose
    // name shows up in every loader warning.
    void SetExecutablePath(std::string path) { executable_path_ = std::move(path); }

    // What the kernel has to hand a new guest thread when the guest calls
    // clone(): the parent's register file exactly as the syscall left it, the
    // child's entry point, stack and TLS. `clear_tid` is CLONE_CHILD_CLEARTID.
    struct CloneStart {
        std::array<std::uint32_t, 16> regs{};
        std::uint32_t entry = 0;
        std::uint32_t sp = 0;
        std::uint32_t tls = 0;
        std::uint32_t clear_tid = 0;
        bool thumb = false;
    };

    // Starts the host thread for a clone(). Returns the child's tid, or 0 when
    // the thread cannot be created.
    using ThreadSpawner = std::function<std::uint32_t(const CloneStart&)>;
    void SetThreadSpawner(ThreadSpawner spawner) { thread_spawner_ = std::move(spawner); }

    // Called on a cloned thread's own host thread, before and after the guest
    // code runs. FinishThread performs CLONE_CHILD_CLEARTID: it clears the word
    // and wakes its futex, which is exactly what pthread_join waits on.
    void PrepareThread(std::uint32_t clear_tid);
    void FinishThread();

    // Reports every guest thread that has been parked in futex for a while.
    // Callers run this from a watchdog: a hang in a guest looks exactly like a
    // thread that never wakes, and the wait address says who owes the wakeup.
    void DumpParkedFutexes();

    // Called when the guest's boot program completes libc initialization
    // (r7 = 0x0f0100, r0 = address of the API table).
    void SetBootReadyHandler(std::function<void(std::uint32_t)> handler) {
        boot_ready_ = std::move(handler);
    }

    // Last syscall number that returned -ENOSYS: naming the missing call is the
    // difference between "libc is stuck" and "implement this one next".
    std::uint32_t LastUnimplemented() const { return last_unimplemented_; }

    std::uint64_t Calls() const { return calls_; }
    std::uint64_t LastNumber() const { return last_number_; }

    // The last few path translations, for diagnosing a library the loader could
    // not open: the mapped host path is the part the guest never sees.
    std::string RecentPaths() const;

private:
    bool Handle(std::uint32_t svc, A32Machine& machine);
    std::int32_t Dispatch(std::uint32_t number, const std::uint32_t* args);
    std::int32_t DispatchPrivate(std::uint32_t number, const std::uint32_t* args);
    bool ReadString(std::uint32_t address, std::string& out) const;
    std::string MapPath(const std::string& guest_path) const;
    // Turns a name relative to the guest's working directory into the absolute
    // guest name of the same file.
    static std::string ResolveGuestPath(std::string guest_path);
    bool WriteGuest(std::uint32_t address, const void* data, std::size_t size);

    // Descriptors that stand in for /dev/socket/logdw. bionic's async-safe
    // logger (used by the linker and libc fatal paths) connects there and
    // writev()s a logd record; the container decodes the record and sends the
    // text to the runtime log instead of the guest's silence.
    bool IsLogSocket(int fd) const { return log_sockets_.count(fd) != 0; }
    void ForwardLogRecord(const void* data, std::size_t size);
    void TracePath(const std::string& guest_path, const std::string& host_path, int result);

    // Descriptor ownership. A guest descriptor is the host's descriptor number,
    // and the two spaces overlap: a guest that closes a number it was never
    // handed closes a socket the host still holds -- liblog's, a Java stream, a
    // surface's -- and the container either goes silent or dies in fdsan with
    // nothing of the guest left in the record. Only the numbers the guest opened
    // are closable by it, which is also what a real kernel says: EBADF.
    void TrackGuestFd(int fd);
    void ForgetGuestFd(int fd);
    bool IsGuestFd(int fd) const;

    A32Machine& machine_;
    GuestArena& arena_;
    GuestMemory memory_;
    PathMapper path_mapper_;
    std::string executable_path_;
    std::function<void(std::uint32_t)> boot_ready_;
    std::unordered_set<int> log_sockets_;
    mutable std::mutex fds_mutex_;
    std::unordered_set<int> guest_fds_;
    std::vector<std::string> path_traces_;
    ThreadSpawner thread_spawner_;
    // Startup visibility: the first time each syscall number is used, and the
    // first few clone() attempts. A launch that stalls is almost always a
    // syscall the guest expected a different answer from.
    //
    // One flag per number instead of a set: every guest thread dispatches
    // through this single kernel, and two concurrent inserts into one
    // unordered_set tear its buckets apart. Ordinary ARM numbers and armbat's
    // private range get separate halves of the array.
    static constexpr std::size_t kTraceNumbers = 512;
    static constexpr std::size_t kTracePrivate = 512;
    std::atomic<bool> traced_syscalls_[kTraceNumbers + kTracePrivate]{};
    int clone_logs_ = 0;
    std::uint64_t calls_ = 0;
    std::uint64_t last_number_ = 0;
    std::uint32_t last_unimplemented_ = 0;
};

}  // namespace armbat
