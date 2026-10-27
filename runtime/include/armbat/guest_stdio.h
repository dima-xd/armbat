#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace armbat {

class HleRegistry;

// Where bytes written to guest stdout (fd 1) and stderr (fd 2) end up. The CLI
// leaves it alone and the process fds are used; an Android app installs a sink
// that forwards to logcat, because a zygote child's stdout goes nowhere. The fd
// write always happens too, so installing a sink cannot hide output from a
// terminal run.
using StdioWriteSink = std::function<void(int fd, std::string_view text)>;

// Installs a new sink and returns the previous one.
StdioWriteSink SetStdioSink(StdioWriteSink sink);

// Sends already-decoded guest output to the installed sink. The kernel uses it
// for guest `write(1/2)`: with the real bionic libc, printf and the linker's
// own diagnostics go through the write syscall, not through the HLE stdio
// table, so both paths have to reach logcat.
void ForwardStdio(int fd, std::string_view text);

// C stdio on top of host streams.
//
// A guest `FILE*` cannot be a host pointer: it is 32 bits wide here and 64 in the
// runtime process. So a guest FILE* is the address of a cell in the HLE data page,
// and the cell's index selects a host FILE* kept on our side. Guest code only ever
// passes the value back to us, which is enough for everything except the very few
// programs that poke at FILE internals.
class GuestStdio {
public:
    explicit GuestStdio(HleRegistry& registry);
    ~GuestStdio();

    GuestStdio(const GuestStdio&) = delete;
    GuestStdio& operator=(const GuestStdio&) = delete;

    // Registers the HLE functions and the stdin/stdout/stderr variables. Must run
    // before the first guest image is loaded, and after the errno cell exists.
    bool Install(std::string* error);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace armbat
