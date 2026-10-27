#include "armbat/guest_container.h"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <initializer_list>
#include <mutex>
#include <unordered_map>

#include <android/log.h>

#include "armbat/log.h"
#include <dynarmic/interface/exclusive_monitor.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "armbat/a32_machine.h"
#include "armbat/bionic_boot.h"
#include "armbat/dynamic_loader.h"
#include "armbat/elf32_loader.h"
#include "armbat/guest_apk.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_cpu.h"
#include "armbat/guest_heap.h"
#include "armbat/guest_sysroot.h"
#include "armbat/handles.h"
#include "armbat/guest_kernel.h"
#include "armbat/guest_libc.h"
#include "armbat/guest_modules.h"
#include "armbat/guest_pthread.h"
#include "armbat/guest_signal.h"
#include "armbat/guest_stdio.h"
#include "armbat/crash_handler.h"
#include "armbat/hle.h"
#include "guest_unwind.h"
#include "guest_attach.h"
#include "armbat/jni_bridge.h"

namespace armbat {
namespace {

// The container's address space inside the guest arena. It is laid out once and
// never changes: a library loaded for one APK must not collide with the heap,
// the kernel's mmap region or a thread stack.
constexpr std::uint32_t kTrapAddr = 0x00f00000;
constexpr std::uint32_t kStubAddr = 0x00e00000;
constexpr std::uint32_t kDataAddr = 0x00500000;
constexpr std::uint32_t kMainStackAddr = 0x00800000;
constexpr std::uint32_t kMainStackSize = 0x00040000;
constexpr std::uint32_t kHeapBase = 0x02000000;
constexpr std::uint32_t kHeapSize = 0x04000000;      // 64 MiB
constexpr std::uint32_t kDlopenBase = 0x06000000;    // images grow upward
// A 32-bit guest is meant to have the whole 4 GiB to spend, and a modern game
// spends it: the linker maps each library's whole reservation, jemalloc keeps
// arenas of hundreds of megabytes, and every thread stack is two. A 256 MiB
// window ran out while the menu was still loading -- pthread_create began
// answering "Out of memory" -- so the window is most of the address space.
constexpr std::uint32_t kMmapBase = 0x20000000;
constexpr std::uint32_t kMmapEnd = 0xC0000000;
constexpr std::uint32_t kBrkBase = 0xC0000000;       // 256 MiB of brk
constexpr std::uint32_t kBrkEnd = 0xD0000000;
constexpr std::uint32_t kThreadStackBase = 0xD0000000;
constexpr std::uint32_t kThreadStackSize = 1u << 20;  // bionic's default
constexpr std::uint32_t kThreadStackCount = 64;

constexpr std::uint32_t kHaltInstruction = 0xEFFFFFFFu;
constexpr std::uint64_t kUnlimitedTicks = 1ull << 60;
// Initializers run with a budget: a library that spins in its constructor must
// return control instead of freezing the caller.
constexpr std::uint64_t kInitializerTicks = 4'000'000'000ull;

// The ARM kernel's helper page, at the fixed address every 32-bit process on a
// 32-bit kernel could reach. It is not a library: the kernel mapped one page at
// 0xffff0000 and put a handful of routines at its top -- a memory barrier is a
// `blx 0xffff0fa0`, a compare-and-swap is `blx 0xffff0fc0`. Code compiled for an
// old ARM ABI calls them, and the archive does: creating a world runs MC's
// leveldb environment setup, which barriers through that page and then fetches
// from an address this process never maps.
//
// The kernel here is 64-bit and maps no such page, so the container provides it,
// at the same addresses and with the same contract, as ARM code the JIT runs
// like any other guest code.
constexpr std::uint32_t kKuserPageAddr = 0xffff0000;

// The contexts the guest lends to platform threads. They live in guest memory
// because the lenders write them, and the container reads them through the same
// mapping; the page is between the main stack and the stub region, which nothing
// else claims.
constexpr std::uint32_t kWorkerJobsAddr = 0x00d00000;
constexpr std::uint32_t kWorkerSlots = 8;

// Mirror of `struct armbat_context` in runtime/guest/attach.c: the two sides
// talk through one page, so the layout is a contract, not an implementation
// detail.
struct WorkerContext {
    std::uint32_t ready;
    std::uint32_t tls;
    std::uint32_t sp;
    std::uint32_t park;
};

static_assert(sizeof(WorkerContext) == 16, "the guest lender stub expects this layout");
static_assert(kWorkerSlots * sizeof(WorkerContext) <= 0x1000, "the contexts must fit one page");

bool InstallKernelHelpers(GuestArena& arena) {
    if (!arena.Map(kKuserPageAddr, 0x1000)) {
        return false;
    }
    std::vector<std::uint32_t> page(0x1000 / 4, 0);
    const auto put = [&page](std::uint32_t address, std::initializer_list<std::uint32_t> words) {
        std::uint32_t offset = (address - kKuserPageAddr) / 4;
        for (const std::uint32_t word : words) {
            if (offset < page.size()) {
                page[offset++] = word;
            }
        }
    };
    // __kuser_memory_barrier: the barrier, then the return.
    put(0xffff0fa0, {0xF57FF05F /* dmb sy */, 0xE12FFF1E /* bx lr */});
    // __kuser_cmpxchg(oldval = r0, newval = r1, ptr = r2), 0 on success and 1
    // when the value had changed. The arguments are the kernel's, not an
    // invented order: libgcc and old bionic call this page as
    // `__kernel_cmpxchg(oldval, newval, ptr)`, so a helper that reads through r0
    // dereferences the old value as a pointer -- address zero for every
    // compare-against-null, which is exactly how a 2012 game touches this page.
    put(0xffff0fc0, {
        0xE1923F9F,  // ldrex r3, [r2]
        0xE1530000,  // cmp   r3, r0
        0x1A000002,  // bne   0xffff0fd8
        0xE1823F91,  // strex r3, r1, [r2]
        0xE3530000,  // cmp   r3, #0
        0x03A00000,  // moveq r0, #0
        0x13A00001,  // movne r0, #1
        0xE12FFF1E,  // bx    lr
    });
    // __kuser_get_tls: the thread pointer, the register bionic's own __get_tls
    // reads.
    put(0xffff0fe0, {0xEE1D0F70 /* mrc p15, 0, r0, c13, c0, 3 */, 0xE12FFF1E /* bx lr */});
    // __kuser_helper_version: the highest helper this page implements.
    put(0xffff0ff0, {5});
    std::memcpy(arena.Ptr(kKuserPageAddr), page.data(), page.size() * sizeof(std::uint32_t));
    ARMBAT_LOG_INFO("Armbat",
                        "kernel helpers: barrier, cmpxchg and tls at 0x%08x",
                        kKuserPageAddr);
    return true;
}

// Reads a whole file into memory. Used to inspect an application library's
// dynamic symbols: on the bionic path the guest's linker owns the mapping, so
// the file is the only place its export names can be read from.
std::vector<unsigned char> ReadWholeFile(const std::string& path) {
    std::vector<unsigned char> bytes;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return bytes;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
        ::close(fd);
        return bytes;
    }
    bytes.resize(static_cast<std::size_t>(info.st_size));
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        const ssize_t count = ::read(fd, bytes.data() + filled, bytes.size() - filled);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (count == 0) {
            break;
        }
        filled += static_cast<std::size_t>(count);
    }
    ::close(fd);
    bytes.resize(filled);
    return bytes;
}

// Replaces a small text file in the container's own directory. The files that
// describe the emulated device to the guest are written this way: they are
// regenerated on every boot, so a stale one from an older container never lies
// about the core the JIT currently implements.
bool WriteTextFile(const std::string& path, const std::string& text) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = ::write(fd, text.data() + written, text.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    ::close(fd);
    return true;
}

}  // namespace

