// A stand-in for the platform libc, used only at link time: it makes the test
// library record DT_NEEDED libc.so exactly like a real NDK build does.
//
// These bodies never execute. Armbat resolves the same symbols to HLE stubs, so
// if one of them ever returns 0 the DT_NEEDED test fails loudly.

typedef unsigned int u32;

unsigned int strlen(const char* text) {
    (void)text;
    return 0;
}

void* memcpy(void* destination, const void* source, u32 length) {
    (void)source;
    (void)length;
    return destination;
}

void* memset(void* destination, int value, u32 length) {
    (void)value;
    (void)length;
    return destination;
}

int strcmp(const char* left, const char* right) {
    (void)left;
    (void)right;
    return 0;
}

void* malloc(u32 size) {
    (void)size;
    return 0;
}

void free(void* pointer) {
    (void)pointer;
}
