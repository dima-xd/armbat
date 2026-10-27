#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <android/log.h>

#include "armbat/log.h"
#include <elf.h>
#include <sys/auxv.h>

#include "armbat/a32_machine.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_cpu.h"
#include "armbat/hle.h"

// The long tail of libc and libm: everything an ARMv7 image imports that the
// container can serve by simply calling the host. Layouts that differ between
// 32-bit and 64-bit (time, utsname, addrinfo, ctype tables) are converted here;
// everything else is the same ABI on both sides.

namespace armbat {
namespace {


namespace {

// A counting semaphore, because the container cannot hand a host sem_t to the
// guest and cannot mix libc implementations anyway.
class CountingSemaphore {
public:
    explicit CountingSemaphore(std::uint32_t initial) : count_(initial) {}

    void Post() {
        {
            const std::lock_guard lock(mutex_);
            ++count_;
        }
        ready_.notify_one();
    }

    void Wait() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::uint32_t count_ = 0;
};

std::vector<CountingSemaphore*> s_handles;

}  // namespace

float FloatOf(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t BitsOf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double DoubleOf(HleCall& call, std::size_t index) {
    const std::uint64_t bits = static_cast<std::uint64_t>(call.Arg(index)) |
                               (static_cast<std::uint64_t>(call.Arg(index + 1)) << 32);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void ReturnDouble(HleCall& call, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    call.ReturnPair(static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32));
}

std::uint64_t U64Of(HleCall& call, std::size_t index) {
    return static_cast<std::uint64_t>(call.Arg(index)) |
           (static_cast<std::uint64_t>(call.Arg(index + 1)) << 32);
}

void ReturnU64(HleCall& call, std::uint64_t value) {
    call.ReturnPair(static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32));
}

// bionic's 32-bit struct tm: nine ints, a 32-bit gmtoff, a 32-bit zone pointer.
struct GuestTm {
    std::int32_t sec, min, hour, mday, mon, year, wday, yday, isdst;
    std::int32_t gmtoff;
    std::uint32_t zone;
};

GuestTm ToGuest(const struct tm& value) {
    GuestTm out{};
    out.sec = value.tm_sec;
    out.min = value.tm_min;
    out.hour = value.tm_hour;
    out.mday = value.tm_mday;
    out.mon = value.tm_mon;
    out.year = value.tm_year;
    out.wday = value.tm_wday;
    out.yday = value.tm_yday;
    out.isdst = value.tm_isdst;
    out.gmtoff = static_cast<std::int32_t>(value.tm_gmtoff);
    out.zone = 0;
    return out;
}

struct tm FromGuest(const GuestTm& value) {
    struct tm out{};
    out.tm_sec = value.sec;
    out.tm_min = value.min;
    out.tm_hour = value.hour;
    out.tm_mday = value.mday;
    out.tm_mon = value.mon;
    out.tm_year = value.year;
    out.tm_wday = value.wday;
    out.tm_yday = value.yday;
    out.tm_isdst = value.isdst;
    return out;
}

// Minimal printf for __android_log_print: the guest passes 32-bit words, and a
// wrong width between the worlds would garble every log line.
//
// The same walk serves a va_list, which on ARM is a pointer to a block of
// consecutive 32-bit words: the guest's own va_start spilled the registers and
// the stack there. The only difference is that the block places a double on an
// eight-byte boundary.
class GuestArgs {
public:
    GuestArgs(HleCall& call, std::size_t first, bool block)
        : call_(call), block_(block), index_(first), offset_(first) {}

    std::uint32_t Word() {
        if (!block_) {
            return call_.Arg(index_++);
        }
        std::uint32_t value = 0;
        if (!call_.CopyIn(offset_, &value, sizeof(value))) {
            offset_ += sizeof(value);
            return 0;
        }
        offset_ += sizeof(value);
        return value;
    }

    double Double() {
        if (!block_) {
            const double value = DoubleOf(call_, index_);
            index_ += 2;
            return value;
        }
        if ((offset_ % 8) != 0) {
            offset_ += 4;
        }
        std::uint64_t bits = 0;
        if (!call_.CopyIn(offset_, &bits, sizeof(bits))) {
            offset_ += sizeof(bits);
            return 0.0;
        }
        offset_ += sizeof(bits);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

private:
    HleCall& call_;
    bool block_;
    std::size_t index_;
    std::uint32_t offset_;
};

std::string FormatArgs(HleCall& call, const std::string& format, GuestArgs args) {
    std::string out;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%' || i + 1 >= format.size()) {
            out += format[i];
            continue;
        }
        ++i;
        if (format[i] == '%') {
            out += '%';
            continue;
        }
        if (format[i] == 's') {
            std::string text;
            call.ReadCString(args.Word(), text, 4096);
            out += text;
            continue;
        }
        if (format[i] == 'l' && i + 1 < format.size() && format[i + 1] == 'l') {
            i += 1;
        }
        if (format[i] == 'l' || format[i] == 'z') {
            ++i;
        }
        if (i >= format.size()) {
            break;
        }
        char buffer[64];
        switch (format[i]) {
        case 'd':
        case 'i':
            std::snprintf(buffer, sizeof(buffer), "%d", static_cast<std::int32_t>(args.Word()));
            out += buffer;
            break;
        case 'u':
            std::snprintf(buffer, sizeof(buffer), "%u", args.Word());
            out += buffer;
            break;
        case 'x':
            std::snprintf(buffer, sizeof(buffer), "%x", args.Word());
            out += buffer;
            break;
        case 'p':
            std::snprintf(buffer, sizeof(buffer), "0x%x", args.Word());
            out += buffer;
            break;
        case 'f':
            std::snprintf(buffer, sizeof(buffer), "%f", args.Double());
            out += buffer;
            break;
        case 'c':
            out += static_cast<char>(args.Word());
            break;
        default:
            out += format[i];
            break;
        }
    }
    return out;
}

std::string FormatGuest(HleCall& call, const std::string& format, std::size_t first_arg) {
    return FormatArgs(call, format, GuestArgs(call, first_arg, false));
}

// Arguments of a va_list that the guest built, read from its own block.
std::string FormatVaList(HleCall& call, const std::string& format, std::uint32_t list) {
    return FormatArgs(call, format, GuestArgs(call, list, true));
}


}  // namespace