struct GuestContainer::Impl {
    bool Start(std::string* error) {
        if (ready) {
            return true;
        }
        if (!arena.Ok()) {
            *error = arena.Error() != nullptr ? arena.Error() : "cannot reserve the arena";
            return false;
        }
        if (!arena.Map(kTrapAddr, 0x1000) || !arena.Map(kMainStackAddr, kMainStackSize) ||
            !arena.Map(kDataAddr, 0x1000)) {
            *error = "cannot map the trap page, the stack or the data page";
            return false;
        }
        std::memcpy(arena.Ptr(kTrapAddr), &kHaltInstruction, sizeof(kHaltInstruction));
        if (!InstallKernelHelpers(arena)) {
            *error = "cannot map the kernel helper page";
            return false;
        }

        // The crash handler is installed before the first Jit exists. dynarmic
        // puts its own SIGSEGV handler on top when a Jit is built -- fastmem
        // needs it to catch an access to a page the container never committed
        // and recompile that site for the callback path -- and it chains to
        // whatever was installed before it. Ours therefore stays the last word
        // on a real host crash while a fastmem miss is handled where it is
        // understood.
        InstallCrashHandler(nullptr);
        machine = std::make_unique<A32Machine>(arena);
        SetCrashMachine(machine.get());
        machine->SetSp(kMainStackAddr + kMainStackSize - 64);
        machine->SetReturnTrap(kTrapAddr);

        heap = std::make_unique<GuestHeap>(arena, kHeapBase, kHeapSize);
        std::string heap_error;
        if (!heap->Init(&heap_error)) {
            *error = "heap: " + heap_error;
            return false;
        }

        hle = std::make_unique<HleRegistry>(*machine, arena, kStubAddr);
        RegisterStandardFunctions(*hle);
        heap->RegisterFunctions(*hle);

        stdio = std::make_unique<GuestStdio>(*hle);
        std::string stdio_error;
        if (!stdio->Install(&stdio_error)) {
            *error = "stdio: " + stdio_error;
            return false;
        }

        threads = std::make_unique<GuestPthread>(*hle);
        std::uint32_t thread_pointer = 0;
        std::string pthread_error;
        GuestThreadConfig thread_config;
        thread_config.stack_base = kThreadStackBase;
        thread_config.stack_size = kThreadStackSize;
        thread_config.stack_count = kThreadStackCount;
        thread_config.main_machine = machine.get();
        if (!threads->Install(thread_config, &thread_pointer, &pthread_error)) {
            *error = "pthread: " + pthread_error;
            return false;
        }
        machine->SetTlsBase(thread_pointer);

        // Signal delivery has to exist before the first guest thread does: the
        // guest's collectors and runtimes stop threads with pthread_kill from
        // the moment they initialize, and the main thread is one of the threads
        // they stop.
        InitGuestSignals(arena);
        machine->SetSafePointHandler(&DeliverGuestSignals);
        RegisterLiveGuestThread(*machine);

        libc = std::make_unique<GuestLibc>(*hle, *heap);
        std::string libc_error;
        if (!libc->Install(&libc_error)) {
            *error = "libc: " + libc_error;
            return false;
        }

        loader = std::make_unique<Elf32Loader>(arena);
        AddSystemLibraries(*loader);
        dynamic = std::make_unique<DynamicLoader>(*machine, arena, *loader, kDlopenBase);
        dynamic->RegisterFunctions(*hle);

        loader->SetImportResolver([this](std::string_view name) {
            if (const std::uint32_t address = dynamic->Resolve(name); address != 0) {
                return address;
            }
            return hle->Resolve(name);
        });

        // Before the archive: its C++ runtime resolves `_Unwind_*` when the image
        // is mapped, and the only implementation available is this one.
        std::string unwind_error;
        if (dynamic->OpenEmbedded(guest_unwind::kUnwindImage,
                                  sizeof(guest_unwind::kUnwindImage), "libarmbat_unwind.so",
                                  &unwind_error) == 0) {
            *error = "unwind: " + unwind_error;
            return false;
        }

        std::string hle_error;
        if (!hle->Prepare(&hle_error)) {
            *error = "hle: " + hle_error;
            return false;
        }

        kernel = std::make_unique<GuestKernel>(
            *machine, arena, KernelConfig{kMmapBase, kMmapEnd, kBrkBase, kBrkEnd});
        std::string kernel_error;
        if (!kernel->Prepare(&kernel_error)) {
            *error = "kernel: " + kernel_error;
            return false;
        }
        // bionic's pthread_create is a clone() syscall. Each guest thread gets a
        // host thread of its own, running its own machine over the same arena;
        // the kernel hands over the parent's register file and the child's own
        // stack and TLS.
        kernel->SetThreadSpawner([this](const GuestKernel::CloneStart& start) -> std::uint32_t {
            A32Machine::Config config = machine->Settings();
            std::uint32_t processor = 0;
            {
                const std::lock_guard lock(processor_mutex);
                if (!free_processors.empty()) {
                    processor = free_processors.back();
                    free_processors.pop_back();
                } else {
                    processor = next_processor_id.fetch_add(1);
                    if (processor >= A32Machine::Config::kProcessorLimit) {
                        ARMBAT_LOG_ERROR("Armbat",
                                            "clone: refused, %u guest threads already exist",
                                            processor);
                        return 0;
                    }
                }
            }
            config.processor_id = processor;
            auto child = std::make_shared<A32Machine>(arena, config);
            child->SetTickBudget(kUnlimitedTicks);
            child->CopySvcHandlersFrom(*machine);
            child->CopySafePointHandlerFrom(*machine);
            child->SetReturnTrap(machine->ReturnTrap());
            {
                const std::lock_guard lock(spawned_mutex);
                spawned.push_back(child);
            }
            // clone() has to return the child's tid, so the parent waits for the
            // new thread to exist rather than for it to start working.
            auto tid_ready = std::make_shared<std::promise<std::uint32_t>>();
            std::future<std::uint32_t> tid = tid_ready->get_future();
            std::thread([this, child, start, tid_ready] {
                const std::uint32_t self = static_cast<std::uint32_t>(::gettid());
                tid_ready->set_value(self);
                kernel->PrepareThread(start.clear_tid);
                ARMBAT_LOG_INFO("Armbat",
                                    "clone: tid=%u entry=0x%08x sp=0x%08x tls=0x%08x thumb=%d",
                                    self, start.entry, start.sp, start.tls, start.thumb ? 1 : 0);
                armbat::RegisterLiveGuestThread(*child);
                child->EnterClonedThread(start.regs, start.entry, start.sp, start.tls,
                                         start.thumb);
                armbat::UnregisterLiveGuestThread(*child);
                if (!child->LastError().empty()) {
                    ARMBAT_LOG_ERROR("Armbat", "thread %u: %s", self,
                                        child->LastError().c_str());
                } else {
                    ARMBAT_LOG_INFO("Armbat", "thread %u: exited", self);
                }
                kernel->FinishThread();
                // The thread is gone: hand its exclusive-monitor slot back and
                // drop the container's reference, which is what releases the
                // machine with its JIT code cache. Without this a game that
                // replaces worker threads keeps every dead thread's translated
                // code resident and eventually runs out of monitor slots.
                {
                    const std::uint32_t processor = child->ProcessorId();
                    if (const auto monitor = child->Settings().exclusive_monitor) {
                        monitor->ClearProcessor(processor);
                    }
                    {
                        const std::lock_guard lock(spawned_mutex);
                        for (auto it = spawned.begin(); it != spawned.end(); ++it) {
                            if (it->get() == child.get()) {
                                spawned.erase(it);
                                break;
                            }
                        }
                    }
                    {
                        const std::lock_guard lock(processor_mutex);
                        free_processors.push_back(processor);
                    }
                }
                // The thread may have called into the framework, which attaches
                // it to the Java runtime on first use.
                DetachHostThread();
            }).detach();
            return tid.get();
        });

        jni = std::make_unique<JniBridge>(*owner);
        std::string jni_error;
        if (!jni->Prepare(&jni_error)) {
            *error = "jni: " + jni_error;
            return false;
        }

        ARMBAT_LOG_INFO("Armbat", "arena: base=%p size=0x%llx heap=0x%08x",
                            static_cast<void*>(arena.Base()),
                            static_cast<unsigned long long>(arena.Size()), kHeapBase);
        // A fault report names the platform calls that led to it; the registry
        // that records them is the one the guest world dispatches through.
        SetDiagnosticRegistry(hle.get());
        ready = true;

        // A guest call that never returns looks exactly like a healthy process
        // until the platform kills it, so say where it is every few seconds.
        std::thread([this] {
            int ticks = 0;
            unsigned long long last_dispatches = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                ++ticks;
                if (kernel != nullptr) {
                    kernel->DumpParkedFutexes();
                }
                std::string trail;
                const std::vector<std::string> calls = hle->RecentFunctions();
                const std::size_t begin = calls.size() > 40 ? calls.size() - 40 : 0;
                for (std::size_t i = begin; i < calls.size(); ++i) {
                    trail += calls[i];
                    trail += ' ';
                }
                // A process that is idle looks the same from here every five
                // seconds; only say so once a minute unless something happened.
                // The guest's own calls are what counts as something happening:
                // a guest that went quiet after init has stopped calling in,
                // while one still working keeps entering the kernel even when it
                // has nothing to say to the platform.
                const unsigned long long dispatches = kernel->Calls();
                const bool progressed = dispatches != last_dispatches;
                last_dispatches = dispatches;
                if (!progressed && ticks % 12 != 0) {
                    continue;
                }
                ARMBAT_LOG_INFO("Armbat", "guest: calls=%llu syscall=%llu imports: %s",
                                    dispatches,
                                    static_cast<unsigned long long>(kernel->LastNumber()),
                                    trail.c_str());
                // The first quiet tick carries the paths the guest asked for
                // last: an init that stopped usually stopped on a file.
                if (!progressed) {
                    const std::string paths = kernel->RecentPaths();
                    if (!paths.empty()) {
                        ARMBAT_LOG_INFO("Armbat", "guest paths:%s",
                                            paths.c_str());
                    }
                }
                const std::string graphics = GraphicsCallSummary();
                if (!graphics.empty()) {
                    ARMBAT_LOG_INFO("Armbat", "gfx counts: %s",
                                        graphics.c_str());
                }
                for (const std::string& tail : hle->ThreadTails(6)) {
                    ARMBAT_LOG_INFO("Armbat", "thread %s", tail.c_str());
                }
                const char* main_hle = machine->HleCallName();
                ARMBAT_LOG_INFO("Armbat",
                                    "guest pc=0x%08x hle=%s busy=%d halt=%s syscall=%u calls=%llu",
                                    machine->Reg(15), main_hle != nullptr ? main_hle : "-",
                                    machine->Busy() ? 1 : 0,
                                    machine->LastHaltReasonName().c_str(), machine->LastSyscall(),
                                    static_cast<unsigned long long>(machine->SyscallCount()));
                if (machine->Busy()) {
                    // Same rule as a fault report: only words that sit behind a
                    // call are frames, so the line names the chain instead of
                    // every pointer the stack happens to hold.
                    const std::uint32_t sp = machine->Reg(13);
                    std::string frames;
                    int named = 0;
                    for (std::uint32_t address : machine->GuestReturnAddresses(sp, 0x1000)) {
                        frames += " " + armbat::DescribeGuestAddress(address);
                        if (++named == 16) {
                            break;
                        }
                    }
                    ARMBAT_LOG_INFO("Armbat", "main frames:%s",
                                        frames.c_str());
                }
                for (const std::string& location : GuestThreadLocations()) {
                    ARMBAT_LOG_INFO("Armbat", "guest %s", location.c_str());
                }
                RequestGuestSample();
                machine->SampleNow();
            }
        }).detach();
        return true;
    }

    // With the bionic runtime the guest owns its own stack frames from the boot
    // program onward, so a host-initiated call must never reset them: CallNested
    // runs on a nested JIT and leaves the boot frame intact.
    std::uint32_t Invoke(std::uint32_t function, std::initializer_list<std::uint32_t> args) {
        if (bionic) {
            return machine->CallNested(function, args);
        }
        return machine->CallFunction(function, args);
    }

    // A distinct exclusive-monitor slot per guest thread. Threads that exit hand
    // theirs back, so a game that re-creates workers does not exhaust them.
    std::uint32_t TakeProcessor() {
        const std::lock_guard lock(processor_mutex);
        if (!free_processors.empty()) {
            const std::uint32_t processor = free_processors.back();
            free_processors.pop_back();
            return processor;
        }
        const std::uint32_t processor = next_processor_id.fetch_add(1);
        if (processor >= A32Machine::Config::kProcessorLimit) {
            return A32Machine::Config::kProcessorLimit;
        }
        return processor;
    }

    // Real guest threads whose only job is to lend their stack and their TLS
    // block to platform threads: an ART thread has both, but they are the
    // host's, and ARM code cannot use them. Created through bionic's own
    // pthread_create -- not through an import the loader might answer with the
    // platform table -- so what they own is what the guest's libc made.
    void StartGuestContexts() {
        if (context_count != 0) {
            return;
        }
        std::string error;
        const std::uint32_t base = dynamic->OpenEmbedded(
            guest_attach::kAttachImage, sizeof(guest_attach::kAttachImage), "libarmbat_attach.so",
            &error);
        if (base == 0) {
            ARMBAT_LOG_ERROR("Armbat", "contexts: %s", error.c_str());
            return;
        }
        const std::uint32_t lender = dynamic->Resolve("armbat_lender");
        const std::uint32_t libc = bionic->Dlopen("libc.so", 2 /*RTLD_NOW*/);
        const std::uint32_t create =
            libc != 0 ? bionic->Dlsym(libc, "pthread_create") : 0;
        if (lender == 0 || create == 0) {
            ARMBAT_LOG_ERROR("Armbat",
                                "contexts: lender=0x%08x pthread_create=0x%08x", lender, create);
            return;
        }
        if (!arena.Map(kWorkerJobsAddr, 0x1000)) {
            ARMBAT_LOG_ERROR("Armbat", "contexts: cannot map the slots");
            return;
        }
        auto* contexts = reinterpret_cast<WorkerContext*>(arena.Ptr(kWorkerJobsAddr));
        std::memset(contexts, 0, kWorkerSlots * sizeof(WorkerContext));
        // pthread_t outputs, above the contexts in the same page.
        const std::uint32_t thread_ids = kWorkerJobsAddr + 0x100;
        int started = 0;
        for (std::uint32_t i = 0; i < kWorkerSlots; ++i) {
            const std::uint32_t context = kWorkerJobsAddr + i * sizeof(WorkerContext);
            const std::uint32_t answer =
                Invoke(create, {thread_ids + i * 4, 0, lender, context});
            if (static_cast<std::int32_t>(answer) != 0) {
                ARMBAT_LOG_ERROR("Armbat",
                                    "contexts: pthread_create failed, %d", static_cast<int>(answer));
                break;
            }
            ++started;
        }
        contexts_addr = kWorkerJobsAddr;
        // The lender publishes its pointers from its own thread, so a context is
        // only usable once it says so.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        int ready = 0;
        while (ready < started && std::chrono::steady_clock::now() < deadline) {
            ready = 0;
            for (int i = 0; i < started; ++i) {
                if (__atomic_load_n(&contexts[i].ready, __ATOMIC_ACQUIRE) != 0) {
                    ++ready;
                }
            }
            if (ready < started) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        context_count = ready;
        ARMBAT_LOG_INFO("Armbat", "contexts: %d guest threads at 0x%08x",
                            context_count, contexts_addr);
    }

    // A machine for a platform thread, built on a lent context. Its own machine
    // is what keeps two platform threads from re-entering one, and the lent
    // stack and TLS are what make its ARM frames legal.
    A32Machine* AttachMachine(std::uint32_t tls, std::uint32_t sp) {
        A32Machine::Config config = machine->Settings();
        config.processor_id = TakeProcessor();
        auto attached_machine = std::make_shared<A32Machine>(arena, config);
        attached_machine->SetTickBudget(kUnlimitedTicks);
        attached_machine->CopySvcHandlersFrom(*machine);
        attached_machine->CopySafePointHandlerFrom(*machine);
        attached_machine->SetReturnTrap(machine->ReturnTrap());
        attached_machine->SetTlsBase(tls);
        attached_machine->SetSp(sp);
        A32Machine* result = attached_machine.get();
        // The watchdog reports what a parked guest thread was doing; a machine
        // that a platform thread runs is one of those threads for as long as the
        // process lives, so it is registered and never handed back.
        RegisterLiveGuestThread(*result);
        {
            const std::lock_guard lock(context_mutex);
            attached.push_back(std::move(attached_machine));
        }
        return result;
    }

    // The machine this platform thread runs guest code on: a context of its own,
    // built on the stack and TLS of a parked guest thread. Null when every
    // context is taken, which the caller reports instead of running the call on
    // a machine another thread is using.
    A32Machine* AttachHere() {
        if (context_count == 0) {
            return nullptr;
        }
        auto* contexts = reinterpret_cast<WorkerContext*>(arena.Ptr(contexts_addr));
        int index = -1;
        {
            const std::lock_guard lock(context_mutex);
            if (next_context < context_count) {
                index = next_context++;
            }
        }
        if (index < 0) {
            ARMBAT_LOG_ERROR("Armbat",
                                "contexts: tid=%d needs one and all %d are taken", ::gettid(),
                                context_count);
            return nullptr;
        }
        const WorkerContext& context = contexts[index];
        A32Machine* attached_machine = AttachMachine(context.tls, context.sp);
        ARMBAT_LOG_INFO("Armbat",
                            "contexts: tid=%d took slot %d tls=0x%08x sp=0x%08x", ::gettid(),
                            index, context.tls, context.sp);
        return attached_machine;
    }

    bool StartBionic(const std::string& cache_dir, std::uint32_t target_sdk, std::string* error) {
        if (bionic) {
            // A previous attempt may have built the sysroot and mapped the linker
            // without reaching the handshake. Retrying the whole world from a
            // half-initialized arena cannot work, so report the stale failure.
            if (!bionic->Booted()) {
                *error = "bionic boot did not complete";
                return false;
            }
            return true;
        }
        sysroot = std::make_unique<GuestSysroot>(GuestSysroot::DefaultDirectory(cache_dir));
        if (!sysroot->Materialize(error)) {
            return false;
        }

        armbat_root = cache_dir + "/armbat";
        app_lib_dir = armbat_root + "/app-libs";
        std::error_code library_error;
        std::filesystem::create_directories(app_lib_dir, library_error);
        // The guest asks the kernel what CPU it runs on, and the host's answers
        // describe an AArch64 core: a different architecture, the ARMv8 crypto
        // extensions and 32 advanced SIMD registers. An ARMv7 library that reads
        // them either refuses to start (Unity's loader requires the "neon" token
        // and HWCAP bit) or picks a vector path from a feature list the JIT does
        // not implement. The two files below describe the emulated core instead,
        // and are mapped over the kernel's own paths by the path mapper below.
        const int cores =
            std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
        const std::string cpu_info_path = cache_dir + "/cpuinfo";
        const std::string cpu_topology_path = cache_dir + "/cpu-topology";
        if (!WriteTextFile(cpu_info_path, GuestCpuInfo(cores)) ||
            !WriteTextFile(cpu_topology_path, "0-" + std::to_string(cores - 1) + "\n")) {
            *error = "cannot write the guest cpu description";
            return false;
        }
        // The application's data directory. A guest that keeps its settings or
        // its saves writes to /data/data/<package>; the container answers that
        // path from the tree the shell chose instead of the real one, which it
        // cannot write. The shell points it at the guest's folder on the shared
        // storage; a caller that sets nothing (the headless selftest) gets the
        // container's private directory.
        if (data_root.empty()) {
            data_root = std::filesystem::path(cache_dir).parent_path().string() + "/files/guest";
        }
        std::error_code directory_error;
        std::filesystem::create_directories(data_root, directory_error);
        // The standard subdirectories exist on a device before an application
        // starts, and code that writes straight into one of them does not create
        // it first.
        for (const char* name : {"files", "cache", "obb", "shared_prefs", "databases"}) {
            std::filesystem::create_directories(data_root + "/" + name, directory_error);
        }
        // The real data directory the platform gave this process. An application's
        // Java code and its native code meet in the middle of a guest: Java reads
        // the paths the framework answers and hands them to native code, so those
        // absolute host paths arrive here as guest paths. They are already where
        // the file belongs; mapping them again would nest the guest tree inside
        // itself and put every save one directory deeper than its reader.
        const std::string app_dir = std::filesystem::path(cache_dir).parent_path().string();
        const std::string host_data_dir = app_dir + "/";
        // The platform has two spellings for this directory -- /data/user/0/<name>
        // and /data/data/<name> -- and a guest library may rewrite one into the
        // other before it opens a file the framework handed it; Unity does that
        // with its own archive path. The second spelling has to reach the
        // container's real files too, or the archive is invisible to the guest.
        // Only paths that exist there are answered that way: a guest's own
        // saves and settings keep landing in its tree under data_root.
        const std::string data_alias =
            "/data/data/" + std::filesystem::path(app_dir).filename().string() + "/";
        const std::string guest_package = package_name;
        const std::string guest_obb_root = obb_root;
        kernel->SetPathMapper([this, host_data_dir, data_alias, cache_dir, cpu_info_path,
                               cpu_topology_path, guest_package,
                               guest_obb_root](const std::string& path) -> std::string {
            // A library name resolved through the guest's class loader lands in
            // the container's stand-in directory, whose files are 64-bit: a
            // guest that resolves a name itself and hands the path to its own
            // loader must get the armv7 library it actually is.
            const std::string stub_root = cache_dir + "/native-stubs/";
            if (path.compare(0, stub_root.size(), stub_root) == 0) {
                const std::string rest = path.substr(stub_root.size());
                const std::size_t slash = rest.find('/');
                return app_lib_dir + "/" +
                       (slash == std::string::npos ? rest : rest.substr(slash + 1));
            }
            if (path.compare(0, host_data_dir.size(), host_data_dir) == 0) {
                return path;
            }
            if (path.compare(0, data_alias.size(), data_alias) == 0) {
                std::string real = host_data_dir + path.substr(data_alias.size());
                std::error_code missing;
                if (std::filesystem::exists(real, missing)) {
                    return real;
                }
            }
            // The CPU the JIT emulates, not the one under the phone's heatsink.
            if (path == "/proc/cpuinfo") {
                return cpu_info_path;
            }
            for (const std::string_view topology : {"/sys/devices/system/cpu/present",
                                                    "/sys/devices/system/cpu/possible",
                                                    "/sys/devices/system/cpu/online"}) {
                if (path == topology) {
                    return cpu_topology_path;
                }
            }
            if (const auto host = sysroot->HostPath(path)) {
                return *host;
            }
            // The guest's /armbat tree, one host directory: the class loader
            // library path and LD_LIBRARY_PATH both name it, and the linker
            // resolves those through realpath(), which needs every component of
            // the path -- including the parent directory -- to exist.
            if (path == "/armbat") {
                return armbat_root;
            }
            constexpr std::string_view prefix = "/armbat/";
            if (path.compare(0, prefix.size(), prefix) == 0) {
                return armbat_root + "/" + path.substr(prefix.size());
            }
            for (const std::string_view data_prefix : {"/data/data/", "/data/user/0/"}) {
                if (path.compare(0, data_prefix.size(), data_prefix) == 0) {
                    const std::string rest = path.substr(data_prefix.size());
                    const std::size_t slash = rest.find('/');
                    return data_root +
                           (slash == std::string::npos ? std::string() : rest.substr(slash));
                }
            }
            // A guest sees the device's shared storage, not the container's
            // folder names. The two Android/<kind>/<package> trees belong to the
            // one application that is running, so they land in the guest's own
            // folders there; everything else is the device's shared storage.
            // Games that hardcode /sdcard/... reach the files the user put in
            // place instead of an empty directory of the platform's own.
            if (path == "/sdcard") {
                return "/storage/emulated/0";
            }
            for (const std::string_view shared :
                 {"/sdcard/", "/storage/emulated/0/", "/storage/self/primary/"}) {
                if (path.compare(0, shared.size(), shared) != 0) {
                    continue;
                }
                const std::string rest = path.substr(shared.size());
                if (!guest_package.empty()) {
                    for (const auto& [prefix, root] :
                         {std::pair{"Android/data/" + guest_package, data_root},
                          std::pair{"Android/obb/" + guest_package, guest_obb_root}}) {
                        if (root.empty() || rest.compare(0, prefix.size(), prefix) != 0) {
                            continue;
                        }
                        if (rest.size() == prefix.size() || rest[prefix.size()] == '/') {
                            return root + rest.substr(prefix.size());
                        }
                    }
                }
                return "/storage/emulated/0/" + rest;
            }
            return path;
        });
        bionic = std::make_unique<BionicBoot>(*machine, arena);
        kernel->SetExecutablePath("/system/bin/armbat_boot");
        kernel->SetBootReadyHandler([this](std::uint32_t api) { bionic->OnReady(api); });
        const auto started = std::chrono::steady_clock::now();
        auto stamp = [&started](const char* what) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            ARMBAT_LOG_INFO("Armbat", "startup: %s at %lld ms", what,
                                static_cast<long long>(elapsed.count()));
        };
        if (!bionic->Boot(error)) {
            return false;
        }
        stamp("boot");
        // A real device tells the process's linker which SDK the application
        // targets (the framework calls this on startup). Without it the linker
        // assumes its own API level and switches off every legacy behaviour:
        // it rejects ARMv5-era libraries with text relocations, and it stops
        // repairing DT_NEEDED entries that name an absolute path instead of a
        // soname. Both are the difference between "the archive's own library
        // loads" and "has text relocations".
        bionic->SetTargetSdk(target_sdk);
        // The platform libraries are not the device's: they are trampolines into
        // this table. Register the API implementations before the linker starts
        // resolving the application's imports through them.
        RegisterExtraFunctions(*hle);
        stamp("extra");
        RegisterAndroidFunctions(*hle);
        stamp("android");
        RegisterGlesFunctions(*hle);
        stamp("gles");
        // The table was empty when the registry prepared before the boot, so the
        // stubs eglGetProcAddress hands out have to be written now that it is not.
        if (!hle->Prepare(error)) {
            return false;
        }
        stamp("stubs");
        // A native reached from a platform thread has nowhere to run until these
        // exist, and the archive reaches one as soon as it initializes.
        StartGuestContexts();
        stamp("contexts");
        return true;
    }

    GuestArena arena{GuestArena::kDefaultSize};
    std::unique_ptr<A32Machine> machine;
    std::unique_ptr<GuestHeap> heap;
    std::unique_ptr<HleRegistry> hle;
    std::unique_ptr<GuestStdio> stdio;
    std::unique_ptr<GuestPthread> threads;
    std::unique_ptr<GuestLibc> libc;
    std::unique_ptr<Elf32Loader> loader;
    std::unique_ptr<DynamicLoader> dynamic;
    std::unique_ptr<GuestApk> apk;
    GuestContainer* owner = nullptr;
    std::unique_ptr<GuestKernel> kernel;
    std::unique_ptr<JniBridge> jni;
    std::unique_ptr<GuestSysroot> sysroot;
    std::unique_ptr<BionicBoot> bionic;
    // The host directory behind the guest's /armbat: its libraries and nothing
    // else, so the guest tree is the only thing the link path reveals.
    std::string armbat_root;
    std::string app_lib_dir;
    // Libraries exporting JNI_OnLoad, in load order, waiting for the guest
    // class loader to exist.
    std::vector<std::pair<std::string, std::uint32_t>> on_load;
    // Java_* entry points of the libraries the bionic path loaded, for the JNI
    // bridge to register with the host runtime.
    std::vector<GuestExport> bionic_exports;
    // Guest threads created by clone(). Each owns its machine; the machines stay
    // alive for as long as their thread runs, and the entry is dropped when the
    // thread exits so that the JIT code cache of a dead thread is released with
    // it.
    std::mutex spawned_mutex;
    std::vector<std::shared_ptr<A32Machine>> spawned;
    std::atomic<std::uint32_t> next_processor_id{1};
    // Exclusive-monitor slots of threads that have exited. Every guest thread
    // needs a slot of its own, and a game that re-creates worker threads would
    // run out of them in a long session; they are handed back here instead.
    std::mutex processor_mutex;
    std::vector<std::uint32_t> free_processors;

    // Guest contexts lent to platform threads: the stacks and TLS blocks of
    // guest threads that do nothing but park, plus the machines the container
    // builds on top of them. A platform thread takes one on its first native
    // call and keeps it, because the guest keeps state in it -- errno, the
    // allocator's thread cache -- and a thread that swapped contexts would lose
    // it between two calls.
    std::uint32_t contexts_addr = 0;
    int context_count = 0;
    std::mutex context_mutex;
    int next_context = 0;
    std::vector<std::shared_ptr<A32Machine>> attached;

    std::uint32_t native_activity = 0;
    std::uint32_t native_window = 0;
    std::uint32_t native_callbacks = 0;
    // The AInputQueue the archive was handed; the container's own object, only
    // ever passed back to it.
    std::uint32_t native_input_queue = 0;
    bool native_input_sent = false;
    // Handles the guest's own linker returned for the archive's libraries. The
    // HLE loader is empty on this path, so anything that has to look a symbol up
    // afterwards - ANativeActivity_onCreate above all - asks the guest.
    std::vector<std::uint32_t> bionic_handles;
    // Where /data/data/<package> lands on the host. Created once, before the
    // guest can write anything. Set from the shell when the guest's tree lives
    // on the shared storage, otherwise derived from the cache directory.
    std::string data_root;
    // Where /sdcard/Android/obb/<package> lands, and the package the archive
    // was built as: the two halves of the path mapping for expansion files.
    std::string obb_root;
    std::string package_name;
    bool ready = false;
};

