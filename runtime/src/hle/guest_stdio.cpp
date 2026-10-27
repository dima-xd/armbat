#include "armbat/guest_stdio.h"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <android/log.h>

#include "armbat/log.h"
#include <unistd.h>

#include <atomic>

#include "armbat/guest_arena.h"
#include "armbat/hle.h"

#include "hle_format.h"

namespace armbat {
namespace {

constexpr std::uint32_t kMaxStreams = 64;
constexpr std::uint32_t kHandleStride = 16;
constexpr std::uint32_t kEof = 0xFFFFFFFFu;

// Reserved slots in every stream table: the C runtime's three standard streams.
constexpr int kStdoutIndex = 1;
constexpr int kStderrIndex = 2;
constexpr int kFirstFileIndex = 3;

// bionic's arm32 `sizeof(FILE)`. It is not in any header (the struct is opaque to
// applications), but guest code computes `&__sF[1]` from it, so the value has to
// match exactly or `stdout` would decode as `stdin`. The loader suite checks the
// guest's own `sizeof(FILE)` against this constant.
constexpr std::uint32_t kGuestFileSize = 84;

// A game writes its own content log to a file, and that log is the only place
// its diagnostics appear. Echo a bounded prefix of every file write so those
// lines reach logcat too.
void EchoFileWrite(const void* data, std::size_t length) {
    static std::atomic<unsigned> echoed{0};
    if (echoed.fetch_add(1, std::memory_order_relaxed) >= 600) {
        return;
    }
    if (length > 320) {
        length = 320;
    }
    std::string text(static_cast<const char*>(data), length);
    for (char& item : text) {
        if (item == '\n' || item == '\r') {
            item = ' ';
        }
    }
    ARMBAT_LOG_INFO("Armbat", "guest file: %s", text.c_str());
}

StdioWriteSink& Sink() {
    static StdioWriteSink sink;
    return sink;
}

}  // namespace

StdioWriteSink SetStdioSink(StdioWriteSink sink) {
    StdioWriteSink previous = std::move(Sink());
    Sink() = std::move(sink);
    return previous;
}

void ForwardStdio(int fd, std::string_view text) {
    if (StdioWriteSink& sink = Sink(); sink) {
        sink(fd, text);
    }
}

struct GuestStdio::State {
    explicit State(HleRegistry& target) : registry(target) {}

    HleRegistry& registry;
    std::uint32_t handle_base = 0;
    std::uint32_t standard_file_base = 0;
    std::uint32_t error_cell = 0;
    std::vector<std::FILE*> streams;

    std::uint32_t HandleFor(int index) const {
        return handle_base + static_cast<std::uint32_t>(index) * kHandleStride;
    }

    // -1 when the value was not handed out by <cstdio> or the slot is closed.
    int IndexOf(std::uint32_t handle) const {
        // `stdout` is `&__sF[1]` in bionic, so a handle may arrive as an address
        // into the standard stream array rather than a table cell.
        const std::uint32_t standard_bytes = 3 * kGuestFileSize;
        if (standard_file_base != 0 && handle >= standard_file_base &&
            handle < standard_file_base + standard_bytes) {
            const std::uint32_t offset = handle - standard_file_base;
            return offset % kGuestFileSize == 0
                       ? static_cast<int>(offset / kGuestFileSize)
                       : -1;
        }
        if (handle < handle_base) {
            return -1;
        }
        const std::uint32_t offset = handle - handle_base;
        if (offset % kHandleStride != 0) {
            return -1;
        }
        const std::uint32_t index = offset / kHandleStride;
        if (index >= streams.size() || streams[index] == nullptr) {
            return -1;
        }
        return static_cast<int>(index);
    }

    int AllocateStream(std::FILE* stream) {
        for (int index = kFirstFileIndex; index < static_cast<int>(streams.size()); ++index) {
            if (streams[index] == nullptr) {
                streams[index] = stream;
                return index;
            }
        }
        return -1;
    }

    void Fail(HleCall& call, int error) const {
        const std::uint32_t cell = call.ErrnoCell();
        if (cell == 0) {
            return;
        }
        const std::int32_t value = error;
        call.CopyOut(cell, &value, sizeof(value));
    }

    int GuestErrno(HleCall& call) const {
        const std::uint32_t cell = call.ErrnoCell();
        std::int32_t value = 0;
        if (cell == 0 || !call.CopyIn(cell, &value, sizeof(value))) {
            return 0;
        }
        return value;
    }

