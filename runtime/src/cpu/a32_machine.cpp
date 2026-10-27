#include "armbat/a32_machine.h"
#include "armbat/crash_handler.h"
#include "armbat/guest_modules.h"
#include "armbat/hle.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <android/log.h>

#include "armbat/log.h"
#include <unistd.h>
#include <functional>
#include <optional>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

namespace armbat {
namespace {

using Dynarmic::A32::CoprocReg;
using Dynarmic::A32::VAddr;
using Dynarmic::HaltReason;

// Guest code ends with SVC #0; the callback halts the JIT, so LR is never used
// to unwind. Kept as a distinct value so a stray return is detectable.
constexpr VAddr kSentinelLr = 0xFFFFFFF0u;
constexpr std::uint32_t kUserModeCpsr = 0x00000010u;
constexpr std::uint32_t kThumbBit = 0x00000020u;

// Which machine is running guest code on this thread. A Java method the guest
// called can reach native code again, and that callback belongs to the machine
// of the thread that made the call: a Jit is owned by one OS thread, never
// shared. Null means "this thread is not inside guest code".
thread_local A32Machine* t_current_machine = nullptr;

struct RunningMachine {
    A32Machine* previous;
    std::atomic<std::uint32_t>* owner_tid;
    std::uint32_t previous_tid;
    RunningMachine(A32Machine* machine, std::atomic<std::uint32_t>* tid)
        : previous(t_current_machine), owner_tid(tid), previous_tid(0) {
        t_current_machine = machine;
        previous_tid = owner_tid->exchange(::gettid());
        if (previous_tid != 0 && previous_tid != static_cast<std::uint32_t>(::gettid())) {
            ARMBAT_LOG_ERROR("Armbat",
                                "machine %p used by tid=%d while tid=%u runs it",
                                static_cast<void*>(machine), ::gettid(), previous_tid);
        }
    }
    ~RunningMachine() {
        owner_tid->store(previous_tid);
        t_current_machine = previous;
    }
};

// SVC #0xFFFFFF tells the host to stop; SVC #0 is the Linux syscall trap and
// every other immediate belongs to an HLE stub.
constexpr std::uint32_t kHaltSvc = 0x00FFFFFFu;

// CP15 as far as a user-mode ARMv7 program can see it. Only the thread pointer
// registers are meaningful here: TPIDRURW (c13, c0, 2) is writable and TPIDRURO
// (c13, c0, 3) is the read-only copy the kernel keeps in sync, which is what
// bionic's inlined TLS access reads.
class Cp15 final : public Dynarmic::A32::Coprocessor {
public:
    explicit Cp15(std::uint32_t* tls_base) : tls_base_(tls_base) {}

    std::optional<Callback> CompileInternalOperation(bool /*two*/, unsigned /*opc1*/,
                                                     CoprocReg /*CRd*/, CoprocReg /*CRn*/,
                                                     CoprocReg /*CRm*/,
                                                     unsigned /*opc2*/) override {
        // A data operation on cp15 has no meaning here; accept and ignore it
        // instead of raising a coprocessor exception the guest cannot handle.
        return Callback{&Cp15::Ignore, nullptr};
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool /*two*/, unsigned opc1, CoprocReg CRn,
                                               CoprocReg CRm, unsigned opc2) override {
        if (opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 2) {
            return Callback{&Cp15::WriteThreadPointer, tls_base_};
        }
        // Writes to any other cp15 register are dropped: the guest asks for
        // features it cannot observe from inside the container.
        return &scratch_[2];
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool /*two*/, unsigned /*opc*/,
                                                 CoprocReg /*CRm*/) override {
        return std::array<std::uint32_t*, 2>{&scratch_[2], &scratch_[3]};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool /*two*/, unsigned opc1, CoprocReg CRn,
                                              CoprocReg CRm, unsigned opc2) override {
        if (opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 &&
            (opc2 == 2 || opc2 == 3)) {
            // Handing back the variable's address makes the JIT emit a direct
            // load, so a later SetTlsBase is visible to already-compiled code.
            return tls_base_;
        }
        return &scratch_[0];
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool /*two*/, unsigned /*opc*/,
                                                CoprocReg /*CRm*/) override {
        // MRRC on cp15 reads a pair the container does not model (the counter
        // is the usual one); zeroes keep the code running.
        return std::array<std::uint32_t*, 2>{&scratch_[0], &scratch_[1]};
    }

    std::optional<Callback> CompileLoadWords(bool /*two*/, bool /*long_transfer*/,
                                             CoprocReg /*CRd*/,
                                             std::optional<std::uint8_t> /*option*/) override {
        return Callback{&Cp15::Ignore, nullptr};
    }

    std::optional<Callback> CompileStoreWords(bool /*two*/, bool /*long_transfer*/,
                                              CoprocReg /*CRd*/,
                                              std::optional<std::uint8_t> /*option*/) override {
        return Callback{&Cp15::Ignore, nullptr};
    }

private:
    static std::uint64_t WriteThreadPointer(void* user_arg, std::uint32_t value,
                                            std::uint32_t /*unused*/) {
        *static_cast<std::uint32_t*>(user_arg) = value;
        return 0;
    }

    static std::uint64_t Ignore(void*, std::uint32_t, std::uint32_t) { return 0; }

    // Reads and writes the container does not model land here. Static storage is
    // required: the JIT embeds these addresses in compiled code.
    inline static std::uint32_t scratch_[4] = {};

    std::uint32_t* tls_base_;
};

class Callbacks final : public Dynarmic::A32::UserCallbacks {
public:
    Callbacks(GuestArena& arena, std::uint64_t tick_budget)
        : arena_(arena), tick_budget_(tick_budget) {}

