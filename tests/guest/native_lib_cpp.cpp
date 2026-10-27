// A C++ library with a static constructor, a function-local static and
// operator new/delete. No libc++ headers: the only C++ runtime it needs is the
// low-level __cxa and allocation surface, which is what a game's own C++ code
// uses before it starts pulling in the standard library.
//
// The globals have external linkage on purpose: if they were internal, the
// optimizer would prove the constructor unobservable and drop it together with
// .init_array, and the test would pass without testing anything.

void* operator new(unsigned int size);
void* operator new[](unsigned int size);
void operator delete(void* pointer);
void operator delete[](void* pointer);

int g_constructed = 0;
volatile int g_seed = 1;  // a volatile read keeps the constructor from folding

struct Counter {
    int value;
    Counter();
};

Counter::Counter() : value(7) {
    g_constructed = g_seed;
}

Counter g_counter;  // static constructor lands in .init_array

extern "C" int cpp_ctor_value() {
    return g_counter.value * 10 + g_constructed;
}

// The initializer must run exactly once: if __cxa_guard_* misbehaves, the
// counter goes up on every call.
int g_guard_runs = 0;

extern "C" int cpp_local_static() {
    static int value = (++g_guard_runs, 42);
    return value + g_guard_runs;
}

extern "C" int cpp_new_array(int count) {
    int* values = new int[count];
    if (values == nullptr) {
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        values[i] = i * 2;
    }
    int sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += values[i];
    }
    delete[] values;
    return sum;
}

// Virtual dispatch through a vtable, plus a heap-allocated object.
struct Shape {
    virtual ~Shape() = default;
    virtual int area() const = 0;
};

struct Square : Shape {
    int side;
    explicit Square(int value) : side(value) {}
    int area() const override {
        return side * side;
    }
};

extern "C" int cpp_virtual_area(int side) {
    Shape* shape = new Square(side);
    const int area = shape->area();
    delete shape;
    return area;
}
