#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace armbat::detail {

inline std::string Format(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

// Collects PASS/FAIL lines for the on-device acceptance suites.
//
// The text is normally printed once at the end. Setting ARMBAT_TRACE mirrors
// every line to stderr as it happens, which is the only way to see how far a
// suite got when something hangs halfway.
class Reporter {
public:
    void Pass(const std::string& name, const std::string& detail = {}) {
        Record("PASS " + name + (detail.empty() ? "" : "  [" + detail + "]"));
    }

    void Fail(const std::string& name, const std::string& detail) {
        ++failures_;
        Record("FAIL " + name + ": " + detail);
    }

    void Check(const std::string& name, bool condition, const std::string& detail) {
        if (condition) {
            Pass(name, detail);
        } else {
            Fail(name, detail);
        }
    }

    int Failures() const { return failures_; }

    std::string Text(const std::string& header) const {
        std::string out = header;
        for (const auto& line : lines_) {
            out += line;
            out += '\n';
        }
        return out;
    }

private:
    void Record(std::string line) {
        if (std::getenv("ARMBAT_TRACE") != nullptr) {
            std::fprintf(stdout, "%s\n", line.c_str());
            std::fflush(stdout);
        }
        lines_.push_back(std::move(line));
    }

    std::vector<std::string> lines_;
    int failures_ = 0;
};

}  // namespace armbat::detail
