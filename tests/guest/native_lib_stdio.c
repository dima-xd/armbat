// C stdio through the HLE layer: buffered files, line access, positioning and
// formatted output. The host never sees a guest FILE* as a pointer, so every
// call here goes through the handle table.

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// wb: fwrite + fprintf + ftell; rb: fgets + fseek + ftell. Returns the final
// file size, or a negative error code.
int lib_stdio_roundtrip(const char* path) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    if (fwrite("line one\n", 1, 9, file) != 9) {
        fclose(file);
        return -2;
    }
    if (fprintf(file, "value=%d\n", 42) != 9) {
        fclose(file);
        return -3;
    }
    const long position = ftell(file);
    fclose(file);
    if (position != 18) {
        return -4;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return -5;
    }
    char line[32] = {0};
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -6;
    }
    if (strcmp(line, "line one\n") != 0) {
        fclose(file);
        return -7;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -8;
    }
    const long size = ftell(file);
    if (feof(file) != 0) {
        fclose(file);
        return -9;
    }
    fclose(file);
    return (int)size;
}

// Reads past the end so fread reports a short count and feof becomes true.
int lib_stdio_eof(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    char buffer[64];
    while (fread(buffer, 1, sizeof(buffer), file) > 0) {
    }
    const int at_end = feof(file);
    fclose(file);
    return at_end;
}

// A missing file must leave ENOENT in the guest errno cell.
int lib_stdio_missing(void) {
    FILE* file = fopen("/nonexistent/armbat-stdio", "rb");
    if (file != NULL) {
        fclose(file);
        return -1;
    }
    return errno;
}

int lib_stdio_printf(const char* label) {
    return printf("guest printf %s %d %08x %.1f\n", label, -7, 0xbeefu, 3.5);
}

// fprintf(stdout, ...) also proves that the imported `stdout` variable holds a
// usable FILE*: the value is loaded from a data cell, not from a function stub.
int lib_stdio_fprintf(const char* label) {
    return fprintf(stdout, "guest fprintf %s\n", label);
}

int lib_stdio_puts(const char* label) {
    if (puts(label) < 0) {
        return -1;
    }
    if (fputs("tail\n", stdout) < 0) {
        return -2;
    }
    if (fputc('!', stdout) < 0) {
        return -3;
    }
    return 0;
}

int lib_stdio_strerror_len(void) {
    return (int)strlen(strerror(ENOENT));
}

// A va_list is a pointer to the AAPCS argument block; the host walks that memory
// instead of the registers the direct-call path uses.
int lib_stdio_vformat(char* buffer, int size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer, (size_t)size, format, args);
    va_end(args);
    return written;
}

int lib_stdio_vfprintf_stdout(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vfprintf(stdout, format, args);
    va_end(args);
    return written;
}

// A 64-bit vararg in the middle of the list: the guest compiler places the double
// at an 8-byte boundary inside the argument block, and the host must skip the
// padding word to find it.
int lib_stdio_vformat_double(char* buffer, int size) {
    return lib_stdio_vformat(buffer, size, "%.2f|%d", 1.25, 9);
}

// The host decodes `&__sF[n]` with a hardcoded stride; this reports the guest's
// own sizeof(FILE) so a bionic change cannot silently break stdout.
int lib_stdio_file_size(void) {
    return (int)sizeof(FILE);
}