    void Emit(int fd, std::string_view text) {
        if (text.empty()) {
            return;
        }
        std::size_t written = 0;
        while (written < text.size()) {
            const ssize_t chunk =
                ::write(fd, text.data() + written, text.size() - written);
            if (chunk <= 0) {
                break;
            }
            written += static_cast<std::size_t>(chunk);
        }
        if (StdioWriteSink& sink = Sink(); sink) {
            sink(fd, text);
        }
    }

    std::FILE* StreamAt(std::uint32_t handle, HleCall& call) {
        const int index = IndexOf(handle);
        if (index < 0) {
            Fail(call, EBADF);
            return nullptr;
        }
        return streams[index];
    }

    void Register();
};

void GuestStdio::State::Register() {
    HleRegistry& registry = this->registry;
    State& self = *this;

    // --- open and close -----------------------------------------------------
    registry.Add("fopen", [&self](HleCall& call) {
        std::string path;
        std::string mode;
        if (!call.ReadCString(call.Arg(0), path, 4096) ||
            !call.ReadCString(call.Arg(1), mode, 16)) {
            self.Fail(call, EFAULT);
            call.Return(0);
            return;
        }
        std::FILE* stream = std::fopen(path.c_str(), mode.c_str());
        if (stream == nullptr) {
            self.Fail(call, errno);
            call.Return(0);
            return;
        }
        const int index = self.AllocateStream(stream);
        if (index < 0) {
            std::fclose(stream);
            self.Fail(call, EMFILE);
            call.Return(0);
            return;
        }
        call.Return(self.HandleFor(index));
    });

    registry.Add("fclose", [&self](HleCall& call) {
        const int index = self.IndexOf(call.Arg(0));
        if (index < kFirstFileIndex) {
            self.Fail(call, EBADF);
            call.Return(kEof);
            return;
        }
        std::FILE* stream = self.streams[index];
        self.streams[index] = nullptr;
        if (std::fclose(stream) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("fdopen", [&self](HleCall& call) {
        std::string mode;
        if (!call.ReadCString(call.Arg(1), mode, 16)) {
            self.Fail(call, EFAULT);
            call.Return(0);
            return;
        }
        std::FILE* stream = ::fdopen(static_cast<int>(call.Arg(0)), mode.c_str());
        if (stream == nullptr) {
            self.Fail(call, errno);
            call.Return(0);
            return;
        }
        const int index = self.AllocateStream(stream);
        if (index < 0) {
            std::fclose(stream);
            self.Fail(call, EMFILE);
            call.Return(0);
            return;
        }
        call.Return(self.HandleFor(index));
    });

    registry.Add("fileno", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        call.Return(static_cast<std::uint32_t>(::fileno(stream)));
    });

    // --- buffered transfer --------------------------------------------------
    registry.Add("fread", [&self](HleCall& call) {
        const std::uint32_t element = call.Arg(1);
        const std::uint32_t count = call.Arg(2);
        const std::uint64_t total = static_cast<std::uint64_t>(element) * count;
        if (total > INT32_MAX) {
            self.Fail(call, EINVAL);
            call.Return(0);
            return;
        }
        std::byte* buffer = call.Mapped(call.Arg(0), static_cast<std::size_t>(total));
        if (buffer == nullptr && total != 0) {
            self.Fail(call, EFAULT);
            call.Return(0);
            return;
        }
        std::FILE* stream = self.StreamAt(call.Arg(3), call);
        if (stream == nullptr) {
            call.Return(0);
            return;
        }
        call.Return(static_cast<std::uint32_t>(
            std::fread(buffer, element, count, stream)));
    });

    registry.Add("fwrite", [&self](HleCall& call) {
        const std::uint32_t element = call.Arg(1);
        const std::uint32_t count = call.Arg(2);
        const std::uint64_t total = static_cast<std::uint64_t>(element) * count;
        if (total > INT32_MAX) {
            self.Fail(call, EINVAL);
            call.Return(0);
            return;
        }
        const std::byte* buffer = call.Mapped(call.Arg(0), static_cast<std::size_t>(total));
        if (buffer == nullptr && total != 0) {
            self.Fail(call, EFAULT);
            call.Return(0);
            return;
        }
        const int index = self.IndexOf(call.Arg(3));
        if (index < 0) {
            self.Fail(call, EBADF);
            call.Return(0);
            return;
        }
        std::size_t written = 0;
        if (index == kStdoutIndex || index == kStderrIndex) {
            self.Emit(index,
                      std::string_view(reinterpret_cast<const char*>(buffer),
                                       static_cast<std::size_t>(total)));
            written = static_cast<std::size_t>(total);
        } else {
            EchoFileWrite(buffer, static_cast<std::size_t>(total));
            written = std::fwrite(buffer, element, count, self.streams[index]);
        }
        call.Return(element == 0 ? 0
                                 : static_cast<std::uint32_t>(written / element));
    });

    // --- positioning --------------------------------------------------------
    registry.Add("fseek", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        const auto offset = static_cast<long>(static_cast<std::int32_t>(call.Arg(1)));
        if (std::fseek(stream, offset, static_cast<int>(call.Arg(2))) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("ftell", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        const long position = std::ftell(stream);
        if (position < 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(static_cast<std::uint32_t>(position));
    });

    registry.Add("rewind", [&self](HleCall& call) {
        if (std::FILE* stream = self.StreamAt(call.Arg(0), call); stream != nullptr) {
            std::rewind(stream);
        }
        call.Return(0);
    });

    registry.Add("fflush", [&self](HleCall& call) {
        const std::uint32_t handle = call.Arg(0);
        if (handle == 0) {
            std::fflush(nullptr);
            call.Return(0);
            return;
        }
        std::FILE* stream = self.StreamAt(handle, call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        if (std::fflush(stream) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("setvbuf", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        call.Return(static_cast<std::uint32_t>(std::setvbuf(
            stream, nullptr, static_cast<int>(call.Arg(2)),
            static_cast<std::size_t>(call.Arg(3)))));
    });

    registry.Add("setbuf", [](HleCall& call) { call.Return(0); });

    // On a 32-bit ABI `off_t` is 32 bits unless the caller asks for the 64-bit
    // variant (which bionic redirects to fseeko64), so these take plain words.
    registry.Add("fseeko", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        if (::fseeko(stream, static_cast<off_t>(static_cast<std::int32_t>(call.Arg(1))),
                     static_cast<int>(call.Arg(2))) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("ftello", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        const off_t position = ::ftello(stream);
        if (position < 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(static_cast<std::uint32_t>(position));
    });

    // Wide characters on a byte-oriented stream: everything outside ASCII fails
    // cleanly instead of writing a truncated sequence.
    registry.Add("fputwc", [&self](HleCall& call) {
        const std::uint32_t value = call.Arg(0);
        const int index = self.IndexOf(call.Arg(1));
        if (index < 0) {
            self.Fail(call, EBADF);
            call.Return(kEof);
            return;
        }
        if (value >= 0x80) {
            self.Fail(call, EILSEQ);
            call.Return(kEof);
            return;
        }
        const char byte = static_cast<char>(value);
        if (index == kStdoutIndex || index == kStderrIndex) {
            self.Emit(index, std::string_view(&byte, 1));
        } else {
            std::fputc(static_cast<unsigned char>(byte), self.streams[index]);
        }
        call.Return(value);
    });

    registry.Add("getwc", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        const int value = std::fgetc(stream);
        call.Return(value == EOF ? kEof : static_cast<std::uint32_t>(value));
    });

    registry.Add("ungetwc", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(1), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        const std::uint32_t value = call.Arg(0);
        if (value >= 0x80) {
            call.Return(kEof);
            return;
        }
        const int result = std::ungetc(static_cast<int>(value), stream);
        call.Return(result == EOF ? kEof : static_cast<std::uint32_t>(result));
    });

    // --- character and line access -----------------------------------------
    registry.Add("fgetc", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        const int ch = std::fgetc(stream);
        call.Return(ch == EOF ? kEof : static_cast<std::uint32_t>(ch));
    });
    registry.Add("getc", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        const int ch = std::fgetc(stream);
        call.Return(ch == EOF ? kEof : static_cast<std::uint32_t>(ch));
    });

    registry.Add("fputc", [&self](HleCall& call) {
        const int index = self.IndexOf(call.Arg(1));
        if (index < 0) {
            self.Fail(call, EBADF);
            call.Return(kEof);
            return;
        }
        const char ch = static_cast<char>(call.Arg(0));
        if (index == kStdoutIndex || index == kStderrIndex) {
            self.Emit(index, std::string_view(&ch, 1));
            call.Return(static_cast<std::uint32_t>(static_cast<unsigned char>(ch)));
            return;
        }
        const int written = std::fputc(static_cast<unsigned char>(ch), self.streams[index]);
        call.Return(written == EOF ? kEof : static_cast<std::uint32_t>(
                                                   static_cast<unsigned char>(ch)));
    });

    registry.Add("ungetc", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(1), call);
        if (stream == nullptr) {
            call.Return(kEof);
            return;
        }
        const int result = std::ungetc(static_cast<int>(call.Arg(0)), stream);
        call.Return(result == EOF ? kEof : static_cast<std::uint32_t>(result));
    });

    registry.Add("fgets", [&self](HleCall& call) {
        const std::uint32_t capacity = call.Arg(1);
        if (capacity == 0) {
            call.Return(0);
            return;
        }
        char* buffer = reinterpret_cast<char*>(call.Mapped(call.Arg(0), capacity));
        if (buffer == nullptr) {
            self.Fail(call, EFAULT);
            call.Return(0);
            return;
        }
        std::FILE* stream = self.StreamAt(call.Arg(2), call);
        if (stream == nullptr) {
            call.Return(0);
            return;
        }
        call.Return(std::fgets(buffer, static_cast<int>(capacity), stream) == nullptr
                        ? 0
                        : call.Arg(0));
    });

    registry.Add("fputs", [&self](HleCall& call) {
        std::string text;
        if (!call.ReadCString(call.Arg(0), text, 1u << 20)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        const int index = self.IndexOf(call.Arg(1));
        if (index < 0) {
            self.Fail(call, EBADF);
            call.Return(kEof);
            return;
        }
        if (index == kStdoutIndex || index == kStderrIndex) {
            self.Emit(index, text);
            call.Return(static_cast<std::uint32_t>(text.size()));
            return;
        }
        const int result = std::fputs(text.c_str(), self.streams[index]);
        EchoFileWrite(text.data(), text.size());
        call.Return(result == EOF ? kEof : static_cast<std::uint32_t>(text.size()));
    });

    registry.Add("puts", [&self](HleCall& call) {
        std::string text;
        if (!call.ReadCString(call.Arg(0), text, 1u << 20)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        text.push_back('\n');
        self.Emit(kStdoutIndex, text);
        call.Return(static_cast<std::uint32_t>(text.size()));
    });

    registry.Add("feof", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        call.Return(stream == nullptr ? 0 : static_cast<std::uint32_t>(std::feof(stream)));
    });
    registry.Add("ferror", [&self](HleCall& call) {
        std::FILE* stream = self.StreamAt(call.Arg(0), call);
        call.Return(stream == nullptr ? 0 : static_cast<std::uint32_t>(std::ferror(stream)));
    });
    registry.Add("clearerr", [&self](HleCall& call) {
        if (std::FILE* stream = self.StreamAt(call.Arg(0), call); stream != nullptr) {
            std::clearerr(stream);
        }
        call.Return(0);
    });

    // --- formatted output ---------------------------------------------------
    //
    // printf and friends cannot share one implementation: the first argument is
    // the stream for fprintf and the destination for sprintf, so the format sits
    // at a different index in each.
    registry.Add("printf", [&self](HleCall& call) {
        std::string format;
        if (!call.ReadCString(call.Arg(0), format, 8192)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        const std::string text = detail::RenderFormat(detail::MakeCallReader(call, 1), format);
        self.Emit(kStdoutIndex, text);
        call.Return(static_cast<std::uint32_t>(text.size()));
    });

    registry.Add("fprintf", [&self](HleCall& call) {
        const int index = self.IndexOf(call.Arg(0));
        if (index < 0) {
            self.Fail(call, EBADF);
            call.Return(kEof);
            return;
        }
        std::string format;
        if (!call.ReadCString(call.Arg(1), format, 8192)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        const std::string text = detail::RenderFormat(detail::MakeCallReader(call, 2), format);
        if (index == kStdoutIndex || index == kStderrIndex) {
            self.Emit(index, text);
        } else {
            std::fwrite(text.data(), 1, text.size(), self.streams[index]);
        }
        call.Return(static_cast<std::uint32_t>(text.size()));
    });

    // Same as fprintf, but the arguments arrive through a guest va_list.
    registry.Add("vfprintf", [&self](HleCall& call) {
        const int index = self.IndexOf(call.Arg(0));
        if (index < 0) {
            self.Fail(call, EBADF);
            call.Return(kEof);
            return;
        }
        std::string format;
        if (!call.ReadCString(call.Arg(1), format, 8192)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        const std::string text =
            detail::RenderFormat(detail::MakeVaListReader(call, call.Arg(2)), format);
        if (index == kStdoutIndex || index == kStderrIndex) {
            self.Emit(index, text);
        } else {
            std::fwrite(text.data(), 1, text.size(), self.streams[index]);
        }
        call.Return(static_cast<std::uint32_t>(text.size()));
    });

    registry.Add("sprintf", [&self](HleCall& call) {
        std::string format;
        if (!call.ReadCString(call.Arg(1), format, 8192)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        const std::string text = detail::RenderFormat(detail::MakeCallReader(call, 2), format);
        if (!text.empty() && !call.CopyOut(call.Arg(0), text.data(), text.size())) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        const char terminator = '\0';
        call.CopyOut(call.Arg(0) + static_cast<std::uint32_t>(text.size()), &terminator, 1);
        call.Return(static_cast<std::uint32_t>(text.size()));
    });

    // --- error reporting ----------------------------------------------------
    registry.Add("strerror", [&self](HleCall& call) {
        const char* message = std::strerror(static_cast<int>(call.Arg(0)));
        const std::size_t length = std::strlen(message);
        const std::size_t capped = length < 127 ? length : 127;
        if (self.error_cell != 0) {
            call.CopyOut(self.error_cell, message, capped);
            const char terminator = '\0';
            call.CopyOut(self.error_cell + static_cast<std::uint32_t>(capped), &terminator, 1);
        }
        call.Return(self.error_cell);
    });

    registry.Add("perror", [&self](HleCall& call) {
        std::string prefix;
        if (!call.ReadCString(call.Arg(0), prefix, 1024)) {
            prefix.clear();
        }
        std::string text;
        if (!prefix.empty()) {
            text += prefix;
            text += ": ";
        }
        text += std::strerror(self.GuestErrno(call));
        text += '\n';
        self.Emit(kStderrIndex, text);
        call.Return(0);
    });

    // --- whole-file operations ---------------------------------------------
    registry.Add("remove", [&self](HleCall& call) {
        std::string path;
        if (!call.ReadCString(call.Arg(0), path, 4096)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        if (std::remove(path.c_str()) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("rename", [&self](HleCall& call) {
        std::string from;
        std::string to;
        if (!call.ReadCString(call.Arg(0), from, 4096) ||
            !call.ReadCString(call.Arg(1), to, 4096)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        if (std::rename(from.c_str(), to.c_str()) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("tmpfile", [&self](HleCall& call) {
        std::FILE* stream = std::tmpfile();
        if (stream == nullptr) {
            self.Fail(call, errno);
            call.Return(0);
            return;
        }
        const int index = self.AllocateStream(stream);
        if (index < 0) {
            std::fclose(stream);
            self.Fail(call, EMFILE);
            call.Return(0);
            return;
        }
        call.Return(self.HandleFor(index));
    });
}

GuestStdio::GuestStdio(HleRegistry& registry) : state_(std::make_unique<State>(registry)) {}

GuestStdio::~GuestStdio() = default;

bool GuestStdio::Install(std::string* error) {
    State& self = *state_;
    HleRegistry& registry = self.registry;

    self.handle_base = registry.AllocateData(kMaxStreams * kHandleStride);
    if (self.handle_base == 0) {
        if (error) *error = "cannot map the stdio handle page";
        return false;
    }
    self.streams.assign(kMaxStreams, nullptr);
    self.streams[0] = stdin;
    self.streams[kStdoutIndex] = stdout;
    self.streams[kStderrIndex] = stderr;

    // strerror writes into this cell; pthread programs expect the pointer to stay
    // valid until the next call, which is exactly what a static buffer gives.
    self.error_cell = registry.AllocateData(128);
    if (self.error_cell == 0) {
        if (error) *error = "cannot map the strerror buffer";
        return false;
    }

    // bionic exposes the three streams as the `__sF` array; the import resolves to
    // this cell, and IndexOf decodes `&__sF[n]` from it.
    self.standard_file_base = registry.AddData("__sF", 3 * kGuestFileSize);
    if (self.standard_file_base == 0) {
        if (error) *error = "cannot map the __sF array";
        return false;
    }

    self.Register();

    // The three standard streams are imported as variables, so the cell has to
    // hold the guest FILE* value, not the address of the stream's data.
    const auto define = [&](const char* name, int index) {
        const std::uint32_t cell = registry.AddData(name);
        if (cell == 0) {
            return false;
        }
        const std::uint32_t value = self.HandleFor(index);
        std::memcpy(registry.Arena().Ptr(cell), &value, sizeof(value));
        return true;
    };
    if (!define("stdin", 0) || !define("stdout", kStdoutIndex) ||
        !define("stderr", kStderrIndex)) {
        if (error) *error = "cannot map the standard stream cells";
        return false;
    }
    return true;
}

}  // namespace armbat