GuestContainer::GuestContainer() : impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    // The audio engine calls back into the guest from a thread of its own, so
    // the container hands OpenSL ES a way to reach a guest context there.
    SetGuestCallRunner([this](std::uint32_t entry, const std::vector<std::uint32_t>& args) {
        std::uint32_t result = 0;
        std::uint32_t high = 0;
        return RunOnGuestThread(entry, args, &result, &high);
    });
}

GuestContainer::~GuestContainer() {
    // That thread runs on the arena, so it has to be gone before the arena is.
    StopGuestCallThread();
    SetGuestCallRunner(nullptr);
}

bool GuestContainer::Start(std::string* error) {
    return impl_->Start(error);
}

bool GuestContainer::Started() const {
    return impl_->ready;
}

void GuestContainer::SetGuestStorage(const std::string& data_root, const std::string& obb_root,
                                     const std::string& package_name) {
    impl_->data_root = data_root;
    impl_->obb_root = obb_root;
    impl_->package_name = package_name;
}

A32Machine& GuestContainer::Machine() { return *impl_->machine; }
GuestArena& GuestContainer::Arena() { return impl_->arena; }
GuestHeap& GuestContainer::Heap() { return *impl_->heap; }
HleRegistry& GuestContainer::Imports() { return *impl_->hle; }
DynamicLoader& GuestContainer::Modules() { return *impl_->dynamic; }
Elf32Loader& GuestContainer::Loader() { return *impl_->loader; }
JniBridge& GuestContainer::Jni() { return *impl_->jni; }

