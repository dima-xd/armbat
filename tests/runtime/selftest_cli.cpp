#include <cstdio>

#include "armbat/selftest.h"

namespace {

// A suite that crashes must still leave the result of the one that finished
// before it: on a device the output is captured through adb, where a buffered
// stream dies with the process and takes the passed tests with it.
void Print(const char* verdict, const armbat::SuiteReport& report) {
    std::fputs(report.text.c_str(), stdout);
    std::printf("\n%s (%d failure(s))\n\n", verdict, report.failures);
    std::fflush(stdout);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const armbat::SuiteReport jit = armbat::RunJitSelftest();
    Print(jit.Ok() ? "JIT OK" : "JIT FAILED", jit);

    const armbat::SuiteReport loader = armbat::RunLoaderSelftest();
    Print(loader.Ok() ? "LOADER OK" : "LOADER FAILED", loader);

    return (jit.Ok() && loader.Ok()) ? 0 : 1;
}
