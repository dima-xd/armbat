#include "armbat/guest_pthread.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <android/log.h>

#include "armbat/log.h"
#include <unistd.h>
#include <ctime>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "armbat/a32_machine.h"
#include "armbat/crash_handler.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_modules.h"
#include "armbat/guest_signal.h"
#include "armbat/hle.h"

namespace armbat {
namespace {

// bionic's arm32 TLS layout: the thread pointer is a small array, and the slots
// are self, thread id and errno. Guest code reaches them with `mrc p15, 0, ..,
// c13, c0, 3`, which the machine routes to A32Machine::TlsBase().
constexpr std::uint32_t kTlsBlockSize = 256;
constexpr std::uint32_t kTlsSlotSelf = 0;
constexpr std::uint32_t kTlsSlotThreadId = 1;
constexpr std::uint32_t kMaxKeys = 128;

constexpr int kSuccess = 0;
constexpr int kEsrch = 3;
constexpr int kEdeadlk = 35;
constexpr int kEinval = 22;
constexpr int kEagain = 11;
constexpr int kEtimedout = 110;
constexpr int kEbusy = 16;

// The mutex word: bit 0 is "locked", bit 1 is "waiters". The waiter bit is what
// lets unlock know that somebody is asleep on the futex and has to be woken;
// without it a waiter can go to sleep after the unlock and never be woken.
constexpr std::uint32_t kMutexLocked = 1;
constexpr std::uint32_t kMutexWaiters = 2;

// bionic packs the mutex type into bits 14-15 (PTHREAD_MUTEX_INITIALIZER is
// `type << 14`), so a statically initialised recursive mutex begins as 0x4000
// rather than zero. Only the low two bits are the lock state; the type bits,
// the shared bit and the owner/depth of a recursive mutex must survive
// lock/unlock unchanged, otherwise a mutex the guest considers free looks
// permanently held to us.
constexpr std::uint32_t kMutexStateMask = 3;
constexpr std::uint32_t kMutexTypeMask = 0xc000;
constexpr std::uint32_t kMutexTypeRecursive = 0x4000;
constexpr std::uint32_t kMutexSharedBit = 0x2000;
constexpr std::uint32_t kMutexDepthStep = 4;   // bits 2-13: recursion depth
constexpr std::uint32_t kMutexDepthMask = 0x3ffc;
constexpr int kMutexOwnerShift = 16;           // bits 16-31: owner tid

// Wait on `word` while it still holds `expected`. Returns true when the caller
// should re-read the word, false on timeout.
// Tracing is read once: getenv() walks the environment on every call, and these
// lines sit on the contended-lock path.
bool FutexTracing() {
    static const bool enabled = false;
    return enabled;
}

bool FutexWait(std::uint32_t* word, std::uint32_t expected,
               const struct timespec* timeout) {
    const long result = ::syscall(SYS_futex, reinterpret_cast<int*>(word), FUTEX_WAIT, expected,
                                  timeout, nullptr, 0);
    if (FutexTracing()) {
        ARMBAT_LOG_INFO("ArmbatPthread",
                            "futex wait %p == %u tid=%d -> %ld/%s", static_cast<void*>(word),
                            expected, ::gettid(), result,
                            result == 0 ? "woken" : (errno == EAGAIN ? "eagain" : "error"));
    }
    return result == 0;
}

void FutexWake(std::uint32_t* word, int count) {
    const long woken =
        ::syscall(SYS_futex, reinterpret_cast<int*>(word), FUTEX_WAKE, count, nullptr, nullptr, 0);
    if (FutexTracing()) {
        ARMBAT_LOG_INFO("ArmbatPthread", "futex wake %p x%d tid=%d -> %ld",
                            static_cast<void*>(word), count, ::gettid(), woken);
    }
}

}  // namespace

struct GuestPthread::State {
    struct Thread {
        std::uint32_t tls = 0;
        std::uint32_t thread_id = 0;
        std::uint32_t stack_top = 0;
        std::uint32_t stack_index = 0;
        bool stack_released = false;
        std::uint32_t result = 0;
        bool exited = false;   // pthread_exit recorded its own return value
        bool detached = false;
        bool finished = false;
        bool is_main = false;
        std::unique_ptr<A32Machine> machine;  // null for the main thread
        std::thread host;
        std::unordered_map<int, std::uint32_t> key_values;

        ~Thread() {
            // A detached thread owns its host thread; joining it here would be a
            // deadlock, so let it go.
            if (host.joinable()) {
                host.detach();
            }
        }
    };

    explicit State(HleRegistry& target) : registry(target), arena(target.Arena()) {}

    HleRegistry& registry;
    GuestArena& arena;

    std::mutex mutex;
    std::unordered_map<const A32Machine*, std::shared_ptr<Thread>> by_machine;
    std::vector<bool> key_used;
    std::uint32_t next_thread_id = 1;
    std::uint32_t main_tls = 0;
    A32Machine* main_machine = nullptr;

    GuestThreadConfig config;
    std::vector<bool> stack_used;
    // Who owns each stack slot. A slot is only handed out again when its owner
    // is gone, so a stale release can never wipe the stack of a live thread.
    std::vector<std::weak_ptr<Thread>> stack_owner;
    std::size_t next_processor_id = 1;