bool GuestContainer::RunOnGuestThread(std::uint32_t entry, const std::vector<std::uint32_t>& args,
                                      std::uint32_t* result, std::uint32_t* result_high) {
    Impl& impl = *impl_;
    // One guest context per platform thread, taken on its first native call and
    // kept: the guest keeps errno and the allocator's thread cache in the TLS
    // block, so a thread that swapped contexts would lose them between calls.
    struct ThreadContext {
        Impl* owner = nullptr;
        A32Machine* machine = nullptr;
    };
    thread_local ThreadContext mine;
    if (mine.owner != &impl) {
        mine.owner = &impl;
        mine.machine = nullptr;
    }
    if (mine.machine == nullptr) {
        mine.machine = impl.AttachHere();
        if (mine.machine == nullptr) {
            return false;
        }
    }
    A32Machine& target = *mine.machine;
    if (target.Executing()) {
        target.CallNested(entry, args);
    } else {
        target.CallFunction(entry, args);
    }
    *result = target.Reg(0);
    *result_high = target.Reg(1);
    return true;
}
const std::vector<GuestContainer::GuestExport>& GuestContainer::BionicExports() const {
    return impl_->bionic_exports;
}

std::string GuestContainer::NotifyNative(int index, std::uint32_t argument, bool has_argument) {
    if (impl_->native_callbacks == 0) {
        return std::string();
    }
    std::uint32_t callback = 0;
    std::memcpy(&callback, impl_->arena.Ptr(impl_->native_callbacks + index * 4),
                sizeof(callback));
    if (callback == 0) {
        return std::string();
    }
    ARMBAT_LOG_INFO("Armbat", "native activity: callback %d enter 0x%08x",
                        index, callback);
    // The platform may deliver a callback while guest code is still on this
    // thread (a Java method the guest called reaches the host activity
    // lifecycle again), and a running Jit cannot be re-entered. The callback
    // belongs to the machine of the calling thread.
    A32Machine& target =
        CurrentGuestMachine() != nullptr ? *CurrentGuestMachine() : *impl_->machine;
    const bool nested = target.Executing();
    ARMBAT_LOG_INFO("Armbat",
                        "native activity: callback %d %s tid=%d current=%p machine=%p busy=%d owner=%d",
                        index, nested ? "nested" : "top level", ::gettid(),
                        static_cast<const void*>(CurrentGuestMachine()), static_cast<void*>(&target),
                        impl_->machine->Busy() ? 1 : 0,
                        static_cast<int>(impl_->machine->BusyThread()));
    if (has_argument) {
        if (nested) {
            target.CallNested(callback, {impl_->native_activity, argument});
        } else {
            target.CallFunction(callback, {impl_->native_activity, argument});
        }
    } else {
        if (nested) {
            target.CallNested(callback, {impl_->native_activity});
        } else {
            target.CallFunction(callback, {impl_->native_activity});
        }
    }
    char line[160];
    std::snprintf(line, sizeof(line), "native activity: callback %d at 0x%08x%s\n", index,
                  callback, target.LastError().c_str());
    return line;
}

