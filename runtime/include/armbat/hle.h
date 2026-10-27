#pragma once

#include <cstddef>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace armbat {

class A32Machine;
class GuestApk;
class GuestArena;

// One call from guest code into a host implementation.
class HleCall {
public:
    HleCall(A32Machine& machine, GuestArena& arena) : machine_(machine), arena_(arena) {}

    // AAPCS argument: r0..r3, then stacked words.
    std::uint32_t Arg(std::size_t index) const;

    void Return(std::uint32_t value);
    void ReturnPair(std::uint32_t first, std::uint32_t second);
    void ReturnQuad(std::uint32_t r0, std::uint32_t r1, std::uint32_t r2, std::uint32_t r3);

    bool CopyIn(std::uint32_t address, void* destination, std::size_t length) const;
    bool CopyOut(std::uint32_t address, const void* source, std::size_t length);
    bool CopyWithin(std::uint32_t destination, std::uint32_t source, std::size_t length);
    bool CompareWithin(std::uint32_t left, std::uint32_t right, std::size_t length, int* result);
    bool Fill(std::uint32_t address, std::uint8_t value, std::size_t length);
    bool Zero(std::uint32_t address, std::size_t length);

    // Copies a NUL-terminated guest string, at most max_length bytes. Returns
    // false when the terminator is not inside that window or the bytes are not
    // mapped: a truncated string is a fault, not a shorter string.
    bool ReadCString(std::uint32_t address, std::string& out, std::size_t max_length) const;

    // Copies the window strncmp is allowed to look at: stops at the terminator or
    // after max_length bytes, whichever comes first. Returns false only when a
    // byte inside that window is unmapped, so a bounded comparison never reads
    // past its limit.
    bool ReadCStringWindow(std::uint32_t address, std::size_t max_length,
                           std::string& out) const;

    // Length of a NUL-terminated guest string, scanned in place: strlen must not
    // allocate a copy of a string that may be megabytes long.
    bool CStringLength(std::uint32_t address, std::uint64_t& length) const;

    // Host view of a mapped guest range, or nullptr when it is not mapped.
    std::byte* Mapped(std::uint32_t address, std::size_t length);

    // The machine that executed the trap. A handler that has to call back into
    // guest code (pthread_once) needs it.
    A32Machine& Machine() { return machine_; }

    // Guest address of this thread's errno cell. bionic keeps it in slot 2 of
    // the thread pointer block, so it follows the calling thread, not the process.
    std::uint32_t ErrnoCell() const;

private:
    bool ReadString(std::uint32_t address, std::size_t max_length, bool require_terminator,
                    std::string& out) const;

    A32Machine& machine_;
    GuestArena& arena_;
};

using HleFunction = std::function<void(HleCall&)>;

// Resolves imported symbols to guest stubs that trap into host code.
//
// Each imported name gets an 8-byte stub: `SVC #id` followed by `BX LR`. Calling
// code jumps there through the PLT/GOT like any other function; the trap runs the
// host implementation and the `BX LR` returns to the caller. SVC #0 stays
// reserved for "halt the JIT".
class HleRegistry {
public:
    HleRegistry(A32Machine& machine, GuestArena& arena, std::uint32_t stub_base);

    // Maps and fills the stub page, and installs the SVC handler on the machine.
    bool Prepare(std::string* error);

    void Add(std::string name, HleFunction function);

    // Guest memory for an imported *variable* rather than a function, such as
    // `stdout`. The import resolves to the returned address, so guest code can
    // load and store the cell like any other global.
    std::uint32_t AddData(std::string name, std::uint32_t size = 4);

    // Anonymous guest memory of at least `size` bytes, 16-byte aligned. Pages are
    // mapped on demand, so this can be called before Prepare().
    std::uint32_t AllocateData(std::uint32_t size);

    GuestArena& Arena() { return arena_; }

    // Guest address of the stub for `name`, or 0 if this name is not implemented.
    std::uint32_t Resolve(std::string_view name) const;

    // Name of the most recent HLE call. A hang report is far more useful with the
    // import that was executing than with a bare PC. Returns a copy: the caller
    // is usually another thread, and Dispatch must not hand out a reference to a
    // string it is about to rewrite.
    std::string LastFunctionName() const;

    // The first four arguments of that call: a fault inside an import is almost
    // always a bad pointer in one of these.
    std::array<std::uint32_t, 4> LastFunctionArgs() const;

    // The last few imports, oldest first. A crash deep inside a library is
    // usually explained by which platform calls led to it. With a thread id the
    // list holds only that thread's calls, each prefixed with where it was made.
    std::vector<std::string> RecentFunctions() const;
    std::vector<std::string> RecentFunctions(std::int32_t thread) const;

    // Per-thread tails of the recent-call ring, newest first: one string per
    // thread, "tid: name@pc ...". Used by the hang watchdog.
    std::vector<std::string> ThreadTails(std::size_t per_thread) const;

    // Adds an entry to the same ring from outside the HLE table, so a JNI
    // dispatch interleaves with the imports that led to it. The JNI bridge
    // identifies an env call by its table index, and formatting the index into a
    // string on the guest's own thread made every JNI call allocate and take a
    // lock; the ring keeps the number and the name is rendered on demand.
    void RecordJniCall(std::uint32_t index, std::uint32_t pc);

    // Returns a block to the data area. `address` must be one AllocateData
    // handed out; anything else is ignored. Guest code releases strings through
    // ReleaseStringUTFChars, and without reuse a startup-long conversation
    // between the guest and the framework runs the area dry.
    void ReleaseData(std::uint32_t address);

    std::size_t Count() const { return functions_.size(); }

