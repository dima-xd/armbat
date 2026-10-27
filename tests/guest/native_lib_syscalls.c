// Guest code that talks to the kernel directly, the way bionic does: `SVC #0`
// with the syscall number in r7 and arguments in r0-r5. Armbat reserves
// SVC #0xFFFFFF for "return to host", so #0 stays free for the Linux ABI.

#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_lseek 19
#define SYS_getpid 20
#define SYS_fstat 108
#define SYS_gettid 224
#define SYS_exit_group 248
#define SYS_openat 322
#define SYS_brk 45
#define SYS_munmap 91
#define SYS_mprotect 125
#define SYS_nanosleep 162
#define SYS_mmap2 192
#define SYS_clock_gettime 263
#define SYS_getrandom 384

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200

static inline int syscall3(int number, int a, int b, int c) {
    register int r0 __asm__("r0") = a;
    register int r1 __asm__("r1") = b;
    register int r2 __asm__("r2") = c;
    register int r7 __asm__("r7") = number;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static inline int syscall2(int number, int a, int b) {
    return syscall3(number, a, b, 0);
}

static inline int syscall1(int number, int a) {
    return syscall3(number, a, 0, 0);
}

// mmap2 needs six arguments; Linux ARM passes the last two in r4 and r5.
static inline int syscall6(int number, int a, int b, int c, int d, int e, int f) {
    register int r0 __asm__("r0") = a;
    register int r1 __asm__("r1") = b;
    register int r2 __asm__("r2") = c;
    register int r3 __asm__("r3") = d;
    register int r4 __asm__("r4") = e;
    register int r5 __asm__("r5") = f;
    register int r7 __asm__("r7") = number;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                     : "memory");
    return r0;
}

static inline int syscall0(int number) {
    return syscall3(number, 0, 0, 0);
}

// Writes `length` bytes, rewinds, reads them back into `buffer`, and returns the
// number of bytes read (or a negative errno).
int lib_file_roundtrip(const char* path, const char* text, int length, char* buffer) {
    const int fd = syscall3(SYS_open, (int)path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fd;
    }

    const int written = syscall3(SYS_write, fd, (int)text, length);
    if (written != length) {
        syscall1(SYS_close, fd);
        return -1000 + written;
    }

    if (syscall3(SYS_lseek, fd, 0, 0) != 0) {
        syscall1(SYS_close, fd);
        return -2000;
    }

    const int read_back = syscall3(SYS_read, fd, (int)buffer, length);
    syscall1(SYS_close, fd);
    return read_back;
}

// Same through openat(AT_FDCWD, ...), which is what modern bionic uses.
int lib_file_roundtrip_at(const char* path, const char* text, int length, char* buffer) {
    const int fd = syscall3(SYS_openat, -100, (int)path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return fd;
    }
    const int written = syscall3(SYS_write, fd, (int)text, length);
    if (written != length) {
        syscall1(SYS_close, fd);
        return -1000 + written;
    }
    if (syscall3(SYS_lseek, fd, 0, 0) != 0) {
        syscall1(SYS_close, fd);
        return -2000;
    }
    const int read_back = syscall3(SYS_read, fd, (int)buffer, length);
    syscall1(SYS_close, fd);
    return read_back;
}

int lib_open_missing(const char* path) {
    return syscall3(SYS_open, (int)path, O_RDONLY, 0);
}

int lib_read_bad_fd(void) {
    char byte = 0;
    return syscall3(SYS_read, -1, (int)&byte, 1);
}

int lib_getpid(void) {
    return syscall0(SYS_getpid);
}

int lib_gettid(void) {
    return syscall0(SYS_gettid);
}

// Reads st_size out of the kernel's 32-bit `struct stat` (offset 24).
int lib_fstat_size(const char* path) {
    const int fd = syscall3(SYS_open, (int)path, O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    unsigned int words[18];
    const int result = syscall3(SYS_fstat, fd, (int)words, 0);
    syscall1(SYS_close, fd);
    if (result != 0) {
        return result;
    }
    return (int)words[6];
}

// exit_group must stop the guest immediately; the statement after it never runs.
int lib_exit_early(int code) {
    syscall1(SYS_exit_group, code);
    return 12345;
}

// --- Guest memory ----------------------------------------------------------

int lib_mmap_anon(void) {
    const int mapping = syscall6(SYS_mmap2, 0, 8192, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping < 0) {
        return mapping;
    }

    int* memory = (int*)mapping;
    for (int i = 0; i < 2048; ++i) {
        memory[i] = i * 7;
    }
    int sum = 0;
    for (int i = 0; i < 2048; ++i) {
        sum += memory[i];
    }
    syscall2(SYS_munmap, mapping, 8192);
    return sum;
}

int lib_mmap_file(const char* path, int length) {
    const int fd = syscall3(SYS_open, (int)path, O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    const int mapping = syscall6(SYS_mmap2, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping < 0) {
        syscall1(SYS_close, fd);
        return mapping;
    }

    const char* text = (const char*)mapping;
    int sum = 0;
    for (int i = 0; i < length; ++i) {
        sum += text[i];
    }
    syscall2(SYS_munmap, mapping, 4096);
    syscall1(SYS_close, fd);
    return sum;
}

int lib_brk_probe(void) {
    const int current = syscall1(SYS_brk, 0);
    if (current <= 0) {
        return -1;
    }
    const int grown = syscall1(SYS_brk, current + 4096);
    if (grown != current + 4096) {
        return -2;
    }
    char* last = (char*)grown - 1;
    *last = 42;
    const int value = *last;
    syscall1(SYS_brk, current);
    return value;
}

// Returns the milliseconds CLOCK_MONOTONIC advanced across a 10 ms nanosleep.
int lib_sleep_measure(void) {
    struct guest_timespec {
        int seconds;
        int nanoseconds;
    } before, after, request;

    if (syscall2(SYS_clock_gettime, 1, (int)&before) != 0) {
        return -1;
    }
    request.seconds = 0;
    request.nanoseconds = 10 * 1000 * 1000;
    syscall2(SYS_nanosleep, (int)&request, 0);
    if (syscall2(SYS_clock_gettime, 1, (int)&after) != 0) {
        return -2;
    }

    return (after.seconds - before.seconds) * 1000 +
           (after.nanoseconds - before.nanoseconds) / 1000000;
}

int lib_random_fill(char* buffer, int length) {
    const int filled = syscall3(SYS_getrandom, (int)buffer, length, 0);
    if (filled != length) {
        return filled < 0 ? filled : -1;
    }
    int nonzero = 0;
    for (int i = 0; i < length; ++i) {
        if (buffer[i] != 0) {
            ++nonzero;
        }
    }
    return nonzero;
}

// Maps and unmaps the same size repeatedly: exercises the kernel's free list
// and proves addresses are reused instead of leaking the arena.
int lib_mmap_churn(int rounds) {
    int sum = 0;
    for (int i = 0; i < rounds; ++i) {
        const int mapping = syscall6(SYS_mmap2, 0, 4096, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping < 0) {
            return mapping;
        }
        int* word = (int*)mapping;
        *word = i;
        sum += *word;
        if (syscall2(SYS_munmap, mapping, 4096) != 0) {
            return -2;
        }
    }
    return sum;
}

int lib_mprotect_probe(void) {
    const int mapping = syscall6(SYS_mmap2, 0, 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping < 0) {
        return mapping;
    }
    const int result = syscall3(SYS_mprotect, mapping, 4096, PROT_READ);
    syscall2(SYS_munmap, mapping, 4096);
    return result;
}