    void Register();
    bool CreateMainThread(std::string* error);
    std::shared_ptr<Thread> FindByMachine(const A32Machine& machine);
    std::shared_ptr<Thread> FindByHandle(std::uint32_t handle);

    // Allocates a TLS block, a stack and a machine. Returns null when the stack
    // range or the arena is exhausted.
    std::shared_ptr<Thread> CreateThread(std::string* error);
    void Run(std::shared_ptr<Thread> thread, std::uint32_t entry, std::uint32_t argument);
    void Finish(Thread& thread, std::uint32_t result);
    // Returns a thread's stack slot to the pool. Only the owner may do it: a
    // join of a stale handle must not recycle somebody else's stack.
    void ReleaseStack(Thread& thread);
};

namespace {

struct LiveGuestThread {
    std::int32_t tid = 0;
    A32Machine* machine = nullptr;
};

std::mutex g_live_mutex;
std::vector<LiveGuestThread> g_live;

// The lock word lives in guest memory; the host view is the same bytes.
std::uint32_t* Word(HleCall& call, std::uint32_t address) {
    return reinterpret_cast<std::uint32_t*>(call.Mapped(address, sizeof(std::uint32_t)));
}

int MutexLock(std::uint32_t address, HleCall& call) {
    std::uint32_t* word = Word(call, address);
    if (word == nullptr) {
        return kEinval;
    }
    const std::uint32_t tid = static_cast<std::uint32_t>(::gettid()) & 0xffffu;
    for (;;) {
        std::uint32_t current = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        const std::uint32_t type = current & kMutexTypeMask;
        const std::uint32_t state = current & kMutexStateMask;
        if (type == kMutexTypeRecursive && (current >> kMutexOwnerShift) == tid &&
            state == kMutexLocked) {
            // Owned by this thread already: recurse instead of deadlocking.
            if ((current & kMutexDepthMask) != kMutexDepthMask) {
                const std::uint32_t next = current + kMutexDepthStep;
                if (__atomic_compare_exchange_n(word, &current, next, false, __ATOMIC_ACQUIRE,
                                                __ATOMIC_RELAXED)) {
                    return kSuccess;
                }
            }
            continue;
        }
        if (state == 0) {
            std::uint32_t next = (current & ~kMutexStateMask) | kMutexLocked;
            if (type == kMutexTypeRecursive) {
                next = (next & ~(kMutexDepthMask | (0xffffu << kMutexOwnerShift))) |
                       (tid << kMutexOwnerShift);
            }
            if (__atomic_compare_exchange_n(word, &current, next, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return kSuccess;
            }
            continue;
        }
        if (state == kMutexLocked) {
            // Held with nobody waiting: set the waiter bit so unlock knows to
            // wake us, then sleep. Setting the bit is not an acquire.
            std::uint32_t next = current | kMutexWaiters;
            (void)__atomic_compare_exchange_n(word, &current, next, false, __ATOMIC_ACQUIRE,
                                              __ATOMIC_RELAXED);
            continue;
        }
        FutexWait(word, current, nullptr);
    }
}

int MutexTryLock(std::uint32_t address, HleCall& call) {
    std::uint32_t* word = Word(call, address);
    if (word == nullptr) {
        return kEinval;
    }
    const std::uint32_t tid = static_cast<std::uint32_t>(::gettid()) & 0xffffu;
    for (;;) {
        std::uint32_t current = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        const std::uint32_t type = current & kMutexTypeMask;
        const std::uint32_t state = current & kMutexStateMask;
        if (type == kMutexTypeRecursive && (current >> kMutexOwnerShift) == tid &&
            state == kMutexLocked) {
            const std::uint32_t next = current + kMutexDepthStep;
            if (__atomic_compare_exchange_n(word, &current, next, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return kSuccess;
            }
            continue;
        }
        if (state != 0) {
            return kEbusy;
        }
        std::uint32_t next = (current & ~kMutexStateMask) | kMutexLocked;
        if (type == kMutexTypeRecursive) {
            next = (next & ~(kMutexDepthMask | (0xffffu << kMutexOwnerShift))) |
                   (tid << kMutexOwnerShift);
        }
        if (__atomic_compare_exchange_n(word, &current, next, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return kSuccess;
        }
    }
}

int MutexUnlock(std::uint32_t address, HleCall& call) {
    std::uint32_t* word = Word(call, address);
    if (word == nullptr) {
        return kEinval;
    }
    const std::uint32_t tid = static_cast<std::uint32_t>(::gettid()) & 0xffffu;
    for (;;) {
        std::uint32_t current = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        const std::uint32_t type = current & kMutexTypeMask;
        if (type == kMutexTypeRecursive && (current >> kMutexOwnerShift) == tid &&
            (current & kMutexDepthMask) != 0) {
            const std::uint32_t next = current - kMutexDepthStep;
            if (__atomic_compare_exchange_n(word, &current, next, false, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED)) {
                return kSuccess;
            }
            continue;
        }
        const std::uint32_t next = current & (kMutexTypeMask | kMutexSharedBit);
        if (__atomic_compare_exchange_n(word, &current, next, false, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            if ((current & kMutexWaiters) != 0) {
                FutexWake(word, 1);
            }
            return kSuccess;
        }
    }
}

// The condition variable is a sequence counter: every signal bumps it, and a
// waiter sleeps while the counter is unchanged. Spurious wakeups are allowed by
// POSIX, so a single wake-up per wait is enough.
void CondSignal(std::uint32_t address, HleCall& call, int waiters) {
    std::uint32_t* word = Word(call, address);
    if (word == nullptr) {
        return;
    }
    __atomic_add_fetch(word, 1, __ATOMIC_RELEASE);
    FutexWake(word, waiters);
}

int CondWait(std::uint32_t cond, std::uint32_t mutex, HleCall& call,
             const struct timespec* timeout) {
    std::uint32_t* word = Word(call, cond);
    if (word == nullptr) {
        return kEinval;
    }
    const std::uint32_t sequence = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if (FutexTracing()) {
        ARMBAT_LOG_INFO("ArmbatPthread",
                            "cond wait 0x%08x seq=%u tid=%d", cond, sequence, ::gettid());
    }
    const int unlocked = MutexUnlock(mutex, call);
    if (unlocked != kSuccess) {
        return unlocked;
    }
    int status = kSuccess;
    if (timeout == nullptr) {
        while (__atomic_load_n(word, __ATOMIC_ACQUIRE) == sequence) {
            FutexWait(word, sequence, nullptr);
        }
    } else if (!FutexWait(word, sequence, timeout)) {
        status = kEtimedout;
    }
    const int relocked = MutexLock(mutex, call);
    if (FutexTracing()) {
        ARMBAT_LOG_INFO("ArmbatPthread",
                            "cond woke 0x%08x now=%u tid=%d", cond,
                            __atomic_load_n(word, __ATOMIC_ACQUIRE), ::gettid());
    }
    return relocked != kSuccess ? relocked : status;
}

// pthread_cond_timedwait takes an absolute time, and guests normally use
// CLOCK_REALTIME. The futex wants a relative timeout.
bool RelativeTimeout(const struct timespec& absolute, bool realtime, struct timespec* out) {
    struct timespec now{};
    if (::clock_gettime(realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    out->tv_sec = absolute.tv_sec - now.tv_sec;
    out->tv_nsec = absolute.tv_nsec - now.tv_nsec;
    if (out->tv_nsec < 0) {
        out->tv_nsec += 1000000000L;
        --out->tv_sec;
    }
    return out->tv_sec >= 0;
}

// A reader/writer lock: the top bit is the writer, the rest counts readers.
constexpr std::uint32_t kWriterBit = 0x80000000u;

int RwlockReadLock(std::uint32_t address, HleCall& call) {
    std::uint32_t* word = Word(call, address);
    if (word == nullptr) {
        return kEinval;
    }
    for (;;) {
        std::uint32_t current = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if ((current & kWriterBit) == 0) {
            if (__atomic_compare_exchange_n(word, &current, current + 1, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return kSuccess;
            }
            continue;
        }
        FutexWait(word, current, nullptr);
    }
}

int RwlockWriteLock(std::uint32_t address, HleCall& call) {
    std::uint32_t* word = Word(call, address);
    if (word == nullptr) {
        return kEinval;
    }
    for (;;) {
        std::uint32_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, kWriterBit, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return kSuccess;
        }
        expected = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        FutexWait(word, expected, nullptr);
    }
}

int RwlockUnlock(std::uint32_t address, HleCall& call) {
    std::uint32_t* word = Word(call, address);
    if (word == nullptr) {
        return kEinval;
    }
    const std::uint32_t current = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if ((current & kWriterBit) != 0) {
        __atomic_store_n(word, 0, __ATOMIC_RELEASE);
    } else {
        __atomic_sub_fetch(word, 1, __ATOMIC_RELEASE);
    }
    FutexWake(word, 0x7fffffff);
    return kSuccess;
}

}  // namespace

bool GuestPthread::State::CreateMainThread(std::string* error) {
    const std::uint32_t block = registry.AllocateData(kTlsBlockSize);
    if (block == 0) {
        if (error) *error = "cannot map the guest thread block";
        return false;
    }

    std::uint32_t slots[kTlsBlockSize / 4] = {};
    slots[kTlsSlotSelf] = block;
    slots[kTlsSlotThreadId] = block;
    std::memcpy(arena.Ptr(block), slots, sizeof(slots));

    auto thread = std::make_shared<Thread>();
    thread->tls = block;
    thread->thread_id = 0;  // bionic hands out ids to created threads only
    thread->is_main = true;
    thread->machine = nullptr;
    {
        const std::lock_guard lock(mutex);
        by_machine[main_machine] = thread;
    }
    main_tls = block;
    if (main_machine != nullptr) {
        main_machine->SetTlsBase(block);
    }
    return true;
}

std::shared_ptr<GuestPthread::State::Thread> GuestPthread::State::FindByMachine(
    const A32Machine& machine) {
    const std::lock_guard lock(mutex);
    const auto it = by_machine.find(&machine);
    return it == by_machine.end() ? nullptr : it->second;
}

std::shared_ptr<GuestPthread::State::Thread> GuestPthread::State::FindByHandle(
    std::uint32_t handle) {
    const std::lock_guard lock(mutex);
    for (const auto& [machine, thread] : by_machine) {
        if (thread->tls == handle) {
            return thread;
        }
    }
    return nullptr;
}

std::shared_ptr<GuestPthread::State::Thread> GuestPthread::State::CreateThread(
    std::string* error) {
    std::uint32_t stack_base = 0;
    std::uint32_t stack_index = 0;
    {
        // Stacks are recycled: a program that creates a thread per request must
        // not run out of the reserved range after the first N threads.
        const std::lock_guard lock(mutex);
        bool found = false;
        for (std::uint32_t index = 0; index < config.stack_count; ++index) {
            if (stack_used[index]) {
                continue;
            }
            const std::shared_ptr<Thread> owner = stack_owner[index].lock();
            if (owner && !owner->finished) {
                // A released stack whose owner is still running would be handed
                // out twice, so the index stays out of the pool. It repeats on
                // every creation, hence the cap.
                if (LogAllowed("stack-free", 8)) {
                    ARMBAT_LOG_WARN("Armbat",
                                        "stack index=%u flagged free but thread %u still runs",
                                        index, owner->thread_id);
                }
                continue;
            }
            stack_used[index] = true;
            stack_index = index;
            found = true;
            break;
        }
        if (!found) {
            if (error) *error = "guest thread stack range is exhausted";
            return nullptr;
        }
        stack_base = config.stack_base + stack_index * config.stack_size;
    }

    ARMBAT_LOG_INFO("Armbat",
                        "thread stack index=%u base=0x%08x size=0x%08x", stack_index, stack_base,
                        config.stack_size);
    if (!arena.Map(stack_base, config.stack_size)) {
        if (error) *error = "cannot map a guest thread stack";
        return nullptr;
    }
    std::memset(arena.Ptr(stack_base), 0, config.stack_size);

    const std::uint32_t tls = registry.AllocateData(kTlsBlockSize);
    if (tls == 0) {
        if (error) *error = "cannot map the guest thread block";
        return nullptr;
    }

    std::uint32_t slots[kTlsBlockSize / 4] = {};
    slots[kTlsSlotSelf] = tls;
    slots[kTlsSlotThreadId] = tls;
    std::memcpy(arena.Ptr(tls), slots, sizeof(slots));

    if (next_processor_id >= A32Machine::Config::kProcessorLimit) {
        *error = "no exclusive monitor slot left for another guest thread";
        return nullptr;
    }
    A32Machine::Config settings = config.main_machine->Settings();
    settings.processor_id = next_processor_id++;
    auto machine = std::make_unique<A32Machine>(arena, settings);
    machine->CopySvcHandlersFrom(*config.main_machine);
    machine->CopySafePointHandlerFrom(*config.main_machine);
    machine->SetReturnTrap(config.main_machine->ReturnTrap());

    auto thread = std::make_shared<Thread>();
    thread->tls = tls;
    thread->stack_top = (stack_base + config.stack_size - 16) & ~7u;
    thread->stack_index = stack_index;
    thread->machine = std::move(machine);
    {
        const std::lock_guard lock(mutex);
        thread->thread_id = next_thread_id++;
        by_machine[thread->machine.get()] = thread;
        stack_owner[stack_index] = thread;
    }
    return thread;
}

void GuestPthread::State::Run(std::shared_ptr<Thread> thread, std::uint32_t entry,
                              std::uint32_t argument) {
    thread->machine->SetTlsBase(thread->tls);
    thread->machine->SetSp(thread->stack_top);
    SetCrashMachine(thread->machine.get());
    RegisterLiveGuestThread(*thread->machine);
    ARMBAT_LOG_INFO("Armbat", "run start entry=0x%08x arg=0x%08x tid=%d",
                        entry, argument, ::gettid());
    const std::uint32_t result = thread->machine->CallFunction(entry, {argument});
    ARMBAT_LOG_INFO("Armbat", "run finished entry=0x%08x result=0x%08x tid=%d",
                        entry, result, ::gettid());
    UnregisterLiveGuestThread(*thread->machine);
    std::string trail;
    {
        const std::vector<std::string> calls = registry.RecentFunctions(::gettid());
        const std::size_t begin = calls.size() > 16 ? calls.size() - 16 : 0;
        for (std::size_t i = begin; i < calls.size(); ++i) {
            trail += " " + calls[i];
        }
    }
    ARMBAT_LOG_INFO("Armbat",
                        "guest thread exit: entry=0x%08x result=0x%08x error=%s tid=%d trail:%s",
                        entry, result, thread->machine->LastError().c_str(), ::gettid(),
                        trail.c_str());
    Finish(*thread, result);
    // The guest stack is dead as soon as the entry function returned; releasing
    // it here keeps a detached thread from parking a megabyte until the
    // process ends.
    ReleaseStack(*thread);
}

void GuestPthread::State::Finish(Thread& thread, std::uint32_t result) {
    const std::lock_guard lock(mutex);
    if (!thread.exited) {
        thread.result = result;
        thread.exited = true;
    }
    thread.finished = true;
    ARMBAT_LOG_INFO("Armbat",
                        "thread finish index=%u detached=%d released=%d tid=%d", thread.stack_index,
                        thread.detached ? 1 : 0, thread.stack_released ? 1 : 0, ::gettid());
    if (thread.detached) {
        for (auto it = by_machine.begin(); it != by_machine.end(); ++it) {
            if (it->second.get() == &thread) {
                by_machine.erase(it);
                break;
            }
        }
    }
}

void GuestPthread::State::ReleaseStack(Thread& thread) {
    const std::lock_guard lock(mutex);
    if (thread.is_main || thread.stack_released || thread.stack_index >= stack_used.size()) {
        return;
    }
    const std::shared_ptr<Thread> owner = stack_owner[thread.stack_index].lock();
    if (owner && owner.get() != &thread) {
        ARMBAT_LOG_WARN("Armbat",
                            "stack index=%u belongs to thread %u, thread %u may not release it",
                            thread.stack_index, owner->thread_id, thread.thread_id);
        return;
    }
    thread.stack_released = true;
    stack_used[thread.stack_index] = false;
    stack_owner[thread.stack_index].reset();
}

void GuestPthread::State::Register() {
    HleRegistry& registry = this->registry;
    State& self = *this;

    // --- identity -----------------------------------------------------------
    registry.Add("__get_tls", [&self](HleCall& call) {
        auto thread = self.FindByMachine(call.Machine());
        call.Return(thread ? thread->tls : self.main_tls);
    });
    registry.Add("pthread_self", [&self](HleCall& call) {
        auto thread = self.FindByMachine(call.Machine());
        call.Return(thread ? thread->tls : self.main_tls);
    });
    registry.Add("pthread_equal", [](HleCall& call) {
        call.Return(call.Arg(0) == call.Arg(1) ? 1u : 0u);
    });
    registry.Add("gettid", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::syscall(SYS_gettid)));
    });

    // --- creation -----------------------------------------------------------
    registry.Add("pthread_create", [&self](HleCall& call) {
        const std::uint32_t handle_out = call.Arg(0);
        const std::uint32_t entry = call.Arg(2);
        const std::uint32_t argument = call.Arg(3);
        ARMBAT_LOG_INFO("Armbat",
                            "pthread_create entry=0x%08x arg=0x%08x from lr=0x%08x tid=%d", entry,
                            argument, call.Machine().Reg(14), ::gettid());

        std::string error;
        auto thread = self.CreateThread(&error);
        if (!thread) {
            ARMBAT_LOG_ERROR("Armbat", "pthread_create failed: %s",
                                error.c_str());
            call.Return(kEagain);
            return;
        }
        const std::uint32_t handle = thread->tls;
        std::shared_ptr<Thread> shared = thread;
        thread->host = std::thread([&self, shared, entry, argument] {
            self.Run(shared, entry, argument);
        });
        call.CopyOut(handle_out, &handle, sizeof(handle));
        call.Return(kSuccess);
    });

    registry.Add("pthread_join", [&self](HleCall& call) {
        const std::uint32_t handle = call.Arg(0);
        const std::uint32_t result_out = call.Arg(1);
        auto thread = self.FindByHandle(handle);
        if (!thread) {
            call.Return(kEsrch);
            return;
        }
        if (thread->is_main) {
            call.Return(kEdeadlk);
            return;
        }
        auto current = self.FindByMachine(call.Machine());
        if (current && current->tls == handle) {
            call.Return(kEdeadlk);
            return;
        }
        {
            const std::lock_guard lock(self.mutex);
            if (thread->detached) {
                call.Return(kEinval);
                return;
            }
        }
        if (thread->host.joinable()) {
            thread->host.join();
        }
        const std::uint32_t result = thread->result;
        {
            const std::lock_guard lock(self.mutex);
            for (auto it = self.by_machine.begin(); it != self.by_machine.end(); ++it) {
                if (it->second == thread) {
                    self.by_machine.erase(it);
                    break;
                }
            }
        }
        self.ReleaseStack(*thread);
        if (result_out != 0) {
            call.CopyOut(result_out, &result, sizeof(result));
        }
        call.Return(kSuccess);
    });

    registry.Add("pthread_detach", [&self](HleCall& call) {
        auto thread = self.FindByHandle(call.Arg(0));
        if (!thread) {
            call.Return(kEsrch);
            return;
        }
        thread->detached = true;
        if (thread->finished && thread->host.joinable()) {
            thread->host.detach();
        }
        call.Return(kSuccess);
    });

    registry.Add("pthread_exit", [&self](HleCall& call) {
        auto thread = self.FindByMachine(call.Machine());
        if (thread) {
            const std::lock_guard lock(self.mutex);
            thread->result = call.Arg(0);
            thread->exited = true;
        }
        call.Machine().Stop();
        call.Return(0);
    });

    registry.Add("sched_yield", [](HleCall& call) {
        std::this_thread::yield();
        call.Return(kSuccess);
    });
    registry.Add("pthread_yield", [](HleCall& call) {
        std::this_thread::yield();
        call.Return(kSuccess);
    });

    // --- mutexes ------------------------------------------------------------
    registry.Add("pthread_mutexattr_init", [](HleCall& call) {
        if (std::uint32_t* word = Word(call, call.Arg(0))) {
            __atomic_store_n(word, 0u, __ATOMIC_RELEASE);
        }
        call.Return(kSuccess);
    });
    registry.Add("pthread_mutexattr_settype", [](HleCall& call) {
        if (std::uint32_t* word = Word(call, call.Arg(0))) {
            const std::uint32_t type = call.Arg(1) & 3u;
            __atomic_store_n(word, type << 14, __ATOMIC_RELEASE);
        }
        call.Return(kSuccess);
    });
    registry.Add("pthread_mutexattr_gettype", [](HleCall& call) {
        if (const std::uint32_t* word = Word(call, call.Arg(0))) {
            if (std::uint32_t* out = Word(call, call.Arg(1))) {
                *out = (__atomic_load_n(word, __ATOMIC_ACQUIRE) & kMutexTypeMask) >> 14;
            }
        }
        call.Return(kSuccess);
    });
    registry.Add("pthread_mutex_init", [](HleCall& call) {
        // The type lives in bits 14-15 of the mutex word, exactly as bionic's
        // PTHREAD_MUTEX_INITIALIZER spells it.
        std::uint32_t value = 0;
        if (call.Arg(1) != 0) {
            if (const std::uint32_t* attr = Word(call, call.Arg(1))) {
                value = __atomic_load_n(attr, __ATOMIC_ACQUIRE) & kMutexTypeMask;
            }
        }
        if (std::uint32_t* word = Word(call, call.Arg(0))) {
            __atomic_store_n(word, value, __ATOMIC_RELEASE);
        }
        call.Return(kSuccess);
    });
    registry.Add("pthread_mutex_destroy", [](HleCall& call) { call.Return(kSuccess); });
    registry.Add("pthread_mutex_lock", [](HleCall& call) {
        call.Return(MutexLock(call.Arg(0), call));
    });
    registry.Add("pthread_mutex_trylock", [](HleCall& call) {
        call.Return(MutexTryLock(call.Arg(0), call));
    });
    registry.Add("pthread_mutex_unlock", [](HleCall& call) {
        call.Return(MutexUnlock(call.Arg(0), call));
    });
    registry.Add("pthread_mutex_timedlock", [](HleCall& call) {
        const std::uint32_t address = call.Arg(0);
        const std::uint32_t deadline = call.Arg(1);
        struct timespec absolute{};
        if (!call.CopyIn(deadline, &absolute, sizeof(absolute))) {
            call.Return(kEinval);
            return;
        }
        struct timespec remaining{};
        if (!RelativeTimeout(absolute, true, &remaining)) {
            call.Return(kEtimedout);
            return;
        }
        const auto limit = std::chrono::steady_clock::now() +
                           std::chrono::seconds(remaining.tv_sec) +
                           std::chrono::nanoseconds(remaining.tv_nsec);
        for (;;) {
            const int status = MutexTryLock(address, call);
            if (status == kSuccess || status != kEbusy) {
                call.Return(status);
                return;
            }
            if (std::chrono::steady_clock::now() >= limit) {
                call.Return(kEtimedout);
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // --- condition variables ------------------------------------------------
    registry.Add("pthread_cond_init", [](HleCall& call) {
        if (std::uint32_t* word = Word(call, call.Arg(0))) {
            __atomic_store_n(word, 0u, __ATOMIC_RELEASE);
        }
        call.Return(kSuccess);
    });
    registry.Add("pthread_cond_destroy", [](HleCall& call) { call.Return(kSuccess); });
    registry.Add("pthread_cond_wait", [](HleCall& call) {
        call.Return(CondWait(call.Arg(0), call.Arg(1), call, nullptr));
    });
    registry.Add("pthread_cond_timedwait", [](HleCall& call) {
        struct timespec absolute{};
        if (!call.CopyIn(call.Arg(2), &absolute, sizeof(absolute))) {
            call.Return(kEinval);
            return;
        }
        struct timespec relative{};
        if (!RelativeTimeout(absolute, true, &relative)) {
            call.Return(CondWait(call.Arg(0), call.Arg(1), call, &relative));
            return;
        }
        call.Return(CondWait(call.Arg(0), call.Arg(1), call, &relative));
    });
    registry.Add("pthread_cond_signal", [](HleCall& call) {
        CondSignal(call.Arg(0), call, 1);
        call.Return(kSuccess);
    });
    registry.Add("pthread_cond_broadcast", [](HleCall& call) {
        CondSignal(call.Arg(0), call, 0x7fffffff);
        call.Return(kSuccess);
    });

    // --- reader/writer locks ------------------------------------------------
    registry.Add("pthread_rwlock_init", [](HleCall& call) {
        if (std::uint32_t* word = Word(call, call.Arg(0))) {
            __atomic_store_n(word, 0u, __ATOMIC_RELEASE);
        }
        call.Return(kSuccess);
    });
    registry.Add("pthread_rwlock_destroy", [](HleCall& call) { call.Return(kSuccess); });
    registry.Add("pthread_rwlock_rdlock", [](HleCall& call) {
        call.Return(RwlockReadLock(call.Arg(0), call));
    });
    registry.Add("pthread_rwlock_wrlock", [](HleCall& call) {
        call.Return(RwlockWriteLock(call.Arg(0), call));
    });
    registry.Add("pthread_rwlock_tryrdlock", [](HleCall& call) {
        std::uint32_t* word = Word(call, call.Arg(0));
        if (word == nullptr) {
            call.Return(kEinval);
            return;
        }
        std::uint32_t current = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        bool taken = false;
        while ((current & kWriterBit) == 0) {
            if (__atomic_compare_exchange_n(word, &current, current + 1, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                taken = true;
                break;
            }
        }
        call.Return(taken ? kSuccess : kEbusy);
    });
    registry.Add("pthread_rwlock_trywrlock", [](HleCall& call) {
        std::uint32_t* word = Word(call, call.Arg(0));
        if (word == nullptr) {
            call.Return(kEinval);
            return;
        }
        std::uint32_t expected = 0;
        call.Return(__atomic_compare_exchange_n(word, &expected, kWriterBit, false,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
                        ? kSuccess
                        : kEbusy);
    });
    registry.Add("pthread_rwlock_unlock", [](HleCall& call) {
        call.Return(RwlockUnlock(call.Arg(0), call));
    });

    // --- attributes ---------------------------------------------------------
    for (const char* name : {"pthread_mutexattr_destroy",
                             "pthread_mutexattr_setpshared", "pthread_mutexattr_setprotocol",
                             "pthread_mutexattr_setrobust", "pthread_condattr_init",
                             "pthread_condattr_destroy", "pthread_condattr_setclock",
                             "pthread_rwlockattr_init", "pthread_rwlockattr_destroy",
                             "pthread_attr_init", "pthread_attr_destroy",
                             "pthread_attr_setdetachstate", "pthread_attr_setstacksize",
                             "pthread_attr_setschedparam", "pthread_attr_setschedpolicy",
                             "pthread_attr_setinheritsched", "pthread_attr_setguardsize",
                             "pthread_mutex_consistent", "pthread_setname_np"}) {
        registry.Add(name, [](HleCall& call) { call.Return(kSuccess); });
    }

    registry.Add("pthread_attr_getstacksize", [&self](HleCall& call) {
        const std::uint32_t size = self.config.stack_size;
        call.CopyOut(call.Arg(1), &size, sizeof(size));
        call.Return(kSuccess);
    });
    registry.Add("pthread_attr_getdetachstate", [](HleCall& call) {
        const std::uint32_t state = 0;  // PTHREAD_CREATE_JOINABLE
        call.CopyOut(call.Arg(1), &state, sizeof(state));
        call.Return(kSuccess);
    });
    registry.Add("pthread_attr_getschedparam", [](HleCall& call) {
        const std::uint32_t param[2] = {0, 0};
        call.CopyOut(call.Arg(1), param, sizeof(param));
        call.Return(kSuccess);
    });

    // --- thread-specific data ----------------------------------------------
    registry.Add("pthread_key_create", [&self](HleCall& call) {
        const std::lock_guard lock(self.mutex);
        int index = -1;
        for (int candidate = 0; candidate < static_cast<int>(self.key_used.size()); ++candidate) {
            if (!self.key_used[candidate]) {
                index = candidate;
                break;
            }
        }
        if (index < 0) {
            if (static_cast<int>(self.key_used.size()) >= static_cast<int>(kMaxKeys)) {
                call.Return(kEagain);
                return;
            }
            self.key_used.push_back(true);
            index = static_cast<int>(self.key_used.size()) - 1;
        } else {
            self.key_used[index] = true;
        }
        const std::uint32_t value = static_cast<std::uint32_t>(index);
        call.CopyOut(call.Arg(0), &value, sizeof(value));
        call.Return(kSuccess);
    });

    registry.Add("pthread_key_delete", [&self](HleCall& call) {
        const std::lock_guard lock(self.mutex);
        const std::uint32_t key = call.Arg(0);
        if (key >= self.key_used.size()) {
            return;
        }
        self.key_used[key] = false;
        for (auto& [machine, thread] : self.by_machine) {
            thread->key_values.erase(static_cast<int>(key));
        }
    });

    registry.Add("pthread_setspecific", [&self](HleCall& call) {
        const std::uint32_t key = call.Arg(0);
        auto thread = self.FindByMachine(call.Machine());
        if (!thread) {
            call.Return(kEsrch);
            return;
        }
        {
            const std::lock_guard lock(self.mutex);
            if (key >= self.key_used.size() || !self.key_used[key]) {
                call.Return(kEinval);
                return;
            }
            thread->key_values[static_cast<int>(key)] = call.Arg(1);
        }
        call.Return(kSuccess);
    });

    registry.Add("pthread_getspecific", [&self](HleCall& call) {
        auto thread = self.FindByMachine(call.Machine());
        if (!thread) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(self.mutex);
        const auto it = thread->key_values.find(static_cast<int>(call.Arg(0)));
        call.Return(it == thread->key_values.end() ? 0 : it->second);
    });

    // pthread_once is a three-state futex word: 0 new, 1 running, 2 done.
    registry.Add("pthread_once", [](HleCall& call) {
        std::uint32_t* word = Word(call, call.Arg(0));
        if (word == nullptr) {
            call.Return(kEinval);
            return;
        }
        std::uint32_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, 1u, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            call.Machine().CallNested(call.Arg(1), {});
            __atomic_store_n(word, 2u, __ATOMIC_RELEASE);
            FutexWake(word, 0x7fffffff);
            call.Return(kSuccess);
            return;
        }
        while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != 2u) {
            FutexWait(word, 1u, nullptr);
        }
        call.Return(kSuccess);
    });
}

GuestPthread::GuestPthread(HleRegistry& registry) : state_(std::make_unique<State>(registry)) {}

GuestPthread::~GuestPthread() { StopAll(); }

void GuestPthread::StopAll() {
    // Guest threads that are still running are stopped at the next block
    // boundary: the JIT checks the halt flag between blocks, so a thread in guest
    // code returns to its runner. A thread parked in a host syscall (a futex
    // wait, a read) cannot be interrupted that way, so teardown detaches instead
    // of joining: the process is going away anyway, and a hang here would be
    // worse than a thread that never wakes.
    std::vector<std::shared_ptr<State::Thread>> threads;
    {
        const std::lock_guard lock(state_->mutex);
        for (auto& [machine, thread] : state_->by_machine) {
            if (!thread->is_main) {
                threads.push_back(thread);
            }
        }
    }
    for (const auto& thread : threads) {
        if (thread->machine) {
            thread->machine->Stop();
        }
    }
    for (const auto& thread : threads) {
        if (thread->host.joinable()) {
            thread->host.detach();
        }
    }
}

bool GuestPthread::Install(const GuestThreadConfig& config, std::uint32_t* thread_pointer,
                           std::string* error) {
    State& self = *state_;
    self.config = config;
    self.main_machine = config.main_machine;
    self.stack_used.assign(config.stack_count, false);
    self.stack_owner.assign(config.stack_count, {});
    if (!self.CreateMainThread(error)) {
        return false;
    }
    self.Register();
    if (thread_pointer != nullptr) {
        *thread_pointer = self.main_tls;
    }
    return true;
}

void RegisterLiveGuestThread(A32Machine& machine) {
    {
        const std::lock_guard lock(g_live_mutex);
        g_live.push_back(LiveGuestThread{static_cast<std::int32_t>(::gettid()), &machine});
    }
    // A thread created before the container installed the wakeup handler, or one
    // that inherited the signal blocked, could never be broken out of a syscall.
    UnblockGuestSignalWakeup();
}

A32Machine* FindLiveGuestThread(std::uint32_t tid) {
    const std::lock_guard lock(g_live_mutex);
    for (const LiveGuestThread& entry : g_live) {
        // The machine records the thread that is actually running it, which is
        // more reliable than the tid captured at registration: the main thread's
        // machine is created on whichever loader thread opened the archive.
        if (entry.machine->BusyThread() == tid ||
            static_cast<std::uint32_t>(entry.tid) == tid) {
            return entry.machine;
        }
    }
    return nullptr;
}

void UnregisterLiveGuestThread(A32Machine& machine) {
    const std::lock_guard lock(g_live_mutex);
    for (auto it = g_live.begin(); it != g_live.end(); ++it) {
        if (it->machine == &machine) {
            g_live.erase(it);
            return;
        }
    }
}

std::string GuestStackFramesForTid(std::uint32_t tid, std::uint32_t sp, std::uint32_t reach) {
    const std::lock_guard lock(g_live_mutex);
    for (const LiveGuestThread& entry : g_live) {
        if (static_cast<std::uint32_t>(entry.tid) != tid) {
            continue;
        }
        std::string frames;
        for (std::uint32_t address : entry.machine->GuestReturnAddresses(sp, reach)) {
            frames += " " + DescribeGuestAddress(address);
            if (frames.size() > 300) {
                break;
            }
        }
        return frames;
    }
    return std::string();
}

std::vector<std::string> GuestThreadLocations() {
    const std::lock_guard lock(g_live_mutex);
    std::vector<std::string> out;
    out.reserve(g_live.size() + 1);
    unsigned parked = 0;
    for (const LiveGuestThread& entry : g_live) {
        const char* hle = entry.machine->HleCallName();
        // A thread asleep in a documented blocking call is not a symptom, so it
        // is marked instead of dropped: a thread missing from this dump reads as
        // "not running", and that is a different story from "waiting".
        const bool blocked =
            hle != nullptr &&
            (std::strcmp(hle, "pthread_cond_wait") == 0 || std::strcmp(hle, "epoll_wait") == 0 ||
             std::strcmp(hle, "poll") == 0 || std::strcmp(hle, "usleep") == 0 ||
             std::strcmp(hle, "nanosleep") == 0 || std::strcmp(hle, "futex") == 0 ||
             std::strcmp(hle, "pthread_join") == 0 || std::strcmp(hle, "read") == 0 ||
             std::strcmp(hle, "accept") == 0 || std::strcmp(hle, "recvfrom") == 0);
        if (blocked) {
            ++parked;
        }
        char line[192];
        std::snprintf(line, sizeof(line), "tid %d pc=%s hle=%s busy=%d syscall=%u%s", entry.tid,
                      DescribeGuestAddress(entry.machine->Reg(15)).c_str(),
                      hle != nullptr ? hle : "-", entry.machine->Busy() ? 1 : 0,
                      entry.machine->LastSyscall(), blocked ? " (blocked)" : "");
        out.emplace_back(line);
    }
    char total[64];
    std::snprintf(total, sizeof(total), "(%zu live, %u blocked)", g_live.size(), parked);
    out.emplace_back(total);
    return out;
}

}  // namespace armbat


