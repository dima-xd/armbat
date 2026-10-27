// A real armeabi-v7a shared object: no libc, no imports, only relocations
// against itself. Loaded by the ELF32 loader and called through the JIT.

typedef unsigned int u32;

static int g_counter = 5;
static int g_table[4] = {1, 2, 3, 4};
static const char* const g_names[] = {"zero", "one", "two"};

int lib_add(int a, int b);
int lib_mul(int a, int b);

// Function pointers in .data.rel.ro: exercises R_ARM_RELATIVE.
static int (*const g_ops[])(int, int) = {lib_add, lib_mul};

int lib_add(int a, int b) {
    return a + b;
}

int lib_mul(int a, int b) {
    return a * b;
}

int lib_fib(int n) {
    int a = 0;
    int b = 1;
    while (n-- > 0) {
        const int next = a + b;
        a = b;
        b = next;
    }
    return a;
}

int lib_data_sum(int count) {
    int sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += g_table[i];
    }
    return sum;
}

// Indirect call through the relocated pointer table.
int lib_call_table(int a, int b, int index) {
    if (index < 0 || index > 1) {
        return -1;
    }
    return g_ops[index](a, b);
}

int lib_name_len(int index) {
    if (index < 0 || index > 2) {
        return -1;
    }
    const char* text = g_names[index];
    int length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

int lib_ctor_value(void) {
    return g_counter;
}

__attribute__((constructor)) static void lib_init(void) {
    g_counter = 42;
}