std::string GuestContainer::SetNativeWindow(void* window) {
    if (window == nullptr) {
        if (impl_->native_window != 0) {
            const std::uint32_t previous = impl_->native_window;
            impl_->native_window = 0;
            return NotifyNative(10, previous, true);  // onNativeWindowDestroyed
        }
        return std::string();
    }

    impl_->native_window = RegisterHostHandle(window);
    std::string report = NotifyNative(7, impl_->native_window, true);   // created
    report += NotifyNative(8, impl_->native_window, true);              // resized
    report += NotifyNative(9, impl_->native_window, true);              // redraw needed
    // The input queue follows the window. The glue's callback waits for its own
    // thread to pick the queue up, and that thread only reaches the point where
    // it does so once the window has been accepted -- exactly the order the
    // platform uses on a device, where the queue arrives with the focused
    // window and never during onCreate.
    if (impl_->native_input_queue != 0 && !impl_->native_input_sent) {
        impl_->native_input_sent = true;
        report += NotifyNative(11, impl_->native_input_queue, true);    // input queue created
    }
    return report;
}

std::string GuestContainer::StartNativeActivity(const std::string& data_path) {
    std::string report;

    std::uint32_t entry = 0;
    // The archive's own libraries are loaded by the guest's linker on the
    // bionic path, where the HLE loader never saw them, so the entry point comes
    // from the guest's dlsym first.
    for (const std::uint32_t handle : impl_->bionic_handles) {
        entry = impl_->bionic != nullptr ? impl_->bionic->Dlsym(handle, "ANativeActivity_onCreate") : 0;
        if (entry != 0) {
            break;
        }
    }
    for (const LoadedLibrary& library : impl_->dynamic->Libraries()) {
        if (entry != 0) {
            break;
        }
        const std::uint32_t symbol = impl_->dynamic->Symbol(library.base, "ANativeActivity_onCreate");
        if (symbol != 0) {
            entry = symbol;
        }
    }
    if (entry == 0) {
        return "native activity: the archive exports no ANativeActivity_onCreate\n";
    }

    const auto allocate = [this](std::uint32_t size) { return impl_->heap->Allocate(size); };
    const auto write_word = [this](std::uint32_t at, std::uint32_t value) {
        std::memcpy(impl_->arena.Ptr(at), &value, sizeof(value));
    };
    const auto write_string = [&](const std::string& text) -> std::uint32_t {
        const std::uint32_t address = allocate(static_cast<std::uint32_t>(text.size() + 1));
        if (address != 0) {
            std::memcpy(impl_->arena.Ptr(address), text.c_str(), text.size() + 1);
        }
        return address;
    };

    // struct ANativeActivity, ten 32-bit fields, and its sixteen callbacks. The
    // callbacks are filled in by the archive itself during on_create; the
    // container only has to hand it writable memory.
    constexpr std::uint32_t kActivitySize = 40;
    constexpr std::uint32_t kCallbackCount = 16;
    const std::uint32_t activity = allocate(kActivitySize);
    const std::uint32_t callbacks = allocate(kCallbackCount * 4);
    if (activity == 0 || callbacks == 0) {
        return "native activity: out of guest memory\n";
    }
    std::memset(impl_->arena.Ptr(activity), 0, kActivitySize);
    std::memset(impl_->arena.Ptr(callbacks), 0, kCallbackCount * 4);

    // The platform hands a native activity its internal data directory, and that
    // is the only place its files can live. The shell points it at the guest's
    // tree on the shared storage, which is also what the guest's own context
    // answers for getFilesDir and getObbDir: a library that compares these paths
    // with the framework's must see the same strings.
    const std::string root =
        !data_path.empty() ? data_path
        : !impl_->data_root.empty()
            ? impl_->data_root
            : std::string("/data/data/") +
                  (impl_->package_name.empty() ? "dev.armbat" : impl_->package_name);
    const std::uint32_t internal_path = write_string(root + "/files");
    const std::uint32_t external_path = write_string(root + "/files");
    // The platform never leaves obbPath null: it points at the app's expansion
    // directory, empty or not. A guest that wraps every one of these fields in a
    // std::string throws on a null.
    const std::string obb = !impl_->obb_root.empty() ? impl_->obb_root : root + "/obb";
    const std::uint32_t obb_path = write_string(obb);

    write_word(activity + 0, callbacks);
    write_word(activity + 4, impl_->jni->GuestVm());
    write_word(activity + 8, impl_->jni->GuestEnv());
    write_word(activity + 12, 0);  // clazz
    write_word(activity + 16, internal_path);
    write_word(activity + 20, external_path);
    write_word(activity + 24, 24);  // sdkVersion
    write_word(activity + 28, 0);   // instance
    write_word(activity + 32, 1);   // asset manager: the HLE ignores the handle
    write_word(activity + 36, obb_path);

    ARMBAT_LOG_INFO("Armbat",
                        "native activity struct: callbacks=0x%08x vm=0x%08x env=0x%08x clazz=0x%08x "
                        "sdk=%d instance=0x%08x assets=0x%08x",
                        callbacks, impl_->jni->GuestVm(), impl_->jni->GuestEnv(), 0, 24, 0, 1);

    impl_->native_activity = activity;
    impl_->native_callbacks = callbacks;

    const std::uint32_t result = impl_->Invoke(entry, {activity, 0, 0});

    {
        std::uint32_t vm = 0;
        std::uint32_t env = 0;
        std::uint32_t instance = 0;
        std::memcpy(&vm, impl_->arena.Ptr(activity + 4), sizeof(vm));
        std::memcpy(&env, impl_->arena.Ptr(activity + 8), sizeof(env));
        std::memcpy(&instance, impl_->arena.Ptr(activity + 28), sizeof(instance));
        ARMBAT_LOG_INFO("Armbat",
                            "native activity after on_create: vm=0x%08x env=0x%08x instance=0x%08x",
                            vm, env, instance);

    }

    std::string table = "native activity: callbacks";
    for (int index = 0; index < 16; ++index) {
        std::uint32_t value = 0;
        std::memcpy(&value, impl_->arena.Ptr(callbacks + index * 4), sizeof(value));
        char cell[16];
        std::snprintf(cell, sizeof(cell), " %08x", value);
        table += cell;
    }
    ARMBAT_LOG_INFO("Armbat", "%s", table.c_str());

    // The platform hands a native activity its input queue: the glue keeps the
    // pointer and drives it from its own loop. It is delivered with the window,
    // not at onCreate: the glue answers the callback by waiting for its own
    // thread to accept the queue, and before the window arrives that thread is
    // still starting the engine -- the wait would never return. The pointer is
    // prepared here and handed over in SetNativeWindow.
    impl_->native_input_queue = impl_->hle->AllocateData(8);

    char line[192];
    std::snprintf(line, sizeof(line), "native activity: on_create 0x%08x returned 0x%08x (%s)\n",
                  entry, result, impl_->machine->LastError().c_str());
    report += line;
    return report;
}

