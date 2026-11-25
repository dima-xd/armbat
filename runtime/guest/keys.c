/*
 * The pthread key API the container owns.
 *
 * The container's implementation of the four key functions (see
 * guest_pthread.cpp) answers with the plain small-integer keys this container's
 * archives were built against, and keeps every value in the creator's own
 * thread storage, which is exactly the shape the bionic these archives linked
 * against expect. The device's bionic instead marks its keys with an "in use"
 * bit and hides its table behind symbols this container does not map, so an
 * archive whose imports bind to it dies the first time it reads a key back.
 *
 * This library is mapped into the global symbol group ahead of the application
 * (see guest_sysroot.cpp), so the application's imports resolve here -- before
 * its constructors run -- and every image inside the process answers through
 * the same implementation. The calls go out through armbat's private syscall:
 * the sub-operation names one of the four functions, the next words are its
 * arguments, and the answer is whatever the container returns.
 */

#include <pthread.h>
#include <stdint.h>

#define ARMBAT_SVC_KEYS (0x0f0000u + 0x103u)

/* The four functions in dispatch order (see guest_container.cpp). */
#define KEY_CREATE 0
#define KEY_DELETE 1
#define KEY_SET 2
#define KEY_GET 3

#if defined(__arm__)
static long armbat_key(unsigned op, long a0, long a1) {
    register long r0 __asm__("r0") = (long)op;
    register long r1 __asm__("r1") = a0;
    register long r2 __asm__("r2") = a1;
    register long r7 __asm__("r7") = (long)ARMBAT_SVC_KEYS;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}
#else
static long armbat_key(unsigned op, long a0, long a1) {
    (void)op;
    (void)a0;
    (void)a1;
    return -1;
}
#endif

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    /* The container writes the key through the pointer itself; the destructor
     * rides along so an implementation that records it gets the chance. */
    long status = armbat_key(KEY_CREATE, (long)key, (long)(uintptr_t)destructor);
    return (int)status;
}

int pthread_key_delete(pthread_key_t key) {
    return (int)armbat_key(KEY_DELETE, (long)key, 0);
}

int pthread_setspecific(pthread_key_t key, const void* value) {
    return (int)armbat_key(KEY_SET, (long)key, (long)value);
}

void* pthread_getspecific(pthread_key_t key) {
    return (void*)armbat_key(KEY_GET, (long)key, 0);
}
