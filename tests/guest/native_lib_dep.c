// A second library from the same APK, linked against the first one. It exists to
// prove the container loads an application's own libraries in dependency order:
// this one cannot be mapped before libguesttest.so, because its import of
// lib_add is resolved during mapping, not lazily.

extern int lib_add(int left, int right);

int lib_dep_double(int value) {
    return lib_add(value, value);
}
