#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "armbat/guest_arena.h"

namespace Dynarmic {
class ExclusiveMonitor;
}

namespace armbat {

// Thin owning wrapper around dynarmic's A32 JIT. Everything 32-bit stays on the
// guest side; the only host-visible state is the register file and the arena.
class A32Machine {
public:
    struct Config {
        // Direct host loads: dynarmic adds the guest address to the arena base
        // and lets the load itself fault. A guest access to a page the container
        // never committed therefore arrives at dynarmic's own SIGSEGV handler,
        // which marks that access site for the callback path and recompiles it,
        // so a miss costs one fault and every later access goes through the
        // container's checks again.
        bool fastmem = true;
        // Guest-visible architecture: ARMv7-A covers the armeabi-v7a target.
        bool enable_vfp = true;
        // Cycle counting keeps guest time, but it costs host calls per block.
        bool cycle_counting = true;
        // Block linking lets a compiled block jump straight to the next one. It
        // is what makes the JIT a JIT: without it every basic block returns to
        // the dispatcher, and a game's inner loops pay that on every iteration.
        bool block_linking = true;
        // LDREX/STREX reservations are process-wide: guest threads share one
        // monitor and each takes its own processor id. The monitor holds one
        // slot per id and indexes it without bounds checks, so it has to be
        // sized for every thread the container can ever hand out.
        static constexpr std::size_t kProcessorLimit = 256;
        std::shared_ptr<Dynarmic::ExclusiveMonitor> exclusive_monitor;
        std::size_t processor_id = 0;
    };

    explicit A32Machine(GuestArena& arena);
    A32Machine(GuestArena& arena, Config config);
    ~A32Machine();

    A32Machine(const A32Machine&) = delete;
    A32Machine& operator=(const A32Machine&) = delete;

    bool LoadCode(std::uint32_t addr, const void* data, std::size_t len);
    void SetSp(std::uint32_t sp);
    std::uint32_t Sp() const;

    // Address a returning function jumps to. The embedder maps a page with
    // `SVC #0` at that address; CallFunction sets LR to it.
    void SetReturnTrap(std::uint32_t addr);

    // Runs guest code at `entry` until it executes SVC, then returns r0.
    std::uint32_t Call(std::uint32_t entry, std::initializer_list<std::uint32_t> args);

    // Same, for real AAPCS functions that return with BX LR.
    std::uint32_t CallFunction(std::uint32_t entry, std::initializer_list<std::uint32_t> args);

    // Same, for a call whose arguments were assembled at runtime (the JNI bridge
    // builds them from a Java signature).
    std::uint32_t CallFunction(std::uint32_t entry, const std::vector<std::uint32_t>& args);

    // Calls guest code from inside an SVC handler. The outer run is suspended in
    // the JIT's host stack, so the register file is saved and restored around the
    // nested run and only r0 is left behind with the result.
    std::uint32_t CallNested(std::uint32_t entry, std::initializer_list<std::uint32_t> args);
    std::uint32_t CallNested(std::uint32_t entry, const std::vector<std::uint32_t>& args);

    // Same, but the interrupted register file is left exactly as it was: a
    // signal handler returns to the code it interrupted instead of producing a
    // value the caller reads.
    std::uint32_t CallNestedPreservingRegisters(std::uint32_t entry,
                                                const std::vector<std::uint32_t>& args);

    // Queues a guest signal on this machine's thread. Safe to call from another
    // thread: the pending set is atomic and the JIT is halted through its own
    // halt flag, which RunJit sees at the next slice boundary.
    void RequestSignal(std::uint32_t signal);

    // Clears and returns the lowest queued signal, or 0 when none is pending.
    std::uint32_t TakePendingSignal();

    // The function RunJit and the SVC entry call when a signal is pending. It
    // runs on the owning thread at a point where the register file is complete.
    using SafePointHandler = std::function<void(A32Machine&)>;
    void SetSafePointHandler(SafePointHandler handler);
    void CopySafePointHandlerFrom(const A32Machine& other);
    void RunSafePoint();

    // Runs a clone()d guest thread: the register file is the parent's exactly as
    // the kernel left it, while the stack, the TLS base and the entry point are
    // the new thread's own. Called once per thread, on that thread's host
    // thread; returns when the thread executes its exit syscall.
    std::uint32_t EnterClonedThread(const std::array<std::uint32_t, 16>& regs,
                                    std::uint32_t entry, std::uint32_t sp, std::uint32_t tls,
                                    bool thumb);

