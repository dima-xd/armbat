#include <cstdio>

#include "armbat/selftest.h"

int main() {
    const armbat::SuiteReport jit = armbat::RunJitSelftest();
    std::fputs(jit.text.c_str(), stdout);
    std::printf("\n%s (%d failure(s))\n\n", jit.Ok() ? "JIT OK" : "JIT FAILED", jit.failures);

    const armbat::SuiteReport loader = armbat::RunLoaderSelftest();
    std::fputs(loader.text.c_str(), stdout);
    std::printf("\n%s (%d failure(s))\n", loader.Ok() ? "LOADER OK" : "LOADER FAILED",
                loader.failures);

    return (jit.Ok() && loader.Ok()) ? 0 : 1;
}