    void Attach(Dynarmic::A32::Jit* jit) { jit_ = jit; }

    std::uint8_t MemoryRead8(VAddr addr) override { return Read<std::uint8_t>(addr); }
    std::uint16_t MemoryRead16(VAddr addr) override { return Read<std::uint16_t>(addr); }
    std::uint32_t MemoryRead32(VAddr addr) override { return Read<std::uint32_t>(addr); }
    std::uint64_t MemoryRead64(VAddr addr) override { return Read<std::uint64_t>(addr); }

    void MemoryWrite8(VAddr addr, std::uint8_t value) override { Write(addr, value); }
    void MemoryWrite16(VAddr addr, std::uint16_t value) override { Write(addr, value); }
    void MemoryWrite32(VAddr addr, std::uint32_t value) override { Write(addr, value); }
    void MemoryWrite64(VAddr addr, std::uint64_t value) override { Write(addr, value); }

    // STREX. The exclusive monitor has already checked that the reservation is
    // intact; the callback still compares the value it saw at LDREX time with
    // what memory holds now, and only then stores. Returning false tells the
    // guest the store failed, which is a normal outcome it retries -- a callback
    // that always fails turns every atomic into an infinite loop.
    bool MemoryWriteExclusive8(VAddr addr, std::uint8_t value,
                               std::uint8_t expected) override {
        return WriteExclusive(addr, value, expected);
    }
    bool MemoryWriteExclusive16(VAddr addr, std::uint16_t value,
                                std::uint16_t expected) override {
        return WriteExclusive(addr, value, expected);
    }
    bool MemoryWriteExclusive32(VAddr addr, std::uint32_t value,
                                std::uint32_t expected) override {
        return WriteExclusive(addr, value, expected);
    }
    bool MemoryWriteExclusive64(VAddr addr, std::uint64_t value,
                                std::uint64_t expected) override {
        return WriteExclusive(addr, value, expected);
    }

    /**
     * The chain that led to a fault.
     *
     * A bare pc tells almost nothing: the instruction is usually a call
     * through a pointer that was never filled in, and the only record of who
     * asked for that call is the stack. A32 code keeps a frame chain -- r11
     * points at the saved r11 with its return address directly above -- and
     * Thumb and ARM agree on that layout, so the callers can be read without
     * symbols, debug data or a shadow stack.
     *
     * Frames without a pointer (leaf functions, hand-written assembly) end the
     * walk early; the stack is then scanned for words that land inside a
     * loaded image, which are return addresses and still name the callers
     * around the fault.
     */
    void DumpGuestTrace(std::uint32_t pc, std::uint32_t lr, std::uint32_t sp,
                        std::uint32_t fp) {
        const char* hle = machine != nullptr ? machine->HleCallName() : nullptr;
        ARMBAT_LOG_ERROR("Armbat",
                            "fault thread: tid=%d hle=%s fp=0x%08x sp=0x%08x", ::gettid(),
                            hle != nullptr ? hle : "-", fp, sp);
        ARMBAT_LOG_ERROR("Armbat", "fault trace:");
        ARMBAT_LOG_ERROR("Armbat", "  #00 pc  %s",
                            armbat::DescribeGuestAddress(pc).c_str());
        ARMBAT_LOG_ERROR("Armbat", "  #01 lr  %s",
                            armbat::DescribeGuestAddress(lr).c_str());
        int slot = 2;
        std::uint32_t frame = fp;
        while (slot < kMaxFrames) {
            if ((frame & 3u) != 0 || frame < sp || !arena_.IsMapped(frame, 8)) {
                break;
            }
            std::uint32_t pair[2];
            std::memcpy(pair, arena_.Ptr(frame), sizeof(pair));
            const std::uint32_t next = pair[0];
            const std::uint32_t ret = pair[1];
            if (ret == 0) {
                break;
            }
            ARMBAT_LOG_ERROR("Armbat", "  #%02d fp  %s", slot,
                                armbat::DescribeGuestAddress(ret).c_str());
            ++slot;
            if (next <= frame) {
                break;
            }
            frame = next;
        }
        if (slot == 2) {
            DumpStackFrames(sp, slot);
        }
    }

    // Without a frame pointer the callers are still on the stack: a call leaves
    // its return address there, and the four bytes in front of it are the call
    // itself. A32Machine does that test, so what lands here is a call chain and
    // not every pointer a frame happens to hold.
    //
    // One frame per line on purpose: logcat drops whatever does not fit into a
    // single entry, and a report that loses its last frames loses the caller
    // that explains the fault.
    void DumpStackFrames(std::uint32_t sp, int first) {
        if (machine == nullptr) {
            return;
        }
        const std::vector<std::uint32_t> frames = machine->GuestReturnAddresses(sp, 0x2000);
        if (frames.empty()) {
            return;
        }
        ARMBAT_LOG_ERROR("Armbat", "fault stack: %zu return addresses",
                            frames.size());
        int slot = first;
        std::uint32_t previous = 0;
        for (std::size_t i = 0; i < frames.size() && slot < kMaxFrames; ++i) {
            if (frames[i] == previous) {
                continue;
            }
            previous = frames[i];
            ARMBAT_LOG_ERROR("Armbat", "  #%02d stk %s", slot,
                                armbat::DescribeGuestAddress(frames[i]).c_str());
            ++slot;
        }
    }

    // The platform calls this thread made just before the fault. A null callback
    // is usually an answer the framework gave to one of them, and the call trail
    // is then the only record of what was asked.
    void DumpRecentCalls() {
        const std::vector<std::string> trail =
            armbat::RecentCallsOfThread(static_cast<std::int32_t>(::gettid()), 12);
        if (trail.empty()) {
            return;
        }
        std::string line;
        for (const std::string& call : trail) {
            if (!line.empty()) {
                line += ' ';
            }
            line += call;
        }
        ARMBAT_LOG_ERROR("Armbat", "fault calls: %s", line.c_str());
    }

