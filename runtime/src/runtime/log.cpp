#include "armbat/log.h"

#include <android/log.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include "armbat/version.h"

namespace armbat {
namespace {

// One session file, rotated when it grows: eight megabytes is a few minutes of
// a game loading and enough of a world session to explain a fault. The older
// files stay next to it so the run that produced a crash is still there after
// the next one started.
constexpr const char* kCurrentName = "current.log";
constexpr const char* kRotatedPrefix = "current.";
constexpr std::uint64_t kRotateBytes = 8ull * 1024 * 1024;
constexpr int kFilesToKeep = 4;

// The queue is fixed: a game that floods the log can slow the disk down, but it
// cannot make the logger allocate or block, and the drop counter says when a
// reader lost records.
constexpr std::uint32_t kQueueSize = 1024;
constexpr std::size_t kRecordText = 896;
constexpr std::size_t kWriteBuffer = 48 * 1024;
constexpr int kWriteTimeoutMs = 2000;

struct Record {
    std::atomic<std::uint64_t> seq{0};
    std::int64_t wall_us = 0;
    std::uint64_t position = 0;
    std::uint32_t tid = 0;
    std::uint16_t length = 0;
    std::uint8_t level = 0;
    char thread[16] = {};
    char tag[16] = {};
    char text[kRecordText] = {};
};

struct Queue {
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> committed{0};
    std::atomic<std::uint32_t> dropped{0};
    std::atomic<int> event{-1};
    std::atomic<bool> wake{false};
    Record records[kQueueSize];

    Queue() {
        for (std::uint32_t index = 0; index < kQueueSize; ++index) {
            records[index].seq.store(index, std::memory_order_relaxed);
        }
    }
};

Queue& Q() {
    static Queue queue;
    return queue;
}

struct State {
    std::atomic<int> file_level{static_cast<int>(LogLevel::Info)};
    std::atomic<int> logcat_level{static_cast<int>(LogLevel::Info)};
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<int> fd{-1};
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> consumed{0};

