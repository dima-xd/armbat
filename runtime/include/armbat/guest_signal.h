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
// with its register file intact. Signal masks, alternate stacks and the
// kernel's ucontext are not emulated; a handler that needs them gets zeroes.

// Maps the per-thread signal frames and installs the host signal used to wake a
// guest thread out of a blocking syscall. Called once, after the arena exists.
void InitGuestSignals(GuestArena& arena);

// The host signal must be deliverable on every guest thread; a thread that
// inherits it blocked would never be woken.
void UnblockGuestSignalWakeup();

// Records an action the guest installed with sigaction. `handler` is a guest
// entry point, or SIG_DFL (0) / SIG_IGN (1).
void SetGuestSignalAction(int signal, std::uint32_t handler, std::uint32_t flags,
                          std::uint32_t restorer);

// True when the guest installed a real handler, so a signal has somewhere to go.
bool HasGuestSignalHandler(int signal);

// Runs the handlers of every signal queued on this machine's thread. Must be
// called on that thread at a safe point: between JIT slices or on SVC entry.
void DeliverGuestSignals(A32Machine& machine);

// Queues `signal` on the guest thread `tid` and interrupts it. `tid` is a host
// thread id, which is what the guest's clone() returned. Returns false when no
// such thread is alive.
bool SendGuestSignal(std::uint32_t tid, int signal);

}  // namespace armbat
