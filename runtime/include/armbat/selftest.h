#pragma once

#include <string>

namespace armbat {

struct SuiteReport {
    std::string text;
    int failures = 0;

    bool Ok() const { return failures == 0; }
};

// JIT correctness (integer, memory, NEON, VFP, FPSCR) plus benchmarks against
// native arm64 code.
SuiteReport RunJitSelftest();

// ELF32 loader: mapping, relocations, symbol lookup, initializers and calls
// into a real ARMv7 shared object.
SuiteReport RunLoaderSelftest();

}  // namespace armbat
