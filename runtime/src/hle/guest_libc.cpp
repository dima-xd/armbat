#include "armbat/guest_libc.h"

#include <android/log.h>

#include "armbat/log.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "armbat/a32_machine.h"
#include "armbat/guest_abi.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_heap.h"
#include "armbat/hle.h"

#include "hle_format.h"

namespace armbat {
namespace {

constexpr std::uint32_t kMaxDirs = 16;
constexpr std::uint32_t kDirStride = 16;
constexpr std::uint32_t kDirentSize = 280;
constexpr std::uint32_t kEnvCellSize = 512;
constexpr std::uint32_t kEnvCells = 4;
constexpr std::uint32_t kEof = 0xFFFFFFFFu;

std::string ArgString(HleCall& call, std::uint32_t address, std::size_t max = 4096) {
    std::string text;
    if (!call.ReadCString(address, text, max)) {
        text.clear();
    }
    return text;
}

std::wstring ArgWideString(HleCall& call, std::uint32_t address, std::size_t max = 4096) {
    std::wstring text;
    for (std::size_t index = 0; index < max; ++index) {
        std::uint32_t value = 0;
        if (!call.CopyIn(address + static_cast<std::uint32_t>(index * 4), &value,
                         sizeof(value)) ||
            value == 0) {
            break;
        }
        text.push_back(static_cast<wchar_t>(value));
    }
    return text;
}

bool PutWideString(HleCall& call, std::uint32_t address, const std::wstring& text,
                   std::size_t capacity) {
    if (capacity == 0) {
        return false;
    }
    const std::size_t written = std::min(text.size(), capacity - 1);
    for (std::size_t index = 0; index < written; ++index) {
        const std::uint32_t value = static_cast<std::uint32_t>(text[index]);
        call.CopyOut(address + static_cast<std::uint32_t>(index * 4), &value, sizeof(value));
    }
    const std::uint32_t zero = 0;
    return call.CopyOut(address + static_cast<std::uint32_t>(written * 4), &zero, sizeof(zero));
}

std::string NarrowWide(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t value : text) {
        out.push_back(value < 128 ? static_cast<char>(value) : '?');
    }
    return out;
}

std::wstring WidenNarrow(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (char value : text) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(value)));
    }
    return out;
}

bool PutString(HleCall& call, std::uint32_t address, const std::string& text) {
    if (!text.empty() && !call.CopyOut(address, text.data(), text.size())) {
        return false;
    }
    const char terminator = '\0';
    return call.CopyOut(address + static_cast<std::uint32_t>(text.size()), &terminator, 1);
}

void ReturnI64(HleCall& call, std::int64_t value) {
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    call.ReturnPair(static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32));
}

void ReturnDouble(HleCall& call, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    call.ReturnPair(static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32));
}

