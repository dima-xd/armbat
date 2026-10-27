// A library with an unresolved import: the loader must reject it by name.
extern int missing_external(int value);

int lib_calls_missing(int value) {
    return missing_external(value) + 1;
}