std::string GuestContainer::DescribeAddress(std::uint32_t address) const {
    for (const LoadedLibrary& library : impl_->dynamic->Libraries()) {
        if (address < library.base || address >= library.base + library.image_size) {
            continue;
        }
        std::uint32_t nearest = 0;
        std::string name = "?";
        for (const auto& [symbol, value] : impl_->loader->Exports(library)) {
            if (value <= address && value >= nearest) {
                nearest = value;
                name = symbol;
            }
        }
        char line[512];
        std::snprintf(line, sizeof(line), "%s+0x%x in %s", library.soname.c_str(),
                      address - library.base, name.c_str());
        return line;
    }
    return "outside the loaded images";
}

std::string GuestContainer::CallOnLoad() {
    std::string report = "JNI_OnLoad:";
    for (const auto& [soname, entry] : impl_->on_load) {
        const std::uint32_t version = impl_->Invoke(entry, {impl_->jni->GuestVm(), 0});
        char line[96];
        std::snprintf(line, sizeof(line), " %s=0x%08x", soname.c_str(), version);
        report += line;
    }
    impl_->on_load.clear();
    return report + "\n";
}

std::string GuestContainer::LoadApk(const std::string& apk_path, const std::string& cache_dir,
                                    std::uint32_t target_sdk, std::string* error) {
    std::string report = "apk: " + apk_path + "\n";

    auto apk_holder = std::make_unique<GuestApk>();
    GuestApk& apk = *apk_holder;
    std::string apk_error;
    if (!apk.Open(apk_path, &apk_error)) {
        *error = apk_error;
        return report + "open failed: " + apk_error + "\n";
    }
    const std::vector<std::string> libraries = apk.NativeLibraries();
    report += "entries: " + std::to_string(apk.Entries().size()) + "\n";
    if (const GuestApk::Entry* dex = apk.Find("classes.dex")) {
        report += "classes.dex: " + std::to_string(dex->size / 1024) + " KB\n";
    }
    report += "arm32 libraries: " + std::to_string(libraries.size()) + "\n";
    if (libraries.empty()) {
        // Not a failure: plenty of applications are pure Java. The shell says so
        // instead of pretending the native side did anything.
        report += "no lib/armeabi-v7a or lib/armeabi entries\n";
        return report;
    }
    report += "abi: " + apk.AbiDirectory() + "\n";

    impl_->apk = std::move(apk_holder);
    SetGuestApk(impl_->apk.get());

#if ARMBAT_GUEST_RUNTIME_BIONIC
    if (!impl_->StartBionic(cache_dir, target_sdk, error)) {
        char line[192];
        std::snprintf(line, sizeof(line), "bionic: syscalls=%llu last=0x%x unimplemented=0x%x\n",
                      static_cast<unsigned long long>(impl_->kernel->Calls()),
                      static_cast<std::uint32_t>(impl_->kernel->LastNumber()),
                      impl_->kernel->LastUnimplemented());
        return report + line + "bionic: failed: " + *error + impl_->kernel->RecentPaths() + "\n";
    }
    report += impl_->bionic->Describe() + "\n";

    // Startup cost is the container's own problem: a phone kills an activity
    // whose main thread is busy for five seconds, and one number per step says
    // which step to attack.
    const auto launch_started = std::chrono::steady_clock::now();
    auto launch_stamp = [&launch_started](const char* what) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - launch_started);
        ARMBAT_LOG_INFO("Armbat", "startup: %s at %lld ms", what,
                            static_cast<long long>(elapsed.count()));
    };
    launch_stamp("booted");

    std::vector<GuestApk::ExtractedNative> extracted;
    std::string extract_error;
    if (!apk.ExtractNativeLibraries(impl_->app_lib_dir, &extracted, &extract_error)) {
        *error = extract_error;
        return report + "bionic: extract failed: " + extract_error + "\n";
    }
    launch_stamp("libraries extracted");
    report += "bionic: app libraries " + std::to_string(extracted.size()) + "\n";
    impl_->on_load.clear();
    impl_->bionic_exports.clear();
    impl_->bionic_handles.clear();

    // Maps every name an archive library answers to: the name of its file and
    // the soname other images put in DT_NEEDED.
    std::unordered_map<std::string, std::size_t> by_name;
    for (std::size_t index = 0; index < extracted.size(); ++index) {
        by_name.emplace(extracted[index].file, index);
        by_name.emplace(extracted[index].soname, index);
    }
    // The archive-local dependencies of one library, by index.
    std::vector<std::vector<std::size_t>> dependencies(extracted.size());
    for (std::size_t index = 0; index < extracted.size(); ++index) {
        for (const std::string& needed : extracted[index].needed) {
            const auto found = by_name.find(needed);
            if (found != by_name.end() && found->second != index) {
                dependencies[index].push_back(found->second);
            }
        }
    }

    // Opens one library: the guest linker maps it, the container collects the
    // Java_* entry points the JNI bridge has to register.
    auto open_library = [&](std::size_t index) {
        const GuestApk::ExtractedNative& library = extracted[index];
        // The file is what the linker can open; the soname is what the image
        // calls itself and is only useful in reports. Archives ship both forms
        // and the two routinely differ.
        const std::string path = impl_->app_lib_dir + "/" + library.file;
        const auto library_started = std::chrono::steady_clock::now();
        const std::uint32_t handle = impl_->bionic->Dlopen(path, 2 /*RTLD_NOW*/);
        if (handle == 0) {
            const std::string reason = impl_->bionic->LastError();
            char line[320];
            std::snprintf(line, sizeof(line), "  %-28s dlopen failed: %s\n", library.file.c_str(),
                          reason.c_str());
            report += line;
            *error = library.file + ": " + reason;
            return false;
        }
        const std::uint32_t on_load = impl_->bionic->Dlsym(handle, "JNI_OnLoad");
        if (on_load != 0) {
            impl_->on_load.emplace_back(library.soname, on_load);
        }
        impl_->bionic_handles.push_back(handle);
        // Java entry points exported by name: the JNI bridge registers them with
        // the host runtime so the archive's Java side can call its own native
        // methods. Their names come from the file, their addresses from the
        // guest's linker.
        const std::vector<unsigned char> image = ReadWholeFile(path);
        std::size_t java_symbols = 0;
        for (const std::string& name : ReadDynamicFunctionNames(image.data(), image.size())) {
            if (name.rfind("Java_", 0) != 0) {
                continue;
            }
            const std::uint32_t address = impl_->bionic->Dlsym(handle, name);
            if (address != 0) {
                impl_->bionic_exports.push_back(GuestExport{name, address});
                ++java_symbols;
            }
        }
        char line[192];
        const std::string label = library.soname == library.file
                                      ? library.file
                                      : library.file + " (" + library.soname + ")";
        std::snprintf(line, sizeof(line), "  %-28s handle=0x%08x  %s  natives=%u\n",
                      label.c_str(), handle, on_load != 0 ? "JNI_OnLoad" : "-",
                      static_cast<unsigned>(java_symbols));
        report += line;
        const auto library_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - library_started);
        ARMBAT_LOG_INFO("Armbat", "startup: %s linked in %lld ms",
                            label.c_str(), static_cast<long long>(library_ms.count()));
        return true;
    };

    // A library that names a sibling in DT_NEEDED leaves the lookup of that
    // name to the guest linker, and the archive order is not a dependency
    // order: the entries come out of the package alphabetically, so libFarm2.so
    // is opened long before the libstlp.so it needs. When the lookup does not
    // resolve -- it did not on the first launch of that archive, then resolved
    // on the next one -- the library is skipped, its Java_* entry points never
    // reach the JNI bridge and the guest dies with UnsatisfiedLinkError the
    // moment it calls one. Opening the dependencies first, by the path the
    // container knows, takes the name lookup out of the picture entirely.
    std::vector<std::size_t> order;
    std::vector<char> visited(extracted.size(), 0);
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        if (visited[index]) {
            return;
        }
        visited[index] = 1;
        for (std::size_t dependency : dependencies[index]) {
            visit(dependency);
        }
        order.push_back(index);
    };
    for (std::size_t index = 0; index < extracted.size(); ++index) {
        visit(index);
    }

    std::vector<char> opened(extracted.size(), 0);
    std::vector<char> reported(extracted.size(), 0);
    // Two passes: the first honours the dependency order, the second retries
    // whatever is left so a dependency cycle or a name the linker refused the
    // first time still gets a second chance.
    for (int pass = 0; pass < 2; ++pass) {
        bool changed = false;
        for (std::size_t index : order) {
            if (opened[index] || reported[index]) {
                continue;
            }
            bool ready = true;
            for (std::size_t dependency : dependencies[index]) {
                if (!opened[dependency] && !reported[dependency]) {
                    ready = false;
                    break;
                }
            }
            if (!ready && pass == 0) {
                continue;
            }
            if (open_library(index)) {
                opened[index] = 1;
            } else {
                reported[index] = 1;
            }
            changed = true;
        }
        if (!changed) {
            break;
        }
    }
    launch_stamp("libraries linked");
    return report;
