// Linked against the libc stub, so this image carries DT_NEEDED libc.so just
// like a real NDK library. The loader must accept the dependency as
// host-provided and bind the calls to HLE stubs.

typedef unsigned int u32;

extern unsigned int strlen(const char* text);
extern void* memcpy(void* destination, const void* source, unsigned int length);

static char g_buffer[32];

int lib_needed_strlen(const char* text) {
    return (int)strlen(text);
}

int lib_needed_checksum(const char* text) {
    const unsigned int length = strlen(text);
    if (length >= sizeof(g_buffer)) {
        return -1;
    }
    memcpy(g_buffer, text, length);
    g_buffer[length] = '\0';

    int sum = 0;
    for (unsigned int i = 0; i < length; ++i) {
        sum += g_buffer[i];
    }
    return sum;
}