    // Instruction set of the code that is running now. A cloned thread resumes
    // in the same one, so clone() has to tell the caller.
    bool Thumb() const;

    // True while guest code is running on this machine, i.e. while the caller is
    // inside one of its callbacks. A call back into a guest image from there has
    // to go through CallNested: dynarmic refuses to re-enter a running Jit.
    bool Executing() const;

    // Whether the JIT of this machine is running anywhere, on any thread. Only
    // for diagnostics: a machine must not be re-entered even when it is busy on
    // another thread, which is exactly what this shows.
    bool Busy() const;

    // Host thread that is running this machine, 0 when idle. Diagnostics only.
    std::uint32_t BusyThread() const;

    // Index this machine owns in the shared exclusive monitor. Every guest thread
    // needs a distinct one, and the container hands the index back when the
    // thread exits.
    std::uint32_t ProcessorId() const;

    // Name of the HLE handler currently running on this machine, null when the
    // guest itself is executing. Diagnostics only: a hang inside a host call
    // and a hang inside guest code look identical from the outside otherwise.
    void SetHleCall(const char* name);
    const char* HleCallName() const;

    // Stops the current run: the JIT returns from Call/CallFunction.
    void Stop();

    // Handles SVC #n with n != 0. Handlers are tried in registration order;
    // the first one that returns true wins. SVC #0 is the Linux syscall ABI and
    // is handled like any other number. Returning false aborts the run with an
    // "unhandled SVC" error.
    using SvcHandler = std::function<bool(std::uint32_t svc, A32Machine& machine)>;
    void AddSvcHandler(SvcHandler handler);

    // Reuses every handler installed on another machine. Guest threads are
    // separate machines, and they must trap into the same HLE table and the same
    // syscall dispatcher.
    void CopySvcHandlersFrom(const A32Machine& other);

    std::uint32_t Reg(std::size_t index) const;

    // Flushes the running code cache so the next fetch reports the guest pc
    // (see RequestGuestSample). Diagnostic only.
    void SampleNow();

    // Return addresses on the guest stack from `sp` up to `scan_bytes` later.
    // Only words that point into a loaded image and are preceded by a call are
    // kept, which turns a raw stack dump into a call chain.
    std::vector<std::uint32_t> GuestReturnAddresses(std::uint32_t sp,
                                                    std::uint32_t scan_bytes) const;
    void SetReg(std::size_t index, std::uint32_t value);

    // Reads one guest word, for diagnostics that run when the machine is stopped.
    bool ReadGuestWord(std::uint32_t address, std::uint32_t* value) const;

    // Guest thread pointer. ARMv7 code reads it with `mrc p15, 0, rX, c13, c0, 3`
    // and writes TPIDRURW through c13, c0, 2; the machine services both from this
    // value, so bionic-style TLS access works without running bionic.
    void SetTlsBase(std::uint32_t base);
    std::uint32_t TlsBase() const;

    // Guest cycles retired since the last ResetStats() (one tick ~= one instruction).
    std::uint64_t RetiredTicks() const;
    void ResetStats();

    // Calls that reached the host memory callbacks instead of fastmem.
    std::uint64_t CallbackReads() const;
    std::uint64_t CallbackWrites() const;

    const std::string& LastError() const;
    // Forgets the previous fault. CallFunction does not clear it, so a caller
    // that runs several independent entries must do it itself.
    void ClearError();
    std::string LastHaltReasonName() const;

    // Which system call this thread entered last, and how many it has made.
    // A thread stuck somewhere reports the same count twice, and the number
    // names the call it is blocked in.
    void NoteSyscall(std::uint32_t number);
    std::uint32_t LastSyscall() const;
    std::uint64_t SyscallCount() const;

    void InvalidateCacheRange(std::uint32_t addr, std::size_t len);
    void ClearCache();

    // Settings this machine was built with, so a guest thread can create its own
    // machine with the same behaviour and a shared exclusive monitor.
    const Config& Settings() const;

    // Address of the return trap installed with SetReturnTrap(), 0 if none.
    std::uint32_t ReturnTrap() const;

    // Caps how many guest cycles one run may retire. A watchdog, not a normal
    // control path: the JIT stops with a cycle-count halt when the budget is gone.
    void SetTickBudget(std::uint64_t ticks);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The machine running guest code on this thread, or null when this thread is not
// inside guest code. A callback that Java makes into a guest image has to use
// this machine: a Jit belongs to the thread that is running it.
A32Machine* CurrentGuestMachine();

}  // namespace armbat