#endif

    impl_->machine->SetTickBudget(kInitializerTicks);
    std::vector<GuestApk::LoadedNative> loaded;
    std::string load_error;
    const bool complete =
        apk.LoadNativeLibraries(*impl_->dynamic, *impl_->loader, cache_dir, &loaded, &load_error);
    impl_->machine->SetTickBudget(kUnlimitedTicks);

    report += "mapped: " + std::to_string(loaded.size()) + " of " +
              std::to_string(libraries.size()) + "\n";
    impl_->on_load.clear();
    for (const GuestApk::LoadedNative& item : loaded) {
        const std::uint32_t on_load = impl_->dynamic->Symbol(item.base, "JNI_OnLoad");
        // The platform runs JNI_OnLoad as part of loading the library, and
        // libraries use it to cache the JavaVM and to look their own Java
        // classes up. FindClass goes through the archive's class loader, which
        // only exists once the Java side has handed it over, so the call waits
        // for RegisterExports to install it.
        if (on_load != 0) {
            impl_->on_load.emplace_back(item.soname, on_load);
        }
        char line[192];
        std::snprintf(line, sizeof(line), "  %-28s 0x%08x  %s\n", item.soname.c_str(), item.base,
                      on_load != 0 ? "JNI_OnLoad" : "-");
        report += line;
    }
    if (!complete) {
        report += "error: " + load_error + "\n";
        char import_line[160];
        const std::array<std::uint32_t, 4>& args = impl_->hle->LastFunctionArgs();
        std::snprintf(import_line, sizeof(import_line),
                      "import: %s(0x%08x, 0x%08x, 0x%08x, 0x%08x)\n",
                      impl_->hle->LastFunctionName().c_str(), args[0], args[1], args[2], args[3]);
        report += import_line;
        char init_line[192];
        std::snprintf(init_line, sizeof(init_line), "initializer: 0x%08x %s (%u of %u)\n",
                      impl_->loader->LastInitializer(),
                      DescribeAddress(impl_->loader->LastInitializer()).c_str(),
                      impl_->loader->LastInitializerIndex(),
                      impl_->loader->LastInitializerCount());
        report += init_line;
        std::string trail;
        for (const std::string& name : impl_->hle->RecentFunctions()) {
            trail += name;
            trail += ' ';
        }
        report += "trail: " + trail + "\n";
        if (const std::uint32_t abort_at = LastGuestAbort(); abort_at != 0) {
            report += "abort: " + DescribeAddress(abort_at) + "\n";
        }
        report += "halt: " + impl_->machine->LastHaltReasonName() + "\n";
        *error = load_error;
    }
    return report;
}

}  // namespace armbat
