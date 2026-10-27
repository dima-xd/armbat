#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace armbat {

class HleCall;

namespace detail {

// Pulls arguments for a printf-style format. Two sources exist: an SVC call
// (arguments in r0-r3 and then the stack) and a guest va_list (a pointer to the
// contiguous argument block AAPCS builds).
struct ArgumentReader {
    std::function<std::uint32_t()> next32;
    std::function<std::uint64_t()> next64;
    std::function<bool(std::uint32_t, std::string&)> read_string;
    // UTF-32 guest wchar_t (4 bytes on ARM32) converted to UTF-8, for %ls.
    std::function<bool(std::uint32_t, std::string&)> read_wide;
};

// Renders a guest printf-style format. Each conversion is handed to the host
// snprintf with the value already extracted, which keeps width, precision and
// flags correct without reimplementing padding.
std::string RenderFormat(const ArgumentReader& arguments, const std::string& format);

// Arguments of the current HLE call, starting at `first_argument`.
ArgumentReader MakeCallReader(HleCall& call, std::size_t first_argument);

// Arguments of a guest va_list. In the ARM AAPCS a va_list is a pointer to the
// argument block: r0-r3 were spilled there by the prologue and the caller's
// stack arguments follow, so reading words in order and aligning 64-bit values
// is the whole algorithm.
ArgumentReader MakeVaListReader(HleCall& call, std::uint32_t va_list);

}  // namespace detail
}  // namespace armbat
