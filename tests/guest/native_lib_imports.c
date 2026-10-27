// A shared object that uses the C library through imports. Every call goes
// through the PLT and the GOT, so loading it exercises R_ARM_JUMP_SLOT; the
// imports themselves are resolved to HLE stubs by the host.

typedef unsigned int u32;

extern unsigned int strlen(const char* text);
extern void* memcpy(void* destination, const void* source, unsigned int length);
extern void* memset(void* destination, int value, unsigned int length);
extern int strcmp(const char* left, const char* right);
extern int memcmp(const void* left, const void* right, unsigned int length);
extern int strncmp(const char* left, const char* right, unsigned int limit);
extern void* malloc(unsigned int size);
extern void* calloc(unsigned int count, unsigned int size);
extern void* realloc(void* pointer, unsigned int size);
extern void free(void* pointer);
extern void* dlopen(const char* path, int flags);
extern void* dlsym(void* handle, const char* name);

static char g_buffer[64];

int lib_strlen(const char* text) {
    return (int)strlen(text);
}

int lib_checksum(const char* text) {
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

// Fills the static buffer, so the caller can read it back through the arena.
int lib_fill(int value, int count) {
    if (count < 0 || count > (int)sizeof(g_buffer)) {
        return -1;
    }
    memset(g_buffer, value, (u32)count);
    return (int)g_buffer[0] + (int)g_buffer[count - 1];
}

char* lib_buffer(void) {
    return g_buffer;
}

int lib_strcmp(const char* left, const char* right) {
    return strcmp(left, right);
}

int lib_strncmp(const char* left, const char* right, int limit) {
    return strncmp(left, right, (u32)limit);
}

int lib_memcmp(const void* left, const void* right, int length) {
    return memcmp(left, right, (u32)length);
}

// Integer division on ARMv7-A goes through the ARM EABI helpers.
int lib_div(int numerator, int denominator) {
    return numerator / denominator;
}

unsigned int lib_udiv(unsigned int numerator, unsigned int denominator) {
    return numerator / denominator;
}

int lib_mod(int numerator, int denominator) {
    return numerator % denominator;
}

long long lib_ldiv(long long numerator, long long denominator) {
    return numerator / denominator;
}

unsigned long long lib_uldiv(unsigned long long numerator, unsigned long long denominator) {
    return numerator / denominator;
}

// --- Guest heap ------------------------------------------------------------

int lib_alloc_sum(int count) {
    int* values = (int*)malloc((u32)count * sizeof(int));
    if (values == 0) {
        return -1;
    }
    int sum = 0;
    for (int i = 0; i < count; ++i) {
        values[i] = i * 3;
        sum += values[i];
    }
    free(values);
    return sum;
}

int lib_calloc_probe(int count) {
    int* values = (int*)calloc((u32)count, sizeof(int));
    if (values == 0) {
        return -1;
    }
    int dirty = 0;
    for (int i = 0; i < count; ++i) {
        if (values[i] != 0) {
            ++dirty;
        }
    }
    free(values);
    return dirty;
}

// Grows a 4-element block and checks that the old contents survived.
int lib_realloc_sum(int base, int extra) {
    int* values = (int*)malloc(4 * sizeof(int));
    if (values == 0) {
        return -1;
    }
    for (int i = 0; i < 4; ++i) {
        values[i] = i + 1;
    }

    const u32 total = (u32)(4 + extra);
    int* grown = (int*)realloc(values, total * sizeof(int));
    if (grown == 0) {
        free(values);
        return -1;
    }
    for (u32 i = 4; i < total; ++i) {
        grown[i] = base + (int)i;
    }

    int sum = 0;
    for (u32 i = 0; i < total; ++i) {
        sum += grown[i];
    }
    free(grown);
    return sum;
}

// --- dlopen ----------------------------------------------------------------

int lib_dlopen_strlen(const char* library, const char* text) {
    void* handle = dlopen(library, 2);  // RTLD_NOW
    if (handle == 0) {
        return -1;
    }
    int (*function)(const char*) =
        (int (*)(const char*))(unsigned long)dlsym(handle, "lib_needed_strlen");
    if (function == 0) {
        return -2;
    }
    return function(text);
}

int lib_dlsym_missing(const char* library) {
    void* handle = dlopen(library, 2);
    if (handle == 0) {
        return -1;
    }
    return dlsym(handle, "no_such_symbol_here") == 0 ? 1 : 0;
}