// Registers the part of libc/libm the ARMv7 images of real applications import.
void RegisterExtraFunctions(HleRegistry& registry) {
    // ---- libm, softfp in and out ------------------------------------------
    const auto add_f1 = [&registry](const char* name, float (*function)(float)) {
        registry.Add(name, [function](HleCall& call) {
            call.Return(BitsOf(function(FloatOf(call.Arg(0)))));
        });
    };
    const auto add_f2 = [&registry](const char* name, float (*function)(float, float)) {
        registry.Add(name, [function](HleCall& call) {
            call.Return(BitsOf(function(FloatOf(call.Arg(0)), FloatOf(call.Arg(1)))));
        });
    };
    add_f1("acosf", ::acosf);
    add_f1("asinf", ::asinf);
    add_f1("atanf", ::atanf);
    add_f1("ceilf", ::ceilf);
    add_f1("cosf", ::cosf);
    add_f1("expf", ::expf);
    add_f1("exp2f", ::exp2f);
    add_f1("floorf", ::floorf);
    add_f1("logf", ::logf);
    add_f1("log10f", ::log10f);
    add_f1("nearbyintf", ::nearbyintf);
    add_f1("rintf", ::rintf);
    add_f1("roundf", ::roundf);
    add_f1("sinf", ::sinf);
    add_f1("tanf", ::tanf);
    add_f1("truncf", ::truncf);
    add_f1("sqrtf", ::sqrtf);
    add_f1("fabsf", ::fabsf);
    add_f2("atan2f", ::atan2f);
    add_f2("powf", ::powf);
    add_f2("fmodf", ::fmodf);

    const auto add_d1 = [&registry](const char* name, double (*function)(double)) {
        registry.Add(name, [function](HleCall& call) {
            ReturnDouble(call, function(DoubleOf(call, 0)));
        });
    };
    const auto add_d2 = [&registry](const char* name, double (*function)(double, double)) {
        registry.Add(name, [function](HleCall& call) {
            ReturnDouble(call, function(DoubleOf(call, 0), DoubleOf(call, 2)));
        });
    };
    add_d1("acos", ::acos);
    add_d1("asin", ::asin);
    add_d1("atan", ::atan);
    add_d1("ceil", ::ceil);
    add_d1("cos", ::cos);
    add_d1("exp", ::exp);
    add_d1("exp2", ::exp2);
    add_d1("floor", ::floor);
    add_d1("log", ::log);
    add_d1("log10", ::log10);
    add_d1("sin", ::sin);
    add_d1("sqrt", ::sqrt);
    add_d1("tan", ::tan);
    add_d1("fabs", ::fabs);
    add_d2("atan2", ::atan2);
    add_d2("pow", ::pow);
    add_d2("fmod", ::fmod);

    // ---- ARM EABI soft-float helpers --------------------------------------
    registry.Add("__aeabi_dadd", [](HleCall& call) { ReturnDouble(call, DoubleOf(call, 0) + DoubleOf(call, 2)); });
    registry.Add("__aeabi_dsub", [](HleCall& call) { ReturnDouble(call, DoubleOf(call, 0) - DoubleOf(call, 2)); });
    registry.Add("__aeabi_dmul", [](HleCall& call) { ReturnDouble(call, DoubleOf(call, 0) * DoubleOf(call, 2)); });
    registry.Add("__aeabi_ddiv", [](HleCall& call) { ReturnDouble(call, DoubleOf(call, 0) / DoubleOf(call, 2)); });
    registry.Add("__aeabi_dcmpge", [](HleCall& call) {
        call.Return(DoubleOf(call, 0) >= DoubleOf(call, 2) ? 1 : 0);
    });
    registry.Add("__aeabi_dcmplt", [](HleCall& call) {
        call.Return(DoubleOf(call, 0) < DoubleOf(call, 2) ? 1 : 0);
    });
    registry.Add("__aeabi_i2d", [](HleCall& call) {
        ReturnDouble(call, static_cast<double>(static_cast<std::int32_t>(call.Arg(0))));
    });
    registry.Add("__aeabi_l2d", [](HleCall& call) {
        ReturnDouble(call, static_cast<double>(static_cast<std::int64_t>(U64Of(call, 0))));
    });
    registry.Add("__aeabi_ul2d", [](HleCall& call) {
        ReturnDouble(call, static_cast<double>(U64Of(call, 0)));
    });
    registry.Add("__aeabi_l2f", [](HleCall& call) {
        call.Return(BitsOf(static_cast<float>(static_cast<std::int64_t>(U64Of(call, 0)))));
    });
    registry.Add("__aeabi_ul2f", [](HleCall& call) {
        call.Return(BitsOf(static_cast<float>(U64Of(call, 0))));
    });
    registry.Add("__aeabi_d2lz", [](HleCall& call) {
        ReturnU64(call, static_cast<std::uint64_t>(static_cast<std::int64_t>(DoubleOf(call, 0))));
    });
    registry.Add("__aeabi_d2ulz", [](HleCall& call) {
        ReturnU64(call, static_cast<std::uint64_t>(DoubleOf(call, 0)));
    });
    registry.Add("__aeabi_f2lz", [](HleCall& call) {
        ReturnU64(call, static_cast<std::uint64_t>(static_cast<std::int64_t>(FloatOf(call.Arg(0)))));
    });
    registry.Add("__aeabi_f2ulz", [](HleCall& call) {
        ReturnU64(call, static_cast<std::uint64_t>(FloatOf(call.Arg(0))));
    });
    registry.Add("__aeabi_llsl", [](HleCall& call) { ReturnU64(call, U64Of(call, 0) << (call.Arg(2) & 63)); });
    registry.Add("__aeabi_llsr", [](HleCall& call) { ReturnU64(call, U64Of(call, 0) >> (call.Arg(2) & 63)); });
    registry.Add("__aeabi_ldiv0", [](HleCall& call) { ReturnU64(call, 0); });
    registry.Add("__gnu_ldivmod_helper", [](HleCall& call) {
        const auto left = static_cast<std::int64_t>(U64Of(call, 0));
        const auto right = static_cast<std::int64_t>(U64Of(call, 2));
        const std::int64_t quotient = right == 0 ? 0 : left / right;
        const std::int64_t remainder = right == 0 ? 0 : left % right;
        const std::uint32_t cell = call.Arg(4);
        if (cell != 0) {
            call.CopyOut(cell, &remainder, sizeof(remainder));
        }
        ReturnU64(call, static_cast<std::uint64_t>(quotient));
    });

    // ---- string -----------------------------------------------------------
    registry.Add("strchr", [](HleCall& call) {
        const std::uint32_t text = call.Arg(0);
        std::string value;
        call.ReadCString(text, value, 1 << 20);
        const auto position = value.find(static_cast<char>(call.Arg(1)));
        call.Return(position == std::string::npos ? 0 : text + static_cast<std::uint32_t>(position));
    });
    registry.Add("strrchr", [](HleCall& call) {
        const std::uint32_t text = call.Arg(0);
        std::string value;
        call.ReadCString(text, value, 1 << 20);
        const auto position = value.rfind(static_cast<char>(call.Arg(1)));
        call.Return(position == std::string::npos ? 0 : text + static_cast<std::uint32_t>(position));
    });
    registry.Add("strstr", [](HleCall& call) {
        const std::uint32_t haystack = call.Arg(0);
        std::string hay, needle;
        call.ReadCString(haystack, hay, 1 << 20);
        call.ReadCString(call.Arg(1), needle, 1 << 20);
        const auto position = hay.find(needle);
        call.Return(position == std::string::npos ? 0 : haystack + static_cast<std::uint32_t>(position));
    });
    registry.Add("strcpy", [](HleCall& call) {
        std::string value;
        call.ReadCString(call.Arg(1), value, 1 << 20);
        call.CopyOut(call.Arg(0), value.c_str(), value.size() + 1);
        call.Return(call.Arg(0));
    });
    registry.Add("strncpy", [](HleCall& call) {
        const std::uint32_t count = call.Arg(2);
        std::string value;
        call.ReadCString(call.Arg(1), value, 1 << 20);
        std::vector<char> buffer(count, 0);
        std::memcpy(buffer.data(), value.data(), std::min<std::size_t>(count, value.size()));
        call.CopyOut(call.Arg(0), buffer.data(), count);
        call.Return(call.Arg(0));
    });
    registry.Add("strcat", [](HleCall& call) {
        std::string left, right;
        call.ReadCString(call.Arg(0), left, 1 << 20);
        call.ReadCString(call.Arg(1), right, 1 << 20);
        left += right;
        call.CopyOut(call.Arg(0), left.c_str(), left.size() + 1);
        call.Return(call.Arg(0));
    });
    registry.Add("strdup", [&registry](HleCall& call) {
        std::string value;
        call.ReadCString(call.Arg(0), value, 1 << 20);
        const std::uint32_t cell = registry.AllocateData(static_cast<std::uint32_t>(value.size() + 1));
        call.CopyOut(cell, value.c_str(), value.size() + 1);
        call.Return(cell);
    });
    registry.Add("strlcpy", [](HleCall& call) {
        const std::uint32_t size = call.Arg(2);
        std::string value;
        call.ReadCString(call.Arg(1), value, 1 << 20);
        if (size > 0) {
            std::vector<char> buffer(size, 0);
            std::memcpy(buffer.data(), value.data(), std::min<std::size_t>(size - 1, value.size()));
            call.CopyOut(call.Arg(0), buffer.data(), size);
        }
        call.Return(static_cast<std::uint32_t>(value.size()));
    });
    registry.Add("strspn", [](HleCall& call) {
        std::string value, set;
        call.ReadCString(call.Arg(0), value, 1 << 20);
        call.ReadCString(call.Arg(1), set, 1 << 20);
        call.Return(static_cast<std::uint32_t>(value.find_first_not_of(set) == std::string::npos
                                                  ? value.size()
                                                  : value.find_first_not_of(set)));
    });
    registry.Add("strcspn", [](HleCall& call) {
        std::string value, set;
        call.ReadCString(call.Arg(0), value, 1 << 20);
        call.ReadCString(call.Arg(1), set, 1 << 20);
        call.Return(static_cast<std::uint32_t>(value.find_first_of(set) == std::string::npos
                                                  ? value.size()
                                                  : value.find_first_of(set)));
    });
    registry.Add("strpbrk", [](HleCall& call) {
        const std::uint32_t text = call.Arg(0);
        std::string value, set;
        call.ReadCString(text, value, 1 << 20);
        call.ReadCString(call.Arg(1), set, 1 << 20);
        const auto position = value.find_first_of(set);
        call.Return(position == std::string::npos ? 0 : text + static_cast<std::uint32_t>(position));
    });
    registry.Add("strcasecmp", [](HleCall& call) {
        std::string left, right;
        call.ReadCString(call.Arg(0), left, 1 << 20);
        call.ReadCString(call.Arg(1), right, 1 << 20);
        call.Return(static_cast<std::uint32_t>(::strcasecmp(left.c_str(), right.c_str())));
    });
    registry.Add("strncasecmp", [](HleCall& call) {
        std::string left, right;
        call.ReadCString(call.Arg(0), left, 1 << 20);
        call.ReadCString(call.Arg(1), right, 1 << 20);
        call.Return(static_cast<std::uint32_t>(::strncasecmp(left.c_str(), right.c_str(), call.Arg(2))));
    });
    registry.Add("strcoll", [](HleCall& call) {
        std::string left, right;
        call.ReadCString(call.Arg(0), left, 1 << 20);
        call.ReadCString(call.Arg(1), right, 1 << 20);
        call.Return(static_cast<std::uint32_t>(left.compare(right) < 0 ? -1 : left == right ? 0 : 1));
    });
    registry.Add("strxfrm", [](HleCall& call) {
        std::string value;
        call.ReadCString(call.Arg(1), value, 1 << 20);
        if (call.Arg(2) > 0 && call.Arg(0) != 0) {
            call.CopyOut(call.Arg(0), value.c_str(), std::min<std::size_t>(value.size() + 1, call.Arg(2)));
        }
        call.Return(static_cast<std::uint32_t>(value.size()));
    });
    registry.Add("memmem", [](HleCall& call) {
        const std::uint32_t haystack = call.Arg(0);
        std::vector<char> hay(call.Arg(1));
        std::vector<char> needle(call.Arg(3));
        call.CopyIn(haystack, hay.data(), hay.size());
        call.CopyIn(call.Arg(2), needle.data(), needle.size());
        if (needle.empty() || needle.size() > hay.size()) {
            call.Return(needle.empty() ? haystack : 0);
            return;
        }
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0) {
                call.Return(haystack + static_cast<std::uint32_t>(i));
                return;
            }
        }
        call.Return(0);
    });

    // ---- ctype ------------------------------------------------------------
    registry.Add("isalnum", [](HleCall& call) { call.Return(::isalnum(static_cast<int>(call.Arg(0))) ? 1 : 0); });
    registry.Add("isspace", [](HleCall& call) { call.Return(::isspace(static_cast<int>(call.Arg(0))) ? 1 : 0); });
    registry.Add("isupper", [](HleCall& call) { call.Return(::isupper(static_cast<int>(call.Arg(0))) ? 1 : 0); });
    registry.Add("isxdigit", [](HleCall& call) { call.Return(::isxdigit(static_cast<int>(call.Arg(0))) ? 1 : 0); });
    registry.Add("tolower", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::tolower(static_cast<int>(call.Arg(0))))); });
    registry.Add("toupper", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::toupper(static_cast<int>(call.Arg(0))))); });
    registry.Add("iswctype", [](HleCall& call) { call.Return(::iswctype(static_cast<wint_t>(call.Arg(0)), static_cast<wctype_t>(call.Arg(1))) ? 1 : 0); });
    registry.Add("iswspace", [](HleCall& call) { call.Return(::iswspace(static_cast<wint_t>(call.Arg(0))) ? 1 : 0); });
    registry.Add("towlower", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::towlower(static_cast<wint_t>(call.Arg(0))))); });
    registry.Add("towupper", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::towupper(static_cast<wint_t>(call.Arg(0))))); });
    registry.Add("wctype", [](HleCall& call) {
        std::string name;
        call.ReadCString(call.Arg(0), name, 256);
        call.Return(static_cast<std::uint32_t>(::wctype(name.c_str())));
    });
    registry.Add("mbstowcs", [](HleCall& call) {
        std::string value;
        call.ReadCString(call.Arg(1), value, 1 << 20);
        std::vector<wchar_t> buffer(value.size() + 1, 0);
        const std::size_t count = ::mbstowcs(buffer.data(), value.c_str(), value.size() + 1);
        if (count != static_cast<std::size_t>(-1)) {
            std::vector<std::uint32_t> narrow(count + 1, 0);
            for (std::size_t i = 0; i < count; ++i) {
                narrow[i] = static_cast<std::uint32_t>(buffer[i]);
            }
            call.CopyOut(call.Arg(0), narrow.data(), narrow.size() * 4);
        }
        call.Return(static_cast<std::uint32_t>(count));
    });
    registry.Add("wcstombs", [](HleCall& call) {
        const std::uint32_t count = call.Arg(2);
        std::vector<std::uint32_t> wide(count, 0);
        call.CopyIn(call.Arg(1), wide.data(), count * 4);
        std::wstring value;
        for (std::uint32_t code : wide) {
            if (code == 0) {
                break;
            }
            value += static_cast<wchar_t>(code);
        }
        std::string narrow(value.size() + 1, '\0');
        const std::size_t written = ::wcstombs(narrow.data(), value.c_str(), narrow.size());
        if (written != static_cast<std::size_t>(-1)) {
            call.CopyOut(call.Arg(0), narrow.data(), written + 1);
        }
        call.Return(static_cast<std::uint32_t>(written));
    });
    registry.Add("wmemcpy", [](HleCall& call) {
        const std::uint32_t count = call.Arg(2) * 4;
        std::vector<char> buffer(count);
        call.CopyIn(call.Arg(1), buffer.data(), count);
        call.CopyOut(call.Arg(0), buffer.data(), count);
        call.Return(call.Arg(0));
    });
    registry.Add("wmemmove", [](HleCall& call) {
        const std::uint32_t count = call.Arg(2) * 4;
        std::vector<char> buffer(count);
        call.CopyIn(call.Arg(1), buffer.data(), count);
        call.CopyOut(call.Arg(0), buffer.data(), count);
        call.Return(call.Arg(0));
    });
    registry.Add("wmemset", [](HleCall& call) {
        const std::uint32_t count = call.Arg(2);
        const std::uint32_t value = call.Arg(1);
        for (std::uint32_t i = 0; i < count; ++i) {
            call.CopyOut(call.Arg(0) + i * 4, &value, 4);
        }
        call.Return(call.Arg(0));
    });
    registry.Add("wcscoll", [](HleCall& call) { call.Return(0); });
    registry.Add("wcsxfrm", [](HleCall& call) { call.Return(0); });
    registry.Add("wcsftime", [](HleCall& call) { call.Return(0); });

    // ---- time -------------------------------------------------------------
    registry.Add("time", [](HleCall& call) {
        const auto now = static_cast<std::int32_t>(::time(nullptr));
        if (call.Arg(0) != 0) {
            call.CopyOut(call.Arg(0), &now, sizeof(now));
        }
        call.Return(static_cast<std::uint32_t>(now));
    });
    registry.Add("clock", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::clock()));
    });
    registry.Add("gettimeofday", [](HleCall& call) {
        struct timeval now{};
        ::gettimeofday(&now, nullptr);
        const std::int32_t values[2] = {static_cast<std::int32_t>(now.tv_sec),
                                        static_cast<std::int32_t>(now.tv_usec)};
        call.CopyOut(call.Arg(0), values, sizeof(values));
        if (call.Arg(1) != 0) {
            const std::int32_t zone[2] = {0, 0};
            call.CopyOut(call.Arg(1), zone, sizeof(zone));
        }
        call.Return(0);
    });
    registry.Add("localtime", [&registry](HleCall& call) {
        const std::int32_t now = static_cast<std::int32_t>(call.Arg(0));
        const std::time_t value = now;
        const struct tm* converted = ::localtime(&value);
        if (converted == nullptr) {
            call.Return(0);
            return;
        }
        const GuestTm out = ToGuest(*converted);
        const std::uint32_t cell = registry.AllocateData(sizeof(out));
        call.CopyOut(cell, &out, sizeof(out));
        call.Return(cell);
    });
    registry.Add("gmtime", [&registry](HleCall& call) {
        const std::int32_t now = static_cast<std::int32_t>(call.Arg(0));
        const std::time_t value = now;
        const struct tm* converted = ::gmtime(&value);
        if (converted == nullptr) {
            call.Return(0);
            return;
        }
        const GuestTm out = ToGuest(*converted);
        const std::uint32_t cell = registry.AllocateData(sizeof(out));
        call.CopyOut(cell, &out, sizeof(out));
        call.Return(cell);
    });
    const auto add_tm_r = [&registry](const char* name, struct tm* (*function)(const std::time_t*, struct tm*)) {
        registry.Add(name, [function](HleCall& call) {
            const std::int32_t now = static_cast<std::int32_t>(call.Arg(0));
            const std::time_t value = now;
            struct tm converted{};
            if (function(&value, &converted) == nullptr) {
                call.Return(0);
                return;
            }
            const GuestTm out = ToGuest(converted);
            call.CopyOut(call.Arg(1), &out, sizeof(out));
            call.Return(call.Arg(1));
        });
    };
    add_tm_r("localtime_r", ::localtime_r);
    add_tm_r("gmtime_r", ::gmtime_r);
    registry.Add("mktime", [](HleCall& call) {
        GuestTm input{};
        call.CopyIn(call.Arg(0), &input, sizeof(input));
        struct tm value = FromGuest(input);
        call.Return(static_cast<std::uint32_t>(static_cast<std::int32_t>(::mktime(&value))));
    });
    registry.Add("difftime", [](HleCall& call) {
        const auto left = static_cast<std::time_t>(static_cast<std::int32_t>(call.Arg(0)));
        const auto right = static_cast<std::time_t>(static_cast<std::int32_t>(call.Arg(1)));
        ReturnDouble(call, ::difftime(left, right));
    });
    registry.Add("ctime", [&registry](HleCall& call) {
        const auto now = static_cast<std::time_t>(static_cast<std::int32_t>(call.Arg(0)));
        const char* text = ::ctime(&now);
        if (text == nullptr) {
            call.Return(0);
            return;
        }
        const std::string value(text);
        const std::uint32_t cell = registry.AllocateData(static_cast<std::uint32_t>(value.size() + 1));
        call.CopyOut(cell, value.c_str(), value.size() + 1);
        call.Return(cell);
    });
    registry.Add("strftime", [](HleCall& call) {
        std::string format;
        call.ReadCString(call.Arg(2), format, 4096);
        GuestTm input{};
        call.CopyIn(call.Arg(3), &input, sizeof(input));
        struct tm value = FromGuest(input);
        std::vector<char> buffer(call.Arg(1) + 1, 0);
        const std::size_t written = ::strftime(buffer.data(), buffer.size(), format.c_str(), &value);
        if (written > 0) {
            call.CopyOut(call.Arg(0), buffer.data(), written + 1);
        }
        call.Return(static_cast<std::uint32_t>(written));
    });
    registry.Add("strptime", [](HleCall& call) {
        std::string text, format;
        call.ReadCString(call.Arg(0), text, 4096);
        call.ReadCString(call.Arg(1), format, 4096);
        struct tm value{};
        const char* end = ::strptime(text.c_str(), format.c_str(), &value);
        const GuestTm out = ToGuest(value);
        call.CopyOut(call.Arg(2), &out, sizeof(out));
        call.Return(end == nullptr ? 0 : static_cast<std::uint32_t>(call.Arg(0) + (end - text.c_str())));
    });
    registry.Add("tzset", [](HleCall&) { ::tzset(); });

    // ---- process, files, environment --------------------------------------
    registry.Add("getuid", [](HleCall& call) { call.Return(::getuid()); });
    registry.Add("geteuid", [](HleCall& call) { call.Return(::geteuid()); });
    registry.Add("getauxval", [&registry](HleCall& call) {
        // The guest sees the CPU the JIT emulates, not the host's. Forwarding the
        // host value is wrong three times over: the host is AArch64, it advertises
        // the ARMv8 crypto extensions, and its Advanced SIMD is the 32-register
        // ARMv8 one. OpenSSL read all three and jumped into its ARMv8 `sha1h`
        // routine and its vector routine.
        //
        // The emulated core is described once in guest_cpu.h, together with the
        // /proc/cpuinfo the same guest reads: a library that trusts one and is
        // denied by the other takes a path this core cannot run.
        constexpr std::uint32_t kHwcap = kGuestHwcap;
        static std::uint32_t random_block = 0;
        switch (call.Arg(0)) {
            case AT_HWCAP:
                call.Return(kHwcap);
                return;
            case AT_HWCAP2:
                // No AES/PMULL/SHA1/SHA2/CRC32: those are ARMv8-only and the
                // emulated core does not have them.
                call.Return(0);
                return;
            case AT_PAGESZ:
                call.Return(4096);
                return;
            case AT_CLKTCK:
                call.Return(100);
                return;
            case AT_SECURE:
                call.Return(0);
                return;
            case AT_RANDOM:
                if (random_block == 0) {
                    random_block = registry.AllocateData(16);
                    std::uint8_t bytes[16] = {};
                    for (std::uint8_t& byte : bytes) {
                        byte = static_cast<std::uint8_t>(std::random_device{}());
                    }
                    std::memcpy(registry.Arena().Ptr(random_block), bytes, sizeof(bytes));
                }
                call.Return(random_block);
                return;
            case AT_SYSINFO_EHDR:
                // The guest has no vDSO, and handing it the host's would make it
                // parse a 64-bit ELF as its own.
                call.Return(0);
                return;
            case AT_PLATFORM:
            case AT_EXECFN:
            case AT_BASE:
            case AT_ENTRY:
            case AT_PHDR:
            case AT_PHENT:
            case AT_PHNUM:
            case AT_FLAGS:
                call.Return(0);
                return;
            default:
                break;
        }
        const unsigned long value = ::getauxval(call.Arg(0));
        call.Return(static_cast<std::uint32_t>(value));
    });
    registry.Add("getrlimit", [](HleCall& call) {
        struct rlimit limit{};
        const int result = ::getrlimit(call.Arg(0), &limit);
        const std::uint32_t values[2] = {static_cast<std::uint32_t>(limit.rlim_cur),
                                         static_cast<std::uint32_t>(limit.rlim_max)};
        call.CopyOut(call.Arg(1), values, sizeof(values));
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("setpriority", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::setpriority(call.Arg(0), call.Arg(1), call.Arg(2))));
    });
    registry.Add("usleep", [](HleCall& call) {
        ::usleep(call.Arg(0));
        call.Return(0);
    });
    registry.Add("uname", [](HleCall& call) {
        struct utsname value{};
        ::uname(&value);
        // bionic's 32-bit utsname is five 65-byte fields, same as on 64-bit.
        char buffer[5 * 65] = {};
        std::memcpy(buffer + 0 * 65, value.sysname, sizeof(value.sysname));
        std::memcpy(buffer + 1 * 65, value.nodename, sizeof(value.nodename));
        std::memcpy(buffer + 2 * 65, value.release, sizeof(value.release));
        std::memcpy(buffer + 3 * 65, value.version, sizeof(value.version));
        std::memcpy(buffer + 4 * 65, value.machine, sizeof(value.machine));
        call.CopyOut(call.Arg(0), buffer, sizeof(buffer));
        call.Return(0);
    });
    registry.Add("setenv", [](HleCall& call) {
        std::string name, value;
        call.ReadCString(call.Arg(0), name, 4096);
        call.ReadCString(call.Arg(1), value, 4096);
        call.Return(static_cast<std::uint32_t>(::setenv(name.c_str(), value.c_str(), call.Arg(2))));
    });
    registry.Add("unsetenv", [](HleCall& call) {
        std::string name;
        call.ReadCString(call.Arg(0), name, 4096);
        call.Return(static_cast<std::uint32_t>(::unsetenv(name.c_str())));
    });
    registry.Add("access", [](HleCall& call) {
        std::string path;
        call.ReadCString(call.Arg(0), path, 4096);
        const int result = ::access(path.c_str(), call.Arg(1));
        if (path.find("ShaderCache") != std::string::npos && LogAllowed("shadercache-access", 40)) {
            ARMBAT_LOG_INFO("Armbat", "file: access %s mode=%u -> %d errno=%d",
                                path.c_str(), call.Arg(1), result, result == 0 ? 0 : errno);
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("chmod", [](HleCall& call) {
        std::string path;
        call.ReadCString(call.Arg(0), path, 4096);
        call.Return(static_cast<std::uint32_t>(::chmod(path.c_str(), call.Arg(1))));
    });
    registry.Add("unlink", [](HleCall& call) {
        std::string path;
        call.ReadCString(call.Arg(0), path, 4096);
        call.Return(static_cast<std::uint32_t>(::unlink(path.c_str())));
    });
    registry.Add("rmdir", [](HleCall& call) {
        std::string path;
        call.ReadCString(call.Arg(0), path, 4096);
        call.Return(static_cast<std::uint32_t>(::rmdir(path.c_str())));
    });
    registry.Add("fsync", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::fsync(call.Arg(0)))); });
    registry.Add("fdatasync", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::fdatasync(call.Arg(0)))); });
    registry.Add("fcntl", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::fcntl(call.Arg(0), call.Arg(1), call.Arg(2))));
    });
    registry.Add("pread", [](HleCall& call) {
        std::vector<char> buffer(call.Arg(2));
        const ssize_t read = ::pread(call.Arg(0), buffer.data(), buffer.size(), call.Arg(3));
        if (read > 0) {
            call.CopyOut(call.Arg(1), buffer.data(), static_cast<std::size_t>(read));
        }
        call.Return(static_cast<std::uint32_t>(read));
    });
    registry.Add("pipe", [](HleCall& call) {
        int fds[2] = {-1, -1};
        const int result = ::pipe(fds);
        ARMBAT_LOG_INFO("Armbat", "pipe %d,%d result=%d", fds[0], fds[1], result);
        call.CopyOut(call.Arg(0), fds, sizeof(fds));
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("poll", [](HleCall& call) {
        // struct pollfd is { int fd; short events; short revents; } on both sides.
        const std::uint32_t count = call.Arg(1);
        std::vector<char> buffer(static_cast<std::size_t>(count) * 8);
        call.CopyIn(call.Arg(0), buffer.data(), buffer.size());
        const int result = ::poll(reinterpret_cast<struct pollfd*>(buffer.data()),
                                  static_cast<nfds_t>(count), call.Arg(2));
        call.CopyOut(call.Arg(0), buffer.data(), buffer.size());
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("select", [](HleCall& call) {
        fd_set read, write, except;
        FD_ZERO(&read);
        FD_ZERO(&write);
        FD_ZERO(&except);
        const auto load = [&call](std::uint32_t address, fd_set* set) {
            if (address == 0) {
                return;
            }
            std::uint32_t words[32] = {};
            call.CopyIn(address, words, sizeof(words));
            for (int i = 0; i < 1024; ++i) {
                if ((words[i / 32] >> (i % 32)) & 1u) {
                    FD_SET(i, set);
                }
            }
        };
        const auto store = [&call](std::uint32_t address, const fd_set* set) {
            if (address == 0) {
                return;
            }
            std::uint32_t words[32] = {};
            for (int i = 0; i < 1024; ++i) {
                if (FD_ISSET(i, set)) {
                    words[i / 32] |= 1u << (i % 32);
                }
            }
            call.CopyOut(address, words, sizeof(words));
        };
        load(call.Arg(1), &read);
        load(call.Arg(2), &write);
        load(call.Arg(3), &except);
        const int result = ::select(static_cast<int>(call.Arg(0)), &read, &write, &except, nullptr);
        store(call.Arg(1), &read);
        store(call.Arg(2), &write);
        store(call.Arg(3), &except);
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("writev", [](HleCall& call) {
        const std::uint32_t count = call.Arg(2);
        std::vector<struct iovec> vectors(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t entry[2] = {};
            call.CopyIn(call.Arg(1) + i * 8, entry, sizeof(entry));
            vectors[i].iov_base = call.Mapped(entry[0], entry[1]);
            vectors[i].iov_len = entry[1];
        }
        call.Return(static_cast<std::uint32_t>(::writev(call.Arg(0), vectors.data(), count)));
    });
    registry.Add("if_nametoindex", [](HleCall& call) {
        std::string name;
        call.ReadCString(call.Arg(0), name, 256);
        call.Return(::if_nametoindex(name.c_str()));
    });
    registry.Add("if_indextoname", [](HleCall& call) {
        char buffer[IF_NAMESIZE] = {};
        const char* name = ::if_indextoname(call.Arg(0), buffer);
        if (name == nullptr) {
            call.Return(0);
            return;
        }
        call.CopyOut(call.Arg(1), buffer, sizeof(buffer));
        call.Return(call.Arg(1));
    });

    // ---- sockets ----------------------------------------------------------
    registry.Add("socket", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::socket(call.Arg(0), call.Arg(1), call.Arg(2)))); });
    registry.Add("bind", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::bind(call.Arg(0), reinterpret_cast<struct sockaddr*>(call.Mapped(call.Arg(1), call.Arg(2))), call.Arg(2))));
    });
    registry.Add("connect", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::connect(call.Arg(0), reinterpret_cast<struct sockaddr*>(call.Mapped(call.Arg(1), call.Arg(2))), call.Arg(2))));
    });
    registry.Add("listen", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::listen(call.Arg(0), call.Arg(1)))); });
    registry.Add("accept", [](HleCall& call) {
        socklen_t length = call.Arg(2) != 0 ? *reinterpret_cast<socklen_t*>(call.Mapped(call.Arg(2), 4)) : 0;
        const int result = ::accept(call.Arg(0), reinterpret_cast<struct sockaddr*>(call.Mapped(call.Arg(1), 128)), &length);
        if (call.Arg(2) != 0) {
            call.CopyOut(call.Arg(2), &length, 4);
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("send", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::send(call.Arg(0), call.Mapped(call.Arg(1), call.Arg(2)), call.Arg(2), call.Arg(3))));
    });
    registry.Add("recv", [](HleCall& call) {
        auto* buffer = call.Mapped(call.Arg(1), call.Arg(2));
        call.Return(static_cast<std::uint32_t>(::recv(call.Arg(0), buffer, call.Arg(2), call.Arg(3))));
    });
    registry.Add("sendto", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::sendto(call.Arg(0), call.Mapped(call.Arg(1), call.Arg(2)), call.Arg(2), call.Arg(3),
                                                       reinterpret_cast<struct sockaddr*>(call.Mapped(call.Arg(4), call.Arg(5))), call.Arg(5))));
    });
    registry.Add("recvfrom", [](HleCall& call) {
        auto* buffer = call.Mapped(call.Arg(1), call.Arg(2));
        socklen_t length = call.Arg(5) != 0 ? *reinterpret_cast<socklen_t*>(call.Mapped(call.Arg(5), 4)) : 0;
        const ssize_t read = ::recvfrom(call.Arg(0), buffer, call.Arg(2), call.Arg(3),
                                        reinterpret_cast<struct sockaddr*>(call.Mapped(call.Arg(4), 128)), &length);
        if (call.Arg(5) != 0) {
            call.CopyOut(call.Arg(5), &length, 4);
        }
        call.Return(static_cast<std::uint32_t>(read));
    });
    registry.Add("sendmsg", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(-1)); });
    registry.Add("recvmsg", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(-1)); });
    registry.Add("shutdown", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::shutdown(call.Arg(0), call.Arg(1)))); });
    registry.Add("getsockname", [](HleCall& call) {
        socklen_t length = call.Arg(2) != 0 ? *reinterpret_cast<socklen_t*>(call.Mapped(call.Arg(2), 4)) : 0;
        const int result = ::getsockname(call.Arg(0), reinterpret_cast<struct sockaddr*>(call.Mapped(call.Arg(1), 128)), &length);
        if (call.Arg(2) != 0) {
            call.CopyOut(call.Arg(2), &length, 4);
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("getpeername", [](HleCall& call) {
        socklen_t length = call.Arg(2) != 0 ? *reinterpret_cast<socklen_t*>(call.Mapped(call.Arg(2), 4)) : 0;
        const int result = ::getpeername(call.Arg(0), reinterpret_cast<struct sockaddr*>(call.Mapped(call.Arg(1), 128)), &length);
        if (call.Arg(2) != 0) {
            call.CopyOut(call.Arg(2), &length, 4);
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("getsockopt", [](HleCall& call) {
        socklen_t length = call.Arg(4) != 0 ? *reinterpret_cast<socklen_t*>(call.Mapped(call.Arg(4), 4)) : 0;
        const int result = ::getsockopt(call.Arg(0), call.Arg(1), call.Arg(2), call.Mapped(call.Arg(3), length), &length);
        if (call.Arg(4) != 0) {
            call.CopyOut(call.Arg(4), &length, 4);
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("setsockopt", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(::setsockopt(call.Arg(0), call.Arg(1), call.Arg(2),
                                                           call.Mapped(call.Arg(3), call.Arg(4)), call.Arg(4))));
    });
    registry.Add("inet_addr", [](HleCall& call) {
        std::string text;
        call.ReadCString(call.Arg(0), text, 64);
        struct in_addr address{};
        address.s_addr = ::inet_addr(text.c_str());
        call.Return(address.s_addr);
    });
    registry.Add("inet_ntoa", [&registry](HleCall& call) {
        struct in_addr address{};
        address.s_addr = call.Arg(0);
        const std::string text = ::inet_ntoa(address);
        const std::uint32_t cell = registry.AllocateData(static_cast<std::uint32_t>(text.size() + 1));
        call.CopyOut(cell, text.c_str(), text.size() + 1);
        call.Return(cell);
    });
    registry.Add("inet_ntop", [](HleCall& call) {
        std::vector<char> buffer(64, 0);
        const void* source = call.Mapped(call.Arg(1), 16);
        const char* text = ::inet_ntop(call.Arg(0), source, buffer.data(), buffer.size());
        if (text == nullptr) {
            call.Return(0);
            return;
        }
        call.CopyOut(call.Arg(2), buffer.data(), std::strlen(buffer.data()) + 1);
        call.Return(call.Arg(2));
    });
    registry.Add("inet_pton", [](HleCall& call) {
        std::string text;
        call.ReadCString(call.Arg(1), text, 64);
        void* target = call.Mapped(call.Arg(2), 16);
        call.Return(static_cast<std::uint32_t>(::inet_pton(call.Arg(0), text.c_str(), target)));
    });
    registry.Add("freeaddrinfo", [](HleCall& call) { (void)call; });
    registry.Add("gai_strerror", [&registry](HleCall& call) {
        const char* text = ::gai_strerror(call.Arg(0));
        const std::uint32_t cell = registry.AllocateData(static_cast<std::uint32_t>(std::strlen(text) + 1));
        call.CopyOut(cell, text, std::strlen(text) + 1);
        call.Return(cell);
    });
    registry.Add("getaddrinfo", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(EAI_NONAME)); });
    registry.Add("gethostbyname", [](HleCall& call) { call.Return(0); });
    registry.Add("getnameinfo", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(EAI_NONAME)); });
    registry.Add("gethostname", [](HleCall& call) {
        // bionic hands back "localhost" and refuses a buffer that cannot hold it;
        // writing past `len` used to trample whatever followed the caller's
        // buffer in the guest heap.
        const char* name = "localhost";
        const std::size_t length = std::strlen(name);
        const std::uint32_t capacity = call.Arg(1);
        if (capacity <= length) {
            const std::uint32_t cell = call.ErrnoCell();
            const std::uint32_t too_long = ENAMETOOLONG;
            call.CopyOut(cell, &too_long, sizeof(too_long));
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        if (!call.CopyOut(call.Arg(0), name, length + 1)) {
            const std::uint32_t cell = call.ErrnoCell();
            const std::uint32_t fault = EFAULT;
            call.CopyOut(cell, &fault, sizeof(fault));
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(0);
    });

    // ---- signals, assertions, logging -------------------------------------
    registry.Add("raise", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::raise(static_cast<int>(call.Arg(0))))); });
    registry.Add("bsd_signal", [](HleCall& call) { call.Return(0); });
    registry.Add("sigaction", [](HleCall& call) { call.Return(0); });
    registry.Add("sigprocmask", [](HleCall& call) { call.Return(0); });
    registry.Add("sigsetjmp", [](HleCall& call) { call.Return(0); });
    registry.Add("siglongjmp", [](HleCall& call) { call.Machine().Stop(); });
    registry.Add("__pthread_cleanup_push", [](HleCall&) {});
    registry.Add("__pthread_cleanup_pop", [](HleCall&) {});
    registry.Add("_exit", [](HleCall& call) { call.Machine().Stop(); });
    registry.Add("exit", [](HleCall& call) { call.Machine().Stop(); });
    registry.Add("__assert2", [](HleCall& call) {
        std::string file, function, message;
        call.ReadCString(call.Arg(0), file, 4096);
        call.ReadCString(call.Arg(1), function, 4096);
        call.ReadCString(call.Arg(2), message, 4096);
        ARMBAT_LOG_ERROR("Armbat", "assert %s:%d %s: %s", file.c_str(),
                            static_cast<int>(call.Arg(3)), function.c_str(), message.c_str());
        call.Machine().Stop();
    });
    registry.Add("__android_log_print", [](HleCall& call) {
        std::string tag, format;
        call.ReadCString(call.Arg(1), tag, 256);
        call.ReadCString(call.Arg(2), format, 4096);
        const std::string text = FormatGuest(call, format, 3);
        LogWriteAndroid(static_cast<int>(call.Arg(0)), tag.empty() ? "guest" : tag.c_str(), "%s",
                        text.c_str());
        call.Return(static_cast<std::uint32_t>(text.size()));
    });
    registry.Add("__android_log_write", [](HleCall& call) {
        std::string tag, text;
        call.ReadCString(call.Arg(1), tag, 256);
        call.ReadCString(call.Arg(2), text, 8192);
        LogWriteAndroid(static_cast<int>(call.Arg(0)), tag.empty() ? "guest" : tag.c_str(), "%s",
                        text.c_str());
        call.Return(static_cast<std::uint32_t>(text.size()));
    });
    // A library that logs through a va_list hands over its own argument block,
    // not the values in the ABI registers: reading it is the only way the line
    // reaches the journal, and a message a guest writes just before it stops is
    // often the only account of why.
    const auto log_vprint = [&registry](const char* name, std::size_t format_argument) {
        registry.Add(name, [format_argument](HleCall& call) {
            std::string tag, format;
            call.ReadCString(call.Arg(1), tag, 256);
            call.ReadCString(call.Arg(format_argument), format, 4096);
            const std::string text =
                FormatVaList(call, format, call.Arg(format_argument + 1));
            LogWriteAndroid(static_cast<int>(call.Arg(0)), tag.empty() ? "guest" : tag.c_str(),
                            "%s", text.c_str());
            call.Return(static_cast<std::uint32_t>(text.size()));
        });
    };
    log_vprint("__android_log_vprint", 2);
    // __android_log_buf_vprint(buffer, priority, tag, format, arguments)
    registry.Add("__android_log_buf_vprint", [](HleCall& call) {
        std::string tag, format;
        call.ReadCString(call.Arg(2), tag, 256);
        call.ReadCString(call.Arg(3), format, 4096);
        const std::string text = FormatVaList(call, format, call.Arg(4));
        LogWriteAndroid(static_cast<int>(call.Arg(1)), tag.empty() ? "guest" : tag.c_str(),
                        "%s", text.c_str());
        call.Return(static_cast<std::uint32_t>(text.size()));
    });
    registry.Add("__google_potentially_blocking_region_begin", [](HleCall&) {});
    registry.Add("__google_potentially_blocking_region_end", [](HleCall&) {});
    registry.Add("srand48", [](HleCall& call) { ::srand48(static_cast<long>(call.Arg(0))); });
    registry.Add("lrand48", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(::lrand48())); });

    RegisterAndroidFunctions(registry);


    registry.Add("pthread_getschedparam", [](HleCall& call) {
        if (call.Arg(2) != 0) {
            const std::int32_t policy = 0;  // SCHED_OTHER
            call.CopyOut(call.Arg(2), &policy, sizeof(policy));
        }
        // struct sched_param is a single int on both sides.
        if (call.Arg(3) != 0) {
            const std::int32_t priority = 0;
            call.CopyOut(call.Arg(3), &priority, sizeof(priority));
        }
        call.Return(0);
    });
    registry.Add("pthread_setschedparam", [](HleCall& call) { call.Return(0); });
    registry.Add("utime", [](HleCall& call) { call.Return(0); });
    registry.Add("_Unwind_DeleteException", [](HleCall&) {});
    registry.Add("_Unwind_GetDataRelBase", [](HleCall& call) { call.Return(0); });
    registry.Add("_Unwind_GetTextRelBase", [](HleCall& call) { call.Return(0); });
    registry.Add("_Unwind_GetRegionStart", [](HleCall& call) { call.Return(0); });
    registry.Add("_Unwind_GetLanguageSpecificData", [](HleCall& call) { call.Return(0); });
    registry.Add("_Unwind_RaiseException", [](HleCall& call) { call.Machine().Stop(); });
    registry.Add("_Unwind_Resume_or_Rethrow", [](HleCall& call) { call.Machine().Stop(); });
    registry.Add("__gnu_unwind_frame", [](HleCall& call) { call.Return(1); });

    // ---- data symbols -----------------------------------------------------
    // bionic exports these as *pointers* to tables, not as tables: libstdc++'s
    // ctype<char>::classic_table() is literally `_ctype_ + 1`, so a zero cell
    // turns every classification into a read from address 1. The tables are
    // indexed by `c + 1` so that entry 0 can describe EOF.
    //
    // The bytes below are bionic's own `_C_ctype_`, copied verbatim from
    // `ndk_cruft_data.o`/`ctype_.o` in the NDK's static libc (`sysroot/usr/lib/
    // arm-linux-androideabi/libc.a`, NDK 21). Bit values: 0x01 upper, 0x02 lower,
    // 0x04 digit, 0x08 space, 0x10 punct, 0x20 cntrl, 0x40 xdigit, 0x80 blank.
    // Notably bionic does not mark digits as xdigit and does not mark tab blank;
    // a table built from first principles gets those two wrong.
    {
        static constexpr std::uint8_t kBionicCtype[257] = {
            0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x28, 0x28,
            0x28, 0x28, 0x28, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
            0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x88, 0x10, 0x10,
            0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
            0x10, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x10,
            0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
            0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
            0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x10, 0x10, 0x10, 0x10,
            0x10, 0x10, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x02, 0x02, 0x02, 0x02,
            0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
            0x02, 0x02, 0x02, 0x02, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00,
        };

        const std::uint32_t cell = registry.AddData("_ctype_", 4);
        const std::uint32_t table = registry.AllocateData(257);
        std::memcpy(registry.Arena().Ptr(table), kBionicCtype, sizeof(kBionicCtype));
        std::memcpy(registry.Arena().Ptr(cell), &table, sizeof(table));

        // tolower/toupper are `const short*` to the same `c + 1` layout. In the
        // BSD/bionic convention every entry *is the character itself* when no
        // conversion applies, and only entry 0 (EOF) is -1; verified against
        // bionic's `_C_tolower_`/`_C_toupper_` in `ndk_cruft_data.o`. Filling the
        // table with -1 instead made every character convert to 0xFF, which is
        // how a URI scheme turned into "\xff\xff\xff\xff\xff".
        for (const char* name : {"_tolower_tab_", "_toupper_tab_"}) {
            const bool lower = std::strcmp(name, "_tolower_tab_") == 0;
            const std::uint32_t short_cell = registry.AddData(name, 4);
            const std::uint32_t short_table = registry.AllocateData(257 * 2);
            auto* entries = reinterpret_cast<std::int16_t*>(registry.Arena().Ptr(short_table));
            entries[0] = -1;  // EOF
            for (int c = 0; c < 256; ++c) {
                entries[1 + c] = static_cast<std::int16_t>(c);
            }
            for (int c = 0; c < 256; ++c) {
                if (lower && c >= 'A' && c <= 'Z') {
                    entries[1 + c] = static_cast<std::int16_t>(c - 'A' + 'a');
                }
                if (!lower && c >= 'a' && c <= 'z') {
                    entries[1 + c] = static_cast<std::int16_t>(c - 'a' + 'A');
                }
            }
            std::memcpy(registry.Arena().Ptr(short_cell), &short_table, sizeof(short_table));
        }
    }
    const std::uint32_t guard = registry.AddData("__stack_chk_guard", 4);
    const std::uint32_t canary = 0x1F2E3D4Cu;
    std::memcpy(registry.Arena().Ptr(guard), &canary, sizeof(canary));

    // ---- anonymous memory -------------------------------------------------
    // The kernel owns the address space; these are the libc wrappers an image
    // imports directly. Growth-only mapping keeps them inside the arena.
    registry.Add("mmap", [&registry](HleCall& call) {
        const std::uint32_t size = call.Arg(1);
        const std::uint32_t flags = call.Arg(3);
        const int fd = static_cast<int>(call.Arg(4));
        const std::uint32_t offset = call.Arg(5);
        const std::uint32_t cell = registry.AllocateData(size == 0 ? 1 : size);
        if (cell == 0) {
            const int error = ENOMEM;
            call.CopyOut(call.ErrnoCell(), &error, sizeof(error));
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        // A mapping of a file has to show the file's bytes. Handing the guest a
        // zeroed block instead makes every file read through mmap look like an
        // empty file, which is a silent wrong answer rather than a failure.
        if ((flags & MAP_ANONYMOUS) == 0 && fd >= 0 && size != 0) {
            void* target = call.Mapped(cell, size);
            if (target != nullptr) {
                const ssize_t got = ::pread(fd, target, size, static_cast<off_t>(offset));
                if (got < 0) {
                    const int error = errno;
                    call.CopyOut(call.ErrnoCell(), &error, sizeof(error));
                    call.Return(static_cast<std::uint32_t>(-1));
                    return;
                }
                if (LogAllowed("mmap-file", 20)) {
                    ARMBAT_LOG_INFO("Armbat",
                                        "mmap: fd=%d offset=%u size=%u -> %zd bytes", fd, offset,
                                        size, got);
                }
            }
        }
        call.Return(cell);
    });
    registry.Add("munmap", [](HleCall& call) { call.Return(0); });

    // ---- epoll -------------------------------------------------------------
    // Boost.Asio builds an epoll_reactor as the first service of every io_service
    // and throws system_error when the descriptor cannot be created, so a stub
    // here kills any application that touches Asio at all. Descriptors cross the
    // container unchanged, so the epoll set is a plain host one; only the event
    // record needs converting, because the guest's 64-bit `data` sits at offset 4
    // of a packed 12-byte struct.
    struct GuestEpollEvent {
        std::uint32_t events;
        std::uint64_t data;
    } __attribute__((packed));

    const auto store_errno = [](HleCall& call, int error) {
        const std::uint32_t cell = call.ErrnoCell();
        call.CopyOut(cell, &error, sizeof(error));
    };

    registry.Add("epoll_create", [store_errno](HleCall& call) {
        const int fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (fd < 0) {
            const int error = errno;
            store_errno(call, error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(static_cast<std::uint32_t>(fd));
    });
    registry.Add("epoll_create1", [store_errno](HleCall& call) {
        int flags = 0;
        if ((call.Arg(0) & EPOLL_CLOEXEC) != 0) {
            flags |= EPOLL_CLOEXEC;
        }
        const int fd = ::epoll_create1(flags);
        if (fd < 0) {
            const int error = errno;
            store_errno(call, error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(static_cast<std::uint32_t>(fd));
    });
    registry.Add("epoll_ctl", [store_errno](HleCall& call) {
        GuestEpollEvent guest{};
        struct epoll_event event {};
        struct epoll_event* pointer = nullptr;
        // EPOLL_CTL_DEL takes no event, and bionic callers pass a stale pointer
        // or null for it, so the record is only read for ADD and MOD.
        if (call.Arg(3) != 0 && call.Arg(1) != EPOLL_CTL_DEL) {
            if (!call.CopyIn(call.Arg(3), &guest, sizeof(guest))) {
                store_errno(call, EFAULT);
                call.Return(static_cast<std::uint32_t>(-1));
                return;
            }
            event.events = guest.events;
            event.data.u64 = guest.data;
            pointer = &event;
        }
        if (::epoll_ctl(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                        static_cast<int>(call.Arg(2)), pointer) != 0) {
            const int error = errno;
            store_errno(call, error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(0);
    });
    registry.Add("epoll_wait", [store_errno](HleCall& call) {
        const int max_events = static_cast<int>(call.Arg(2));
        if (max_events <= 0) {
            store_errno(call, EINVAL);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        std::vector<struct epoll_event> events(static_cast<std::size_t>(max_events));
        const int ready = ::epoll_wait(static_cast<int>(call.Arg(0)), events.data(), max_events,
                                      static_cast<int>(call.Arg(3)));
        if (ready < 0) {
            const int error = errno;
            store_errno(call, error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        for (int index = 0; index < ready; ++index) {
            const GuestEpollEvent guest{events[static_cast<std::size_t>(index)].events,
                                        events[static_cast<std::size_t>(index)].data.u64};
            if (!call.CopyOut(call.Arg(1) + static_cast<std::uint32_t>(index) * sizeof(guest),
                              &guest, sizeof(guest))) {
                store_errno(call, EFAULT);
                call.Return(static_cast<std::uint32_t>(-1));
                return;
            }
        }
        call.Return(static_cast<std::uint32_t>(ready));
    });
    // The guest's sigset_t is a 32-bit 128-byte mask and the host's is 512 bytes;
    // the container does not deliver guest signals, so the mask is dropped.
    registry.Add("epoll_pwait", [store_errno](HleCall& call) {
        const int max_events = static_cast<int>(call.Arg(2));
        if (max_events <= 0) {
            store_errno(call, EINVAL);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        std::vector<struct epoll_event> events(static_cast<std::size_t>(max_events));
        const int ready = ::epoll_pwait(static_cast<int>(call.Arg(0)), events.data(), max_events,
                                       static_cast<int>(call.Arg(3)), nullptr);
        if (ready < 0) {
            const int error = errno;
            store_errno(call, error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        for (int index = 0; index < ready; ++index) {
            const GuestEpollEvent guest{events[static_cast<std::size_t>(index)].events,
                                        events[static_cast<std::size_t>(index)].data.u64};
            if (!call.CopyOut(call.Arg(1) + static_cast<std::uint32_t>(index) * sizeof(guest),
                              &guest, sizeof(guest))) {
                store_errno(call, EFAULT);
                call.Return(static_cast<std::uint32_t>(-1));
                return;
            }
        }
        call.Return(static_cast<std::uint32_t>(ready));
    });

    // ---- unwinder entries the compiler emits on every function with a
    // landing pad: the container cannot unwind through HLE frames, so they
    // report "no handler" instead of walking into the host.
    registry.Add("__aeabi_unwind_cpp_pr0", [](HleCall& call) { call.Return(0); });
    registry.Add("__aeabi_unwind_cpp_pr1", [](HleCall& call) { call.Return(0); });
    registry.Add("__aeabi_unwind_cpp_pr2", [](HleCall& call) { call.Return(0); });
    registry.Add("qsort", [](HleCall& call) { (void)call; });
    registry.Add("vsprintf", [](HleCall& call) { call.Return(0); });
    registry.Add("putc", [](HleCall& call) { call.Return(static_cast<std::uint32_t>(call.Arg(0) & 0xFF)); });
    registry.Add("putwc", [](HleCall& call) { call.Return(call.Arg(0)); });

    // ---- semaphores -------------------------------------------------------
    // bionic's sem_t is a 32-bit cell on both sides; the container keeps the
    // real semaphore behind it, because the host's sem_t is 32 bytes.
    registry.Add("sem_init", [](HleCall& call) {
        auto* semaphore = new CountingSemaphore(static_cast<std::uint32_t>(call.Arg(2)));
        const std::uint32_t handle = static_cast<std::uint32_t>(s_handles.size());
        s_handles.push_back(semaphore);
        call.CopyOut(call.Arg(0), &handle, sizeof(handle));
        call.Return(0);
    });
    registry.Add("sem_destroy", [](HleCall& call) {
        call.Return(0);
    });
    registry.Add("sem_post", [](HleCall& call) {
        std::uint32_t handle = 0;
        call.CopyIn(call.Arg(0), &handle, sizeof(handle));
        if (handle < s_handles.size()) {
            s_handles[handle]->Post();
        }
        call.Return(0);
    });
    registry.Add("sem_wait", [](HleCall& call) {
        std::uint32_t handle = 0;
        call.CopyIn(call.Arg(0), &handle, sizeof(handle));
        if (handle < s_handles.size()) {
            s_handles[handle]->Wait();
        }
        call.Return(0);
    });

    // ---- more math with mixed integer arguments ---------------------------
    registry.Add("ldexp", [](HleCall& call) {
        ReturnDouble(call, ::ldexp(DoubleOf(call, 0), static_cast<int>(call.Arg(2))));
    });
    registry.Add("ldexpf", [](HleCall& call) {
        call.Return(BitsOf(::ldexpf(FloatOf(call.Arg(0)), static_cast<int>(call.Arg(1)))));
    });
    registry.Add("frexp", [](HleCall& call) {
        int exponent = 0;
        const double result = ::frexp(DoubleOf(call, 0), &exponent);
        if (call.Arg(2) != 0) {
            call.CopyOut(call.Arg(2), &exponent, sizeof(exponent));
        }
        ReturnDouble(call, result);
    });
}

}  // namespace armbat