    // The graphics calls that build a shader or issue a draw, as one compact
    // line of name=count pairs. Counted per function on the call path, read
    // here.
    std::string CallSummary() const;

private:
    struct RecentCall;
    bool Dispatch(std::uint32_t svc, A32Machine& machine);
    bool DispatchIndex(std::uint32_t index, A32Machine& machine);
    void CensusCall(std::uint32_t index) const;
    void Record(std::uint32_t index, std::uint32_t pc, bool jni) const;
    std::string DescribeRecent(const RecentCall& entry) const;

    A32Machine& machine_;
    GuestArena& arena_;
    std::uint32_t stub_base_;
    std::uint32_t data_cursor_ = 0;
    std::vector<HleFunction> functions_;
    std::unordered_map<std::string, std::uint32_t> by_name_;
    std::unordered_map<std::string, std::uint32_t> data_by_name_;
    struct DataBlock {
        std::uint32_t address = 0;
        std::uint32_t size = 0;
    };
    std::mutex data_mutex_;
    std::vector<DataBlock> data_free_;
    std::unordered_map<std::uint32_t, std::uint32_t> data_live_;
    std::vector<std::string> names_;
    // Prepare() can be called again after more functions are added: the stub
    // page grows with the table, but the SVC handler is installed only once.
    bool handler_installed_ = false;
    // The most recent call, for the watchdog: written by Dispatch and read from
    // another thread, so the fields are atomics instead of a locked snapshot.
    std::atomic<std::uint32_t> last_index_{0xFFFFFFFFu};
    std::array<std::atomic<std::uint32_t>, 4> last_args_{};
    static constexpr std::size_t kRecentCount = 512;
    struct RecentCall {        std::int32_t thread = 0;
        std::uint32_t pc = 0;
        std::uint32_t index = 0;
        bool jni = false;
    };
    // A ring of plain records: formatting a name per call would allocate on the
    // guest's thread, and the hang report is the only reader.
    mutable std::array<RecentCall, kRecentCount> recent_{};
    mutable std::atomic<std::uint64_t> recent_index_{0};
    // Per-function call counts. Allocated when the table is complete; a function
    // added later simply is not counted.
    mutable std::unique_ptr<std::atomic<std::uint32_t>[]> counts_;
    mutable std::size_t counts_size_ = 0;
    // The platform shims dispatch by name, and a trampoline always hands over
    // the address of the same name string. Resolving it through a string hash on
    // every GL call is pure overhead, so the address is the key of a direct
    // mapped cache and a hit takes one atomic load. A slot packs
    // (name address << 32) | (index + 1); zero means empty, 0xFFFFFFFF is a
    // name that is not implemented.
    static constexpr std::size_t kNameCacheSlots = 1024;
    mutable std::array<std::atomic<std::uint64_t>, kNameCacheSlots> name_cache_{};
    mutable std::mutex name_cache_mutex_;
};

// Registers the functions Armbat currently implements: the C memory and string
// routines and the ARM EABI integer division helpers that compiled ARMv7 code
// cannot do without.
void RegisterStandardFunctions(HleRegistry& registry);

// The address a guest passed to abort(), recorded so the container can name
// the function it came from. An ARMv7 image has no unwinder here.
void NoteGuestAbort(std::uint32_t return_address);
std::uint32_t LastGuestAbort();

// The Android platform libraries: assets, configuration, window, looper, input
// and EGL. They need the archive the guest was started from, hence the setter.
void SetGuestApk(GuestApk* apk);
void RegisterAndroidFunctions(HleRegistry& registry);

// GLES 2.0 entry points, reached through eglGetProcAddress.
void RegisterGlesFunctions(HleRegistry& registry);

// OpenSL ES. Unlike the flat entry points these hand the guest objects with
// vtables of their own, so they are built here instead of forwarded to a host
// engine.
void RegisterOpenslesFunctions(HleRegistry& registry);

// Runs a guest function on a host thread that owns a guest context. The audio
// engine needs it: OpenSL ES delivers buffer-queue callbacks on a thread of its
// own, and an engine's mixer only refills the queue from there. The container is
// the only place that can hand a host thread a guest context, so it injects the
// runner.
using GuestCallRunner =
    std::function<bool(std::uint32_t entry, const std::vector<std::uint32_t>& args)>;
void SetGuestCallRunner(GuestCallRunner runner);

// Stops the thread that delivers those callbacks. Called before the container
// tears down the arena that thread runs on.
void StopGuestCallThread();

// libm, the rest of libc that platform libraries call, and liblog. The bionic
// path needs this for liblog alone: its shim traps __android_log_print by name,
// and without the registration the guest's own diagnostics go nowhere.
void RegisterExtraFunctions(HleRegistry& registry);

// logd drops a process's own lines once it logs faster than its share allows,
// and the lines it drops are the ones that explain a startup problem. A
// diagnostic category that repeats is capped instead: the first `limit` calls
// are logged and the rest are silently counted.
bool LogAllowed(const char* category, unsigned limit);

// Counts of the graphics calls that build a shader and issue a draw, as one
// compact line: whether a program was ever created and a frame ever drawn.
std::string GraphicsCallSummary();

// The registry a fault report reads: a guest that dies deep inside a library is
// usually explained by the platform calls that led there, and those are only
// known by the registry that recorded them.
void SetDiagnosticRegistry(HleRegistry* registry);

// The last `count` platform calls of one thread, oldest first, each with the
// guest address it was made from. Empty when that thread never made one.
std::vector<std::string> RecentCallsOfThread(std::int32_t thread, std::size_t count);

}  // namespace armbat
