// A C++ library built the ordinary way: it links against libc++_shared.so and
// uses the standard library rather than declaring its own operators.
//
// Loading it through Armbat exercises the whole chain: the loader resolves
// DT_NEEDED to the real libc++ image that was opened first, the app's mangled
// imports are satisfied from libc++'s exports, and the std:: code then calls
// back into libc (malloc, memcpy) through HLE.

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int cppstd_string_ops() {
    std::string greeting = "hello";
    std::string suffix = " world";
    std::string combined = greeting + suffix;
    combined += "!";
    // 12 characters, "world" at index 6.
    return static_cast<int>(combined.size()) * 1000 + static_cast<int>(combined.find("world"));
}

extern "C" int cppstd_vector_sum(int count) {
    std::vector<int> values;
    for (int index = 0; index < count; ++index) {
        values.push_back(index * 3);
    }
    std::sort(values.rbegin(), values.rend());
    int sum = 0;
    for (int value : values) {
        sum += value;
    }
    return sum;
}

extern "C" int cppstd_map_lookup(int key) {
    std::string text = "abcdef";
    const std::size_t found = text.find(static_cast<char>('a' + key));
    return found == std::string::npos ? -1 : static_cast<int>(found);
}

// Atomically incrementing a counter is ldrex/strex in a retry loop: the same
// sequence libc++ uses for every shared_ptr refcount, and the reason a broken
// exclusive store shows up as a hang rather than a wrong answer.
extern "C" int cppstd_atomic(int loops) {
    std::atomic<int> counter{0};
    for (int index = 0; index < loops; ++index) {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    return counter.load();
}

// Exceptions travel through libc++'s __cxa_throw and the ARM EHABI unwinder.
// The message length is measured by hand so a failure distinguishes a broken
// unwinder (the catch never runs) from a broken std::string at the landing pad.
extern "C" int cppstd_exception(int divisor) {
    try {
        if (divisor == 0) {
            throw std::runtime_error("division by zero");
        }
        return 100 / divisor;
    } catch (const std::exception& error) {
        const char* text = error.what();
        if (text == nullptr) {
            return -9999;
        }
        int length = 0;
        while (length < 64 && text[length] != '\0') {
            ++length;
        }
        return -(1000 + length);
    }
}
