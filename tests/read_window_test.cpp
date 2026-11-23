// Checks armbat::ReadWindow against what a guest's read()/lseek() mean: the
// window changes where the bytes come from, nothing else. Runs on the device
// against a real file, because the point of the window is the storage underneath
// it and the position bookkeeping is what a wrong answer would corrupt.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include "armbat/read_window.h"

namespace {

int failures = 0;

void Check(bool condition, const char* what, long long got, long long expected) {
    if (condition) {
        printf("  ok   %s\n", what);
        return;
    }
    printf("  FAIL %s: got %lld, expected %lld\n", what, got, expected);
    ++failures;
}

// A file with a recognisable byte at every offset, so a window that hands back
// the wrong bytes is caught by the bytes themselves and not only by a count.
std::vector<std::uint8_t> MakeContent(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<std::uint8_t>((index * 7 + index / 251) & 0xFF);
    }
    return bytes;
}

std::int64_t ReadAt(armbat::ReadWindow& window, int fd, std::uint8_t* out, std::uint32_t length,
                    std::uint64_t generation) {
    if (window.Holds(generation, length)) {
        window.CopyHeld(out, length);
        return length;
    }
    return window.Fill(fd, out, length, generation);
}

void ExpectBytes(const char* what, const std::vector<std::uint8_t>& content, std::uint64_t offset,
                 const std::uint8_t* got, std::size_t length) {
    const bool same = offset + length <= content.size() &&
                      std::memcmp(content.data() + offset, got, length) == 0;
    Check(same, what, static_cast<long long>(length), static_cast<long long>(length));
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/data/local/tmp/read_window_test.bin";
    const std::size_t size = 300 * 1024 + 1234;
    const std::vector<std::uint8_t> content = MakeContent(size);

    const int write_fd = ::open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (write_fd < 0 || ::write(write_fd, content.data(), content.size()) !=
                            static_cast<ssize_t>(content.size())) {
        perror("create");
        return 2;
    }
    ::close(write_fd);

    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 2;
    }

    armbat::ReadWindow window;
    window.SetSize(size);
    std::vector<std::uint8_t> buffer(256 * 1024);

    printf("sequential reads\n");
    {
        std::uint64_t expected_position = 0;
        for (int step = 0; step < 5; ++step) {
            const std::int64_t got = ReadAt(window, fd, buffer.data(), 4096, 0);
            Check(got == 4096, "read returns the request", got, 4096);
            ExpectBytes("read returns the file's bytes", content, expected_position, buffer.data(),
                        4096);
            expected_position += 4096;
            Check(static_cast<long long>(window.position()) ==
                      static_cast<long long>(expected_position),
                  "position follows the reads", static_cast<long long>(window.position()),
                  static_cast<long long>(expected_position));
        }
    }

    printf("a byte at a time over one window\n");
    {
        std::int64_t moved = 0;
        Check(window.Seek(64 * 1024, SEEK_SET, &moved), "seek to a window start", moved, 65536);
        for (int step = 0; step < 4096; ++step) {
            if (!window.Seek(65536 + step, SEEK_SET, &moved)) {
                Check(false, "byte-wise seek inside the window", step, 0);
                break;
            }
            const std::int64_t got = ReadAt(window, fd, buffer.data(), 4096, 0);
            if (got != 4096) {
                Check(false, "byte-wise read returns the request", got, 4096);
                break;
            }
            // Every 512th step is compared in full; the rest check the first and
            // the last byte, which is where a window offset goes wrong first.
            if (step % 512 == 0) {
                ExpectBytes("byte-wise read returns the file's bytes", content,
                            65536 + step, buffer.data(), 4096);
            } else if (buffer[0] != content[65536 + step] ||
                       buffer[4095] != content[65536 + step + 4095]) {
                Check(false, "byte-wise read edges", buffer[0], content[65536 + step]);
                break;
            }
        }
        Check(true, "byte-wise walk over 4096 positions", 4096, 4096);
    }

    printf("a read that straddles the end of the window\n");
    {
        std::int64_t moved = 0;
        window.Seek(65536 + 4096 - 100, SEEK_SET, &moved);
        const std::int64_t got = ReadAt(window, fd, buffer.data(), 4096, 0);
        Check(got == 4096, "straddling read returns the request", got, 4096);
        ExpectBytes("straddling read returns the file's bytes", content, 65536 + 4096 - 100,
                    buffer.data(), 4096);
    }

    printf("seeks the window answers itself\n");
    {
        std::int64_t moved = 0;
        Check(window.Seek(0, SEEK_SET, &moved), "SEEK_SET", moved, 0);
        Check(window.Seek(1000, SEEK_CUR, &moved), "SEEK_CUR", moved, 1000);
        Check(window.Seek(-1000, SEEK_CUR, &moved), "SEEK_CUR backwards", moved, 0);
        Check(!window.Seek(-1, SEEK_SET, &moved), "SEEK_SET below zero is refused", 0, 0);
        Check(window.Seek(-10, SEEK_END, &moved), "SEEK_END", moved,
              static_cast<long long>(size) - 10);
        const std::int64_t got = ReadAt(window, fd, buffer.data(), 4, 0);
        Check(got == 4, "read at SEEK_END - 10", got, 4);
        ExpectBytes("read at SEEK_END - 10 returns the file's bytes", content, size - 10,
                    buffer.data(), 4);
    }

    printf("end of file\n");
    {
        std::int64_t moved = 0;
        window.Seek(static_cast<std::int64_t>(size) - 10, SEEK_SET, &moved);
        const std::int64_t got = ReadAt(window, fd, buffer.data(), 4096, 0);
        Check(got == 10, "a read past the end is short", got, 10);
        const std::int64_t again = ReadAt(window, fd, buffer.data(), 4096, 0);
        Check(again == 0, "a read at the end is empty", again, 0);
    }

    printf("a write anywhere drops the window\n");
    {
        std::int64_t moved = 0;
        window.Seek(0, SEEK_SET, &moved);
        ReadAt(window, fd, buffer.data(), 4096, 0);  // fills at generation 0
        Check(window.Holds(0, 4096), "the window holds the next read", 1, 1);
        window.Seek(armbat::ReadWindow::kBytes, SEEK_SET, &moved);
        Check(!window.Holds(0, 4096), "the window does not hold a read past it", 0, 0);
        window.Seek(0, SEEK_SET, &moved);
        Check(!window.Holds(1, 4096), "a write drops it", 0, 0);
        const std::int64_t got = ReadAt(window, fd, buffer.data(), 4096, 1);
        Check(got == 4096, "the refill after a write returns the request", got, 4096);
        ExpectBytes("the refill after a write returns the file's bytes", content, 0, buffer.data(),
                    4096);
    }

    printf("a request larger than the window\n");
    {
        std::int64_t moved = 0;
        window.Seek(0, SEEK_SET, &moved);
        const std::int64_t got = ReadAt(window, fd, buffer.data(), 200 * 1024, 1);
        Check(got == 200 * 1024, "a large read returns the request", got, 200 * 1024);
        ExpectBytes("a large read returns the file's bytes", content, 0, buffer.data(),
                    200 * 1024);
        Check(static_cast<long long>(window.position()) == 200 * 1024, "position after a large read",
              static_cast<long long>(window.position()), 200 * 1024);
    }

    ::close(fd);
    ::unlink(path);
    printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
