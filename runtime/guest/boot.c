// Guest-side entry point for the bionic runtime.
//
// The bionic linker loads this as the main executable of the guest world, so it
// runs the real C runtime startup: _start, __libc_init, constructors, then main.
// Everything after that is driven by the container, which needs to reach the
// guest's own loader and allocator through the pointers published here.
//
// Nothing in this file is glibc-specific: it is built with the same NDK and
// linked against the same libc.so the container ships.

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>

// ARM Linux private system call numbers live in the 0x0f0000 range. Armbat uses
// the same trap (svc #0, number in r7) for its own handshake.
#define ARMBAT_SVC_READY 0x0f0100u

#define ARMBAT_API_MAGIC 0x41524254u  // "ARBT"
#define ARMBAT_API_VERSION 1u

typedef void* (*dlopen_fn)(const char*, int);
typedef void* (*dlsym_fn)(void*, const char*);
typedef int (*dlclose_fn)(void*);
typedef char* (*dlerror_fn)(void);
typedef void (*exit_fn)(int) __attribute__((noreturn));
typedef void (*abort_fn)(void) __attribute__((noreturn));
typedef int (*atexit_fn)(void (*)(void));
typedef void (*free_fn)(void*);
typedef void* (*malloc_fn)(size_t);

// Everything the container may call from the host side. The guest fills it in
// and passes its address with the ready trap; from then on the addresses are
// stable for the life of the process.
struct armbat_api {
    uint32_t magic;
    uint32_t version;
    uint32_t size;

    dlopen_fn dlopen_;
    dlsym_fn dlsym_;
    dlclose_fn dlclose_;
    dlerror_fn dlerror_;
    exit_fn _exit_;
    abort_fn abort_;
    atexit_fn atexit_;
    malloc_fn malloc_;
    free_fn free_;
};



// The trap instruction is the same one every guest syscall uses. r7 carries the
// number, r0 the argument; the container answers in r0 like the kernel would.
static inline uint32_t armbat_trap(uint32_t number, uint32_t argument) {
    register uint32_t r0 __asm__("r0") = argument;
    register uint32_t r7 __asm__("r7") = number;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory");
    return r0;
}

static void armbat_say(const char* text, unsigned long length, int fd) {
    (void)!write(fd, text, length);
}

static void* armbat_thread_body(void* argument) {
    return argument;
}

// One thread round trip before the container takes over.
//
// clone(), the TLS register a thread carries, the futex pthread_join waits on
// and CLONE_CHILD_CLEARTID are part of the runtime every application depends
// on, and a broken one does not fail loudly: the application simply never
// starts its render thread. A single create-and-join here says whether the
// whole path works, in one line, on every launch.
static unsigned long armbat_decimal(unsigned long value, char* out) {
    char digits[24];
    unsigned long count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (unsigned long index = 0; index < count; ++index) {
        out[index] = digits[count - 1 - index];
    }
    return count;
}

static void armbat_check_threads(void) {
    static const char prefix[] = "boot: thread joined by tid ";
    static const char tail[] = "\n";
    static const char create_failed[] = "boot: pthread_create failed\n";
    static const char join_failed[] = "boot: pthread_join failed\n";
    pthread_t thread;
    if (pthread_create(&thread, 0, armbat_thread_body, (void*)0x1234) != 0) {
        armbat_say(create_failed, sizeof(create_failed) - 1, 2);
        return;
    }
    void* result = 0;
    if (pthread_join(thread, &result) != 0 || result != (void*)0x1234) {
        armbat_say(join_failed, sizeof(join_failed) - 1, 2);
        return;
    }
    char line[64];
    unsigned long length = 0;
    for (unsigned long index = 0; index < sizeof(prefix) - 1; ++index) {
        line[length++] = prefix[index];
    }
    length += armbat_decimal((unsigned long)gettid(), line + length);
    line[length++] = tail[0];
    armbat_say(line, length, 1);
}

// The platform libraries are the container's trampolines, not the device's, and
// every call the application makes into the framework goes through one of them.
// Loading one and calling it is the cheapest proof that the escape hatch is in
// place before the application's own libraries start depending on it.
static void armbat_check_shims(void) {
    static const char ok[] = "boot: shim dispatch ok\n";
    static const char absent[] = "boot: liblog shim missing\n";
    static const char failed[] = "boot: liblog shim has no __android_log_print\n";
    void* library = dlopen("/system/lib/liblog.so", 2 /*RTLD_NOW*/);
    if (library == 0) {
        armbat_say(absent, sizeof(absent) - 1, 2);
        return;
    }
    typedef int (*log_print_fn)(int, const char*, const char*);
    log_print_fn print = (log_print_fn)dlsym(library, "__android_log_print");
    if (print == 0) {
        armbat_say(failed, sizeof(failed) - 1, 2);
        return;
    }
    print(4 /*ANDROID_LOG_INFO*/, "boot", "shim check");
    armbat_say(ok, sizeof(ok) - 1, 1);
}

#if defined(ARMBAT_BOOT_SELFTEST)
// Build with -DARMBAT_BOOT_SELFTEST to get a program that exercises the same
// linker/libc combination on a normal Linux kernel (qemu-arm on a workstation).
// It proves the sysroot before any of the container's boot machinery is
// involved: the real linker has to relocate itself, load libc, run its
// constructors, and resolve dlopen through the libdl stubs it patches.

static void say(const char* text, unsigned long length, int fd) {
    write(fd, text, length);
}

int main(int argc, char** argv) {
    static const char start[] = "boot: libc initialized\n";
    say(start, sizeof(start) - 1, 1);
    armbat_check_threads();

    void* math = dlopen("libm.so", 2 /*RTLD_NOW*/);
    if (math == 0) {
        static const char failure[] = "boot: dlopen(libm.so) failed\n";
        say(failure, sizeof(failure) - 1, 2);
        return 2;
    }
    void* cosine = dlsym(math, "cos");
    if (cosine == 0) {
        static const char failure[] = "boot: dlsym(cos) failed\n";
        say(failure, sizeof(failure) - 1, 2);
        return 3;
    }
    static const char done[] = "boot: ARMBAT_BOOT_OK\n";
    say(done, sizeof(done) - 1, 1);
    _exit(0);
    (void)argc;
    (void)argv;
}
#else
// Kept out of the C runtime's teardown path: the container owns the process and
// an exit() here would destroy the Java side as well. The SVC parks this thread
// inside the guest, and the host resumes driving it.
int main(int argc, char** argv) {
    static struct armbat_api api;
    api.magic = ARMBAT_API_MAGIC;
    api.version = ARMBAT_API_VERSION;
    api.size = (uint32_t)sizeof(api);
    api.dlopen_ = dlopen;
    api.dlsym_ = dlsym;
    api.dlclose_ = dlclose;
    api.dlerror_ = dlerror;
    api._exit_ = _exit;
    api.abort_ = abort;
    api.atexit_ = atexit;
    api.malloc_ = malloc;
    api.free_ = free;

    armbat_check_threads();
    armbat_check_shims();

    armbat_trap(ARMBAT_SVC_READY, (uint32_t)(uintptr_t)&api);

    // The handshake is the last thing main does. Parking here keeps every
    // initializer frame alive, exactly as a real process would after main()
    // entered its event loop.
    for (;;) {
        __asm__ volatile("wfe");
    }
    (void)argc;
    (void)argv;
}
#endif  // ARMBAT_BOOT_SELFTEST