    // A PLT stub loads its target from a GOT cell and leaves that cell's address
    // in r12, so the cell says whether the guest had no import resolved or the
    // stub computed the wrong address.
    void DumpImportCell(std::uint32_t cell) {
        const std::string where = armbat::DescribeGuestAddress(cell);
        if (!arena_.IsMapped(cell - 8, 24)) {
            ARMBAT_LOG_ERROR("Armbat", "fault got: r12=%s unreadable",
                                where.c_str());
            return;
        }
        std::uint32_t words[6];
        std::memcpy(words, arena_.Ptr(cell - 8), sizeof(words));
        ARMBAT_LOG_ERROR("Armbat",
                            "fault got: r12=%s target=%s cells: %08x %08x [%08x] %08x %08x %08x",
                            where.c_str(), armbat::DescribeGuestAddress(words[2]).c_str(),
                            words[0], words[1], words[2], words[3], words[4], words[5]);
    }

    void DumpFaultBlock(const char* label, std::uint32_t addr, std::size_t length) {
        if (!arena_.IsMapped(addr, length)) {
            ARMBAT_LOG_ERROR("Armbat", "fault %s 0x%08x unmapped", label,
                                addr);
            return;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(arena_.Ptr(addr));
        std::string hex;
        std::string text;
        char pair[4];
        for (std::size_t i = 0; i < length; ++i) {
            std::snprintf(pair, sizeof(pair), "%02x", bytes[i]);
            hex += pair;
            text += (bytes[i] >= 0x20 && bytes[i] < 0x7f) ? static_cast<char>(bytes[i]) : '.';
        }
        ARMBAT_LOG_ERROR("Armbat", "fault %s 0x%08x %s | %s", label, addr,
                            hex.c_str(), text.c_str());
    }

    void DumpFaultStrings(const std::uint32_t regs[16]) {
        DumpFaultBlock("r4", regs[4], 0x38);
        std::uint32_t size = 0;
        if (arena_.IsMapped(regs[4] + 8, 4)) {
            std::memcpy(&size, arena_.Ptr(regs[4] + 8), 4);
        }
        // std::string lives at wrapper+8: length at +8, inline bytes or a heap
        // pointer at +12 depending on the short-string threshold.
        if (size > 0 && size < 256) {
            std::uint32_t data = regs[4] + 12;
            if (size > 24 && arena_.IsMapped(regs[4] + 12, 4)) {
                std::memcpy(&data, arena_.Ptr(regs[4] + 12), 4);
            }
            DumpFaultBlock("name", data, size);
        }
        if (regs[2] > 0 && regs[2] < 256) {
            DumpFaultBlock("str", regs[1], regs[2]);
        }
    }

    std::optional<std::uint32_t> MemoryReadCode(VAddr addr) override {
        if (jit_ != nullptr) {
            recent_pcs_[recent_count_ & (kRecentPcs - 1)] = addr;
            ++recent_count_;
        }
        // Sampling: the JIT fetches code only after a cache flush, and the
        // watchdog flushes on demand, so this names the block a spinning guest
        // is in without stopping it.
        if (TakeGuestSample()) {
            ARMBAT_LOG_INFO("Armbat", "sample: block pc=0x%08x", addr);
        }
        if (!arena_.IsMapped(addr, 4)) {
            // A jump to nothing is almost always a call through a pointer the
            // guest never filled in; the frames above still hold the call chain.
            Fail("instruction fetch from unmapped guest address " + Hex(addr));
            if (jit_ != nullptr) {
                DumpImportCell(jit_->Regs()[12]);
            }
            return std::nullopt;
        }
        std::uint32_t value;
        std::memcpy(&value, arena_.Ptr(addr), sizeof(value));
        return value;
    }

    void InterpreterFallback(VAddr pc, size_t num_instructions) override {
        Fail("interpreter fallback (unimplemented instruction) at " + Hex(pc) + ", " +
             std::to_string(num_instructions) + " instruction(s)");
    }

    void CallSVC(std::uint32_t svc) override {
        // A blocking syscall on the other side of a signal is the only place a
        // parked thread comes back to life, so pending signals are delivered
        // before the call runs: the guest's own signal frame expects the
        // registers of the code that was interrupted, and here they are whole.
        if (machine != nullptr) {
            machine->RunSafePoint();
        }
        if (svc == kHaltSvc) {
            if (jit_ != nullptr) {
                jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            }
            return;
        }
        if (svc_handlers != nullptr) {
            for (auto& handler : *svc_handlers) {
                if (handler && handler(svc, *machine)) {
                    return;
                }
            }
        }
        Fail("unhandled SVC #" + std::to_string(svc));
        if (jit_ != nullptr) {
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        std::uint32_t word = 0;
        (void)machine->ReadGuestWord(pc, &word);
        std::string around;
        for (std::uint32_t offset : {16u, 12u, 8u, 4u}) {
            std::uint32_t previous = 0;
            if (machine->ReadGuestWord(pc - offset, &previous)) {
                around += " " + Hex(previous);
            }
        }
        Fail("guest exception " + std::to_string(static_cast<int>(exception)) + " at " + Hex(pc) +
             " word=" + Hex(word) + " before:" + around +
             " cpsr=" + Hex(jit_ != nullptr ? jit_->Cpsr() : 0));
        if (jit_ != nullptr) {
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
    }

    void AddTicks(std::uint64_t ticks) override {
        ticks_ += ticks;
        if (jit_ != nullptr) {
            recent_pcs_[recent_count_ & (kRecentPcs - 1)] = jit_->Regs()[15];
            ++recent_count_;
        }
        if (jit_ != nullptr && TakeGuestSample()) {
            ARMBAT_LOG_INFO("Armbat", "sample: pc=0x%08x lr=0x%08x sp=0x%08x",
                                jit_->Regs()[15], jit_->Regs()[14], jit_->Regs()[13]);
        }
    }

    // A guest that jumps to nothing leaves no trace of how it got there, so
    // every thread keeps the last few block addresses it ran.
    void DumpRecentBlocks() const {
        if (recent_count_ == 0) {
            return;
        }
        std::string line = "recent blocks:";
        const std::uint32_t samples = std::min<std::uint32_t>(recent_count_, kRecentPcs);
        for (std::uint32_t index = 0; index < samples; ++index) {
            const std::uint32_t slot = (recent_count_ - samples + index) & (kRecentPcs - 1);
            line += " " + armbat::DescribeGuestAddress(recent_pcs_[slot]);
        }
        ARMBAT_LOG_ERROR("Armbat", "%s", line.c_str());
    }

    std::uint64_t GetTicksRemaining() override {
        return tick_budget_ > ticks_ ? tick_budget_ - ticks_ : 0;
    }

    // Relative to now: the counter is cumulative across runs, so an absolute
    // budget would be silently useless after the first few million ticks.
    void SetTickBudget(std::uint64_t budget) { tick_budget_ = ticks_ + budget; }

    std::uint64_t RetiredTicks() const { return ticks_; }
    std::uint64_t CallbackReads() const { return reads_; }
    std::uint64_t CallbackWrites() const { return writes_; }
    void ResetTicks() { ticks_ = 0; }
    void ResetCounters() const { reads_ = 0; writes_ = 0; }

    const std::string& Error() const { return error_; }
    void ClearError() { error_.clear(); }
    void NoteError(std::string message) { Fail(std::move(message)); }

private:
    template <typename T>
    T Read(VAddr addr) {
        ++reads_;
        if (!arena_.IsMapped(addr, sizeof(T))) {
            Fail("read of " + std::to_string(sizeof(T)) + " bytes from unmapped guest address " +
                 Hex(addr));
            return 0;
        }
        T value;
        std::memcpy(&value, arena_.Ptr(addr), sizeof(value));
        return value;
    }

    template <typename T>
    void Write(VAddr addr, T value) {
        ++writes_;
        if (!arena_.IsMapped(addr, sizeof(T))) {
            Fail("write of " + std::to_string(sizeof(T)) + " bytes to unmapped guest address " +
                 Hex(addr));
            return;
        }
        std::memcpy(arena_.Ptr(addr), &value, sizeof(value));
    }

    template <typename T>
    bool WriteExclusive(VAddr addr, T value, T expected) {
        ++writes_;
        if (!arena_.IsMapped(addr, sizeof(T))) {
            Fail("exclusive write of " + std::to_string(sizeof(T)) +
                 " bytes to unmapped guest address " + Hex(addr));
            return false;
        }
        // A compare and a store are one step: another guest thread's plain
        // store between them would be lost, and the guest would be told the
        // reservation held when it no longer did.
        T* const cell = reinterpret_cast<T*>(arena_.Ptr(addr));
        if (!__atomic_compare_exchange_n(cell, &expected, value, false, __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST)) {
            return false;  // the reservation was lost to another writer
        }
        return true;
    }

    static std::string Hex(std::uint32_t value) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
        return buffer;
    }

    void Fail(std::string message) {
        if (error_.empty()) {
            // The JIT keeps the register file of the running block; r15 is the
            // instruction that faulted, which is the only way to tell where in
            // an initializer the container handed out a bad pointer.
            std::uint32_t pc = 0;
            std::uint32_t regs[16] = {};
            if (jit_ != nullptr) {
                for (std::size_t i = 0; i < 16; ++i) {
                    regs[i] = jit_->Regs()[i];
                }
                pc = regs[15];
            }
            char suffix[288];
            std::snprintf(suffix, sizeof(suffix),
                          " (pc=0x%08x lr=0x%08x sp=0x%08x cpsr=0x%08x r0=0x%08x r1=0x%08x "
                          "r2=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x "
                          "r8=0x%08x r12=0x%08x)",
                          pc, regs[14], regs[13], jit_ != nullptr ? jit_->Cpsr() : 0, regs[0],
                          regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7], regs[8],
                          regs[12]);
            error_ = std::move(message) + suffix;
            ARMBAT_LOG_ERROR("Armbat", "%s", error_.c_str());

            // Every fault gets the same report. A read of address zero used to
            // arrive with less than a jump to zero: the stack scan and the
            // blocks, but not the frame chain and not the calls that led here,
            // which are exactly what names the caller of the bad pointer.
            if (jit_ != nullptr) {
                DumpFaultStrings(regs);
                DumpGuestTrace(pc, regs[14], regs[13], regs[11]);
                DumpRecentBlocks();
                DumpRecentCalls();
            }
            // A fault has to stop the block. Running it again would re-execute
            // the same bad access forever: a load from an unmapped page returns
            // a value, and a store that cannot complete turns an atomic loop
            // into a spin that never reports anything.
            if (jit_ != nullptr) {
                jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            }
            // The report is the reason this run is being read at all, and the
            // process may die before the writer thread drains its queue.
            armbat::LogFlush();
        }
    }

    GuestArena& arena_;
    std::uint64_t tick_budget_;
    std::uint64_t ticks_ = 0;
    static constexpr std::uint32_t kRecentPcs = 8;
    // A report is read by a person and by tools/debug_log.py, not replayed: a
    // deep chain of static constructors says nothing past the first frames.
    static constexpr int kMaxFrames = 24;
    std::uint32_t recent_pcs_[kRecentPcs] = {};
    std::uint32_t recent_count_ = 0;
    mutable std::uint64_t reads_ = 0;
    mutable std::uint64_t writes_ = 0;
    Dynarmic::A32::Jit* jit_ = nullptr;
    std::string error_;

public:
    // Set by Impl; tried in order for SVC #n. First true wins.
    std::vector<A32Machine::SvcHandler>* svc_handlers = nullptr;
    A32Machine* machine = nullptr;
};

}  // namespace

struct A32Machine::Impl {
    // dynarmic drops its whole translation cache when less than a megabyte is
    // left, and re-translates everything the guest touches afterwards. A game
    // with tens of megabytes of armv7 code compiles into a cache this size many
    // times over, so a small cache turns into a permanent recompile loop: the
    // frame rate falls, the world takes longer to load, and the CPU is busy
    // translating code it had already translated a second earlier. This is the
    // largest cache dynarmic supports, and the pages behind it are only
    // committed as translation reaches them.
    static constexpr std::size_t kMainCodeCache = 128 * 1024 * 1024;
    static constexpr std::size_t kNestedCodeCache = 16 * 1024 * 1024;

