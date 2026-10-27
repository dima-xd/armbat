// A normally built NDK library: linked against bionic, not -nostdlib. It is
// what real game code looks like on the import side, so loading it drives the
// HLE surface instead of a hand-picked subset.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int real_strlen(const char* text) {
    return (int)strlen(text);
}

int real_alloc_sum(int count) {
    int* values = (int*)malloc((unsigned)count * sizeof(int));
    if (values == 0) {
        return -1;
    }
    int sum = 0;
    for (int i = 0; i < count; ++i) {
        values[i] = i * 5;
        sum += values[i];
    }
    free(values);
    return sum;
}

// Returns the number of bytes snprintf produced.
int real_format(int value, char* out, int size) {
    return snprintf(out, (unsigned)size, "value=%d", value);
}

// bionic's open() sets errno through its TLS slot, so this exercises both the
// open import and __errno.
int real_errno_probe(const char* path) {
    errno = 0;
    const int fd = open(path, O_RDONLY);
    const int saved = errno;
    if (fd >= 0) {
        close(fd);
    }
    return saved;
}

int real_atoi(const char* text) {
    return atoi(text);
}

// Every conversion here has to survive the guest varargs layout, including the
// 8-byte-aligned long long.
int real_format_many(char* out, int size, int number, const char* text, long long wide,
                     unsigned int hex) {
    return snprintf(out, (unsigned)size, "[%04d|%s|%lld|0x%08x|%c|%%]", number, text, wide, hex,
                    'Z');
}