// `int sscanf(const char*, const char*, ...)` shares one scanner with the
// va_list variant; the reader abstracts where the arguments come from.
int ScanArguments(HleCall& call, const std::string& input, const std::string& format,
                  detail::ArgumentReader& arguments) {
    std::size_t position = 0;
    std::size_t index = 0;
    int assigned = 0;

    const auto skip_space = [&]() {
        while (position < input.size() &&
               std::isspace(static_cast<unsigned char>(input[position])) != 0) {
            ++position;
        }
    };

    while (index < format.size()) {
        const char current = format[index];
        if (std::isspace(static_cast<unsigned char>(current)) != 0) {
            skip_space();
            ++index;
            continue;
        }
        if (current != '%') {
            if (position >= input.size() || input[position] != current) {
                return assigned;
            }
            ++position;
            ++index;
            continue;
        }

        ++index;
        if (index >= format.size()) {
            return assigned;
        }
        if (format[index] == '%') {
            if (position >= input.size() || input[position] != '%') {
                return assigned;
            }
            ++position;
            ++index;
            continue;
        }

        bool suppressed = false;
        if (format[index] == '*') {
            suppressed = true;
            ++index;
        }
        int width = 0;
        while (index < format.size() &&
               std::isdigit(static_cast<unsigned char>(format[index])) != 0) {
            width = width * 10 + (format[index] - '0');
            ++index;
        }
        int longs = 0;
        while (index < format.size() && format[index] == 'l') {
            ++longs;
            ++index;
        }
        if (index < format.size() &&
            (format[index] == 'h' || format[index] == 'z' || format[index] == 'j')) {
            ++index;
        }
        if (index >= format.size()) {
            return assigned;
        }
        const char conversion = format[index++];

        if (conversion != 'c') {
            skip_space();
        }
        if (position >= input.size() && conversion != 'n') {
            return assigned;
        }

        const std::size_t begin = position;
        char* end = nullptr;
        switch (conversion) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            const int base = conversion == 'o' ? 8
                            : (conversion == 'x' || conversion == 'X') ? 16
                                                                      : 10;
            const long long value = std::strtoll(input.c_str() + position, &end, base);
            if (end == input.c_str() + position) {
                return assigned;
            }
            position = static_cast<std::size_t>(end - input.c_str());
            if (!suppressed) {
                const std::uint32_t address = arguments.next32();
                if (longs >= 2) {
                    call.CopyOut(address, &value, sizeof(value));
                } else {
                    const std::int32_t narrow = static_cast<std::int32_t>(value);
                    call.CopyOut(address, &narrow, sizeof(narrow));
                }
                ++assigned;
            }
            break;
        }
        case 'f':
        case 'e':
        case 'g':
        case 'F':
        case 'E':
        case 'G': {
            const double value = std::strtod(input.c_str() + position, &end);
            if (end == input.c_str() + position) {
                return assigned;
            }
            position = static_cast<std::size_t>(end - input.c_str());
            if (!suppressed) {
                const std::uint32_t address = arguments.next32();
                if (longs >= 1) {
                    call.CopyOut(address, &value, sizeof(value));
                } else {
                    const float narrow = static_cast<float>(value);
                    call.CopyOut(address, &narrow, sizeof(narrow));
                }
                ++assigned;
            }
            break;
        }
        case 's': {
            const std::size_t limit =
                width > 0 ? std::min<std::size_t>(width, input.size()) : input.size();
            while (position < input.size() && position - begin < limit &&
                   std::isspace(static_cast<unsigned char>(input[position])) == 0) {
                ++position;
            }
            if (position == begin) {
                return assigned;
            }
            if (!suppressed) {
                PutString(call, arguments.next32(), input.substr(begin, position - begin));
                ++assigned;
            }
            break;
        }
        case 'c': {
            const std::size_t count = width > 0 ? static_cast<std::size_t>(width) : 1;
            if (position + count > input.size()) {
                return assigned;
            }
            if (!suppressed) {
                call.CopyOut(arguments.next32(), input.data() + position, count);
                ++assigned;
            }
            position += count;
            break;
        }
        case 'n': {
            if (!suppressed) {
                const std::uint32_t address = arguments.next32();
                const std::int32_t written = static_cast<std::int32_t>(position);
                call.CopyOut(address, &written, sizeof(written));
            }
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

}  // namespace

struct GuestLibc::State {
    State(HleRegistry& target, GuestHeap& target_heap) : registry(target), heap(target_heap) {}

    HleRegistry& registry;
    GuestHeap& heap;

    std::uint32_t dir_base = 0;
    std::vector<DIR*> dirs;
    std::uint32_t dirent_cell = 0;
    std::uint32_t env_cells[kEnvCells] = {};
    int env_next = 0;
    std::uint32_t locale_cell = 0;
    std::uint32_t lconv_cell = 0;

    void Fail(HleCall& call, int error) const {
        const std::uint32_t cell = call.ErrnoCell();
        if (cell == 0) {
            return;
        }
        const std::int32_t value = error;
        call.CopyOut(cell, &value, sizeof(value));
    }

    int DirIndex(std::uint32_t handle) const {
        if (handle < dir_base) {
            return -1;
        }
        const std::uint32_t offset = handle - dir_base;
        if (offset % kDirStride != 0) {
            return -1;
        }
        const std::uint32_t index = offset / kDirStride;
        if (index >= dirs.size() || dirs[index] == nullptr) {
            return -1;
        }
        return static_cast<int>(index);
    }

    std::uint32_t RegisterDir(DIR* stream) {
        for (int index = 0; index < static_cast<int>(dirs.size()); ++index) {
            if (dirs[index] == nullptr) {
                dirs[index] = stream;
                return dir_base + static_cast<std::uint32_t>(index) * kDirStride;
            }
        }
        return 0;
    }

    std::uint32_t EnvCell() {
        const std::uint32_t cell = env_cells[env_next];
        env_next = (env_next + 1) % static_cast<int>(kEnvCells);
        return cell;
    }

    void Register();
};

void GuestLibc::State::Register() {
    HleRegistry& registry = this->registry;
    State& self = *this;

    // --- process and runtime bookkeeping ------------------------------------
    registry.Add("abort", [&self](HleCall& call) {
        // The guest aborts with a reason it has usually already handed to
        // android_set_abort_message; the return address says where it came from.
        NoteGuestAbort(call.Machine().Reg(14));
        ARMBAT_LOG_ERROR("Armbat", "guest abort at pc=0x%08x lr=0x%08x",
                            call.Machine().Reg(15), call.Machine().Reg(14));
        {
            std::string trail;
            for (const std::string& name : self.registry.RecentFunctions(::gettid())) {
                trail += name;
                trail += ' ';
            }
            std::size_t offset = 0;
            while (offset < trail.size()) {
                ARMBAT_LOG_ERROR("Armbat", "abort trail: %s",
                                    trail.substr(offset, 900).c_str());
                offset += 900;
            }
        }
        {
            // An uncaught exception does not unwind: the frames that threw are
            // still on the stack, so a scan for return addresses into the loaded
            // images recovers the call chain that led to terminate().
            const std::uint32_t sp = call.Machine().Reg(13);
            std::string frames;
            char text[16];
            for (std::uint32_t offset = 0; offset < 16384; offset += 4) {
                const std::byte* cell = call.Mapped(sp + offset, 4);
                if (cell == nullptr) {
                    break;
                }
                std::uint32_t word = 0;
                std::memcpy(&word, cell, sizeof(word));
                if (word < 0x06000000u || word >= 0x09000000u) {
                    continue;
                }
                std::snprintf(text, sizeof(text), " %08x", word & ~1u);
                frames += text;
            }
            ARMBAT_LOG_ERROR("Armbat", "abort stack:%s", frames.c_str());
        }
        call.Return(0);
        call.Machine().Stop();
    });
    registry.Add("android_set_abort_message", [](HleCall& call) {
        std::string message;
        call.ReadCString(call.Arg(0), message, 8192);
        ARMBAT_LOG_ERROR("Armbat", "guest abort message: %s", message.c_str());
    });
    registry.Add("__cxa_thread_atexit_impl", [](HleCall& call) { call.Return(0); });
    registry.Add("__ctype_get_mb_cur_max", [](HleCall& call) { call.Return(1); });
    // dl_unwind_find_exidx belongs to the loader, which is the only part that
    // knows where each module's .ARM.exidx table ended up.
    registry.Add("getpid", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::getpid()));
    });

    registry.Add("sysconf", [](HleCall& call) {
        const long value = ::sysconf(static_cast<int>(call.Arg(0)));
        call.Return(static_cast<std::uint32_t>(value));
    });

    registry.Add("syscall", [&self](HleCall& call) {
        const long number = static_cast<long>(call.Arg(0));
        if (number == SYS_getpid) {
            call.Return(static_cast<std::uint32_t>(::getpid()));
            return;
        }
        if (number == SYS_gettid) {
            call.Return(static_cast<std::uint32_t>(::gettid()));
            return;
        }
        self.Fail(call, ENOSYS);
        call.Return(kEof);
    });

    registry.Add("clock_gettime", [&self](HleCall& call) {
        timespec host{};
        if (::clock_gettime(static_cast<clockid_t>(call.Arg(0)), &host) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        const GuestTimespec guest{static_cast<std::int32_t>(host.tv_sec),
                                  static_cast<std::int32_t>(host.tv_nsec)};
        if (!call.CopyOut(call.Arg(1), &guest, sizeof(guest))) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("nanosleep", [&self](HleCall& call) {
        GuestTimespec guest{};
        if (!call.CopyIn(call.Arg(0), &guest, sizeof(guest))) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        const timespec request{guest.seconds, guest.nanoseconds};
        if (::nanosleep(&request, nullptr) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    // --- strings and characters --------------------------------------------
    registry.Add("memchr", [](HleCall& call) {
        const std::uint32_t address = call.Arg(0);
        const std::uint32_t length = call.Arg(2);
        const std::byte* buffer = call.Mapped(address, length);
        if (buffer == nullptr) {
            call.Return(0);
            return;
        }
        const void* found = std::memchr(buffer, static_cast<int>(call.Arg(1)), length);
        call.Return(found == nullptr
                        ? 0
                        : address + static_cast<std::uint32_t>(
                                        static_cast<const std::byte*>(found) - buffer));
    });

    registry.Add("wcslen", [](HleCall& call) {
        const std::uint32_t address = call.Arg(0);
        std::uint32_t length = 0;
        for (;; ++length) {
            std::uint32_t value = 0;
            if (!call.CopyIn(address + length * 4, &value, sizeof(value)) || value == 0) {
                break;
            }
        }
        call.Return(length);
    });

    registry.Add("wmemchr", [](HleCall& call) {
        const std::uint32_t address = call.Arg(0);
        const std::uint32_t value = call.Arg(1);
        const std::uint32_t count = call.Arg(2);
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t current = 0;
            if (!call.CopyIn(address + index * 4, &current, sizeof(current))) {
                break;
            }
            if (current == value) {
                call.Return(address + index * 4);
                return;
            }
        }
        call.Return(0);
    });

    registry.Add("wmemcmp", [](HleCall& call) {
        const std::uint32_t left = call.Arg(0);
        const std::uint32_t right = call.Arg(1);
        const std::uint32_t count = call.Arg(2);
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            call.CopyIn(left + index * 4, &a, sizeof(a));
            call.CopyIn(right + index * 4, &b, sizeof(b));
            if (a != b) {
                call.Return(a < b ? kEof : 1);
                return;
            }
        }
        call.Return(0);
    });

    // --- number parsing -----------------------------------------------------
    registry.Add("strtol", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        call.Return(static_cast<std::uint32_t>(
            std::strtol(text.c_str(), nullptr, static_cast<int>(call.Arg(2)))));
    });
    registry.Add("strtoul", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        call.Return(static_cast<std::uint32_t>(
            std::strtoul(text.c_str(), nullptr, static_cast<int>(call.Arg(2)))));
    });
    registry.Add("strtoll", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        ReturnI64(call, std::strtoll(text.c_str(), nullptr, static_cast<int>(call.Arg(2))));
    });
    registry.Add("strtoull", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        ReturnI64(call, static_cast<std::int64_t>(
                            std::strtoull(text.c_str(), nullptr, static_cast<int>(call.Arg(2)))));
    });
    registry.Add("strtoll_l", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        ReturnI64(call, std::strtoll(text.c_str(), nullptr, static_cast<int>(call.Arg(2))));
    });
    registry.Add("strtoull_l", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        ReturnI64(call, static_cast<std::int64_t>(
                            std::strtoull(text.c_str(), nullptr, static_cast<int>(call.Arg(2)))));
    });

    registry.Add("strtod", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        ReturnDouble(call, std::strtod(text.c_str(), nullptr));
    });
    registry.Add("strtof", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        const float value = std::strtof(text.c_str(), nullptr);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        call.Return(bits);
    });
    registry.Add("strtold", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        ReturnDouble(call, std::strtod(text.c_str(), nullptr));
    });
    registry.Add("strtold_l", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0));
        ReturnDouble(call, std::strtod(text.c_str(), nullptr));
    });

    // --- environment --------------------------------------------------------
    registry.Add("getenv", [&self](HleCall& call) {
        const std::string name = ArgString(call, call.Arg(0), 256);
        const char* value = std::getenv(name.c_str());
        ARMBAT_LOG_INFO("Armbat", "getenv %s=%s", name.c_str(),
                            value != nullptr ? value : "(null)");
        if (value == nullptr) {
            call.Return(0);
            return;
        }
        const std::uint32_t cell = self.EnvCell();
        PutString(call, cell, value);
        call.Return(cell);
    });

    registry.Add("getcwd", [&self](HleCall& call) {
        const std::uint32_t size = call.Arg(1);
        if (size == 0) {
            self.Fail(call, EINVAL);
            call.Return(0);
            return;
        }
        char buffer[4096];
        if (::getcwd(buffer, std::min<std::size_t>(size, sizeof(buffer))) == nullptr) {
            self.Fail(call, errno);
            call.Return(0);
            return;
        }
        PutString(call, call.Arg(0), buffer);
        call.Return(call.Arg(0));
    });

    registry.Add("chdir", [&self](HleCall& call) {
        if (::chdir(ArgString(call, call.Arg(0)).c_str()) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("isatty", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::isatty(static_cast<int>(call.Arg(0)))));
    });

    // --- locale -------------------------------------------------------------
    registry.Add("setlocale", [&self](HleCall& call) {
        if (call.Arg(1) == 0) {
            call.Return(self.locale_cell);
            return;
        }
        const std::string requested = ArgString(call, call.Arg(1), 64);
        if (requested.empty() || requested == "C" || requested == "POSIX" ||
            requested == "C.UTF-8" || requested == "en_US.UTF-8") {
            PutString(call, self.locale_cell, requested.empty() ? "C" : requested);
            call.Return(self.locale_cell);
            return;
        }
        // Only the C locale is provided; pretending otherwise would make callers
        // believe in collation and formatting we do not implement.
        call.Return(0);
    });

    registry.Add("newlocale", [](HleCall& call) { call.Return(1); });
    registry.Add("freelocale", [](HleCall& call) { call.Return(0); });
    registry.Add("uselocale", [](HleCall& call) { call.Return(0); });
    registry.Add("localeconv", [&self](HleCall& call) { call.Return(self.lconv_cell); });

    registry.Add("strcoll_l", [](HleCall& call) {
        const std::string left = ArgString(call, call.Arg(0));
        const std::string right = ArgString(call, call.Arg(1));
        call.Return(static_cast<std::uint32_t>(std::strcmp(left.c_str(), right.c_str())));
    });
    registry.Add("strxfrm_l", [](HleCall& call) {
        const std::string source = ArgString(call, call.Arg(1));
        const std::uint32_t size = call.Arg(2);
        if (size > 0) {
            const std::size_t copied = std::min<std::size_t>(source.size(), size - 1);
            call.CopyOut(call.Arg(0), source.data(), copied);
            const char terminator = '\0';
            call.CopyOut(call.Arg(0) + static_cast<std::uint32_t>(copied), &terminator, 1);
        }
        call.Return(static_cast<std::uint32_t>(source.size()));
    });
    registry.Add("wcscoll_l", [](HleCall& call) {
        const std::uint32_t left = call.Arg(0);
        const std::uint32_t right = call.Arg(1);
        for (std::uint32_t index = 0;; ++index) {
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            call.CopyIn(left + index * 4, &a, sizeof(a));
            call.CopyIn(right + index * 4, &b, sizeof(b));
            if (a != b) {
                call.Return(a < b ? kEof : 1);
                return;
            }
            if (a == 0) {
                call.Return(0);
                return;
            }
        }
    });
    registry.Add("wcsxfrm_l", [](HleCall& call) {
        const std::uint32_t source = call.Arg(1);
        const std::uint32_t size = call.Arg(2);
        std::uint32_t length = 0;
        for (;; ++length) {
            std::uint32_t value = 0;
            if (!call.CopyIn(source + length * 4, &value, sizeof(value)) || value == 0) {
                break;
            }
            if (size > 0 && length < size - 1) {
                call.CopyOut(call.Arg(0) + length * 4, &value, sizeof(value));
            }
        }
        if (size > 0) {
            const std::uint32_t zero = 0;
            call.CopyOut(call.Arg(0) + std::min<std::uint32_t>(length, size - 1) * 4, &zero, 4);
        }
        call.Return(length);
    });

    const auto add_wide_predicate = [&registry](const char* name,
                                                int (*predicate)(std::wint_t)) {
        registry.Add(name, [predicate](HleCall& call) {
            call.Return(static_cast<std::uint32_t>(
                predicate(static_cast<std::wint_t>(call.Arg(0))) != 0 ? 1u : 0u));
        });
    };
    add_wide_predicate("iswalpha_l", std::iswalpha);
    add_wide_predicate("iswblank_l", std::iswblank);
    add_wide_predicate("iswcntrl_l", std::iswcntrl);
    add_wide_predicate("iswdigit_l", std::iswdigit);
    add_wide_predicate("iswlower_l", std::iswlower);
    add_wide_predicate("iswprint_l", std::iswprint);
    add_wide_predicate("iswpunct_l", std::iswpunct);
    add_wide_predicate("iswspace_l", std::iswspace);
    add_wide_predicate("iswupper_l", std::iswupper);
    add_wide_predicate("iswxdigit_l", std::iswxdigit);

    registry.Add("towlower_l", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(
            std::towlower(static_cast<std::wint_t>(call.Arg(0)))));
    });
    registry.Add("towupper_l", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(
            std::towupper(static_cast<std::wint_t>(call.Arg(0)))));
    });

    registry.Add("strftime_l", [](HleCall& call) {
        const std::string format = ArgString(call, call.Arg(2));
        GuestTm guest{};
        if (!call.CopyIn(call.Arg(3), &guest, sizeof(guest))) {
            call.Return(0);
            return;
        }
        const struct tm host = FromGuestTm(guest);
        char buffer[1024];
        const std::size_t written = std::strftime(buffer, sizeof(buffer), format.c_str(), &host);
        if (written == 0) {
            call.Return(0);
            return;
        }
        PutString(call, call.Arg(0), buffer);
        call.Return(static_cast<std::uint32_t>(written));
    });

    // --- wide characters ----------------------------------------------------
    registry.Add("btowc", [](HleCall& call) {
        const int value = static_cast<int>(call.Arg(0));
        call.Return(value == EOF ? kEof : static_cast<std::uint32_t>(value));
    });
    registry.Add("wctob", [](HleCall& call) {
        const std::uint32_t value = call.Arg(0);
        call.Return(value < 128 ? value : kEof);
    });
    registry.Add("mbrtowc", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(1), 16);
        std::mbstate_t state{};
        if (call.Arg(3) != 0) {
            call.CopyIn(call.Arg(3), &state, sizeof(state));
        }
        wchar_t wide = 0;
        const std::size_t result =
            std::mbrtowc(&wide, text.empty() ? "" : text.c_str(), text.size(), &state);
        if (call.Arg(3) != 0) {
            call.CopyOut(call.Arg(3), &state, sizeof(state));
        }
        if (call.Arg(0) != 0) {
            call.CopyOut(call.Arg(0), &wide, sizeof(wide));
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("wcrtomb", [](HleCall& call) {
        std::mbstate_t state{};
        if (call.Arg(2) != 0) {
            call.CopyIn(call.Arg(2), &state, sizeof(state));
        }
        char buffer[16] = {};
        const std::size_t result =
            std::wcrtomb(buffer, static_cast<wchar_t>(call.Arg(1)), &state);
        if (call.Arg(2) != 0) {
            call.CopyOut(call.Arg(2), &state, sizeof(state));
        }
        if (result != static_cast<std::size_t>(-1) && call.Arg(0) != 0) {
            call.CopyOut(call.Arg(0), buffer, result);
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("mbrlen", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(0), 16);
        std::mbstate_t state{};
        if (call.Arg(2) != 0) {
            call.CopyIn(call.Arg(2), &state, sizeof(state));
        }
        const std::size_t result = std::mbrlen(text.c_str(), text.size(), &state);
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("mbtowc", [](HleCall& call) {
        const std::string text = ArgString(call, call.Arg(1), 16);
        wchar_t wide = 0;
        const int result = std::mbtowc(&wide, text.c_str(), text.size());
        if (result > 0 && call.Arg(0) != 0) {
            call.CopyOut(call.Arg(0), &wide, sizeof(wide));
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("mbsrtowcs", [](HleCall& call) {
        std::uint32_t source_pointer = 0;
        call.CopyIn(call.Arg(1), &source_pointer, sizeof(source_pointer));
        const std::string text = ArgString(call, source_pointer, 1u << 16);
        std::mbstate_t state{};
        if (call.Arg(3) != 0) {
            call.CopyIn(call.Arg(3), &state, sizeof(state));
        }
        std::size_t produced = 0;
        std::size_t consumed = 0;
        while (consumed < text.size()) {
            if (call.Arg(2) != 0 && produced >= call.Arg(2)) {
                break;
            }
            wchar_t wide = 0;
            const std::size_t step =
                std::mbrtowc(&wide, text.c_str() + consumed, text.size() - consumed, &state);
            if (step == static_cast<std::size_t>(-1) || step == static_cast<std::size_t>(-2)) {
                call.Return(kEof);
                return;
            }
            if (step == 0) {
                consumed = text.size();
                break;
            }
            if (call.Arg(0) != 0) {
                call.CopyOut(call.Arg(0) + static_cast<std::uint32_t>(produced) * 4, &wide,
                             sizeof(wide));
            }
            ++produced;
            consumed += step;
        }
        const std::uint32_t advanced =
            consumed >= text.size() ? 0 : source_pointer + static_cast<std::uint32_t>(consumed);
        call.CopyOut(call.Arg(1), &advanced, sizeof(advanced));
        if (call.Arg(0) != 0) {
            const std::uint32_t zero = 0;
            call.CopyOut(call.Arg(0) + static_cast<std::uint32_t>(produced) * 4, &zero,
                         sizeof(zero));
        }
        call.Return(static_cast<std::uint32_t>(produced));
    });

    registry.Add("mbsnrtowcs", [](HleCall& call) {
        std::uint32_t source_pointer = 0;
        call.CopyIn(call.Arg(1), &source_pointer, sizeof(source_pointer));
        const std::string text = ArgString(call, source_pointer, 1u << 16);
        const std::size_t limit = std::min<std::size_t>(call.Arg(2), text.size());
        std::mbstate_t state{};
        if (call.Arg(4) != 0) {
            call.CopyIn(call.Arg(4), &state, sizeof(state));
        }
        std::size_t produced = 0;
        std::size_t consumed = 0;
        while (consumed < limit) {
            if (call.Arg(3) != 0 && produced >= call.Arg(3)) {
                break;
            }
            wchar_t wide = 0;
            const std::size_t step =
                std::mbrtowc(&wide, text.c_str() + consumed, limit - consumed, &state);
            if (step == static_cast<std::size_t>(-1) || step == static_cast<std::size_t>(-2)) {
                call.Return(kEof);
                return;
            }
            if (step == 0) {
                consumed = limit;
                break;
            }
            if (call.Arg(0) != 0) {
                call.CopyOut(call.Arg(0) + static_cast<std::uint32_t>(produced) * 4, &wide,
                             sizeof(wide));
            }
            ++produced;
            consumed += step;
        }
        if (call.Arg(0) != 0 && consumed >= limit) {
            const std::uint32_t zero = 0;
            call.CopyOut(call.Arg(0) + static_cast<std::uint32_t>(produced) * 4, &zero,
                         sizeof(zero));
            call.CopyOut(call.Arg(1), &zero, sizeof(zero));
        } else {
            const std::uint32_t advanced = source_pointer + static_cast<std::uint32_t>(consumed);
            call.CopyOut(call.Arg(1), &advanced, sizeof(advanced));
        }
        call.Return(static_cast<std::uint32_t>(produced));
    });

    registry.Add("wcsnrtombs", [](HleCall& call) {
        std::uint32_t source_pointer = 0;
        call.CopyIn(call.Arg(1), &source_pointer, sizeof(source_pointer));
        const std::wstring text = ArgWideString(call, source_pointer);
        const std::size_t limit = std::min<std::size_t>(call.Arg(2), text.size());
        std::mbstate_t state{};
        if (call.Arg(4) != 0) {
            call.CopyIn(call.Arg(4), &state, sizeof(state));
        }
        std::size_t produced = 0;
        std::size_t consumed = 0;
        while (consumed < limit) {
            if (call.Arg(3) != 0 && produced >= call.Arg(3)) {
                break;
            }
            char buffer[8] = {};
            const std::size_t step = std::wcrtomb(buffer, text[consumed], &state);
            if (step == static_cast<std::size_t>(-1)) {
                call.Return(kEof);
                return;
            }
            if (call.Arg(0) != 0) {
                call.CopyOut(call.Arg(0) + static_cast<std::uint32_t>(produced), buffer, step);
            }
            produced += step;
            ++consumed;
        }
        const std::uint32_t advanced =
            consumed >= limit ? 0 : source_pointer + static_cast<std::uint32_t>(consumed) * 4;
        call.CopyOut(call.Arg(1), &advanced, sizeof(advanced));
        if (call.Arg(0) != 0) {
            const char zero = '\0';
            call.CopyOut(call.Arg(0) + static_cast<std::uint32_t>(produced), &zero, 1);
        }
        call.Return(static_cast<std::uint32_t>(produced));
    });

    // --- wide string conversions -------------------------------------------
    registry.Add("wcstol", [](HleCall& call) {
        const std::wstring text = ArgWideString(call, call.Arg(0));
        call.Return(static_cast<std::uint32_t>(
            std::wcstol(text.c_str(), nullptr, static_cast<int>(call.Arg(2)))));
    });
    registry.Add("wcstoul", [](HleCall& call) {
        const std::wstring text = ArgWideString(call, call.Arg(0));
        call.Return(static_cast<std::uint32_t>(
            std::wcstoul(text.c_str(), nullptr, static_cast<int>(call.Arg(2)))));
    });
    registry.Add("wcstoll", [](HleCall& call) {
        const std::wstring text = ArgWideString(call, call.Arg(0));
        ReturnI64(call, std::wcstoll(text.c_str(), nullptr, static_cast<int>(call.Arg(2))));
    });
    registry.Add("wcstoull", [](HleCall& call) {
        const std::wstring text = ArgWideString(call, call.Arg(0));
        ReturnI64(call, static_cast<std::int64_t>(
                            std::wcstoull(text.c_str(), nullptr, static_cast<int>(call.Arg(2)))));
    });
    registry.Add("wcstof", [](HleCall& call) {
        const std::wstring text = ArgWideString(call, call.Arg(0));
        const float value = std::wcstof(text.c_str(), nullptr);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        call.Return(bits);
    });
    registry.Add("wcstod", [](HleCall& call) {
        const std::wstring text = ArgWideString(call, call.Arg(0));
        ReturnDouble(call, std::wcstod(text.c_str(), nullptr));
    });
    registry.Add("wcstold", [](HleCall& call) {
        const std::wstring text = ArgWideString(call, call.Arg(0));
        ReturnDouble(call, std::wcstod(text.c_str(), nullptr));
    });

    registry.Add("swprintf", [](HleCall& call) {
        std::wstring format = ArgWideString(call, call.Arg(2));
        const std::string narrow_format = NarrowWide(format);
        const std::string rendered =
            detail::RenderFormat(detail::MakeCallReader(call, 3), narrow_format);
        const std::wstring wide = WidenNarrow(rendered);
        if (call.Arg(1) != 0 && wide.size() + 1 > call.Arg(1)) {
            call.Return(kEof);
            return;
        }
        PutWideString(call, call.Arg(0), wide, call.Arg(1));
        call.Return(static_cast<std::uint32_t>(wide.size()));
    });
    registry.Add("vswprintf", [](HleCall& call) {
        std::wstring format = ArgWideString(call, call.Arg(2));
        const std::string narrow_format = NarrowWide(format);
        const std::string rendered =
            detail::RenderFormat(detail::MakeVaListReader(call, call.Arg(3)), narrow_format);
        const std::wstring wide = WidenNarrow(rendered);
        if (call.Arg(1) != 0 && wide.size() + 1 > call.Arg(1)) {
            call.Return(kEof);
            return;
        }
        PutWideString(call, call.Arg(0), wide, call.Arg(1));
        call.Return(static_cast<std::uint32_t>(wide.size()));
    });

    registry.Add("sendfile", [&self](HleCall& call) {
        std::int32_t offset = 0;
        const bool has_offset = call.Arg(2) != 0;
        if (has_offset) {
            call.CopyIn(call.Arg(2), &offset, sizeof(offset));
        }
        off_t host_offset = offset;
        const ssize_t written =
            ::sendfile(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                       has_offset ? &host_offset : nullptr, call.Arg(3));
        if (written < 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        if (has_offset) {
            offset = static_cast<std::int32_t>(host_offset);
            call.CopyOut(call.Arg(2), &offset, sizeof(offset));
        }
        call.Return(static_cast<std::uint32_t>(written));
    });

    // --- logging (Android has no syslog; the messages are dropped) ----------
    registry.Add("openlog", [](HleCall& call) { call.Return(0); });
    registry.Add("closelog", [](HleCall& call) { call.Return(0); });
    registry.Add("syslog", [](HleCall& call) { call.Return(0); });

    // --- file system --------------------------------------------------------
    registry.Add("mkdir", [&self](HleCall& call) {
        if (::mkdir(ArgString(call, call.Arg(0)).c_str(),
                    static_cast<mode_t>(call.Arg(1))) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("link", [&self](HleCall& call) {
        if (::link(ArgString(call, call.Arg(0)).c_str(),
                   ArgString(call, call.Arg(1)).c_str()) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("symlink", [&self](HleCall& call) {
        if (::symlink(ArgString(call, call.Arg(0)).c_str(),
                      ArgString(call, call.Arg(1)).c_str()) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("unlinkat", [&self](HleCall& call) {
        if (::unlinkat(static_cast<int>(call.Arg(0)), ArgString(call, call.Arg(1)).c_str(),
                       static_cast<int>(call.Arg(2))) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("readlink", [&self](HleCall& call) {
        const std::uint32_t size = call.Arg(2);
        std::string buffer(size, '\0');
        const ssize_t written =
            ::readlink(ArgString(call, call.Arg(0)).c_str(), buffer.data(), size);
        if (written < 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.CopyOut(call.Arg(1), buffer.data(), static_cast<std::size_t>(written));
        call.Return(static_cast<std::uint32_t>(written));
    });
    registry.Add("realpath", [&self](HleCall& call) {
        char buffer[4096];
        if (::realpath(ArgString(call, call.Arg(0)).c_str(), buffer) == nullptr) {
            self.Fail(call, errno);
            call.Return(0);
            return;
        }
        if (call.Arg(1) != 0) {
            PutString(call, call.Arg(1), buffer);
            call.Return(call.Arg(1));
            return;
        }
        const std::uint32_t length = static_cast<std::uint32_t>(std::strlen(buffer) + 1);
        const std::uint32_t cell = self.heap.Allocate(length);
        if (cell == 0) {
            self.Fail(call, ENOMEM);
            call.Return(0);
            return;
        }
        PutString(call, cell, buffer);
        call.Return(cell);
    });
    registry.Add("truncate", [&self](HleCall& call) {
        if (::truncate(ArgString(call, call.Arg(0)).c_str(),
                       static_cast<off_t>(call.Arg(1))) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("ftruncate", [&self](HleCall& call) {
        if (::ftruncate(static_cast<int>(call.Arg(0)), static_cast<off_t>(call.Arg(1))) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("fchmod", [&self](HleCall& call) {
        if (::fchmod(static_cast<int>(call.Arg(0)), static_cast<mode_t>(call.Arg(1))) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("fchmodat", [&self](HleCall& call) {
        if (::fchmodat(static_cast<int>(call.Arg(0)), ArgString(call, call.Arg(1)).c_str(),
                       static_cast<mode_t>(call.Arg(2)), 0) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("openat", [&self](HleCall& call) {
        // ARM32 and ARM64 share the O_DIRECTORY/O_NOFOLLOW/O_DIRECT/O_LARGEFILE
        // encodings, so no translation is needed (and masking 0x8000 used to
        // clear O_NOFOLLOW).
        const int flags = static_cast<int>(call.Arg(2));
        const std::string path = ArgString(call, call.Arg(1));
        const bool traced = path.find("ShaderCache") != std::string::npos;
        if (traced && LogAllowed("shadercache-openat", 40)) {
            ARMBAT_LOG_INFO("Armbat", "file: openat %s flags=0x%x",
                                path.c_str(), flags);
        }
        const int fd = ::openat(static_cast<int>(call.Arg(0)), path.c_str(), flags,
                                static_cast<int>(call.Arg(3)));
        if (fd < 0) {
            if (traced && LogAllowed("shadercache-openat-fail", 20)) {
                ARMBAT_LOG_INFO("Armbat", "file: openat %s failed errno=%d",
                                    path.c_str(), errno);
            }
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(static_cast<std::uint32_t>(fd));
    });
    registry.Add("ioctl", [&self](HleCall& call) {
        const int result = ::ioctl(static_cast<int>(call.Arg(0)),
                                   static_cast<unsigned long>(call.Arg(1)),
                                   call.Arg(2) == 0 ? nullptr : call.Mapped(call.Arg(2), 1));
        if (result < 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("pathconf", [&self](HleCall& call) {
        const long value = ::pathconf(ArgString(call, call.Arg(0)).c_str(),
                                      static_cast<int>(call.Arg(1)));
        if (value < 0) {
            self.Fail(call, errno);
        }
        call.Return(static_cast<std::uint32_t>(value));
    });
    registry.Add("utimensat", [&self](HleCall& call) {
        timespec times[2] = {};
        if (call.Arg(2) != 0) {
            GuestTimespec guest[2] = {};
            if (!call.CopyIn(call.Arg(2), guest, sizeof(guest))) {
                self.Fail(call, EFAULT);
                call.Return(kEof);
                return;
            }
            times[0] = {guest[0].seconds, guest[0].nanoseconds};
            times[1] = {guest[1].seconds, guest[1].nanoseconds};
        }
        if (::utimensat(static_cast<int>(call.Arg(0)), ArgString(call, call.Arg(1)).c_str(),
                        call.Arg(2) == 0 ? nullptr : times,
                        static_cast<int>(call.Arg(3))) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });

    registry.Add("stat", [&self](HleCall& call) {
        struct stat host {};
        if (::stat(ArgString(call, call.Arg(0)).c_str(), &host) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        const GuestStat guest = ToGuestStat(host);
        if (!call.CopyOut(call.Arg(1), &guest, sizeof(guest))) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("lstat", [&self](HleCall& call) {
        struct stat host {};
        if (::lstat(ArgString(call, call.Arg(0)).c_str(), &host) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        const GuestStat guest = ToGuestStat(host);
        if (!call.CopyOut(call.Arg(1), &guest, sizeof(guest))) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("fstat", [&self](HleCall& call) {
        struct stat host {};
        if (::fstat(static_cast<int>(call.Arg(0)), &host) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        const GuestStat guest = ToGuestStat(host);
        if (!call.CopyOut(call.Arg(1), &guest, sizeof(guest))) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        call.Return(0);
    });
    registry.Add("statvfs", [&self](HleCall& call) {
        struct statvfs host {};
        if (::statvfs(ArgString(call, call.Arg(0)).c_str(), &host) != 0) {
            self.Fail(call, errno);
            call.Return(kEof);
            return;
        }
        // 32-bit ARM layout: eleven 32-bit words plus bionic's f_type extension.
        std::uint32_t guest[12] = {};
        guest[0] = static_cast<std::uint32_t>(host.f_bsize);
        guest[1] = static_cast<std::uint32_t>(host.f_frsize);
        guest[2] = static_cast<std::uint32_t>(host.f_blocks);
        guest[3] = static_cast<std::uint32_t>(host.f_bfree);
        guest[4] = static_cast<std::uint32_t>(host.f_bavail);
        guest[5] = static_cast<std::uint32_t>(host.f_files);
        guest[6] = static_cast<std::uint32_t>(host.f_ffree);
        guest[7] = static_cast<std::uint32_t>(host.f_favail);
        guest[8] = static_cast<std::uint32_t>(host.f_fsid);
        guest[9] = static_cast<std::uint32_t>(host.f_flag);
        guest[10] = static_cast<std::uint32_t>(host.f_namemax);
        // bionic adds f_type; the host struct has no equivalent here.
        guest[11] = 0;
        call.CopyOut(call.Arg(1), guest, sizeof(guest));
        call.Return(0);
    });

    // --- directory streams --------------------------------------------------
    registry.Add("opendir", [&self](HleCall& call) {
        DIR* stream = ::opendir(ArgString(call, call.Arg(0)).c_str());
        if (stream == nullptr) {
            self.Fail(call, errno);
            call.Return(0);
            return;
        }
        const std::uint32_t handle = self.RegisterDir(stream);
        if (handle == 0) {
            ::closedir(stream);
            self.Fail(call, EMFILE);
            call.Return(0);
            return;
        }
        call.Return(handle);
    });
    registry.Add("fdopendir", [&self](HleCall& call) {
        DIR* stream = ::fdopendir(static_cast<int>(call.Arg(0)));
        if (stream == nullptr) {
            self.Fail(call, errno);
            call.Return(0);
            return;
        }
        const std::uint32_t handle = self.RegisterDir(stream);
        if (handle == 0) {
            ::closedir(stream);
            self.Fail(call, EMFILE);
            call.Return(0);
            return;
        }
        call.Return(handle);
    });
    registry.Add("closedir", [&self](HleCall& call) {
        const int index = self.DirIndex(call.Arg(0));
        if (index < 0) {
            self.Fail(call, EBADF);
            call.Return(kEof);
            return;
        }
        ::closedir(self.dirs[index]);
        self.dirs[index] = nullptr;
        call.Return(0);
    });
    registry.Add("readdir", [&self](HleCall& call) {
        const int index = self.DirIndex(call.Arg(0));
        if (index < 0) {
            self.Fail(call, EBADF);
            call.Return(0);
            return;
        }
        dirent* entry = ::readdir(self.dirs[index]);
        if (entry == nullptr) {
            call.Return(0);
            return;
        }
        // 32-bit ARM dirent: 64-bit inode and offset, name at offset 19.
        std::uint8_t buffer[kDirentSize] = {};
        std::memcpy(buffer + 0, &entry->d_ino, 8);
        const std::int64_t offset = entry->d_off;
        std::memcpy(buffer + 8, &offset, 8);
        const std::uint16_t length = static_cast<std::uint16_t>(
            offsetof(dirent, d_name) + std::strlen(entry->d_name) + 1);
        std::memcpy(buffer + 16, &length, 2);
        buffer[18] = entry->d_type;
        std::memcpy(buffer + 19, entry->d_name, std::strlen(entry->d_name) + 1);
        call.CopyOut(self.dirent_cell, buffer, sizeof(buffer));
        call.Return(self.dirent_cell);
    });

    // --- heap-backed helpers ------------------------------------------------
    registry.Add("posix_memalign", [&self](HleCall& call) {
        const std::uint32_t address = self.heap.Allocate(call.Arg(2));
        if (address == 0) {
            call.Return(12);  // ENOMEM
            return;
        }
        call.CopyOut(call.Arg(0), &address, sizeof(address));
        call.Return(0);
    });

    registry.Add("vasprintf", [&self](HleCall& call) {
        std::string format;
        if (!call.ReadCString(call.Arg(1), format, 8192)) {
            self.Fail(call, EFAULT);
            call.Return(kEof);
            return;
        }
        const std::string text =
            detail::RenderFormat(detail::MakeVaListReader(call, call.Arg(2)), format);
        const std::uint32_t address =
            self.heap.Allocate(static_cast<std::uint32_t>(text.size() + 1));
        if (address == 0) {
            self.Fail(call, ENOMEM);
            call.Return(kEof);
            return;
        }
        PutString(call, address, text);
        call.CopyOut(call.Arg(0), &address, sizeof(address));
        call.Return(static_cast<std::uint32_t>(text.size()));
    });

    registry.Add("strerror_r", [](HleCall& call) {
        const char* message = std::strerror(static_cast<int>(call.Arg(0)));
        const std::uint32_t size = call.Arg(2);
        const std::size_t length = std::strlen(message);
        const std::size_t capped =
            size == 0 ? 0 : std::min(length, static_cast<std::size_t>(size - 1));
        if (capped != 0) {
            call.CopyOut(call.Arg(1), message, capped);
        }
        if (size != 0) {
            const char terminator = '\0';
            call.CopyOut(call.Arg(1) + static_cast<std::uint32_t>(capped), &terminator, 1);
        }
        call.Return(0);
    });

    registry.Add("sscanf", [](HleCall& call) {
        const std::string input = ArgString(call, call.Arg(0), 1u << 16);
        const std::string format = ArgString(call, call.Arg(1));
        auto reader = detail::MakeCallReader(call, 2);
        call.Return(static_cast<std::uint32_t>(ScanArguments(call, input, format, reader)));
    });
    registry.Add("vsscanf", [](HleCall& call) {
        const std::string input = ArgString(call, call.Arg(0), 1u << 16);
        const std::string format = ArgString(call, call.Arg(1));
        auto reader = detail::MakeVaListReader(call, call.Arg(2));
        call.Return(static_cast<std::uint32_t>(ScanArguments(call, input, format, reader)));
    });
}

GuestLibc::GuestLibc(HleRegistry& registry, GuestHeap& heap)
    : state_(std::make_unique<State>(registry, heap)) {}

GuestLibc::~GuestLibc() = default;

bool GuestLibc::Install(std::string* error) {
    State& self = *state_;
    HleRegistry& registry = self.registry;

    self.dir_base = registry.AllocateData(kMaxDirs * kDirStride);
    self.dirent_cell = registry.AllocateData(kDirentSize);
    self.locale_cell = registry.AllocateData(64);
    self.lconv_cell = registry.AllocateData(64);
    if (self.dir_base == 0 || self.dirent_cell == 0 || self.locale_cell == 0 ||
        self.lconv_cell == 0) {
        if (error) *error = "cannot map the libc data cells";
        return false;
    }
    for (std::uint32_t& cell : self.env_cells) {
        cell = registry.AllocateData(kEnvCellSize);
        if (cell == 0) {
            if (error) *error = "cannot map the environment cells";
            return false;
        }
    }
    self.dirs.assign(kMaxDirs, nullptr);

    const auto write = [&registry](std::uint32_t cell, const char* text) {
        const std::size_t length = std::strlen(text);
        std::memcpy(registry.Arena().Ptr(cell), text, length + 1);
    };
    write(self.locale_cell, "C");

    // struct lconv: ten string pointers followed by the numeric fields. Only the
    // decimal point is non-empty in the C locale.
    const std::uint32_t decimal_point = registry.AllocateData(8);
    const std::uint32_t empty = registry.AllocateData(8);
    if (decimal_point == 0 || empty == 0) {
        if (error) *error = "cannot map the locale strings";
        return false;
    }
    write(decimal_point, ".");
    write(empty, "");
    std::uint32_t lconv[16] = {};
    lconv[0] = decimal_point;  // decimal_point
    for (std::uint32_t i = 1; i < 10; ++i) {
        lconv[i] = empty;
    }
    for (std::uint32_t i = 10; i < 16; ++i) {
        lconv[i] = 0x7F7F7F7F;  // CHAR_MAX for the char fields
    }
    std::memcpy(registry.Arena().Ptr(self.lconv_cell), lconv, sizeof(lconv));

    self.Register();
    return true;
}

}  // namespace armbat
