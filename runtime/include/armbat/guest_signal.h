#pragma once

#include <cstdint>

namespace armbat {

class A32Machine;
class GuestArena;

// Asynchronous signals, as far as the container can honour them. The guest's
// own bionic installs handlers with sigaction and the platform delivers them
// with tkill/tgkill; without that path a thread cannot be stopped, and a
// conservative garbage collector that stops the world (Mono's Boehm GC does)
// aborts on the first collection.
//
// The model here is deliberately small: a signal with a handler is queued on the
// target thread and the handler runs at the next safe point, on that thread,
// with its register file intact. Blocked signals are emulated -- they stay
// queued and are delivered once the thread unblocks them -- because the
// stop-the-world protocol of a conservative collector is built on exactly that.
// Alternate stacks and the kernel's ucontext are not emulated; a handler that
// needs them gets zeroes.

// Maps the per-thread signal frames and installs the host signal used to wake a
// guest thread out of a blocking syscall. Called once, after the arena exists.
void InitGuestSignals(GuestArena& arena);

// The host signal must be deliverable on every guest thread; a thread that
// inherits it blocked would never be woken.
void UnblockGuestSignalWakeup();

// Records an action the guest installed with sigaction. `handler` is a guest
// entry point, or SIG_DFL (0) / SIG_IGN (1). `mask` is the action's sa_mask: the
// signals the handler itself blocks while it runs.
void SetGuestSignalAction(int signal, std::uint32_t handler, std::uint32_t flags,
                          std::uint32_t restorer, std::uint64_t mask);

// True when the guest installed a real handler, so a signal has somewhere to go.
bool HasGuestSignalHandler(int signal);

// Runs the handlers of every signal queued on this machine's thread. Must be
// called on that thread at a safe point: between JIT slices or on SVC entry.
void DeliverGuestSignals(A32Machine& machine);

// Reports a guest memory fault as the signal a device would raise, so the
// guest's own handler decides what the fault means: Mono turns a read of a
// near-null address into a NullReferenceException and rewrites the context so
// the thread resumes at its thrower instead of at the instruction that faulted.
// Must be called on the faulting thread with the register file of the code that
// faulted, i.e. after the JIT stopped on the fault. Returns true when the guest
// took the fault and the register file now holds where to continue; false means
// nobody took it and the caller reports the fault and ends the run.
bool DeliverGuestFault(A32Machine& machine, std::uint32_t address);

// Queues `signal` on the guest thread `tid` and interrupts it. `tid` is a host
// thread id, which is what the guest's clone() returned. Returns false when no
// such thread is alive.
bool SendGuestSignal(std::uint32_t tid, int signal);

}  // namespace armbat
