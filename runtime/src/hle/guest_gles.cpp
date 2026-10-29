#include <android/log.h>

#include "armbat/log.h"
#include <dlfcn.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "armbat/a32_machine.h"
#include "armbat/guest_arena.h"
#include "armbat/hle.h"

// GLES is forwarded to the host driver. The guest address space is the
// arena and the arena is ordinary host memory, so a guest pointer is already a
// pointer the driver can read and write. Everything else has to be marshalled:
//
//   * EGL objects are 64-bit host pointers and live in the shared handle table,
//     so only plain ints cross the boundary here;
//   * pointers the driver hands back (glGetString, glGetVertexAttribPointerv)
//     are host pointers and have to become guest addresses;
//   * the version string carries the driver's ES 3.x surface because loaders
//     such as glad parse it to decide which entry points to fetch; the ES 3.0
//     core calls this container serves (texture storage, framebuffer ops,
//     glReadBuffer) are forwarded to the host driver, and anything an engine
//     resolves but never calls costs nothing;
//   * the extension list is still filtered and GL_MAJOR/MINOR stay at 2.0, so
//     feature detection that goes through those paths keeps an engine on the
//     subset this container can serve.
//
// Every pointer that comes from the guest is range-checked against the arena
// with the size the driver is about to touch, so a bad guest pointer faults in
// the guest instead of taking the host process down.