    Impl(GuestArena& arena, const Config& settings)
        : arena(arena), config(settings) {
        if (config.exclusive_monitor == nullptr) {
            config.exclusive_monitor =
                std::make_shared<Dynarmic::ExclusiveMonitor>(Config::kProcessorLimit);
        }
        stack.push_back(AddJit(kMainCodeCache));
        // jits.front() is never replaced, so another thread may halt it without
        // walking the vector that nested calls grow.
        base_jit = jits.front().get();
    }

    // A Jit cannot be re-entered (dynarmic asserts on it), so a guest call made
    // from inside an SVC handler runs on its own Jit with its own register file.
    // The stack mirrors the nesting depth; nothing is copied between them except
    // the result and the thread-local state, which lives outside the register
    // file anyway.
    Dynarmic::A32::Jit* AddJit(std::size_t code_cache_size) {
        auto callbacks = std::make_unique<Callbacks>(arena, tick_budget);

        Dynarmic::A32::UserConfig user_config{};
        user_config.callbacks = callbacks.get();
        user_config.global_monitor = config.exclusive_monitor.get();
        user_config.processor_id = config.processor_id;
        user_config.arch_version = Dynarmic::A32::ArchVersion::v7;
        user_config.enable_cycle_counting = config.cycle_counting;
        user_config.always_little_endian = true;
        user_config.code_cache_size = code_cache_size;
        if (config.fastmem && arena.Ok()) {
            user_config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(arena.Base());
        }
        if (!config.block_linking) {
            user_config.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        // cp15 must be installed before the Jit is built: the coprocessor table is
        // read when blocks are compiled, and `mrc p15` appears in ordinary code.
        user_config.coprocessors[15] = std::make_shared<Cp15>(&tls_base);
        auto jit = std::make_unique<Dynarmic::A32::Jit>(user_config);
        jit->SetCpsr(kUserModeCpsr);
        callbacks->Attach(jit.get());
        callbacks->svc_handlers = &svc_handlers;
        callbacks->machine = owner;
        callback_set.push_back(std::move(callbacks));
        jits.push_back(std::move(jit));
        return jits.back().get();
    }

    Dynarmic::A32::Jit& Current() { return *stack.back(); }

    // Runs guest code at `entry` with the given AAPCS arguments. `lr` is the
    // address the callee returns to: either the SVC sentinel (blob routines that
    // end with SVC themselves) or the return trap (real library functions).
    std::uint32_t Invoke(Dynarmic::A32::Jit& target, std::uint32_t lr, std::uint32_t entry,
                         const std::vector<std::uint32_t>& args,
                         std::uint32_t stack_pointer) {
        if (target.IsExecuting()) {
            ARMBAT_LOG_ERROR("Armbat",
                                "run: jit already executing jit=%p tid=%d owner=%u depth=%zu "
                                "front=%p",
                                static_cast<void*>(&target), ::gettid(), running_tid.load(),
                                stack.size(), static_cast<void*>(stack.front()));
        }
        RunningMachine running(owner, &running_tid);
        auto& regs = target.Regs();
        regs.fill(0);
        regs[13] = stack_pointer;

        std::size_t index = 0;
        for (std::uint32_t arg : args) {
            if (index < 4) {
                regs[index] = arg;
            } else {
                const std::uint32_t slot = stack_pointer + static_cast<std::uint32_t>(4 * (index - 4));
                if (arena.IsMapped(slot, 4)) {
                    std::memcpy(arena.Ptr(slot), &arg, sizeof(arg));
                }
            }
            ++index;
        }
        // A function pointer with bit 0 set means Thumb: the JIT selects the
        // decoder from CPSR.T, not from the address, so both have to be set up.
        const bool thumb = (entry & 1u) != 0;
        regs[14] = lr;
        regs[15] = entry & ~1u;
        target.SetCpsr(kUserModeCpsr | (thumb ? kThumbBit : 0u));
        return RunJit(target);
    }

    // A run is sliced. dynarmic re-reads the halt flag -- and therefore the
    // watchdog's stop request -- only where a block hands control back to it,
    // and a guest loop of linked blocks never does. A finite tick budget forces
    // that hand-back once per slice, which costs one return per few tens of
    // milliseconds of guest time and keeps Stop(), hang samples and cache
    // flushes as responsive as they were without linking.
    static constexpr std::uint64_t kSliceTicks = 20'000'000;

    // The JIT's run loop: CacheInvalidation is a normal exit path, not a failure.
    std::uint32_t RunJit(Dynarmic::A32::Jit& target) {
        RunningMachine running(owner, &running_tid);
        for (;;) {
            for (auto& callbacks : callback_set) {
                callbacks->SetTickBudget(kSliceTicks);
            }
            halt_reason = target.Run();
            if ((static_cast<std::uint32_t>(halt_reason) &
                 static_cast<std::uint32_t>(HaltReason::UserDefined2)) != 0) {
                // Another thread asked this machine to stop so it could run a
                // signal handler here. That is a safe point, not the end of the
                // run: deliver and continue with the same register file.
                target.ClearHalt(HaltReason::UserDefined2);
                if (owner != nullptr) {
                    owner->RunSafePoint();
                }
                continue;
            }
            if (halt_reason == HaltReason::CacheInvalidation) {
                target.ClearHalt(HaltReason::CacheInvalidation);
                continue;
            }
            if (static_cast<std::uint32_t>(halt_reason) != 0) {
                break;
            }
            // The slice ran out with nothing to report: the guest simply has
            // more work, so it gets another slice.
        }
        return target.Regs()[0];
    }

    GuestArena& arena;
    Config config;
    A32Machine* owner = nullptr;
    // The base JIT, for halting this machine from another thread.
    Dynarmic::A32::Jit* base_jit = nullptr;
    // Signals queued by other threads, one bit per signal number.
    std::atomic<std::uint64_t> pending_signals{0};
    A32Machine::SafePointHandler safe_point_handler;
    // Thread currently running this machine, so a machine that is busy can be
    // attributed instead of guessed at.
    std::atomic<std::uint32_t> running_tid{0};
    std::atomic<const char*> hle_call{nullptr};
    std::atomic<std::uint32_t> last_syscall{0};
    std::atomic<std::uint64_t> syscall_count{0};
    std::uint64_t tick_budget = 1ull << 60;
    std::uint32_t sp = 0;
    std::uint32_t tls_base = 0;
    std::uint32_t return_trap = 0;
    std::vector<A32Machine::SvcHandler> svc_handlers;
    std::vector<std::unique_ptr<Callbacks>> callback_set;
    std::vector<std::unique_ptr<Dynarmic::A32::Jit>> jits;
    std::vector<Dynarmic::A32::Jit*> stack;
    HaltReason halt_reason{};
};

A32Machine::A32Machine(GuestArena& arena) : A32Machine(arena, Config{}) {}

A32Machine::A32Machine(GuestArena& arena, Config config) : impl_(std::make_unique<Impl>(arena, config)) {
    impl_->owner = this;
    for (auto& callbacks : impl_->callback_set) {
        callbacks->machine = this;
    }
}
A32Machine::~A32Machine() = default;

bool A32Machine::LoadCode(std::uint32_t addr, const void* data, std::size_t len) {
    if (!impl_->arena.IsMapped(addr, len)) {
        return false;
    }
    std::memcpy(impl_->arena.Ptr(addr), data, len);
    for (auto& jit : impl_->jits) {
        jit->InvalidateCacheRange(addr, len);
    }
    return true;
}

void A32Machine::SetSp(std::uint32_t sp) {
    impl_->sp = sp;
    impl_->stack.front()->Regs()[13] = sp;
}

std::uint32_t A32Machine::Sp() const { return impl_->sp; }

std::uint32_t A32Machine::Call(std::uint32_t entry, std::initializer_list<std::uint32_t> args) {
    return impl_->Invoke(*impl_->stack.front(), kSentinelLr, entry,
                         std::vector<std::uint32_t>(args), impl_->sp);
}

void A32Machine::SetReturnTrap(std::uint32_t addr) {
    impl_->return_trap = addr;
}

void A32Machine::Stop() {
    impl_->Current().HaltExecution(HaltReason::UserDefined1);
}

void A32Machine::AddSvcHandler(SvcHandler handler) {
    impl_->svc_handlers.push_back(std::move(handler));
}

void A32Machine::CopySvcHandlersFrom(const A32Machine& other) {
    impl_->svc_handlers = other.impl_->svc_handlers;
}

std::uint32_t A32Machine::CallFunction(std::uint32_t entry,
                                      const std::vector<std::uint32_t>& args) {
    if (impl_->return_trap == 0) {
        impl_->callback_set.front()->NoteError("return trap is not configured");
        return 0;
    }
    return impl_->Invoke(*impl_->stack.front(), impl_->return_trap, entry, args, impl_->sp);
}

std::uint32_t A32Machine::CallFunction(std::uint32_t entry,
                                      std::initializer_list<std::uint32_t> args) {
    return CallFunction(entry, std::vector<std::uint32_t>(args));
}

std::uint32_t A32Machine::CallNested(std::uint32_t entry,
                                     std::initializer_list<std::uint32_t> args) {
    if (impl_->return_trap == 0) {
        impl_->callback_set.front()->NoteError("return trap is not configured");
        return 0;
    }

    // The outer run is suspended on the host stack, so the nested call needs its
    // own Jit: dynarmic refuses to re-enter one.
    const std::size_t depth = impl_->stack.size();
    if (impl_->jits.size() <= depth) {
        impl_->AddJit(Impl::kNestedCodeCache);
    }
    Dynarmic::A32::Jit& outer = impl_->Current();
    Dynarmic::A32::Jit& nested = *impl_->jits[depth];
    nested.Regs() = outer.Regs();
    nested.SetCpsr(outer.Cpsr());

    // Start below the outer frame: the stack grows down, so reusing the base
    // pointer would overwrite the caller's locals.
    const std::uint32_t nested_sp = (nested.Regs()[13] - 64u) & ~7u;
    impl_->stack.push_back(&nested);
    const std::uint32_t result =
        impl_->Invoke(nested, impl_->return_trap, entry, std::vector<std::uint32_t>(args),
                      nested_sp);
    impl_->stack.pop_back();

    nested.ClearHalt(HaltReason::UserDefined1);
    nested.ClearHalt(HaltReason::CacheInvalidation);
    outer.Regs()[0] = result;
    outer.Regs()[1] = nested.Regs()[1];  // 64-bit results live in r0:r1
    return result;
}

bool A32Machine::Executing() const {
    return t_current_machine == this && !impl_->stack.empty() && impl_->stack.front()->IsExecuting();
}

bool A32Machine::Busy() const {
    return !impl_->stack.empty() && impl_->stack.front()->IsExecuting();
}

std::uint32_t A32Machine::BusyThread() const {
    return impl_->running_tid.load();
}

std::uint32_t A32Machine::ProcessorId() const {
    return impl_->config.processor_id;
}

void A32Machine::SetHleCall(const char* name) { impl_->hle_call.store(name); }

const char* A32Machine::HleCallName() const { return impl_->hle_call.load(); }

A32Machine* CurrentGuestMachine() { return t_current_machine; }

std::uint32_t A32Machine::CallNested(std::uint32_t entry,
                                     const std::vector<std::uint32_t>& args) {
    if (impl_->return_trap == 0) {
        impl_->callback_set.front()->NoteError("return trap is not configured");
        return 0;
    }
    const std::size_t depth = impl_->stack.size();
    if (impl_->jits.size() <= depth) {
        impl_->AddJit(Impl::kNestedCodeCache);
    }
    Dynarmic::A32::Jit& outer = impl_->Current();
    Dynarmic::A32::Jit& nested = *impl_->jits[depth];
    nested.Regs() = outer.Regs();
    nested.SetCpsr(outer.Cpsr());

    const std::uint32_t nested_sp = (nested.Regs()[13] - 64u) & ~7u;
    impl_->stack.push_back(&nested);
    const std::uint32_t result = impl_->Invoke(nested, impl_->return_trap, entry, args, nested_sp);
    impl_->stack.pop_back();

    nested.ClearHalt(HaltReason::UserDefined1);
    nested.ClearHalt(HaltReason::CacheInvalidation);
    outer.Regs()[0] = result;
    outer.Regs()[1] = nested.Regs()[1];  // 64-bit results live in r0:r1
    return result;
}

bool A32Machine::Thumb() const {
    return (impl_->Current().Cpsr() & kThumbBit) != 0;
}

std::uint32_t A32Machine::CallNestedPreservingRegisters(
    std::uint32_t entry, const std::vector<std::uint32_t>& args) {
    if (impl_->return_trap == 0) {
        impl_->callback_set.front()->NoteError("return trap is not configured");
        return 0;
    }
    const std::size_t depth = impl_->stack.size();
    if (impl_->jits.size() <= depth) {
        impl_->AddJit(Impl::kNestedCodeCache);
    }
    Dynarmic::A32::Jit& outer = impl_->Current();
    Dynarmic::A32::Jit& nested = *impl_->jits[depth];
    // The handler runs on the interrupted thread's own stack, so its frames sit
    // where the guest's stack scan expects them.
    const std::array<std::uint32_t, 16> saved_regs = outer.Regs();
    const std::uint32_t saved_cpsr = outer.Cpsr();

    const std::uint32_t nested_sp = (saved_regs[13] - 64u) & ~7u;
    impl_->stack.push_back(&nested);
    const std::uint32_t result =
        impl_->Invoke(nested, impl_->return_trap, entry, args, nested_sp);
    impl_->stack.pop_back();

    nested.ClearHalt(HaltReason::UserDefined1);
    nested.ClearHalt(HaltReason::UserDefined2);
    nested.ClearHalt(HaltReason::CacheInvalidation);
    // Unlike CallNested, nothing is carried back: a signal handler returns to
    // the instruction stream it interrupted, and that stream keeps its r0/r1.
    outer.Regs() = saved_regs;
    outer.SetCpsr(saved_cpsr);
    return result;
}

void A32Machine::RequestSignal(std::uint32_t signal) {
    if (signal == 0 || signal > 64) {
        return;
    }
    impl_->pending_signals.fetch_or(1ull << (signal - 1), std::memory_order_release);
    // The halt flag is atomic inside dynarmic; it is read where a block hands
    // control back to the dispatcher, which is a safe point.
    if (impl_->base_jit != nullptr) {
        impl_->base_jit->HaltExecution(HaltReason::UserDefined2);
    }
}

std::uint32_t A32Machine::TakePendingSignal() {
    std::uint64_t mask = impl_->pending_signals.load(std::memory_order_acquire);
    while (mask != 0) {
        const std::uint32_t signal = static_cast<std::uint32_t>(__builtin_ctzll(mask)) + 1;
        if (impl_->pending_signals.compare_exchange_weak(mask, mask & ~(1ull << (signal - 1)),
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
            return signal;
        }
    }
    return 0;
}

void A32Machine::SetSafePointHandler(SafePointHandler handler) {
    impl_->safe_point_handler = std::move(handler);
}

void A32Machine::CopySafePointHandlerFrom(const A32Machine& other) {
    impl_->safe_point_handler = other.impl_->safe_point_handler;
}

void A32Machine::RunSafePoint() {
    if (impl_->pending_signals.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (!impl_->safe_point_handler) {
        return;
    }
    impl_->safe_point_handler(*this);
}

std::uint32_t A32Machine::EnterClonedThread(const std::array<std::uint32_t, 16>& regs,
                                            std::uint32_t entry, std::uint32_t sp,
                                            std::uint32_t tls, bool thumb) {
    Dynarmic::A32::Jit& target = impl_->Current();
    impl_->tls_base = tls;
    impl_->sp = sp;
    auto& registers = target.Regs();
    registers = regs;
    registers[13] = sp;
    registers[15] = entry & ~1u;
    target.SetCpsr(kUserModeCpsr | (thumb ? kThumbBit : 0u));
    return impl_->RunJit(target);
}

void A32Machine::SampleNow() {
    if (impl_->stack.empty()) {
        return;
    }
    Dynarmic::A32::Jit* running = impl_->stack.back();
    ARMBAT_LOG_INFO("Armbat", "sample: depth=%zu jit=%p pc=0x%08x sp=0x%08x",
                        impl_->stack.size(), static_cast<void*>(running), running->Regs()[15],
                        running->Regs()[13]);
    running->ClearCache();
}

std::uint32_t A32Machine::Reg(std::size_t index) const {
    return impl_->stack.back()->Regs().at(index);
}

bool A32Machine::ReadGuestWord(std::uint32_t address, std::uint32_t* value) const {
    if (!impl_->arena.IsMapped(address, sizeof(*value))) {
        return false;
    }
    std::memcpy(value, impl_->arena.Ptr(address), sizeof(*value));
    return true;
}

std::vector<std::uint32_t> A32Machine::GuestReturnAddresses(std::uint32_t sp,
                                                            std::uint32_t scan_bytes) const {
    std::vector<std::uint32_t> addresses;
    for (std::uint32_t offset = 0; offset < scan_bytes; offset += 4) {
        std::uint32_t word = 0;
        if (!ReadGuestWord(sp + offset, &word)) {
            break;
        }
        const std::uint32_t target = word & ~1u;
        if (!armbat::IsGuestModuleAddress(target)) {
            continue;
        }
        // A return address is only real when the bytes right before it are a
        // call: that filters out the pointers a frame happens to hold.
        std::uint32_t previous = 0;
        if (!ReadGuestWord(target - 4, &previous)) {
            continue;
        }
        const bool thumb_call = ((previous >> 16) & 0xD000u) == 0xD000u ||
                                ((previous >> 16) & 0xD000u) == 0xC000u;
        const bool thumb_blx_register = ((previous & 0xFF80u) == 0x4780u);
        const bool arm_call = ((previous >> 24) & 0x0Fu) == 0x0Bu;
        if (thumb_call || thumb_blx_register || arm_call) {
            addresses.push_back(word);
        }
    }
    return addresses;
}

void A32Machine::SetReg(std::size_t index, std::uint32_t value) {
    if (index < 16) {
        impl_->stack.back()->Regs()[index] = value;
    }
}

void A32Machine::SetTlsBase(std::uint32_t base) { impl_->tls_base = base; }

std::uint32_t A32Machine::TlsBase() const { return impl_->tls_base; }

std::uint64_t A32Machine::RetiredTicks() const {
    return impl_->callback_set.front()->RetiredTicks();
}

void A32Machine::ResetStats() {
    impl_->callback_set.front()->ResetTicks();
    impl_->callback_set.front()->ResetCounters();
}

std::uint64_t A32Machine::CallbackReads() const {
    return impl_->callback_set.front()->CallbackReads();
}

std::uint64_t A32Machine::CallbackWrites() const {
    return impl_->callback_set.front()->CallbackWrites();
}

const std::string& A32Machine::LastError() const {
    return impl_->callback_set.front()->Error();
}

void A32Machine::ClearError() {
    if (!impl_->callback_set.empty()) {
        impl_->callback_set.front()->ClearError();
    }
}

void A32Machine::NoteSyscall(std::uint32_t number) {
    impl_->last_syscall.store(number, std::memory_order_relaxed);
    impl_->syscall_count.fetch_add(1, std::memory_order_relaxed);
}

std::uint32_t A32Machine::LastSyscall() const {
    return impl_->last_syscall.load(std::memory_order_relaxed);
}

std::uint64_t A32Machine::SyscallCount() const {
    return impl_->syscall_count.load(std::memory_order_relaxed);
}

std::string A32Machine::LastHaltReasonName() const {
    if (impl_->halt_reason == HaltReason::UserDefined1) {
        return "svc";
    }
    if (impl_->halt_reason == HaltReason::CacheInvalidation) {
        return "cache-invalidation";
    }
    if (!impl_->halt_reason) {
        // The JIT returns without a reason set from exactly one place: the tick
        // budget ran out between blocks. Reporting "halt=0" here read like a
        // normal exit and hid every watchdog trip.
        return "budget-exhausted";
    }
    return "halt=" + std::to_string(static_cast<std::uint32_t>(impl_->halt_reason));
}

void A32Machine::InvalidateCacheRange(std::uint32_t addr, std::size_t len) {
    for (auto& jit : impl_->jits) {
        jit->InvalidateCacheRange(addr, len);
    }
}

void A32Machine::SetTickBudget(std::uint64_t ticks) {
    impl_->tick_budget = ticks;
    for (auto& callbacks : impl_->callback_set) {
        callbacks->SetTickBudget(ticks);
    }
}

void A32Machine::ClearCache() {
    for (auto& jit : impl_->jits) {
        jit->ClearCache();
    }
}

const A32Machine::Config& A32Machine::Settings() const {
    return impl_->config;
}

std::uint32_t A32Machine::ReturnTrap() const {
    return impl_->return_trap;
}

}  // namespace armbat

