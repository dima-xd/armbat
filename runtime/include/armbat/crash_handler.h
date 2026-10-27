#pragma once

namespace armbat {

class A32Machine;

// Reports a host signal (SIGILL/SIGSEGV/SIGBUS) with the guest register file, so
// a crash inside generated code names the guest instruction instead of leaving
// one anonymous address. Installed once per process; `machine` must outlive it.
void InstallCrashHandler(A32Machine* machine);

// The machine the calling thread is running. Guest threads own their machine,
// so a signal there has to read that one, not the main thread's.
void SetCrashMachine(A32Machine* machine);

// Asks the next JIT block boundary to report the guest pc. A guest that spins
// in its own code never reaches an import, so the import trail cannot say where
// it is; this samples the machine without stopping it.
void RequestGuestSample();

// Prints a host stack for every thread of the process. A guest that spins
// inside the JIT or an import never reaches a guest pc that says anything, but
// the host stack still names the import or the JIT entry it is under.
void DumpThreadStacks();
bool TakeGuestSample();

}  // namespace armbat
