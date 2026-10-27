#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace armbat {

class A32Machine;
class HleRegistry;

// Where guest thread stacks come from. Every thread needs its own stack and its
// own TLS block; both live in the arena, and the range has to be reserved by the
// embedder because the arena is shared with libraries, the heap and mmap.
struct GuestThreadConfig {
    std::uint32_t stack_base = 0x00840000;
    std::uint32_t stack_size = 512 * 1024;
    std::uint32_t stack_count = 8;
    // The main thread's machine is the template for every other thread's: same
    // JIT settings, same exclusive monitor, its own processor id.
    A32Machine* main_machine = nullptr;
};

// Guest threads: each one is a host thread with its own Dynarmic JIT.
//
// A Jit is not re-entrant and holds one register file, so a thread cannot share
// another thread's machine. What is shared is everything the guest sees as
// process-wide: the arena, the heap, descriptors, the HLE table and the
// exclusive monitor behind LDREX/STREX.
//
// Locks are real, not stubbed: mutexes, condition variables and rwlocks are
// futex-based state machines on the guest's own lock word, so a blocked thread
// sleeps in the host kernel instead of spinning under the JIT.
class GuestPthread {
public:
    explicit GuestPthread(HleRegistry& registry);
    ~GuestPthread();

    GuestPthread(const GuestPthread&) = delete;
    GuestPthread& operator=(const GuestPthread&) = delete;

    // Allocates the main thread's block, registers the functions and reports the
    // thread pointer. The caller installs it with A32Machine::SetTlsBase.
    bool Install(const GuestThreadConfig& config, std::uint32_t* thread_pointer,
                 std::string* error);

    bool Install(std::uint32_t* thread_pointer, std::string* error) {
        return Install(GuestThreadConfig{}, thread_pointer, error);
    }

    // Sends SIGUSR1-style stop requests to guest threads; joins whatever is left
    // when the process is done with the guest. Called by the destructor.
    void StopAll();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Diagnostics: where every live guest thread is, as "tid pc=0x........ hle=name".
// A thread that never makes another import looks identical to a spinning one in
// the call trail; this shows the program counter and the host call it is in.
std::vector<std::string> GuestThreadLocations();

// Diagnostics: return addresses on the stack of the live guest thread with this
// host tid, as " lib+0x... lib+0x...". Empty when the thread is not in the
// table. The lookup and the walk happen with the table locked, so a thread that
// is exiting cannot free its machine under the walk.
std::string GuestStackFramesForTid(std::uint32_t tid, std::uint32_t sp, std::uint32_t reach);

// The same table, for threads the container starts itself: on the bionic path a
// clone() becomes a host thread here rather than through the guest pthread
// runtime, and a thread missing from the table is invisible to every dump.
void RegisterLiveGuestThread(A32Machine& machine);
void UnregisterLiveGuestThread(A32Machine& machine);

// The machine of the live thread with this host tid, or null. Signals target a
// thread, and a thread is only reachable through this table.
A32Machine* FindLiveGuestThread(std::uint32_t tid);

}  // namespace armbat
