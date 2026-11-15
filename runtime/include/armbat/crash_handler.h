#pragma once

namespace armbat {

class A32Machine;

// Reports a host signal (SIGILL/SIGSEGV/SIGBUS/SIGFPE) with the guest register
// file and with the host mapping the fault came from, so a crash names the
// instruction and the module instead of leaving two anonymous addresses.
//
// The handler does not own the process: it reports and then hands the fault to
// the action that was installed before it. ART recovers from the faults a
// device recovers from -- an implicit null check in compiled Java is a
// NullPointerException there, a stack overflow is a StackOverflowError -- and
// that has to keep working inside the container. Installed once per process.
void InstallCrashHandler();

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
