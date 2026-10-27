#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>

namespace armbat {

// How loud a record is. The numbers match the Java side (ArmbatLog.kt) and the
// logger's own filter, so nothing has to translate between them.
enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

// Starts the asynchronous logger.
//
// A record is formatted on the calling thread and handed to a fixed-size
// lock-free queue; a writer thread drains it into `directory/current.log` and
// mirrors it to logcat. Nothing blocks on the disk, and nothing allocates for
// the file: a call costs a timestamp and a vsnprintf.
//
// The directory is created when missing; `ARMBAT_LOG_DIR` is used when the
// caller passes an empty string. Returns false when the file cannot be opened,
// in which case records still reach logcat, exactly as before.
bool LogStart(const std::string& directory);

// Stops the writer thread after draining what is queued.
void LogStop();

// Both filters are independent on purpose: the file keeps everything worth
// reading later while logcat stays quiet enough to be usable over adb.
void LogSetLevels(LogLevel file, LogLevel logcat);
LogLevel LogFileLevel();
bool LogRunning();
std::string LogDirectory();
std::string LogFilePath();

// One record. `format` follows printf; a record that does not fit is truncated.
void LogWrite(LogLevel level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
void LogWriteV(LogLevel level, const char* tag, const char* format, va_list args);

// Already-rendered text, typically a guest's stdout. Embedded newlines become
// separate records so one guest line stays one log line, and a line longer than
// a record is split instead of lost.
void LogText(LogLevel level, const char* tag, std::string_view text);

// Blocks until everything queued so far is in the file. Used right after a
// fault report: the process may not live long enough for the writer to catch up.
void LogFlush();

// Last-resort path for a fatal signal handler: async-signal-safe, no
// allocation, may be called from any thread.
void LogPanic(const char* text);

// Android priorities, for the shims that receive them from the guest.
LogLevel LogLevelFromAndroid(int priority);

// Same as LogWrite, but takes an Android priority. Used where the priority is
// a value from the guest rather than a constant in this code.
void LogWriteAndroid(int priority, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

}  // namespace armbat

// The names the container's code uses. A tag names the subsystem, so a reader
// can follow one part of the runtime without inverting a filter.
#define ARMBAT_LOG_VERBOSE(tag, ...) \
    ::armbat::LogWrite(::armbat::LogLevel::Trace, tag, __VA_ARGS__)
#define ARMBAT_LOG_DEBUG(tag, ...) \
    ::armbat::LogWrite(::armbat::LogLevel::Debug, tag, __VA_ARGS__)
#define ARMBAT_LOG_INFO(tag, ...) \
    ::armbat::LogWrite(::armbat::LogLevel::Info, tag, __VA_ARGS__)
#define ARMBAT_LOG_WARN(tag, ...) \
    ::armbat::LogWrite(::armbat::LogLevel::Warn, tag, __VA_ARGS__)
#define ARMBAT_LOG_ERROR(tag, ...) \
    ::armbat::LogWrite(::armbat::LogLevel::Error, tag, __VA_ARGS__)
#define ARMBAT_LOG_FATAL(tag, ...) \
    ::armbat::LogWrite(::armbat::LogLevel::Error, tag, __VA_ARGS__)

#define ARMBAT_LOG_TEXT(level, tag, text) ::armbat::LogText(level, tag, text)