namespace armbat {
namespace {

constexpr bool kTraceGles = false;

void* GlSymbol(const char* name) {
    static void* library = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    return library != nullptr ? ::dlsym(library, name) : nullptr;
}

template <typename Function>
Function Gl(const char* name) {
    return reinterpret_cast<Function>(GlSymbol(name));
}

// GLES 1.x entry points live in a different host library than the ES2 ones, and
// the fixed-function calls below exist only there. The guest asks EGL for a 1.0
// context, so the driver state those calls touch is the state its draw uses.
void* Gl1Symbol(const char* name) {
    static void* library = dlopen("libGLESv1_CM.so", RTLD_NOW | RTLD_LOCAL);
    return library != nullptr ? ::dlsym(library, name) : nullptr;
}

template <typename Function>
Function Gl1(const char* name) {
    return reinterpret_cast<Function>(Gl1Symbol(name));
}

float FloatArg(HleCall& call, std::size_t index) {
    const std::uint32_t bits = call.Arg(index);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A guest range the driver is about to read or write. `valid` is false when the
// guest passed a non-null address whose range is not mapped: that is a guest
// bug, and calling the driver with it would be a host crash, so the call is
// skipped. A null address is a legitimate GL null argument.
struct Buffer {
    void* bytes = nullptr;
    bool valid = true;
};

Buffer GuestBuffer(HleCall& call, std::uint32_t address, std::size_t length) {
    if (address == 0) {
        return Buffer{nullptr, true};
    }
    std::byte* bytes = call.Mapped(address, length);
    return Buffer{bytes, bytes != nullptr};
}

void RejectBuffer(const char* name, std::uint32_t address, std::size_t length) {
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) < 32) {
        ARMBAT_LOG_WARN("Armbat",
                            "gles: %s got unmapped guest range 0x%08x+%zu", name, address,
                            length);
    }
}

// An engine that reflects a program it could not build passes handle 0 to the
// query calls. The driver answers an invalid handle with an error and leaves the
// out-parameters alone; an engine that then trusts an uninitialized count
// enumerates for millions of iterations, which is indistinguishable from a hang.
// Answering "nothing" keeps the failure cheap, and the call site is worth naming
// once because it means a shader did not come back from the driver.
bool MissingObject(HleCall& call, std::uint32_t handle, const char* function) {
    if (handle != 0) {
        return false;
    }
    static std::mutex seen_mutex;
    static std::set<std::string> seen;
    const std::lock_guard lock(seen_mutex);
    if (seen.insert(function).second) {
        ARMBAT_LOG_WARN("Armbat",
                            "gles: %s called on handle 0 (lr=0x%08x)", function,
                            call.Machine().Reg(14));
    }
    return true;
}

bool CheckBuffer(const char* name, std::uint32_t address, const Buffer& buffer,
                 std::size_t length) {
    if (buffer.valid) {
        return true;
    }
    RejectBuffer(name, address, length);
    return false;
}

// Arrays of names are required when the count is non-zero: the driver writes
// count entries, so a null pointer there is a guest bug rather than GL null.
bool RequireArray(const char* name, std::uint32_t address, const Buffer& buffer,
                  std::size_t length, int count) {
    if (count > 0 && buffer.bytes == nullptr) {
        RejectBuffer(name, address, length);
        return false;
    }
    return CheckBuffer(name, address, buffer, length);
}

std::uint32_t GuestAddress(GuestArena& arena, const void* pointer) {
    const auto base = reinterpret_cast<std::uintptr_t>(arena.Base());
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    if (value < base || value - base >= arena.Size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(value - base);
}

// GLES 2.0 attribute list pointers are byte offsets into the bound buffer when
// one is bound and guest addresses otherwise.
void* OffsetOrPointer(HleCall& call, std::uint32_t address, std::uint32_t buffer,
                      std::size_t length, const char* name, bool* ok) {
    *ok = true;
    if (buffer != 0) {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    }
    const Buffer range = GuestBuffer(call, address, length);
    *ok = CheckBuffer(name, address, range, length);
    return range.bytes;
}

// A fixed-function array argument (a matrix, a fog or material colour). Unlike a
// vertex array this is always guest memory and never a buffer offset, and the
// driver dereferences it immediately, so a null or unmapped range is refused
// instead of handed to the host.
template <typename Element, std::size_t Count>
const Element* ArrayArg(HleCall& call, std::uint32_t address, const char* name) {
    const std::size_t length = Count * sizeof(Element);
    const Buffer range = GuestBuffer(call, address, length);
    if (range.bytes == nullptr) {
        RejectBuffer(name, address, length);
        return nullptr;
    }
    return static_cast<const Element*>(range.bytes);
}

// Size of one pixel of a client texture / readback buffer, or 0 for a format
// the helper does not know (then the range check falls back to one byte).
std::size_t PixelSize(unsigned format, unsigned type) {
    const bool packed = type == 0x8363 ||  // GL_UNSIGNED_SHORT_5_6_5
                        type == 0x8033 ||  // GL_UNSIGNED_SHORT_4_4_4_4
                        type == 0x8034;    // GL_UNSIGNED_SHORT_5_5_5_1
    if (packed) {
        return 2;
    }
    std::size_t components = 0;
    switch (format) {
        case 0x1908:  // GL_RGBA
            components = 4;
            break;
        case 0x1907:  // GL_RGB
            components = 3;
            break;
        case 0x190a:  // GL_LUMINANCE_ALPHA
            components = 2;
            break;
        case 0x1909:  // GL_LUMINANCE
        case 0x1906:  // GL_ALPHA
            components = 1;
            break;
        default:
            return 0;
    }
    std::size_t element = 0;
    switch (type) {
        case 0x1401:  // GL_UNSIGNED_BYTE
            element = 1;
            break;
        case 0x1406:  // GL_FLOAT
            element = 4;
            break;
        case 0x8d61:  // GL_HALF_FLOAT_OES
            element = 2;
            break;
        default:
            return 0;
    }
    return components * element;
}

std::size_t ImageSize(unsigned width, unsigned height, unsigned format, unsigned type,
                      int alignment) {
    const std::size_t pixel = PixelSize(format, type);
    if (pixel == 0 || width == 0 || height == 0) {
        return 0;
    }
    const int align = alignment > 0 ? alignment : 1;
    const std::size_t row = (static_cast<std::size_t>(width) * pixel + align - 1) /
                            static_cast<std::size_t>(align) * static_cast<std::size_t>(align);
    return row * height;
}

// How many ints glGetIntegerv writes for this pname. Multi-value state is the
// exception; most queries write one value.
std::size_t ValueCount(unsigned pname) {
    switch (pname) {
        case 0x0ba2:  // GL_VIEWPORT
        case 0x0c10:  // GL_SCISSOR_BOX
        case 0x0c23:  // GL_COLOR_WRITEMASK
        case 0x8005:  // GL_BLEND_COLOR
            return 4;
        case 0x0b70:  // GL_DEPTH_RANGE
        case 0x0d3a:  // GL_MAX_VIEWPORT_DIMS
        case 0x846d:  // GL_ALIASED_POINT_SIZE_RANGE
        case 0x846e:  // GL_ALIASED_LINE_WIDTH_RANGE
            return 2;
        case 0x86a3:  // GL_COMPRESSED_TEXTURE_FORMATS
            return 16;
        default:
            return 1;
    }
}

std::size_t IndexSize(unsigned type) {
    switch (type) {
        case 0x1401:  // GL_UNSIGNED_BYTE
            return 1;
        case 0x1403:  // GL_UNSIGNED_SHORT
            return 2;
        case 0x1405:  // GL_UNSIGNED_INT
            return 4;
        default:
            return 0;
    }
}

// Per-container GL state. The bindings drive the offset/pointer decision for the
// vertex and index calls, the string cache exists because the driver's strings
// live on the host and the guest needs a stable copy in its own address space.
// Keeping both here means a second container in the same process never sees the
// previous container's cells or bindings.
struct StringCell {
    std::uint32_t address = 0;
    std::string text;
};

struct GlesState {
    std::mutex mutex;
    std::unordered_map<unsigned int, StringCell> strings;
    std::unordered_map<unsigned long long, StringCell> indexed_strings;
    std::uint32_t array_buffer = 0;
    std::uint32_t element_buffer = 0;
    int unpack_alignment = 4;
    int pack_alignment = 4;
};

// Copies `text` into a guest cell and returns its address. The cell is reused as
// long as the text does not change: the guest keeps the pointer, so it has to
// stay valid and identical between calls.
std::uint32_t StringCellAddress(HleRegistry& registry, StringCell& cell,
                                const std::string& text) {
    if (cell.address == 0 || cell.text != text) {
        if (cell.address != 0) {
            registry.ReleaseData(cell.address);
        }
        cell.text = text;
        cell.address = registry.AllocateData(static_cast<std::uint32_t>(text.size() + 1));
        if (cell.address == 0) {
            return 0;
        }
        std::memcpy(registry.Arena().Ptr(cell.address), text.c_str(), text.size() + 1);
    }
    return cell.address;
}

// Extensions are filtered down to the ones that add enums only, never entry
// points: the container serves exactly the GLES 2.0 functions, and an extension
// with its own functions would invite the guest to call a stub that is not
// there. The list must not be empty, though -- engines probe it to pick
// internal formats for their off-screen targets. Without packed_depth_stencil
// Unity builds a depth-only renderbuffer, attaches it at the stencil point and
// keeps drawing into an incomplete framebuffer: every draw then fails with
// GL_INVALID_FRAMEBUFFER_OPERATION and the window presents pure black while all
// the call counters look healthy.
std::string FilterExtensions(const std::string& driver) {
    static const char* const kSafe[] = {
        "GL_OES_packed_depth_stencil",
        "GL_OES_depth24",
        "GL_OES_stencil8",
        "GL_OES_stencil4",
        "GL_OES_rgb8_rgba8",
        "GL_OES_fbo_render_mipmap",
        "GL_OES_standard_derivatives",
        "GL_EXT_texture_format_BGRA8888",
    };
    std::string out;
    for (const char* name : kSafe) {
        const std::size_t at = driver.find(name);
        if (at == std::string::npos) {
            continue;
        }
        const std::size_t end = at + std::strlen(name);
        if (end < driver.size() && driver[end] != ' ') {
            continue;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += name;
    }
    return out;
}

}  // namespace

void RegisterGlesFunctions(HleRegistry& registry) {
    auto state = std::make_shared<GlesState>();

    // ---- strings and errors ------------------------------------------------

    registry.Add("glGetString", [&registry, state](HleCall& call) {
        static const auto function = Gl<const unsigned char* (*)(unsigned int)>("glGetString");
        if (function == nullptr) {
            call.Return(0);
            return;
        }
        const unsigned int name = call.Arg(0);
        const unsigned char* text = function(name);
        if (text == nullptr) {
            call.Return(0);
            return;
        }
        std::string value(reinterpret_cast<const char*>(text));
        // The version string tells loaders what to resolve; the extension list
        // stays filtered because the extensions it would expose add entry
        // points this container does not serve.
        if (name == 0x1f02) {  // GL_VERSION
            // Loaders such as glad parse this string to decide which entry
            // points to fetch. The host driver is ES 3.x even for the ES 2.0
            // context the guest asks for, and engines built against that
            // driver call the ES 3.0 surface unconditionally (Minecraft's
            // screenshot path does exactly that with glReadBuffer). Reporting
            // 2.0 here left those pointers null and the guest jumped to
            // address zero. 3.0 rather than the driver's 3.2 keeps the loader
            // to the core ES 3.0 list this container serves.
            value = "OpenGL ES 3.0 (Armbat)";
        } else if (name == 0x8b8c) {  // GL_SHADING_LANGUAGE_VERSION
            value = "OpenGL ES GLSL ES 1.00";
        } else if (name == 0x1f03) {  // GL_EXTENSIONS
            value = FilterExtensions(value);
        }
        const std::lock_guard lock(state->mutex);
        const std::uint32_t cell = StringCellAddress(registry, state->strings[name], value);
        if (kTraceGles) {
            ARMBAT_LOG_INFO("Armbat", "glGetString(0x%x): %s", name,
                                value.substr(0, 120).c_str());
        }
        call.Return(cell);
    });
    registry.Add("glGetStringi", [&registry, state](HleCall& call) {
        static const auto function =
            Gl<const unsigned char* (*)(unsigned int, unsigned int)>("glGetStringi");
        if (function == nullptr) {
            call.Return(0);
            return;
        }
        const unsigned int name = call.Arg(0);
        const unsigned int index = call.Arg(1);
        const unsigned char* text = function(name, index);
        if (text == nullptr) {
            call.Return(0);
            return;
        }
        const std::string value(reinterpret_cast<const char*>(text));
        const unsigned long long key = (static_cast<unsigned long long>(name) << 32) | index;
        const std::lock_guard lock(state->mutex);
        const std::uint32_t cell =
            StringCellAddress(registry, state->indexed_strings[key], value);
        call.Return(cell);
    });
    registry.Add("glGetError", [](HleCall& call) {
        static const auto function = Gl<unsigned int (*)(void)>("glGetError");
        call.Return(function != nullptr ? function() : 0);
    });
    registry.Add("glGetIntegerv", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int, int*)>("glGetIntegerv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(1), ValueCount(call.Arg(0)) *
                                                                sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetIntegerv", call.Arg(1), ValueCount(call.Arg(0)) * sizeof(int));
            return;
        }
        switch (call.Arg(0)) {
            case 0x821b:  // GL_MAJOR_VERSION
                *out = 2;
                return;
            case 0x821c:  // GL_MINOR_VERSION
                *out = 0;
                return;
            case 0x821d:  // GL_NUM_EXTENSIONS
                *out = 0;
                return;
            default:
                function(call.Arg(0), out);
                return;
        }
    });
    registry.Add("glGetFloatv", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int, float*)>("glGetFloatv");
        if (function == nullptr) {
            return;
        }
        const std::size_t length = ValueCount(call.Arg(0)) * sizeof(float);
        float* out = reinterpret_cast<float*>(call.Mapped(call.Arg(1), length));
        if (out == nullptr) {
            RejectBuffer("glGetFloatv", call.Arg(1), length);
            return;
        }
        function(call.Arg(0), out);
    });
    registry.Add("glGetBooleanv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned char*)>("glGetBooleanv");
        if (function == nullptr) {
            return;
        }
        const std::size_t length = ValueCount(call.Arg(0));
        unsigned char* out = reinterpret_cast<unsigned char*>(call.Mapped(call.Arg(1), length));
        if (out == nullptr) {
            RejectBuffer("glGetBooleanv", call.Arg(1), length);
            return;
        }
        function(call.Arg(0), out);
    });
    registry.Add("glGetShaderPrecisionFormat", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int*, int*)>("glGetShaderPrecisionFormat");
        if (function == nullptr) {
            return;
        }
        int* range = call.Arg(2) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(2), 8)) : nullptr;
        int* precision =
            call.Arg(3) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(3), 4)) : nullptr;
        if ((call.Arg(2) != 0 && range == nullptr) ||
            (call.Arg(3) != 0 && precision == nullptr)) {
            RejectBuffer("glGetShaderPrecisionFormat", call.Arg(2), 8);
            return;
        }
        function(call.Arg(0), call.Arg(1), range, precision);
    });

    // ---- state -------------------------------------------------------------

    registry.Add("glEnable", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glEnable");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glDisable", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glDisable");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glIsEnabled", [](HleCall& call) {
        static const auto function = Gl<unsigned char (*)(unsigned int)>("glIsEnabled");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glClear", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glClear");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glClearColor", [](HleCall& call) {
        static const auto function = Gl<void (*)(float, float, float, float)>("glClearColor");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2), FloatArg(call, 3));
        }
    });
    registry.Add("glClearDepthf", [](HleCall& call) {
        static const auto function = Gl<void (*)(float)>("glClearDepthf");
        if (function != nullptr) function(FloatArg(call, 0));
    });
    registry.Add("glClearStencil", [](HleCall& call) {
        static const auto function = Gl<void (*)(int)>("glClearStencil");
        if (function != nullptr) function(static_cast<int>(call.Arg(0)));
    });
    registry.Add("glDepthFunc", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glDepthFunc");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glDepthMask", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned char)>("glDepthMask");
        if (function != nullptr) function(static_cast<unsigned char>(call.Arg(0)));
    });
    registry.Add("glDepthRangef", [](HleCall& call) {
        static const auto function = Gl<void (*)(float, float)>("glDepthRangef");
        if (function != nullptr) function(FloatArg(call, 0), FloatArg(call, 1));
    });
    registry.Add("glColorMask", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned char, unsigned char, unsigned char, unsigned char)>("glColorMask");
        if (function != nullptr) {
            function(static_cast<unsigned char>(call.Arg(0)),
                     static_cast<unsigned char>(call.Arg(1)),
                     static_cast<unsigned char>(call.Arg(2)),
                     static_cast<unsigned char>(call.Arg(3)));
        }
    });
    registry.Add("glViewport", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, int, int, int)>("glViewport");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)));
        }
    });
    registry.Add("glScissor", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, int, int, int)>("glScissor");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)));
        }
    });
    registry.Add("glBlendFunc", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int, unsigned int)>("glBlendFunc");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glBlendFuncSeparate", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, unsigned int, unsigned int)>(
                "glBlendFuncSeparate");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), call.Arg(2), call.Arg(3));
        }
    });
    registry.Add("glBlendEquation", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glBlendEquation");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glBlendEquationSeparate", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int)>("glBlendEquationSeparate");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glBlendColor", [](HleCall& call) {
        static const auto function = Gl<void (*)(float, float, float, float)>("glBlendColor");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2), FloatArg(call, 3));
        }
    });
    registry.Add("glCullFace", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glCullFace");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glFrontFace", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glFrontFace");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glLineWidth", [](HleCall& call) {
        static const auto function = Gl<void (*)(float)>("glLineWidth");
        if (function != nullptr) function(FloatArg(call, 0));
    });
    registry.Add("glPolygonOffset", [](HleCall& call) {
        static const auto function = Gl<void (*)(float, float)>("glPolygonOffset");
        if (function != nullptr) function(FloatArg(call, 0), FloatArg(call, 1));
    });
    registry.Add("glHint", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int, unsigned int)>("glHint");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glPixelStorei", [state](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int, int)>("glPixelStorei");
        if (function != nullptr) {
            function(call.Arg(0), static_cast<int>(call.Arg(1)));
        }
        const std::lock_guard lock(state->mutex);
        if (call.Arg(0) == 0x0cf5) {  // GL_UNPACK_ALIGNMENT
            state->unpack_alignment = static_cast<int>(call.Arg(1));
        } else if (call.Arg(0) == 0x0d05) {  // GL_PACK_ALIGNMENT
            state->pack_alignment = static_cast<int>(call.Arg(1));
        }
    });
    // ES 2.0 has a single read buffer, so the desktop call that selects it has
    // nothing to select. Engines still call it before glReadPixels; on a device
    // the loader exports it from libGLESv2, and here the name has to exist or a
    // glad-style loader leaves a null entry point that is then called.
    registry.Add("glReadBuffer", [](HleCall&) {});

    registry.Add("glSampleCoverage", [](HleCall& call) {
        static const auto function = Gl<void (*)(float, unsigned char)>("glSampleCoverage");
        if (function != nullptr) {
            function(FloatArg(call, 0), static_cast<unsigned char>(call.Arg(1)));
        }
    });
    registry.Add("glStencilFunc", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int)>("glStencilFunc");
        if (function != nullptr) {
            function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2));
        }
    });
    registry.Add("glStencilFuncSeparate", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int, unsigned int)>("glStencilFuncSeparate");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)), call.Arg(3));
        }
    });
    registry.Add("glStencilMask", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glStencilMask");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glStencilMaskSeparate", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int)>("glStencilMaskSeparate");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glStencilOp", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, unsigned int)>("glStencilOp");
        if (function != nullptr) function(call.Arg(0), call.Arg(1), call.Arg(2));
    });
    registry.Add("glStencilOpSeparate", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, unsigned int, unsigned int)>(
                "glStencilOpSeparate");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), call.Arg(2), call.Arg(3));
        }
    });

    registry.Add("glGenBuffers", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, unsigned int*)>("glGenBuffers");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer out = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glGenBuffers", call.Arg(1), out, length, count)) {
            return;
        }
        function(count, static_cast<unsigned int*>(out.bytes));
    });
    registry.Add("glDeleteBuffers", [state](HleCall& call) {
        static const auto function = Gl<void (*)(int, const unsigned int*)>("glDeleteBuffers");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer names = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glDeleteBuffers", call.Arg(1), names, length, count)) {
            return;
        }
        if (names.bytes != nullptr) {
            // Deleting a bound buffer unbinds it; the offset/pointer decision for
            // later vertex calls depends on knowing that.
            const auto* values = reinterpret_cast<const unsigned int*>(names.bytes);
            const std::lock_guard lock(state->mutex);
            for (int i = 0; i < count; ++i) {
                if (values[i] == state->array_buffer) state->array_buffer = 0;
                if (values[i] == state->element_buffer) state->element_buffer = 0;
            }
        }
        function(count, static_cast<const unsigned int*>(names.bytes));
    });
    registry.Add("glBindBuffer", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int)>("glBindBuffer");
        const unsigned int target = call.Arg(0);
        const unsigned int buffer = call.Arg(1);
        {
            const std::lock_guard lock(state->mutex);
            if (target == 0x8892) {  // GL_ARRAY_BUFFER
                state->array_buffer = buffer;
            } else if (target == 0x8893) {  // GL_ELEMENT_ARRAY_BUFFER
                state->element_buffer = buffer;
            }
        }
        if (function != nullptr) function(target, buffer);
    });
    registry.Add("glBufferData", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, std::ptrdiff_t, const void*, unsigned int)>("glBufferData");
        if (function == nullptr) {
            return;
        }
        const std::ptrdiff_t size = static_cast<std::int32_t>(call.Arg(1));
        if (size < 0) {
            return;
        }
        // `data` is a pointer to the new contents (or null), never an offset.
        const Buffer data = GuestBuffer(call, call.Arg(2), static_cast<std::size_t>(size));
        if (!CheckBuffer("glBufferData", call.Arg(2), data, static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), size, data.bytes, call.Arg(3));
    });
    registry.Add("glBufferSubData", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, std::ptrdiff_t, std::ptrdiff_t, const void*)>(
                "glBufferSubData");
        if (function == nullptr) {
            return;
        }
        const std::ptrdiff_t offset = static_cast<std::int32_t>(call.Arg(1));
        const std::ptrdiff_t size = static_cast<std::int32_t>(call.Arg(2));
        if (offset < 0 || size < 0) {
            return;
        }
        const Buffer data = GuestBuffer(call, call.Arg(3), static_cast<std::size_t>(size));
        if (!CheckBuffer("glBufferSubData", call.Arg(3), data, static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), offset, size, data.bytes);
    });
    registry.Add("glGetBufferParameteriv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int*)>("glGetBufferParameteriv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetBufferParameteriv", call.Arg(2), sizeof(int));
            return;
        }
        function(call.Arg(0), call.Arg(1), out);
    });
    registry.Add("glIsBuffer", [](HleCall& call) {
        static const auto function = Gl<unsigned char (*)(unsigned int)>("glIsBuffer");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });

    // ---- shaders and programs ---------------------------------------------

    registry.Add("glCreateShader", [](HleCall& call) {
        static const auto function = Gl<unsigned int (*)(unsigned int)>("glCreateShader");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glCreateProgram", [](HleCall& call) {
        static const auto function = Gl<unsigned int (*)(void)>("glCreateProgram");
        const unsigned int program = function != nullptr ? function() : 0;
        static std::atomic<unsigned> seen{0};
        if (seen.fetch_add(1, std::memory_order_relaxed) < 8) {
            ARMBAT_LOG_DEBUG("Armbat", "gles: createProgram -> %u", program);
        }
        if (program == 0) {
            static const auto get_error = Gl<unsigned int (*)(void)>("glGetError");
            ARMBAT_LOG_WARN("Armbat", "gles: createProgram failed, error=0x%x",
                                get_error != nullptr ? get_error() : 0);
        }
        call.Return(program);
    });
    registry.Add("glShaderSource", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, const char* const*, const int*)>("glShaderSource");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(1));
        if (count <= 0 || count > 64) {
            return;
        }
        std::vector<std::uint32_t> pointers(static_cast<std::size_t>(count), 0);
        const std::size_t pointer_bytes = pointers.size() * sizeof(std::uint32_t);
        if (!call.CopyIn(call.Arg(2), pointers.data(), pointer_bytes)) {
            RejectBuffer("glShaderSource", call.Arg(2), pointer_bytes);
            return;
        }
        std::vector<int> lengths(static_cast<std::size_t>(count), -1);
        if (call.Arg(3) != 0 &&
            !call.CopyIn(call.Arg(3), lengths.data(), lengths.size() * sizeof(int))) {
            RejectBuffer("glShaderSource", call.Arg(3), lengths.size() * sizeof(int));
            return;
        }
        // The sources are copied so the driver never scans guest memory looking
        // for a terminator it was not promised.
        std::vector<std::string> copies(static_cast<std::size_t>(count));
        std::vector<const char*> strings(static_cast<std::size_t>(count), nullptr);
        std::vector<int> sizes(static_cast<std::size_t>(count), 0);
        for (int i = 0; i < count; ++i) {
            if (pointers[static_cast<std::size_t>(i)] == 0) {
                continue;
            }
            if (lengths[static_cast<std::size_t>(i)] < 0) {
                std::string text;
                if (!call.ReadCString(pointers[static_cast<std::size_t>(i)], text, 1u << 20)) {
                    RejectBuffer("glShaderSource", pointers[static_cast<std::size_t>(i)], 1);
                    return;
                }
                copies[static_cast<std::size_t>(i)] = std::move(text);
            } else {
                const std::size_t length =
                    static_cast<std::size_t>(lengths[static_cast<std::size_t>(i)]);
                const std::byte* source =
                    call.Mapped(pointers[static_cast<std::size_t>(i)], length);
                if (source == nullptr) {
                    RejectBuffer("glShaderSource", pointers[static_cast<std::size_t>(i)], length);
                    return;
                }
                copies[static_cast<std::size_t>(i)].assign(
                    reinterpret_cast<const char*>(source), length);
            }
            strings[static_cast<std::size_t>(i)] = copies[static_cast<std::size_t>(i)].data();
            sizes[static_cast<std::size_t>(i)] =
                static_cast<int>(copies[static_cast<std::size_t>(i)].size());
        }
        function(call.Arg(0), count, strings.data(), sizes.data());
    });
    registry.Add("glCompileShader", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glCompileShader");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glReleaseShaderCompiler", [](HleCall&) {
        static const auto function = Gl<void (*)(void)>("glReleaseShaderCompiler");
        if (function != nullptr) function();
    });
    registry.Add("glShaderBinary", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, const unsigned int*, unsigned int, const void*, int)>("glShaderBinary");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        const int binary_length = static_cast<int>(call.Arg(4));
        if (count < 0 || count > 4096 || binary_length < 0) {
            return;
        }
        const std::size_t names_length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer names = GuestBuffer(call, call.Arg(1), names_length);
        const Buffer binary =
            GuestBuffer(call, call.Arg(3), static_cast<std::size_t>(binary_length));
        if (!RequireArray("glShaderBinary", call.Arg(1), names, names_length, count) ||
            !CheckBuffer("glShaderBinary", call.Arg(3), binary,
                         static_cast<std::size_t>(binary_length))) {
            return;
        }
        function(count, static_cast<const unsigned int*>(names.bytes), call.Arg(2), binary.bytes,
                 binary_length);
    });
    registry.Add("glAttachShader", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int)>("glAttachShader");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glDetachShader", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int)>("glDetachShader");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glLinkProgram", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glLinkProgram");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glValidateProgram", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glValidateProgram");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glUseProgram", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glUseProgram");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glDeleteShader", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glDeleteShader");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glDeleteProgram", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glDeleteProgram");
        if (function != nullptr) function(call.Arg(0));
    });
    // A shader that fails to compile or link is deleted and the engine silently
    // drops the feature, so the reason has to be read out of the driver while
    // the object still exists.
    registry.Add("glGetShaderiv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int*)>("glGetShaderiv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetShaderiv", call.Arg(2), sizeof(int));
            return;
        }
        const unsigned int shader = call.Arg(0);
        const unsigned int name = call.Arg(1);
        if (MissingObject(call, shader, "glGetShaderiv")) {
            *out = 0;
            return;
        }
        function(shader, name, out);
        if (name != 0x8b81 /* GL_COMPILE_STATUS */ || *out != 0) {
            return;
        }
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) >= 8) {
            return;
        }
        char log[1024] = {};
        int written = 0;
        static const auto info_log =
            Gl<void (*)(unsigned int, int, int*, char*)>("glGetShaderInfoLog");
        if (info_log != nullptr) {
            info_log(shader, static_cast<int>(sizeof(log)), &written, log);
        }
        ARMBAT_LOG_WARN("Armbat", "gles: shader %u failed: %s", shader, log);
        int length = 0;
        function(shader, 0x8b84 /* GL_INFO_LOG_LENGTH */, &length);
        int source_length = 0;
        function(shader, 0x8b88 /* GL_SHADER_SOURCE_LENGTH */, &source_length);
        if (source_length > 0 && source_length < (1 << 20)) {
            std::string source(static_cast<std::size_t>(source_length), '\0');
            int copied = 0;
            static const auto get_source =
                Gl<void (*)(unsigned int, int, int*, char*)>("glGetShaderSource");
            if (get_source != nullptr) {
                get_source(shader, source_length, &copied, source.data());
            }
            ARMBAT_LOG_WARN("Armbat",
                                "gles: shader %u source is %d chars, %s", shader, copied,
                                source.find("#include") == std::string::npos
                                    ? "no include directive reached the driver"
                                    : "include directives were not inlined");
            std::size_t start = 0;
            unsigned line_number = 1;
            while (start < source.size() && line_number <= 400) {
                std::size_t end = source.find('\n', start);
                if (end == std::string::npos) {
                    end = source.size();
                }
                const std::string line = source.substr(start, end - start);
                if (!line.empty()) {
                    ARMBAT_LOG_WARN("Armbat", "gles: %3u | %s",
                                        line_number, line.c_str());
                }
                start = end + 1;
                ++line_number;
            }
        }
    });
    registry.Add("glGetProgramiv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int*)>("glGetProgramiv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetProgramiv", call.Arg(2), sizeof(int));
            return;
        }
        const unsigned int program = call.Arg(0);
        const unsigned int name = call.Arg(1);
        if (MissingObject(call, program, "glGetProgramiv")) {
            *out = 0;
            return;
        }
        function(program, name, out);
        if (name == 0x8b86 /* GL_ACTIVE_UNIFORMS */ || name == 0x8b89 /* GL_ACTIVE_ATTRIBUTES */
            || program == 0) {
            static std::atomic<unsigned> counted{0};
            if (counted.fetch_add(1, std::memory_order_relaxed) < 24) {
                ARMBAT_LOG_DEBUG("Armbat",
                                    "gles: programiv program=%u name=0x%x -> %d", program, name, *out);
            }
        }
        if (name != 0x8b82 /* GL_LINK_STATUS */ || *out != 0) {
            return;
        }
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) >= 8) {
            return;
        }
        char log[1024] = {};
        int written = 0;
        static const auto info_log =
            Gl<void (*)(unsigned int, int, int*, char*)>("glGetProgramInfoLog");
        if (info_log != nullptr) {
            info_log(program, static_cast<int>(sizeof(log)), &written, log);
        }
        ARMBAT_LOG_WARN("Armbat", "gles: program %u failed: %s", program, log);
        unsigned int shaders[8] = {};
        int attached = 0;
        static const auto get_attached =
            Gl<void (*)(unsigned int, int, int*, unsigned int*)>("glGetAttachedShaders");
        if (get_attached != nullptr) {
            get_attached(program, 8, &attached, shaders);
        }
        static const auto shader_info =
            Gl<void (*)(unsigned int, int, int*, char*)>("glGetShaderInfoLog");
        for (int i = 0; i < attached && i < 8; ++i) {
            char text[1024] = {};
            int size = 0;
            if (shader_info != nullptr) {
                shader_info(shaders[i], static_cast<int>(sizeof(text)), &size, text);
            }
            ARMBAT_LOG_WARN("Armbat", "gles: program %u shader %u: %s",
                                program, shaders[i], text);
        }
    });
    registry.Add("glGetShaderInfoLog", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int*, char*)>("glGetShaderInfoLog");
        if (function == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(1));
        if (size < 0) {
            return;
        }
        int* length = call.Arg(2) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)))
                                       : nullptr;
        const Buffer log = GuestBuffer(call, call.Arg(3), static_cast<std::size_t>(size));
        if ((call.Arg(2) != 0 && length == nullptr) ||
            !CheckBuffer("glGetShaderInfoLog", call.Arg(3), log, static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), size, length, static_cast<char*>(log.bytes));
    });
    registry.Add("glGetProgramInfoLog", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int*, char*)>("glGetProgramInfoLog");
        if (function == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(1));
        if (size < 0) {
            return;
        }
        int* length = call.Arg(2) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)))
                                       : nullptr;
        const Buffer log = GuestBuffer(call, call.Arg(3), static_cast<std::size_t>(size));
        if ((call.Arg(2) != 0 && length == nullptr) ||
            !CheckBuffer("glGetProgramInfoLog", call.Arg(3), log, static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), size, length, static_cast<char*>(log.bytes));
    });
    registry.Add("glGetShaderSource", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int*, char*)>("glGetShaderSource");
        if (function == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(1));
        if (size < 0) {
            return;
        }
        int* length = call.Arg(2) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)))
                                       : nullptr;
        const Buffer source = GuestBuffer(call, call.Arg(3), static_cast<std::size_t>(size));
        if ((call.Arg(2) != 0 && length == nullptr) ||
            !CheckBuffer("glGetShaderSource", call.Arg(3), source, static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), size, length, static_cast<char*>(source.bytes));
    });
    registry.Add("glGetAttachedShaders", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int*, unsigned int*)>("glGetAttachedShaders");
        if (function == nullptr) {
            return;
        }
        const int max_count = static_cast<int>(call.Arg(1));
        if (max_count < 0 || max_count > 65536) {
            return;
        }
        int* count = call.Arg(2) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)))
                                      : nullptr;
        const std::size_t length =
            static_cast<std::size_t>(max_count) * sizeof(unsigned int);
        const Buffer shaders = GuestBuffer(call, call.Arg(3), length);
        if ((call.Arg(2) != 0 && count == nullptr) ||
            !RequireArray("glGetAttachedShaders", call.Arg(3), shaders, length, max_count)) {
            return;
        }
        function(call.Arg(0), max_count, count,
                 static_cast<unsigned int*>(shaders.bytes));
    });
    registry.Add("glGetAttribLocation", [](HleCall& call) {
        static const auto function =
            Gl<int (*)(unsigned int, const char*)>("glGetAttribLocation");
        if (function == nullptr) {
            call.Return(0xffffffffu);
            return;
        }
        std::string name;
        if (!call.ReadCString(call.Arg(1), name, 1024)) {
            call.Return(0xffffffffu);
            return;
        }
        call.Return(static_cast<std::uint32_t>(function(call.Arg(0), name.c_str())));
    });
    registry.Add("glGetUniformLocation", [](HleCall& call) {
        static const auto function =
            Gl<int (*)(unsigned int, const char*)>("glGetUniformLocation");
        if (function == nullptr) {
            call.Return(0xffffffffu);
            return;
        }
        std::string name;
        if (!call.ReadCString(call.Arg(1), name, 1024)) {
            call.Return(0xffffffffu);
            return;
        }
        if (MissingObject(call, call.Arg(0), "glGetUniformLocation")) {
            call.Return(0xffffffffu);
            return;
        }
        call.Return(static_cast<std::uint32_t>(function(call.Arg(0), name.c_str())));
    });
    registry.Add("glBindAttribLocation", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, const char*)>("glBindAttribLocation");
        if (function == nullptr) {
            return;
        }
        std::string name;
        if (!call.ReadCString(call.Arg(2), name, 1024)) {
            return;
        }
        function(call.Arg(0), call.Arg(1), name.c_str());
    });
    registry.Add("glGetActiveAttrib", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int, int*, int*, unsigned int*, char*)>(
                "glGetActiveAttrib");
        if (function == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(2));
        if (size < 0) {
            return;
        }
        int* length = call.Arg(3) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(3), sizeof(int)))
                                       : nullptr;
        int* components =
            call.Arg(4) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(4), sizeof(int))) : nullptr;
        unsigned int* type =
            call.Arg(5) != 0
                ? reinterpret_cast<unsigned int*>(call.Mapped(call.Arg(5), sizeof(unsigned int)))
                : nullptr;
        const Buffer name = GuestBuffer(call, call.Arg(6), static_cast<std::size_t>(size));
        if ((call.Arg(3) != 0 && length == nullptr) ||
            (call.Arg(4) != 0 && components == nullptr) ||
            (call.Arg(5) != 0 && type == nullptr) ||
            !CheckBuffer("glGetActiveAttrib", call.Arg(6), name, static_cast<std::size_t>(size))) {
            return;
        }
        if (MissingObject(call, call.Arg(0), "glGetActiveAttrib")) {
            if (length != nullptr) *length = 0;
            if (components != nullptr) *components = 0;
            if (type != nullptr) *type = 0;
            if (name.bytes != nullptr) static_cast<char*>(name.bytes)[0] = '\0';
            return;
        }
        function(call.Arg(0), call.Arg(1), size, length, components, type,
                 static_cast<char*>(name.bytes));
    });
    registry.Add("glGetActiveUniform", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int, int*, int*, unsigned int*, char*)>(
                "glGetActiveUniform");
        if (function == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(2));
        if (size < 0) {
            return;
        }
        int* length = call.Arg(3) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(3), sizeof(int)))
                                       : nullptr;
        int* components =
            call.Arg(4) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(4), sizeof(int))) : nullptr;
        unsigned int* type =
            call.Arg(5) != 0
                ? reinterpret_cast<unsigned int*>(call.Mapped(call.Arg(5), sizeof(unsigned int)))
                : nullptr;
        const Buffer name = GuestBuffer(call, call.Arg(6), static_cast<std::size_t>(size));
        if ((call.Arg(3) != 0 && length == nullptr) ||
            (call.Arg(4) != 0 && components == nullptr) ||
            (call.Arg(5) != 0 && type == nullptr) ||
            !CheckBuffer("glGetActiveUniform", call.Arg(6), name, static_cast<std::size_t>(size))) {
            return;
        }
        if (MissingObject(call, call.Arg(0), "glGetActiveUniform")) {
            if (length != nullptr) *length = 0;
            if (components != nullptr) *components = 0;
            if (type != nullptr) *type = 0;
            if (name.bytes != nullptr) static_cast<char*>(name.bytes)[0] = '\0';
            return;
        }
        function(call.Arg(0), call.Arg(1), size, length, components, type,
                 static_cast<char*>(name.bytes));
        // A render thread was seen re-entering this call and glGetUniformLocation
        // in a tight loop, which is what an enumeration with a bogus bound looks
        // like. Log the start of every enumeration as well as the first few
        // calls, so the beginning of such a loop is not invisible.
        static std::atomic<unsigned> seen{0};
        const unsigned count = seen.fetch_add(1, std::memory_order_relaxed);
        if (count < 8 || call.Arg(1) == 0 || count % (1u << 20) == 0) {
            ARMBAT_LOG_DEBUG("Armbat",
                                "gles: getActiveUniform #%u program=%u index=%u name='%s' length=%d",
                                count, call.Arg(0), call.Arg(1),
                                static_cast<const char*>(name.bytes),
                                length != nullptr ? *length : -1);
        }
    });
    registry.Add("glIsProgram", [](HleCall& call) {
        static const auto function = Gl<unsigned char (*)(unsigned int)>("glIsProgram");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glIsShader", [](HleCall& call) {
        static const auto function = Gl<unsigned char (*)(unsigned int)>("glIsShader");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });

    // ---- vertex attributes and drawing -------------------------------------

    registry.Add("glVertexAttribPointer", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, unsigned char, int, const void*)>(
                "glVertexAttribPointer");
        if (function == nullptr) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        bool ok = true;
        void* pointer =
            OffsetOrPointer(call, call.Arg(5), buffer, 1, "glVertexAttribPointer", &ok);
        if (!ok) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                 static_cast<unsigned char>(call.Arg(3)), static_cast<int>(call.Arg(4)), pointer);
    });
    registry.Add("glEnableVertexAttribArray", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int)>("glEnableVertexAttribArray");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glDisableVertexAttribArray", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int)>("glDisableVertexAttribArray");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glVertexAttrib1f", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int, float)>("glVertexAttrib1f");
        if (function != nullptr) function(call.Arg(0), FloatArg(call, 1));
    });
    registry.Add("glVertexAttrib2f", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, float, float)>("glVertexAttrib2f");
        if (function != nullptr) function(call.Arg(0), FloatArg(call, 1), FloatArg(call, 2));
    });
    registry.Add("glVertexAttrib3f", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, float, float, float)>("glVertexAttrib3f");
        if (function != nullptr) {
            function(call.Arg(0), FloatArg(call, 1), FloatArg(call, 2), FloatArg(call, 3));
        }
    });
    registry.Add("glVertexAttrib4f", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, float, float, float, float)>("glVertexAttrib4f");
        if (function != nullptr) {
            function(call.Arg(0), FloatArg(call, 1), FloatArg(call, 2), FloatArg(call, 3),
                     FloatArg(call, 4));
        }
    });
    registry.Add("glVertexAttrib1fv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, const float*)>("glVertexAttrib1fv");
        if (function == nullptr) {
            return;
        }
        const Buffer values = GuestBuffer(call, call.Arg(1), 4);
        if (!CheckBuffer("glVertexAttrib1fv", call.Arg(1), values, 4)) {
            return;
        }
        function(call.Arg(0), static_cast<const float*>(values.bytes));
    });
    registry.Add("glVertexAttrib2fv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, const float*)>("glVertexAttrib2fv");
        if (function == nullptr) {
            return;
        }
        const Buffer values = GuestBuffer(call, call.Arg(1), 8);
        if (!CheckBuffer("glVertexAttrib2fv", call.Arg(1), values, 8)) {
            return;
        }
        function(call.Arg(0), static_cast<const float*>(values.bytes));
    });
    registry.Add("glVertexAttrib3fv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, const float*)>("glVertexAttrib3fv");
        if (function == nullptr) {
            return;
        }
        const Buffer values = GuestBuffer(call, call.Arg(1), 12);
        if (!CheckBuffer("glVertexAttrib3fv", call.Arg(1), values, 12)) {
            return;
        }
        function(call.Arg(0), static_cast<const float*>(values.bytes));
    });
    registry.Add("glVertexAttrib4fv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, const float*)>("glVertexAttrib4fv");
        if (function == nullptr) {
            return;
        }
        const Buffer values = GuestBuffer(call, call.Arg(1), 16);
        if (!CheckBuffer("glVertexAttrib4fv", call.Arg(1), values, 16)) {
            return;
        }
        function(call.Arg(0), static_cast<const float*>(values.bytes));
    });
    registry.Add("glGetVertexAttribfv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, float*)>("glGetVertexAttribfv");
        if (function == nullptr) {
            return;
        }
        float* out = reinterpret_cast<float*>(call.Mapped(call.Arg(2), 16));
        if (out == nullptr) {
            RejectBuffer("glGetVertexAttribfv", call.Arg(2), 16);
            return;
        }
        function(call.Arg(0), call.Arg(1), out);
    });
    registry.Add("glGetVertexAttribiv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int*)>("glGetVertexAttribiv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(2), 16));
        if (out == nullptr) {
            RejectBuffer("glGetVertexAttribiv", call.Arg(2), 16);
            return;
        }
        function(call.Arg(0), call.Arg(1), out);
    });
    registry.Add("glGetVertexAttribPointerv", [&registry, state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, void**)>("glGetVertexAttribPointerv");
        if (function == nullptr) {
            return;
        }
        std::uint32_t* out = reinterpret_cast<std::uint32_t*>(call.Mapped(call.Arg(2), 4));
        if (out == nullptr) {
            RejectBuffer("glGetVertexAttribPointerv", call.Arg(2), 4);
            return;
        }
        // The host stores a 64-bit pointer here; the guest cell is 32 bits. With
        // a buffer bound the value is an offset and passes through unchanged,
        // otherwise it is an arena address and has to be rebased.
        void* host = nullptr;
        function(call.Arg(0), call.Arg(1), &host);
        std::uint32_t value = 0;
        if (host != nullptr) {
            std::uint32_t buffer = 0;
            {
                const std::lock_guard lock(state->mutex);
                buffer = state->array_buffer;
            }
            if (buffer != 0) {
                value = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(host));
            } else {
                value = GuestAddress(registry.Arena(), host);
            }
        }
        const std::uint32_t cell = call.Arg(2);
        call.CopyOut(cell, &value, sizeof(value));
    });
    registry.Add("glDrawArrays", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int)>("glDrawArrays");
        if (function == nullptr) {
            return;
        }
        const int first = static_cast<int>(call.Arg(1));
        const int count = static_cast<int>(call.Arg(2));
        if (first < 0 || count < 0) {
            return;
        }
        function(call.Arg(0), first, count);
    });
    registry.Add("glDrawElements", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, const void*)>("glDrawElements");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(1));
        if (count < 0) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->element_buffer;
        }
        const std::size_t index_size = IndexSize(call.Arg(2));
        const std::size_t length =
            index_size != 0 ? static_cast<std::size_t>(count) * index_size : 1;
        bool ok = true;
        void* indices = OffsetOrPointer(call, call.Arg(3), buffer, length, "glDrawElements", &ok);
        if (!ok) {
            return;
        }
        function(call.Arg(0), count, call.Arg(2), indices);
    });

    // ---- fixed-function pipeline (GLES 1.x) --------------------------------
    //
    // The guest may ask EGL for a 1.x context; such an engine draws with client
    // arrays and the fixed-function matrix stack instead of shaders. Those
    // entry points exist only in libGLESv1_CM, and their pointer arguments are
    // guest addresses. Leaving them unresolved does not fail loudly: the array
    // stays unset in the driver, and the first glDrawElements reads the vertex
    // array through a null pointer and takes the host process down inside the
    // driver.
    registry.Add("glEnableClientState", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int)>("glEnableClientState");
        if (function != nullptr) {
            function(call.Arg(0));
        }
    });
    registry.Add("glDisableClientState", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int)>("glDisableClientState");
        if (function != nullptr) {
            function(call.Arg(0));
        }
    });
    registry.Add("glVertexPointer", [state](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, unsigned int, int, const void*)>("glVertexPointer");
        if (function == nullptr) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        bool ok = true;
        void* pointer = OffsetOrPointer(call, call.Arg(3), buffer, 1, "glVertexPointer", &ok);
        if (!ok) {
            return;
        }
        function(static_cast<int>(call.Arg(0)), call.Arg(1), static_cast<int>(call.Arg(2)),
                 pointer);
    });
    registry.Add("glColorPointer", [state](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, unsigned int, int, const void*)>("glColorPointer");
        if (function == nullptr) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        bool ok = true;
        void* pointer = OffsetOrPointer(call, call.Arg(3), buffer, 1, "glColorPointer", &ok);
        if (!ok) {
            return;
        }
        function(static_cast<int>(call.Arg(0)), call.Arg(1), static_cast<int>(call.Arg(2)),
                 pointer);
    });
    registry.Add("glTexCoordPointer", [state](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, unsigned int, int, const void*)>("glTexCoordPointer");
        if (function == nullptr) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        bool ok = true;
        void* pointer = OffsetOrPointer(call, call.Arg(3), buffer, 1, "glTexCoordPointer", &ok);
        if (!ok) {
            return;
        }
        function(static_cast<int>(call.Arg(0)), call.Arg(1), static_cast<int>(call.Arg(2)),
                 pointer);
    });
    registry.Add("glNormalPointer", [state](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, int, const void*)>("glNormalPointer");
        if (function == nullptr) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        bool ok = true;
        void* pointer = OffsetOrPointer(call, call.Arg(2), buffer, 1, "glNormalPointer", &ok);
        if (!ok) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), pointer);
    });
    registry.Add("glPointSizePointerOES", [state](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, int, const void*)>("glPointSizePointerOES");
        if (function == nullptr) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        bool ok = true;
        void* pointer =
            OffsetOrPointer(call, call.Arg(2), buffer, 1, "glPointSizePointerOES", &ok);
        if (!ok) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), pointer);
    });
    // OES_matrix_palette, the skinning path of every GLES1 engine of that era.
    // The Adreno GLES1 driver implements it, but the two pointer setters take
    // guest addresses, so they need the same rebasing as the plain vertex array.
    // Serving them as unknown imports is worse than useless: the array-enabled
    // enum goes through glEnableClientState untouched, so the driver believes
    // the matrix-index array is live while its pointer was never set, and the
    // next draw dereferences it inside the driver and kills the host.
    registry.Add("glMatrixIndexPointerOES", [state](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, unsigned int, int, const void*)>("glMatrixIndexPointerOES");
        if (function == nullptr) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        bool ok = true;
        void* pointer =
            OffsetOrPointer(call, call.Arg(3), buffer, 1, "glMatrixIndexPointerOES", &ok);
        if (!ok) {
            return;
        }
        function(static_cast<int>(call.Arg(0)), call.Arg(1), static_cast<int>(call.Arg(2)),
                 pointer);
    });
    registry.Add("glWeightPointerOES", [state](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, unsigned int, int, const void*)>("glWeightPointerOES");
        if (function == nullptr) {
            return;
        }
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        bool ok = true;
        void* pointer = OffsetOrPointer(call, call.Arg(3), buffer, 1, "glWeightPointerOES", &ok);
        if (!ok) {
            return;
        }
        function(static_cast<int>(call.Arg(0)), call.Arg(1), static_cast<int>(call.Arg(2)),
                 pointer);
    });
    registry.Add("glCurrentPaletteMatrixOES", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int)>("glCurrentPaletteMatrixOES");
        if (function != nullptr) {
            function(call.Arg(0));
        }
    });
    registry.Add("glLoadPaletteFromModelViewMatrixOES", [](HleCall&) {
        static const auto function = Gl1<void (*)()>("glLoadPaletteFromModelViewMatrixOES");
        if (function != nullptr) {
            function();
        }
    });
    registry.Add("glMatrixMode", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int)>("glMatrixMode");
        if (function != nullptr) {
            function(call.Arg(0));
        }
    });
    registry.Add("glPushMatrix", [](HleCall&) {
        static const auto function = Gl1<void (*)()>("glPushMatrix");
        if (function != nullptr) {
            function();
        }
    });
    registry.Add("glPopMatrix", [](HleCall&) {
        static const auto function = Gl1<void (*)()>("glPopMatrix");
        if (function != nullptr) {
            function();
        }
    });
    registry.Add("glTranslatef", [](HleCall& call) {
        static const auto function = Gl1<void (*)(float, float, float)>("glTranslatef");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2));
        }
    });
    registry.Add("glRotatef", [](HleCall& call) {
        static const auto function = Gl1<void (*)(float, float, float, float)>("glRotatef");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2), FloatArg(call, 3));
        }
    });
    registry.Add("glScalef", [](HleCall& call) {
        static const auto function = Gl1<void (*)(float, float, float)>("glScalef");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2));
        }
    });

    // ---- the rest of the fixed-function state ------------------------------
    //
    // A GLES 1.x engine sets up its pipeline with many small state calls between
    // draws: the projection matrix, the texture environment, fog, the colour of
    // the geometry that follows. An entry point that has no stub is not an error
    // the guest can see -- the platform answers zero and the call vanishes -- so
    // a missing glLoadIdentity/glOrthof pair leaves the projection wherever the
    // previous frame left it and the engine draws its next screen in clip space:
    // the window fills with garbage instead of failing. The whole ES 1.1
    // fixed-function surface is forwarded for that reason.
    registry.Add("glLoadIdentity", [](HleCall&) {
        static const auto function = Gl1<void (*)()>("glLoadIdentity");
        if (function != nullptr) {
            function();
        }
    });
    registry.Add("glLoadMatrixf", [](HleCall& call) {
        static const auto function = Gl1<void (*)(const float*)>("glLoadMatrixf");
        const float* values = ArrayArg<float, 16>(call, call.Arg(0), "glLoadMatrixf");
        if (function != nullptr && values != nullptr) {
            function(values);
        }
    });
    registry.Add("glLoadMatrixx", [](HleCall& call) {
        static const auto function = Gl1<void (*)(const int*)>("glLoadMatrixx");
        const int* values = ArrayArg<int, 16>(call, call.Arg(0), "glLoadMatrixx");
        if (function != nullptr && values != nullptr) {
            function(values);
        }
    });
    registry.Add("glMultMatrixf", [](HleCall& call) {
        static const auto function = Gl1<void (*)(const float*)>("glMultMatrixf");
        const float* values = ArrayArg<float, 16>(call, call.Arg(0), "glMultMatrixf");
        if (function != nullptr && values != nullptr) {
            function(values);
        }
    });
    registry.Add("glMultMatrixx", [](HleCall& call) {
        static const auto function = Gl1<void (*)(const int*)>("glMultMatrixx");
        const int* values = ArrayArg<int, 16>(call, call.Arg(0), "glMultMatrixx");
        if (function != nullptr && values != nullptr) {
            function(values);
        }
    });
    registry.Add("glOrthof", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(float, float, float, float, float, float)>("glOrthof");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2), FloatArg(call, 3),
                     FloatArg(call, 4), FloatArg(call, 5));
        }
    });
    registry.Add("glOrthox", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, int, int, int, int, int)>("glOrthox");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                     static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)));
        }
    });
    registry.Add("glFrustumf", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(float, float, float, float, float, float)>("glFrustumf");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2), FloatArg(call, 3),
                     FloatArg(call, 4), FloatArg(call, 5));
        }
    });
    registry.Add("glFrustumx", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, int, int, int, int, int)>("glFrustumx");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                     static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)));
        }
    });
    registry.Add("glTranslatex", [](HleCall& call) {
        static const auto function = Gl1<void (*)(int, int, int)>("glTranslatex");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glScalex", [](HleCall& call) {
        static const auto function = Gl1<void (*)(int, int, int)>("glScalex");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glRotatex", [](HleCall& call) {
        static const auto function = Gl1<void (*)(int, int, int, int)>("glRotatex");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)));
        }
    });
    registry.Add("glColor4f", [](HleCall& call) {
        static const auto function = Gl1<void (*)(float, float, float, float)>("glColor4f");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2), FloatArg(call, 3));
        }
    });
    registry.Add("glColor4x", [](HleCall& call) {
        static const auto function = Gl1<void (*)(int, int, int, int)>("glColor4x");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)));
        }
    });
    registry.Add("glColor4ub", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned char, unsigned char, unsigned char, unsigned char)>(
                "glColor4ub");
        if (function != nullptr) {
            function(static_cast<unsigned char>(call.Arg(0)),
                     static_cast<unsigned char>(call.Arg(1)),
                     static_cast<unsigned char>(call.Arg(2)),
                     static_cast<unsigned char>(call.Arg(3)));
        }
    });
    registry.Add("glNormal3f", [](HleCall& call) {
        static const auto function = Gl1<void (*)(float, float, float)>("glNormal3f");
        if (function != nullptr) {
            function(FloatArg(call, 0), FloatArg(call, 1), FloatArg(call, 2));
        }
    });
    registry.Add("glNormal3x", [](HleCall& call) {
        static const auto function = Gl1<void (*)(int, int, int)>("glNormal3x");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glShadeModel", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int)>("glShadeModel");
        if (function != nullptr) {
            function(call.Arg(0));
        }
    });
    registry.Add("glAlphaFunc", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, float)>("glAlphaFunc");
        if (function != nullptr) {
            function(call.Arg(0), FloatArg(call, 1));
        }
    });
    registry.Add("glAlphaFuncx", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, int)>("glAlphaFuncx");
        if (function != nullptr) {
            function(call.Arg(0), static_cast<int>(call.Arg(1)));
        }
    });
    registry.Add("glFogf", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, float)>("glFogf");
        if (function != nullptr) {
            function(call.Arg(0), FloatArg(call, 1));
        }
    });
    registry.Add("glFogx", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, int)>("glFogx");
        if (function != nullptr) {
            function(call.Arg(0), static_cast<int>(call.Arg(1)));
        }
    });
    registry.Add("glFogfv", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, const float*)>("glFogfv");
        const float* values = ArrayArg<float, 4>(call, call.Arg(1), "glFogfv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), values);
        }
    });
    registry.Add("glFogxv", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, const int*)>("glFogxv");
        const int* values = ArrayArg<int, 4>(call, call.Arg(1), "glFogxv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), values);
        }
    });
    registry.Add("glLightModelf", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, float)>("glLightModelf");
        if (function != nullptr) {
            function(call.Arg(0), FloatArg(call, 1));
        }
    });
    registry.Add("glLightModelx", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, int)>("glLightModelx");
        if (function != nullptr) {
            function(call.Arg(0), static_cast<int>(call.Arg(1)));
        }
    });
    registry.Add("glLightModelfv", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, const float*)>("glLightModelfv");
        const float* values = ArrayArg<float, 4>(call, call.Arg(1), "glLightModelfv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), values);
        }
    });
    registry.Add("glLightModelxv", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, const int*)>("glLightModelxv");
        const int* values = ArrayArg<int, 4>(call, call.Arg(1), "glLightModelxv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), values);
        }
    });
    registry.Add("glLightf", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, unsigned int, float)>("glLightf");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), FloatArg(call, 2));
        }
    });
    registry.Add("glLightx", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, unsigned int, int)>("glLightx");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glLightfv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, const float*)>("glLightfv");
        const float* values = ArrayArg<float, 4>(call, call.Arg(2), "glLightfv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), call.Arg(1), values);
        }
    });
    registry.Add("glLightxv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, const int*)>("glLightxv");
        const int* values = ArrayArg<int, 4>(call, call.Arg(2), "glLightxv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), call.Arg(1), values);
        }
    });
    registry.Add("glMaterialf", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, float)>("glMaterialf");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), FloatArg(call, 2));
        }
    });
    registry.Add("glMaterialx", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, int)>("glMaterialx");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glMaterialfv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, const float*)>("glMaterialfv");
        const float* values = ArrayArg<float, 4>(call, call.Arg(2), "glMaterialfv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), call.Arg(1), values);
        }
    });
    registry.Add("glMaterialxv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, const int*)>("glMaterialxv");
        const int* values = ArrayArg<int, 4>(call, call.Arg(2), "glMaterialxv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), call.Arg(1), values);
        }
    });
    registry.Add("glTexEnvi", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, int)>("glTexEnvi");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glTexEnvf", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, float)>("glTexEnvf");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), FloatArg(call, 2));
        }
    });
    registry.Add("glTexEnvx", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, int)>("glTexEnvx");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glTexEnvfv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, const float*)>("glTexEnvfv");
        const float* values = ArrayArg<float, 4>(call, call.Arg(2), "glTexEnvfv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), call.Arg(1), values);
        }
    });
    registry.Add("glTexEnvxv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, const int*)>("glTexEnvxv");
        const int* values = ArrayArg<int, 4>(call, call.Arg(2), "glTexEnvxv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), call.Arg(1), values);
        }
    });
    registry.Add("glTexGeni", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, int)>("glTexGeni");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glTexGenf", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, float)>("glTexGenf");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), FloatArg(call, 2));
        }
    });
    registry.Add("glTexGenx", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, int)>("glTexGenx");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glTexGenfv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, const float*)>("glTexGenfv");
        const float* values = ArrayArg<float, 4>(call, call.Arg(2), "glTexGenfv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), call.Arg(1), values);
        }
    });
    registry.Add("glTexGenxv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, const int*)>("glTexGenxv");
        const int* values = ArrayArg<int, 4>(call, call.Arg(2), "glTexGenxv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), call.Arg(1), values);
        }
    });
    registry.Add("glClientActiveTexture", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int)>("glClientActiveTexture");
        if (function != nullptr) {
            function(call.Arg(0));
        }
    });
    registry.Add("glPointSize", [](HleCall& call) {
        static const auto function = Gl1<void (*)(float)>("glPointSize");
        if (function != nullptr) {
            function(FloatArg(call, 0));
        }
    });
    registry.Add("glPointParameterf", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, float)>("glPointParameterf");
        if (function != nullptr) {
            function(call.Arg(0), FloatArg(call, 1));
        }
    });
    registry.Add("glPointParameterfv", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, const float*)>("glPointParameterfv");
        const float* values = ArrayArg<float, 4>(call, call.Arg(1), "glPointParameterfv");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), values);
        }
    });
    registry.Add("glClipPlanef", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, const float*)>("glClipPlanef");
        const float* values = ArrayArg<float, 4>(call, call.Arg(1), "glClipPlanef");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), values);
        }
    });
    registry.Add("glClipPlanex", [](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int, const int*)>("glClipPlanex");
        const int* values = ArrayArg<int, 4>(call, call.Arg(1), "glClipPlanex");
        if (function != nullptr && values != nullptr) {
            function(call.Arg(0), values);
        }
    });
    registry.Add("glDepthRangex", [](HleCall& call) {
        static const auto function = Gl1<void (*)(int, int)>("glDepthRangex");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)));
        }
    });
    registry.Add("glClearDepthx", [](HleCall& call) {
        static const auto function = Gl1<void (*)(int)>("glClearDepthx");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)));
        }
    });

    // ---- uniforms ----------------------------------------------------------

    registry.Add("glUniform1i", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, int)>("glUniform1i");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)));
        }
    });
    registry.Add("glUniform2i", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, int, int)>("glUniform2i");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glUniform3i", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, int, int, int)>("glUniform3i");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)));
        }
    });
    registry.Add("glUniform4i", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, int, int, int, int)>("glUniform4i");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                     static_cast<int>(call.Arg(4)));
        }
    });
    registry.Add("glUniform1f", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, float)>("glUniform1f");
        if (function != nullptr) function(static_cast<int>(call.Arg(0)), FloatArg(call, 1));
    });
    registry.Add("glUniform2f", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, float, float)>("glUniform2f");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), FloatArg(call, 1), FloatArg(call, 2));
        }
    });
    registry.Add("glUniform3f", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, float, float, float)>("glUniform3f");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), FloatArg(call, 1), FloatArg(call, 2),
                     FloatArg(call, 3));
        }
    });
    registry.Add("glUniform4f", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, float, float, float, float)>("glUniform4f");
        if (function != nullptr) {
            function(static_cast<int>(call.Arg(0)), FloatArg(call, 1), FloatArg(call, 2),
                     FloatArg(call, 3), FloatArg(call, 4));
        }
    });
    // Array uniforms: count is the number of vectors the guest promised, and the
    // driver reads count * components floats.
    const auto uniform_vector = [&registry](const char* name, std::size_t components) {
        using Function = void (*)(int, int, const float*);
        const Function function = Gl<Function>(name);  // resolved once, at registration
        registry.Add(name, [name, function, components](HleCall& call) {
            if (function == nullptr) {
                return;
            }
            const int count = static_cast<int>(call.Arg(1));
            if (count < 0 || count > 65536) {
                return;
            }
            const std::size_t length = static_cast<std::size_t>(count) * components * sizeof(float);
            const Buffer values = GuestBuffer(call, call.Arg(2), length);
            if (!CheckBuffer(name, call.Arg(2), values, length)) {
                return;
            }
            function(static_cast<int>(call.Arg(0)), count,
                     static_cast<const float*>(values.bytes));
        });
    };
    uniform_vector("glUniform1fv", 1);
    uniform_vector("glUniform2fv", 2);
    uniform_vector("glUniform3fv", 3);
    uniform_vector("glUniform4fv", 4);
    const auto uniform_int_vector = [&registry](const char* name, std::size_t components) {
        using Function = void (*)(int, int, const int*);
        const Function function = Gl<Function>(name);  // resolved once, at registration
        registry.Add(name, [name, function, components](HleCall& call) {
            if (function == nullptr) {
                return;
            }
            const int count = static_cast<int>(call.Arg(1));
            if (count < 0 || count > 65536) {
                return;
            }
            const std::size_t length = static_cast<std::size_t>(count) * components * sizeof(int);
            const Buffer values = GuestBuffer(call, call.Arg(2), length);
            if (!CheckBuffer(name, call.Arg(2), values, length)) {
                return;
            }
            function(static_cast<int>(call.Arg(0)), count,
                     static_cast<const int*>(values.bytes));
        });
    };
    uniform_int_vector("glUniform1iv", 1);
    uniform_int_vector("glUniform2iv", 2);
    uniform_int_vector("glUniform3iv", 3);
    uniform_int_vector("glUniform4iv", 4);
    const auto uniform_matrix = [&registry](const char* name, std::size_t edge) {
        using Function = void (*)(int, int, unsigned char, const float*);
        const Function function = Gl<Function>(name);  // resolved once, at registration
        registry.Add(name, [name, function, edge](HleCall& call) {
            if (function == nullptr) {
                return;
            }
            const int count = static_cast<int>(call.Arg(1));
            if (count < 0 || count > 65536) {
                return;
            }
            const std::size_t length =
                static_cast<std::size_t>(count) * edge * edge * sizeof(float);
            const Buffer values = GuestBuffer(call, call.Arg(3), length);
            if (!CheckBuffer(name, call.Arg(3), values, length)) {
                return;
            }
            function(static_cast<int>(call.Arg(0)), count,
                     static_cast<unsigned char>(call.Arg(2)),
                     static_cast<const float*>(values.bytes));
        });
    };
    uniform_matrix("glUniformMatrix2fv", 2);
    uniform_matrix("glUniformMatrix3fv", 3);
    uniform_matrix("glUniformMatrix4fv", 4);
    registry.Add("glGetUniformfv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, float*)>("glGetUniformfv");
        if (function == nullptr) {
            return;
        }
        // A uniform is at most a mat4 here; the arena commits whole pages, so a
        // 16-float window is a safe upper bound for the range check.
        float* out = reinterpret_cast<float*>(call.Mapped(call.Arg(2), 16 * sizeof(float)));
        if (out == nullptr) {
            RejectBuffer("glGetUniformfv", call.Arg(2), 16 * sizeof(float));
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), out);
    });
    registry.Add("glGetUniformiv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int*)>("glGetUniformiv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(2), 16 * sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetUniformiv", call.Arg(2), 16 * sizeof(int));
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), out);
    });

    // ---- textures ----------------------------------------------------------

    registry.Add("glGenTextures", [](HleCall& call) {
        static const auto function = Gl<void (*)(int, unsigned int*)>("glGenTextures");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer out = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glGenTextures", call.Arg(1), out, length, count)) {
            return;
        }
        function(count, static_cast<unsigned int*>(out.bytes));
    });
    registry.Add("glDeleteTextures", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, const unsigned int*)>("glDeleteTextures");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer names = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glDeleteTextures", call.Arg(1), names, length, count)) {
            return;
        }
        function(count, static_cast<const unsigned int*>(names.bytes));
    });
    registry.Add("glBindTexture", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int)>("glBindTexture");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glActiveTexture", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glActiveTexture");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glTexImage2D", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, unsigned int, unsigned int,
                        const void*)>("glTexImage2D");
        if (function == nullptr) {
            return;
        }
        int alignment = 4;
        {
            const std::lock_guard lock(state->mutex);
            alignment = state->unpack_alignment;
        }
        const std::size_t length = ImageSize(call.Arg(3), call.Arg(4), call.Arg(6), call.Arg(7),
                                             alignment);
        const Buffer pixels = GuestBuffer(call, call.Arg(8), length != 0 ? length : 1);
        if (!CheckBuffer("glTexImage2D", call.Arg(8), pixels, length != 0 ? length : 1)) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), call.Arg(6), call.Arg(7), pixels.bytes);
    });
    registry.Add("glTexSubImage2D", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, unsigned int, unsigned int,
                        const void*)>("glTexSubImage2D");
        if (function == nullptr) {
            return;
        }
        int alignment = 4;
        {
            const std::lock_guard lock(state->mutex);
            alignment = state->unpack_alignment;
        }
        const std::size_t length = ImageSize(call.Arg(3), call.Arg(4), call.Arg(6), call.Arg(7),
                                             alignment);
        const Buffer pixels = GuestBuffer(call, call.Arg(8), length != 0 ? length : 1);
        if (!CheckBuffer("glTexSubImage2D", call.Arg(8), pixels, length != 0 ? length : 1)) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), call.Arg(6), call.Arg(7), pixels.bytes);
    });
    // ---- ES 3.0 texture storage and framebuffer ---------------------------
    // The host driver is ES 3.x and reports that even for the ES 2.0 context the
    // guest asks for. Engines read the version string and therefore reach these
    // calls; forward them so the renderer sees the same surface as on a device.
    registry.Add("glTexStorage2D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, int, int)>("glTexStorage2D");
        if (function == nullptr) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)));
    });
    registry.Add("glTexStorage3D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, int, int, int)>(
                "glTexStorage3D");
        if (function == nullptr) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)));
    });
    registry.Add("glTexImage3D", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, int, unsigned int,
                        unsigned int, const void*)>("glTexImage3D");
        if (function == nullptr) return;
        int alignment = 4;
        {
            const std::lock_guard lock(state->mutex);
            alignment = state->unpack_alignment;
        }
        const std::size_t slice = ImageSize(call.Arg(3), call.Arg(4), call.Arg(7),
                                            call.Arg(8), alignment);
        const std::size_t length = slice * static_cast<std::size_t>(call.Arg(5));
        const Buffer pixels = GuestBuffer(call, call.Arg(9), length != 0 ? length : 1);
        if (!CheckBuffer("glTexImage3D", call.Arg(9), pixels, length != 0 ? length : 1)) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                 static_cast<int>(call.Arg(6)), call.Arg(7), call.Arg(8), pixels.bytes);
    });
    registry.Add("glTexSubImage3D", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, int, int, unsigned int,
                        unsigned int, const void*)>("glTexSubImage3D");
        if (function == nullptr) return;
        int alignment = 4;
        {
            const std::lock_guard lock(state->mutex);
            alignment = state->unpack_alignment;
        }
        const std::size_t slice = ImageSize(call.Arg(5), call.Arg(6), call.Arg(8),
                                            call.Arg(9), alignment);
        const std::size_t length = slice * static_cast<std::size_t>(call.Arg(7));
        const Buffer pixels = GuestBuffer(call, call.Arg(10), length != 0 ? length : 1);
        if (!CheckBuffer("glTexSubImage3D", call.Arg(10), pixels,
                         length != 0 ? length : 1)) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                 static_cast<int>(call.Arg(6)), static_cast<int>(call.Arg(7)),
                 call.Arg(8), call.Arg(9), pixels.bytes);
    });
    registry.Add("glCompressedTexImage3D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, int, int, int, int, int,
                        const void*)>("glCompressedTexImage3D");
        if (function == nullptr) return;
        const int size = static_cast<int>(call.Arg(7));
        if (size < 0) return;
        const Buffer data = GuestBuffer(call, call.Arg(8), static_cast<std::size_t>(size));
        if (!CheckBuffer("glCompressedTexImage3D", call.Arg(8), data,
                         static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), static_cast<int>(call.Arg(6)), size,
                 data.bytes);
    });
    registry.Add("glCompressedTexSubImage3D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, int, int, unsigned int,
                        int, const void*)>("glCompressedTexSubImage3D");
        if (function == nullptr) return;
        const int size = static_cast<int>(call.Arg(9));
        if (size < 0) return;
        const Buffer data = GuestBuffer(call, call.Arg(10), static_cast<std::size_t>(size));
        if (!CheckBuffer("glCompressedTexSubImage3D", call.Arg(10), data,
                         static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                 static_cast<int>(call.Arg(6)), static_cast<int>(call.Arg(7)),
                 call.Arg(8), size, data.bytes);
    });
    registry.Add("glCopyTexSubImage3D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, int, int, int)>(
                "glCopyTexSubImage3D");
        if (function == nullptr) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                 static_cast<int>(call.Arg(6)), static_cast<int>(call.Arg(7)),
                 static_cast<int>(call.Arg(8)));
    });
    registry.Add("glFramebufferTextureLayer", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, unsigned int, int, int)>(
                "glFramebufferTextureLayer");
        if (function == nullptr) return;
        function(call.Arg(0), call.Arg(1), call.Arg(2), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)));
    });
    registry.Add("glRenderbufferStorageMultisample", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, int, int)>(
                "glRenderbufferStorageMultisample");
        if (function == nullptr) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)));
    });
    registry.Add("glBlitFramebuffer", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, int, int, int, int, int, int, int, unsigned int,
                        unsigned int)>("glBlitFramebuffer");
        if (function == nullptr) return;
        function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                 static_cast<int>(call.Arg(6)), static_cast<int>(call.Arg(7)),
                 call.Arg(8), call.Arg(9));
    });
    registry.Add("glDrawBuffers", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, const unsigned int*)>("glDrawBuffers");
        if (function == nullptr) return;
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0) return;
        const Buffer buffers =
            GuestBuffer(call, call.Arg(1), static_cast<std::size_t>(count) * sizeof(unsigned));
        if (!RequireArray("glDrawBuffers", call.Arg(1), buffers,
                          static_cast<std::size_t>(count) * sizeof(unsigned), count)) {
            return;
        }
        function(count, static_cast<const unsigned int*>(buffers.bytes));
    });
    registry.Add("glInvalidateFramebuffer", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, const unsigned int*)>(
                "glInvalidateFramebuffer");
        if (function == nullptr) return;
        const int count = static_cast<int>(call.Arg(1));
        if (count < 0) return;
        const Buffer attachments =
            GuestBuffer(call, call.Arg(2), static_cast<std::size_t>(count) * sizeof(unsigned));
        if (!RequireArray("glInvalidateFramebuffer", call.Arg(2), attachments,
                          static_cast<std::size_t>(count) * sizeof(unsigned), count)) {
            return;
        }
        function(call.Arg(0), count,
                 static_cast<const unsigned int*>(attachments.bytes));
    });
    registry.Add("glInvalidateSubFramebuffer", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, const unsigned int*, int, int, int, int)>(
                "glInvalidateSubFramebuffer");
        if (function == nullptr) return;
        const int count = static_cast<int>(call.Arg(1));
        if (count < 0) return;
        const Buffer attachments =
            GuestBuffer(call, call.Arg(2), static_cast<std::size_t>(count) * sizeof(unsigned));
        if (!RequireArray("glInvalidateSubFramebuffer", call.Arg(2), attachments,
                          static_cast<std::size_t>(count) * sizeof(unsigned), count)) {
            return;
        }
        function(call.Arg(0), count,
                 static_cast<const unsigned int*>(attachments.bytes),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), static_cast<int>(call.Arg(6)));
    });
    registry.Add("glGetInternalformativ", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, unsigned int, int, int*)>(
                "glGetInternalformativ");
        if (function == nullptr) return;
        const int count = static_cast<int>(call.Arg(3));
        if (count < 0) return;
        int* out = reinterpret_cast<int*>(
            call.Mapped(call.Arg(4), static_cast<std::size_t>(count) * sizeof(int)));
        if (count > 0 && out == nullptr) {
            RejectBuffer("glGetInternalformativ", call.Arg(4),
                         static_cast<std::size_t>(count) * sizeof(int));
            return;
        }
        function(call.Arg(0), call.Arg(1), call.Arg(2), count, out);
    });
    registry.Add("glClearBufferfv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, const float*)>("glClearBufferfv");
        if (function == nullptr) return;
        const Buffer value = GuestBuffer(call, call.Arg(2), 4 * sizeof(float));
        if (!CheckBuffer("glClearBufferfv", call.Arg(2), value, 4 * sizeof(float))) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<const float*>(value.bytes));
    });
    registry.Add("glClearBufferiv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, const int*)>("glClearBufferiv");
        if (function == nullptr) return;
        const Buffer value = GuestBuffer(call, call.Arg(2), 4 * sizeof(int));
        if (!CheckBuffer("glClearBufferiv", call.Arg(2), value, 4 * sizeof(int))) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<const int*>(value.bytes));
    });
    registry.Add("glClearBufferuiv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, const unsigned int*)>("glClearBufferuiv");
        if (function == nullptr) return;
        const Buffer value = GuestBuffer(call, call.Arg(2), 4 * sizeof(unsigned));
        if (!CheckBuffer("glClearBufferuiv", call.Arg(2), value, 4 * sizeof(unsigned))) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<const unsigned int*>(value.bytes));
    });
    registry.Add("glClearBufferfi", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, float, int)>("glClearBufferfi");
        if (function == nullptr) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)), FloatArg(call, 2),
                 static_cast<int>(call.Arg(3)));
    });
    registry.Add("glCompressedTexImage2D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, int, int, int, int, const void*)>(
                "glCompressedTexImage2D");
        if (function == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(6));
        if (size < 0) {
            return;
        }
        const Buffer data = GuestBuffer(call, call.Arg(7), static_cast<std::size_t>(size));
        if (!CheckBuffer("glCompressedTexImage2D", call.Arg(7), data,
                         static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), size, data.bytes);
    });
    registry.Add("glCompressedTexSubImage2D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, unsigned int, int, const void*)>(
                "glCompressedTexSubImage2D");
        if (function == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(7));
        if (size < 0) {
            return;
        }
        const Buffer data = GuestBuffer(call, call.Arg(8), static_cast<std::size_t>(size));
        if (!CheckBuffer("glCompressedTexSubImage2D", call.Arg(8), data,
                         static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), call.Arg(6), size, data.bytes);
    });
    registry.Add("glCopyTexImage2D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, int, int, int, int)>("glCopyTexImage2D");
        if (function != nullptr) {
            function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                     static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                     static_cast<int>(call.Arg(5)), static_cast<int>(call.Arg(6)));
        }
    });
    registry.Add("glCopyTexSubImage2D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, int, int)>("glCopyTexSubImage2D");
        if (function != nullptr) {
            function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                     static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                     static_cast<int>(call.Arg(5)), static_cast<int>(call.Arg(6)),
                     static_cast<int>(call.Arg(7)));
        }
    });
    registry.Add("glTexParameteri", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int)>("glTexParameteri");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)));
        }
    });
    registry.Add("glTexParameterf", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, float)>("glTexParameterf");
        if (function != nullptr) function(call.Arg(0), call.Arg(1), FloatArg(call, 2));
    });
    registry.Add("glTexParameterfv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, const float*)>("glTexParameterfv");
        if (function == nullptr) {
            return;
        }
        const Buffer values = GuestBuffer(call, call.Arg(2), 4 * sizeof(float));
        if (!CheckBuffer("glTexParameterfv", call.Arg(2), values, 4 * sizeof(float))) {
            return;
        }
        function(call.Arg(0), call.Arg(1), static_cast<const float*>(values.bytes));
    });
    registry.Add("glTexParameteriv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, const int*)>("glTexParameteriv");
        if (function == nullptr) {
            return;
        }
        const Buffer values = GuestBuffer(call, call.Arg(2), 4 * sizeof(int));
        if (!CheckBuffer("glTexParameteriv", call.Arg(2), values, 4 * sizeof(int))) {
            return;
        }
        function(call.Arg(0), call.Arg(1), static_cast<const int*>(values.bytes));
    });
    registry.Add("glGetTexParameterfv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, float*)>("glGetTexParameterfv");
        if (function == nullptr) {
            return;
        }
        float* out = reinterpret_cast<float*>(call.Mapped(call.Arg(2), 4 * sizeof(float)));
        if (out == nullptr) {
            RejectBuffer("glGetTexParameterfv", call.Arg(2), 4 * sizeof(float));
            return;
        }
        function(call.Arg(0), call.Arg(1), out);
    });
    registry.Add("glGetTexParameteriv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int*)>("glGetTexParameteriv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(2), 4 * sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetTexParameteriv", call.Arg(2), 4 * sizeof(int));
            return;
        }
        function(call.Arg(0), call.Arg(1), out);
    });
    registry.Add("glGenerateMipmap", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int)>("glGenerateMipmap");
        if (function != nullptr) function(call.Arg(0));
    });
    registry.Add("glIsTexture", [](HleCall& call) {
        static const auto function = Gl<unsigned char (*)(unsigned int)>("glIsTexture");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });

    // ---- framebuffer objects (core in GLES 2.0) ---------------------------

    registry.Add("glGenFramebuffers", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, unsigned int*)>("glGenFramebuffers");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer out = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glGenFramebuffers", call.Arg(1), out, length, count)) {
            return;
        }
        function(count, static_cast<unsigned int*>(out.bytes));
    });
    registry.Add("glDeleteFramebuffers", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, const unsigned int*)>("glDeleteFramebuffers");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer names = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glDeleteFramebuffers", call.Arg(1), names, length, count)) {
            return;
        }
        function(count, static_cast<const unsigned int*>(names.bytes));
    });
    registry.Add("glBindFramebuffer", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int)>("glBindFramebuffer");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glFramebufferTexture2D", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, unsigned int, unsigned int, int)>(
                "glFramebufferTexture2D");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), call.Arg(2), call.Arg(3),
                     static_cast<int>(call.Arg(4)));
        }
    });
    registry.Add("glFramebufferRenderbuffer", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, unsigned int, unsigned int)>(
                "glFramebufferRenderbuffer");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), call.Arg(2), call.Arg(3));
        }
    });
    registry.Add("glCheckFramebufferStatus", [](HleCall& call) {
        static const auto function = Gl<unsigned int (*)(unsigned int)>("glCheckFramebufferStatus");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glIsFramebuffer", [](HleCall& call) {
        static const auto function = Gl<unsigned char (*)(unsigned int)>("glIsFramebuffer");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glGenRenderbuffers", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, unsigned int*)>("glGenRenderbuffers");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer out = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glGenRenderbuffers", call.Arg(1), out, length, count)) {
            return;
        }
        function(count, static_cast<unsigned int*>(out.bytes));
    });
    registry.Add("glDeleteRenderbuffers", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, const unsigned int*)>("glDeleteRenderbuffers");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer names = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glDeleteRenderbuffers", call.Arg(1), names, length, count)) {
            return;
        }
        function(count, static_cast<const unsigned int*>(names.bytes));
    });
    registry.Add("glBindRenderbuffer", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int)>("glBindRenderbuffer");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glRenderbufferStorage", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int, int)>("glRenderbufferStorage");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)),
                     static_cast<int>(call.Arg(3)));
        }
    });
    registry.Add("glIsRenderbuffer", [](HleCall& call) {
        static const auto function = Gl<unsigned char (*)(unsigned int)>("glIsRenderbuffer");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glGetFramebufferAttachmentParameteriv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, unsigned int, int*)>(
                "glGetFramebufferAttachmentParameteriv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(3), sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetFramebufferAttachmentParameteriv", call.Arg(3), sizeof(int));
            return;
        }
        function(call.Arg(0), call.Arg(1), call.Arg(2), out);
    });
    registry.Add("glGetRenderbufferParameteriv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, int*)>("glGetRenderbufferParameteriv");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetRenderbufferParameteriv", call.Arg(2), sizeof(int));
            return;
        }
        function(call.Arg(0), call.Arg(1), out);
    });

    // ---- readback and synchronisation -------------------------------------

    registry.Add("glReadPixels", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(int, int, int, int, unsigned int, unsigned int, void*)>("glReadPixels");
        if (function == nullptr) {
            return;
        }
        int alignment = 4;
        {
            const std::lock_guard lock(state->mutex);
            alignment = state->pack_alignment;
        }
        const std::size_t length = ImageSize(call.Arg(2), call.Arg(3), call.Arg(4), call.Arg(5),
                                             alignment);
        const Buffer pixels = GuestBuffer(call, call.Arg(6), length != 0 ? length : 1);
        if (!CheckBuffer("glReadPixels", call.Arg(6), pixels, length != 0 ? length : 1)) {
            return;
        }
        function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)), call.Arg(4),
                 call.Arg(5), pixels.bytes);
    });
    registry.Add("glFlush", [](HleCall&) {
        static const auto function = Gl<void (*)(void)>("glFlush");
        if (function != nullptr) function();
    });
    registry.Add("glFinish", [](HleCall&) {
        static const auto function = Gl<void (*)(void)>("glFinish");
        if (function != nullptr) function();
    });
}

}  // namespace armbat
