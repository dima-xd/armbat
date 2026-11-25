/*
 * The pthread key API a game library imports.
 *
 * The guest's own bionic keeps a "key in use" bit inside the value it returns
 * from pthread_key_create(), while the images this container runs were built
 * against bionic versions whose keys are plain slot numbers -- and the container
 * implements the key table on the host side (see hle/guest_pthread.cpp). A game
 * that creates one format of key and reads back through the other implementation
 * indexes a table with a value it was never meant to hold, and dies on the first
 * thread-specific read.
 *
 * This library is mapped into the global symbol group ahead of the application,
 * so its pthread_key_create() and friends are bound at relocation time -- before
 * any constructor the image carries has had a chance to create a key. The four
 * functions carry the question out through one private syscall: the container's
 * own implementation answers it, so every image inside the process shares a
 * single key space and a single answer to "what does this key mean".
 *
 * The sub-operation numbers match kKeyTrapNames in guest_container.cpp.
 */

#define ARMBAT_SVC_KEYS (0x0f0000u + 0x103u)

#if defined(__arm__)

static long armbat_keys(unsigned sub, long a0, long a1) {
    register long r0 __asm__("r0") = (long)sub;
    register long r1 __asm__("r1") = a0;
    register long r2 __asm__("r2") = a1;
    register long r7 __asm__("r7") = (long)ARMBAT_SVC_KEYS;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

int pthread_key_create(int* key, void (*destructor)(void*)) {
    *key = 0;
    return (int)armbat_keys(0, (long)key, (long)destructor);
}

int pthread_key_delete(int key) {
    return (int)armbat_keys(1, (long)key, 0);
}

void pthread_setspecific(int key, const void* value) {
    (void)armbat_keys(2, (long)key, (long)value);
}

void* pthread_getspecific(int key) {
    return (void*)armbat_keys(3, (long)key, 0);
}

#else

int pthread_key_create(int* key, void (*destructor)(void*)) {
    (void)destructor;
    *key = -1;
    return -1;
}

int pthread_key_delete(int key) {
    (void)key;
    return -1;
}

void pthread_setspecific(int key, const void* value) {
    (void)key;
    (void)value;
}

void* pthread_getspecific(int key) {
    (void)key;
    return 0;
}

#endif