    std::mutex flush_mutex;
    std::condition_variable flush_cv;
    std::string directory;
    std::string path;
    std::thread writer;
};

State& S() {
    static State state;
    return state;
}

char LevelChar(int level) {
    switch (static_cast<LogLevel>(level)) {
        case LogLevel::Trace:
            return 'V';
        case LogLevel::Debug:
            return 'D';
        case LogLevel::Info:
            return 'I';
        case LogLevel::Warn:
            return 'W';
        case LogLevel::Error:
            return 'E';
        case LogLevel::Off:
            break;
    }
    return '?';
}

const char* LevelName(int level) {
    switch (static_cast<LogLevel>(level)) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Off:
            return "off";
    }
    return "?";
}

int AndroidPriority(int level) {    switch (static_cast<LogLevel>(level)) {
        case LogLevel::Trace:
            return ANDROID_LOG_VERBOSE;
        case LogLevel::Debug:
            return ANDROID_LOG_DEBUG;
        case LogLevel::Info:
            return ANDROID_LOG_INFO;
        case LogLevel::Warn:
            return ANDROID_LOG_WARN;
        case LogLevel::Error:
            return ANDROID_LOG_ERROR;
        case LogLevel::Off:
            break;
    }
    return ANDROID_LOG_INFO;
}

// The guest's own threads get no name from the JIT, and the platform's threads
// have the Java name; both are what a reader wants to see next to a tid.
const char* ThreadName() {
    thread_local char name[16] = {};
    thread_local bool ready = false;
    if (!ready) {
        name[0] = '\0';
        if (::pthread_getname_np(::pthread_self(), name, sizeof(name)) != 0) {
            name[0] = '\0';
        } else {
            // A guest thread inherits its creator's name; the tid is what tells
            // two of them apart, and the name is only a hint here.
            name[sizeof(name) - 1] = '\0';
        }
        ready = true;
    }
    return name;
}

std::int64_t NowMicros() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

void SignalWake() {
    // The state object owns the writer thread; touching it first keeps the
    // wakeup ordered after its construction.
    (void)S();
    Queue& queue = Q();
    if (queue.wake.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const int event = queue.event.load(std::memory_order_relaxed);
    if (event < 0) {
        return;
    }
    const std::uint64_t one = 1;
    (void)!::write(event, &one, sizeof(one));
}

struct Claim {
    Record* record = nullptr;
    std::uint64_t position = 0;
};

Claim ClaimRecord(LogLevel level, const char* tag) {
    Queue& queue = Q();
    const std::uint64_t position = queue.head.fetch_add(1, std::memory_order_relaxed);
    Record& record = queue.records[position & (kQueueSize - 1)];
    if (record.seq.load(std::memory_order_acquire) != position) {
        // The writer is behind by a whole ring; dropping is what keeps the
        // producer's cost bounded, and the drop counter reports it.
        queue.dropped.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    record.position = position;
    record.wall_us = NowMicros();
    record.tid = static_cast<std::uint32_t>(::gettid());
    record.level = static_cast<std::uint8_t>(level);
    std::snprintf(record.tag, sizeof(record.tag), "%s", tag != nullptr ? tag : "-");
    std::snprintf(record.thread, sizeof(record.thread), "%s", ThreadName());
    return Claim{&record, position};
}

void CommitRecord(const Claim& claim, std::size_t length) {
    if (claim.record == nullptr) {
        return;
    }
    const std::size_t cap = sizeof(claim.record->text) - 1;
    claim.record->length = static_cast<std::uint16_t>(length < cap ? length : cap);
    claim.record->seq.store(claim.position + 1, std::memory_order_release);
    Q().committed.fetch_add(1, std::memory_order_release);
    SignalWake();
}

std::size_t RenderLine(const Record& record, char* out, std::size_t capacity) {
    const time_t seconds = static_cast<time_t>(record.wall_us / 1000000);
    const int millis = static_cast<int>((record.wall_us / 1000) % 1000);
    tm broken{};
    ::localtime_r(&seconds, &broken);
    const int prefix = std::snprintf(
        out, capacity, "%04d-%02d-%02d %02d:%02d:%02d.%03d %c/%-10s tid=%-5u %-10s ", 
        broken.tm_year + 1900, broken.tm_mon + 1, broken.tm_mday, broken.tm_hour, broken.tm_min,
        broken.tm_sec, millis, LevelChar(record.level), record.tag, record.tid, record.thread);
    if (prefix < 0) {
        return 0;
    }
    std::size_t used = std::min<std::size_t>(static_cast<std::size_t>(prefix), capacity);
    const std::size_t room = capacity > used ? capacity - used - 1 : 0;
    const std::size_t text = std::min<std::size_t>(record.length, room);
    std::memcpy(out + used, record.text, text);
    used += text;
    out[used++] = '\n';
    return used;
}

bool WriteFully(int fd, const char* data, std::size_t length) {
    std::size_t written = 0;
    while (written < length) {
        const ssize_t result = ::write(fd, data + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

std::string RotatedName(int index) {
    return S().directory + "/" + kRotatedPrefix + std::to_string(index) + ".log";
}

// Caller owns the writer thread, so nothing else rotates at the same time.
void Rotate() {
    State& state = S();
    const int old = state.fd.exchange(-1, std::memory_order_acq_rel);
    if (old >= 0) {
        ::close(old);
    }
    std::error_code ignored;
    std::filesystem::remove(RotatedName(kFilesToKeep), ignored);
    for (int index = kFilesToKeep - 1; index >= 1; --index) {
        std::filesystem::rename(RotatedName(index), RotatedName(index + 1), ignored);
    }
    std::filesystem::rename(state.path, RotatedName(1), ignored);

    const int fresh =
        ::open(state.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    state.written.store(0, std::memory_order_release);
    state.fd.store(fresh, std::memory_order_release);
    if (fresh >= 0) {
        const std::string note = "-- log rotated at " + std::to_string(NowMicros() / 1000) +
                                 " ms; previous file is current.1.log\n";
        (void)WriteFully(fresh, note.data(), note.size());
    }
}

void AppendToFile(const std::string& buffer) {
    State& state = S();
    const int fd = state.fd.load(std::memory_order_acquire);
    if (fd < 0) {
        return;
    }
    if (!WriteFully(fd, buffer.data(), buffer.size())) {
        // A full disk must not spin: the file is gone for this session, logcat
        // keeps working, and one line says why.
        ARMBAT_LOG_ERROR("Armbat", "log: write to %s failed: %s",
                            state.path.c_str(), std::strerror(errno));
        state.fd.store(-1, std::memory_order_release);
        ::close(fd);
        return;
    }
    const std::uint64_t total = state.written.fetch_add(buffer.size()) + buffer.size();
    if (total >= kRotateBytes) {
        Rotate();
    }
}

void WriterLoop() {
    State& state = S();
    Queue& queue = Q();
    std::string buffer;
    buffer.reserve(kWriteBuffer);
    std::uint64_t position = 0;

    while (true) {
        const std::uint32_t dropped = queue.dropped.exchange(0, std::memory_order_relaxed);
        if (dropped != 0) {
            char note[96];
            const int length = std::snprintf(note, sizeof(note),
                                             "log: dropped %u records the writer could not keep up "
                                             "with\n",
                                             dropped);
            buffer.append(note, static_cast<std::size_t>(length));
        }

        for (;;) {
            Record& record = queue.records[position & (kQueueSize - 1)];
            if (record.seq.load(std::memory_order_acquire) != position + 1) {
                break;
            }
            if (static_cast<int>(record.level) >=
                state.logcat_level.load(std::memory_order_relaxed)) {
                // logcat is mirrored from this thread, so a slow log reader
                // delays the file a little but never the guest.
                __android_log_write(AndroidPriority(record.level), record.tag, record.text);
            }
            if (state.fd.load(std::memory_order_relaxed) >= 0) {
                char line[kRecordText + 160];
                const std::size_t length = RenderLine(record, line, sizeof(line));
                if (length != 0) {
                    buffer.append(line, length);
                }
            }
            record.seq.store(position + kQueueSize, std::memory_order_release);
            ++position;
            if (buffer.size() >= kWriteBuffer) {
                AppendToFile(buffer);
                buffer.clear();
            }
        }
        if (!buffer.empty()) {
            AppendToFile(buffer);
            buffer.clear();
        }
        state.consumed.store(position, std::memory_order_release);
        state.flush_cv.notify_all();

        if (state.stop.load(std::memory_order_acquire)) {
            return;
        }
        queue.wake.store(false, std::memory_order_release);
        Record& next = queue.records[position & (kQueueSize - 1)];
        if (next.seq.load(std::memory_order_acquire) == position + 1) {
            continue;
        }
        pollfd descriptor{queue.event.load(std::memory_order_relaxed), POLLIN, 0};
        if (descriptor.fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const int ready = ::poll(&descriptor, 1, 250);
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            std::uint64_t value = 0;
            (void)!::read(descriptor.fd, &value, sizeof(value));
        }
    }
}

LogLevel ParseLevel(const char* text, LogLevel fallback) {
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    if (std::strcmp(text, "trace") == 0 || std::strcmp(text, "verbose") == 0) {
        return LogLevel::Trace;
    }
    if (std::strcmp(text, "debug") == 0) {
        return LogLevel::Debug;
    }
    if (std::strcmp(text, "info") == 0) {
        return LogLevel::Info;
    }
    if (std::strcmp(text, "warn") == 0) {
        return LogLevel::Warn;
    }
    if (std::strcmp(text, "error") == 0) {
        return LogLevel::Error;
    }
    if (std::strcmp(text, "off") == 0 || std::strcmp(text, "silent") == 0) {
        return LogLevel::Off;
    }
    return fallback;
}

// `adb shell setprop debug.armbat.log debug` before a run is the whole control
// surface; there is no switch in the UI, and the property needs no rebuild.
void ApplyProperties() {
    State& state = S();
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.armbat.log", value) > 0) {
        state.file_level.store(static_cast<int>(ParseLevel(value, LogLevel::Info)));
    }
    value[0] = '\0';
    if (__system_property_get("debug.armbat.logcat", value) > 0) {
        state.logcat_level.store(static_cast<int>(ParseLevel(value, LogLevel::Info)));
    }
    if (const char* env = std::getenv("ARMBAT_LOG_LEVEL"); env != nullptr && *env != '\0') {
        state.file_level.store(static_cast<int>(ParseLevel(env, LogLevel::Info)));
    }
}

std::string Property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, value) <= 0) {
        return "-";
    }
    return value;
}

void WriteHeader(int fd) {
    char line[512];
    time_t seconds = ::time(nullptr);
    tm broken{};
    ::localtime_r(&seconds, &broken);
    const int length = std::snprintf(
        line, sizeof(line),
        "======== armbat %s pid=%d %04d-%02d-%02d %02d:%02d:%02d ========\n"
        "device: %s sdk=%s release=%s abi=%s page=%ld\n",
        VersionString(), static_cast<int>(::getpid()), broken.tm_year + 1900, broken.tm_mon + 1,
        broken.tm_mday, broken.tm_hour, broken.tm_min, broken.tm_sec,
        Property("ro.product.model").c_str(), Property("ro.build.version.sdk").c_str(),
        Property("ro.build.version.release").c_str(), Property("ro.product.cpu.abi").c_str(),
        static_cast<long>(::sysconf(_SC_PAGESIZE)));
    if (length > 0) {
        (void)WriteFully(fd, line, std::min<std::size_t>(static_cast<std::size_t>(length),
                                                         sizeof(line) - 1));
    }
    char levels[128];
    const int levels_length = std::snprintf(
        levels, sizeof(levels), "log: file=%s logcat=%s\n",
        LevelName(S().file_level.load(std::memory_order_relaxed)),
        LevelName(S().logcat_level.load(std::memory_order_relaxed)));
    if (levels_length > 0) {
        (void)WriteFully(fd, levels, static_cast<std::size_t>(levels_length));
    }
}

}  // namespace

bool LogStart(const std::string& directory) {
    State& state = S();
    if (state.started.load(std::memory_order_acquire)) {
        return true;
    }
    std::string target = directory;
    if (target.empty()) {
        if (const char* env = std::getenv("ARMBAT_LOG_DIR"); env != nullptr) {
            target = env;
        }
    }
    if (target.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(target, error);
    const std::string path = target + "/" + kCurrentName;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        ARMBAT_LOG_ERROR("Armbat", "log: cannot open %s: %s", path.c_str(),
                            std::strerror(errno));
        return false;
    }
    struct stat status {};
    if (::fstat(fd, &status) == 0) {
        state.written.store(static_cast<std::uint64_t>(status.st_size));
    }
    state.directory = target;
    state.path = path;
    state.fd.store(fd, std::memory_order_release);
    ApplyProperties();
    WriteHeader(fd);
    Q().event.store(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), std::memory_order_release);
    state.stop.store(false, std::memory_order_release);
    state.started.store(true, std::memory_order_release);
    state.writer = std::thread(WriterLoop);
    return true;
}

void LogStop() {
    State& state = S();
    if (!state.started.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    state.stop.store(true, std::memory_order_release);
    SignalWake();
    if (state.writer.joinable()) {
        state.writer.join();
    }
    const int event = Q().event.exchange(-1, std::memory_order_acq_rel);
    if (event >= 0) {
        ::close(event);
    }
    const int fd = state.fd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
        ::close(fd);
    }
}

void LogSetLevels(LogLevel file, LogLevel logcat) {
    S().file_level.store(static_cast<int>(file));
    S().logcat_level.store(static_cast<int>(logcat));
}

LogLevel LogFileLevel() { return static_cast<LogLevel>(S().file_level.load()); }

bool LogRunning() {
    return S().started.load(std::memory_order_acquire) &&
           S().fd.load(std::memory_order_acquire) >= 0;
}

std::string LogDirectory() { return S().directory; }

std::string LogFilePath() { return S().path; }

LogLevel LogLevelFromAndroid(int priority) {
    switch (priority) {
        case ANDROID_LOG_VERBOSE:
            return LogLevel::Trace;
        case ANDROID_LOG_DEBUG:
            return LogLevel::Debug;
        case ANDROID_LOG_INFO:
            return LogLevel::Info;
        case ANDROID_LOG_WARN:
            return LogLevel::Warn;
        case ANDROID_LOG_ERROR:
        case ANDROID_LOG_FATAL:
            return LogLevel::Error;
        default:
            return LogLevel::Info;
    }
}

void LogWriteV(LogLevel level, const char* tag, const char* format, va_list args) {
    State& state = S();
    const int value = static_cast<int>(level);
    const int file = state.file_level.load(std::memory_order_relaxed);
    const int logcat = state.logcat_level.load(std::memory_order_relaxed);
    if (value < file && value < logcat) {
        return;
    }
    if (!state.started.load(std::memory_order_acquire)) {
        char text[kRecordText];
        va_list copy;
        va_copy(copy, args);
        const int length = std::vsnprintf(text, sizeof(text), format, copy);
        va_end(copy);
        if (length < 0) {
            return;
        }
        __android_log_write(AndroidPriority(value), tag != nullptr ? tag : "-", text);
        return;
    }
    Claim claim = ClaimRecord(level, tag);
    if (claim.record == nullptr) {
        return;
    }
    va_list copy;
    va_copy(copy, args);
    const int length = std::vsnprintf(claim.record->text, sizeof(claim.record->text), format, copy);
    va_end(copy);
    if (length < 0) {
        claim.record->text[0] = '\0';
        CommitRecord(claim, 0);
        return;
    }
    CommitRecord(claim, static_cast<std::size_t>(length));
}

void LogWrite(LogLevel level, const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    LogWriteV(level, tag, format, args);
    va_end(args);
}

void LogWriteAndroid(int priority, const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    LogWriteV(LogLevelFromAndroid(priority), tag, format, args);
    va_end(args);
}

void LogText(LogLevel level, const char* tag, std::string_view text) {
    if (text.empty()) {
        return;
    }
    std::size_t offset = 0;
    while (offset <= text.size()) {
        std::size_t end = text.find('\n', offset);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(offset, end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        while (!line.empty()) {
            const std::size_t chunk = std::min<std::size_t>(line.size(), kRecordText - 1);
            LogWrite(level, tag, "%.*s", static_cast<int>(chunk), line.data());
            line.remove_prefix(chunk);
        }
        if (end == text.size()) {
            break;
        }
        offset = end + 1;
    }
}

void LogFlush() {
    State& state = S();
    if (!state.started.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint64_t target = Q().committed.load(std::memory_order_acquire);
    SignalWake();
    std::unique_lock<std::mutex> lock(state.flush_mutex);
    state.flush_cv.wait_for(lock, std::chrono::milliseconds(kWriteTimeoutMs), [&state, target] {
        return state.consumed.load(std::memory_order_acquire) >= target;
    });
}

void LogPanic(const char* text) {
    State& state = S();
    const int fd = state.fd.load(std::memory_order_acquire);
    if (fd >= 0) {
        const char* newline = "\n";
        (void)WriteFully(fd, newline, 1);
        (void)WriteFully(fd, text, std::strlen(text));
        (void)WriteFully(fd, newline, 1);
    }
    __android_log_write(ANDROID_LOG_FATAL, "Armbat", text);
}

}  // namespace armbat
