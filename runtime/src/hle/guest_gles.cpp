#include <android/log.h>

#include "armbat/log.h"
#include <dlfcn.h>

#include <atomic>
#include <array>
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
#include "armbat/guest_modules.h"
#include "armbat/hle.h"
#include "armbat/handles.h"

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

// The ES 3.0 entry points are exported by libGLESv3.so; libGLESv2.so carries the
// ES 2.0 set and, depending on the platform build, an incomplete copy of the 3.0
// one. Resolving through the ES 2.0 library alone left every 3.0 call null --
// glTexStorage2D, glBindSampler, glDrawBuffers, glMapBufferRange and the rest --
// and a null entry point is silently dropped by the handlers below. An engine on
// the 3.0 path then draws from textures that never received storage and presents
// a black frame with no error anywhere in the log. Both libraries are asked, the
// 3.0 one first.
void* GlSymbol(const char* name) {
    static void* es3 = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    static void* es2 = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (es3 != nullptr) {
        if (void* symbol = ::dlsym(es3, name)) {
            return symbol;
        }
    }
    if (es2 != nullptr) {
        if (void* symbol = ::dlsym(es2, name)) {
            return symbol;
        }
    }
    // A missing entry point used to be indistinguishable from a call that did
    // nothing, which is how the entire ES 3.0 surface stayed dead without a line
    // in the log. Say it once per name.
    static std::mutex reported_mutex;
    static std::set<std::string> reported;
    {
        const std::lock_guard lock(reported_mutex);
        if (reported.insert(name).second) {
            ARMBAT_LOG_WARN("Armbat", "gles: entry point %s is not on this driver", name);
        }
    }
    return nullptr;
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

// Client version of the EGL context that is current on this thread, or 0 when
// EGL cannot say. The context is usually created by the guest's Java half,
// which talks to the host EGL directly, so the container never sees that
// eglCreateContext and cannot remember the version at creation time. A GLES 1.x
// context has neither shaders nor the ES2 entry points, and telling an engine
// that it has an ES 3 context sends it down a path the context cannot serve.
int CurrentContextVersion() {
    using GetCurrentDisplay = void* (*)(void);
    using GetCurrentContext = void* (*)(void);
    using QueryContext = int (*)(void*, void*, int, int*);
    static void* library = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        return 0;
    }
    static const auto get_display =
        reinterpret_cast<GetCurrentDisplay>(::dlsym(library, "eglGetCurrentDisplay"));
    static const auto get_context =
        reinterpret_cast<GetCurrentContext>(::dlsym(library, "eglGetCurrentContext"));
    static const auto query_context =
        reinterpret_cast<QueryContext>(::dlsym(library, "eglQueryContext"));
    if (get_display == nullptr || get_context == nullptr || query_context == nullptr) {
        return 0;
    }
    void* display = get_display();
    void* context = get_context();
    int version = 0;
    if (display == nullptr || context == nullptr ||
        query_context(display, context, 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */, &version) == 0) {
        return 0;
    }
    return version;
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

// Bytes per pixel of a client texture / readback buffer, or 0 for a format the
// helper does not know. A zero makes the caller skip the range check, so the
// table has to cover the ES 3.0 formats as well as the ES 2.0 ones.
std::size_t PixelSize(unsigned format, unsigned type) {
    // Types that pack a whole pixel into one element; the format does not add
    // components on top of them.
    switch (type) {
        case 0x8363:  // GL_UNSIGNED_SHORT_5_6_5
        case 0x8033:  // GL_UNSIGNED_SHORT_4_4_4_4
        case 0x8034:  // GL_UNSIGNED_SHORT_5_5_5_1
            return 2;
        case 0x8368:  // GL_UNSIGNED_INT_2_10_10_10_REV
        case 0x84fa:  // GL_UNSIGNED_INT_24_8
        case 0x8c3b:  // GL_UNSIGNED_INT_10F_11F_11F_REV
        case 0x8c3e:  // GL_UNSIGNED_INT_5_9_9_9_REV
            return 4;
        case 0x8dad:  // GL_FLOAT_32_UNSIGNED_INT_24_8_REV
            return 8;
        default:
            break;
    }
    std::size_t components = 0;
    switch (format) {
        case 0x1903:  // GL_RED
        case 0x8d94:  // GL_RED_INTEGER
            components = 1;
            break;
        case 0x8227:  // GL_RG
        case 0x8228:  // GL_RG_INTEGER
            components = 2;
            break;
        case 0x1907:  // GL_RGB
        case 0x8d98:  // GL_RGB_INTEGER
            components = 3;
            break;
        case 0x1908:  // GL_RGBA
        case 0x8d99:  // GL_RGBA_INTEGER
            components = 4;
            break;
        case 0x190a:  // GL_LUMINANCE_ALPHA
            components = 2;
            break;
        case 0x1909:  // GL_LUMINANCE
        case 0x1906:  // GL_ALPHA
        case 0x1902:  // GL_DEPTH_COMPONENT
        case 0x84f9:  // GL_DEPTH_STENCIL
            components = 1;
            break;
        default:
            return 0;
    }
    std::size_t element = 0;
    switch (type) {
        case 0x1400:  // GL_BYTE
        case 0x1401:  // GL_UNSIGNED_BYTE
            element = 1;
            break;
        case 0x1402:  // GL_SHORT
        case 0x1403:  // GL_UNSIGNED_SHORT
        case 0x140b:  // GL_HALF_FLOAT
        case 0x8d61:  // GL_HALF_FLOAT_OES
            element = 2;
            break;
        case 0x1404:  // GL_INT
        case 0x1405:  // GL_UNSIGNED_INT
        case 0x1406:  // GL_FLOAT
            element = 4;
            break;
        default:
            return 0;
    }
    return components * element;
}

// Where the image sits inside the guest buffer the driver is about to read or
// write. GL_UNPACK_ROW_LENGTH and the skip values move it: a row can be longer
// than the image is wide and the first pixel can start several rows into the
// pointer. Ignoring them makes the check either too short -- the driver reads
// past the checked range -- or too long, and a valid call is refused.
std::size_t ImageSize(unsigned width, unsigned height, unsigned format, unsigned type,
                      int alignment, int row_length = 0, int skip_rows = 0,
                      int skip_pixels = 0) {
    const std::size_t pixel = PixelSize(format, type);
    if (pixel == 0 || width == 0 || height == 0) {
        return 0;
    }
    const std::size_t align = static_cast<std::size_t>(alignment > 0 ? alignment : 1);
    const std::size_t stride =
        static_cast<std::size_t>(row_length > 0 ? row_length : static_cast<int>(width));
    std::size_t row = stride * pixel;
    row = (row + align - 1) / align * align;
    const std::size_t first_row = static_cast<std::size_t>(skip_rows > 0 ? skip_rows : 0);
    const std::size_t first_column =
        static_cast<std::size_t>(skip_pixels > 0 ? skip_pixels : 0);
    const std::size_t first = first_row * row + first_column * pixel;
    return first + static_cast<std::size_t>(height - 1) * row +
           static_cast<std::size_t>(width) * pixel;
}

// The same for a 3D upload: images are padded to GL_UNPACK_IMAGE_HEIGHT rows and
// GL_UNPACK_SKIP_IMAGES of them can precede the first image.
std::size_t ImageSize3D(unsigned width, unsigned height, unsigned depth, unsigned format,
                        unsigned type, int alignment, int row_length, int skip_rows,
                        int skip_pixels, int image_height, int skip_images) {
    const std::size_t slice =
        ImageSize(width, height, format, type, alignment, row_length, skip_rows, skip_pixels);
    if (slice == 0 || depth == 0) {
        return 0;
    }
    const std::size_t pixel = PixelSize(format, type);
    const std::size_t align = static_cast<std::size_t>(alignment > 0 ? alignment : 1);
    const std::size_t stride =
        static_cast<std::size_t>(row_length > 0 ? row_length : static_cast<int>(width));
    std::size_t row = stride * pixel;
    row = (row + align - 1) / align * align;
    const std::size_t rows =
        static_cast<std::size_t>(image_height > 0 ? image_height : static_cast<int>(height));
    const std::size_t image = rows * row;
    const std::size_t first =
        static_cast<std::size_t>(skip_images > 0 ? skip_images : 0) * image;
    return first + slice + static_cast<std::size_t>(depth - 1) * image;
}

// How many elements glGet* writes for this pname. Multi-value state is the
// exception; most queries write one value. A multi-value pname missing from this
// table makes the range check too short and the driver then writes past the
// guest buffer that was checked, so the table has to cover every ES 2.0/3.0
// query that answers with more than one element.
std::size_t ValueCount(unsigned pname) {
    switch (pname) {
        case 0x0ba2:  // GL_VIEWPORT
        case 0x0c10:  // GL_SCISSOR_BOX
        case 0x0c22:  // GL_COLOR_CLEAR_VALUE
        case 0x0c23:  // GL_COLOR_WRITEMASK
        case 0x8005:  // GL_BLEND_COLOR
            return 4;
        case 0x0b70:  // GL_DEPTH_RANGE
        case 0x0d3a:  // GL_MAX_VIEWPORT_DIMS
        case 0x846d:  // GL_ALIASED_POINT_SIZE_RANGE
        case 0x846e:  // GL_ALIASED_LINE_WIDTH_RANGE
            return 2;
        case 0x91be:  // GL_MAX_COMPUTE_WORK_GROUP_COUNT
        case 0x91bf:  // GL_MAX_COMPUTE_WORK_GROUP_SIZE
            return 3;
        default:
            return 1;
    }
}

// The format lists have no fixed length: the driver answers how many entries it
// has, and the guest's buffer is sized from that same number. The number is
// capped so a broken answer cannot ask for an unbounded check.
std::size_t ValueCountQuery(unsigned pname) {
    unsigned int count_name = 0;
    switch (pname) {
        case 0x86a3:  // GL_COMPRESSED_TEXTURE_FORMATS
            count_name = 0x86a2;  // GL_NUM_COMPRESSED_TEXTURE_FORMATS
            break;
        case 0x8df7:  // GL_PROGRAM_BINARY_FORMATS
            count_name = 0x87fe;  // GL_NUM_PROGRAM_BINARY_FORMATS
            break;
        case 0x8df6:  // GL_SHADER_BINARY_FORMATS
            count_name = 0x8df9;  // GL_NUM_SHADER_BINARY_FORMATS
            break;
        default:
            return ValueCount(pname);
    }
    const auto get_integer = Gl<void (*)(unsigned int, int*)>("glGetIntegerv");
    if (get_integer == nullptr) {
        return 0;
    }
    int count = 0;
    get_integer(count_name, &count);
    if (count <= 0) {
        return 0;
    }
    return count > 64 ? 64u : static_cast<std::size_t>(count);
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
    // A bound pixel buffer turns the pixels argument into an offset, so the two
    // have to be told apart before anything is range-checked.
    std::uint32_t unpack_buffer = 0;  // GL_PIXEL_UNPACK_BUFFER
    std::uint32_t pack_buffer = 0;    // GL_PIXEL_PACK_BUFFER
    int unpack_alignment = 4;
    int pack_alignment = 4;
    // Pixel store state that moves the first pixel of an image away from the
    // pointer: a row length longer than the image, skipped rows, pixels and
    // images. ES 3.0 engines set them for atlas sub-uploads.
    int unpack_row_length = 0;
    int unpack_skip_rows = 0;
    int unpack_skip_pixels = 0;
    int unpack_image_height = 0;
    int unpack_skip_images = 0;
    int pack_row_length = 0;
    int pack_skip_rows = 0;
    int pack_skip_pixels = 0;
    // A glMapBufferRange range held in a staging cell in the HLE data area,
    // because the driver's own mapping is host memory the guest cannot address.
    // Ranges are identified by the buffer object: an engine maps several buffers
    // through one target (GL_COPY_WRITE_BUFFER is its staging path) and every one
    // of them stays mapped until its own unmap, so a table keyed by target throws
    // away the range that was mapped first.
    struct Mapping {
        std::uint32_t cell = 0;
        std::intptr_t offset = 0;
        std::intptr_t length = 0;
        std::uint32_t buffer = 0;
        unsigned int target = 0;
        bool writable = false;
    };
    // Keyed by buffer object.
    std::unordered_map<unsigned int, Mapping> mappings;
    // Every binding the guest establishes, by target. The four named fields above
    // are the hot paths; a target the container has no field for still has to be
    // known, because a mapped range is looked up by the buffer bound to its
    // target when the guest unmaps it.
    std::unordered_map<unsigned int, unsigned int> bindings;
    // Client-side vertex arrays: the driver dereferences these only at the draw
    // that reads them, so the range has to be checked there, not at the call
    // that sets the pointer.
    struct ClientArray {
        bool enabled = false;
        std::uint32_t pointer = 0;
        unsigned int buffer = 0;  // non-zero: the pointer is an offset and the data is safe
        int size = 4;
        unsigned int type = 0x1406;  // GL_FLOAT
        int stride = 0;
    };
    std::array<ClientArray, 16> attribs{};
    ClientArray legacy_vertex;
    ClientArray legacy_color;
    ClientArray legacy_normal;
    ClientArray legacy_texcoord;
    // Largest index value an element buffer holds, per index width: a draw with
    // client arrays has no other way to know how far the arrays are read.
    struct IndexMax {
        std::uint32_t bytes = 0;
        std::uint32_t shorts = 0;
        std::uint32_t words = 0;
    };
    std::unordered_map<unsigned int, IndexMax> index_max;
};

// Pixel store state, copied out under the state lock so a call site can compute
// a buffer layout without holding it.
struct PixelStore {
    int alignment = 4;
    int row_length = 0;
    int skip_rows = 0;
    int skip_pixels = 0;
    int image_height = 0;
    int skip_images = 0;
};

PixelStore UnpackStoreLocked(const GlesState& state) {
    PixelStore store;
    store.alignment = state.unpack_alignment;
    store.row_length = state.unpack_row_length;
    store.skip_rows = state.unpack_skip_rows;
    store.skip_pixels = state.unpack_skip_pixels;
    store.image_height = state.unpack_image_height;
    store.skip_images = state.unpack_skip_images;
    return store;
}

PixelStore PackStoreLocked(const GlesState& state) {
    PixelStore store;
    store.alignment = state.pack_alignment;
    store.row_length = state.pack_row_length;
    store.skip_rows = state.pack_skip_rows;
    store.skip_pixels = state.pack_skip_pixels;
    return store;
}

// Largest upload the container copies through the data area. Staging is the
// fallback for a range that is not entirely mapped, not the normal path.
constexpr std::size_t kMaxStagedBytes = 32u << 20;

// The driver reads guest pixels through the fastmem view, so a range that is not
// entirely mapped must not reach it: the read would run past the mapping and
// take the host process down. The image itself can still be valid -- an engine
// uploads a sub-rectangle of an atlas, or the tail of a buffer it allocated
// flat -- so the mapped bytes are copied into a staging block and the rest is
// zeroed instead of dropping the upload.
class StagedRead {
public:
    StagedRead(HleRegistry& registry, HleCall& call, std::uint32_t address, std::size_t length)
        : registry_(registry) {
        if (length == 0) {
            ok_ = true;
            return;
        }
        // A null pixels argument is legal on the upload paths: allocate the
        // storage without data. Staging a zero-filled block for it would copy
        // megabytes for every mip level of a texture that has none.
        if (address == 0) {
            ok_ = true;
            return;
        }
        if (static_cast<std::uint64_t>(address) + length > 0xffffffffull) {
            return;
        }
        if (std::byte* mapped = call.Mapped(address, length)) {
            bytes_ = mapped;
            ok_ = true;
            return;
        }
        if (length > kMaxStagedBytes) {
            return;
        }
        cell_ = registry_.AllocateData(static_cast<std::uint32_t>(length));
        if (cell_ == 0) {
            return;
        }
        auto* staging = static_cast<std::byte*>(registry_.Arena().Ptr(cell_));
        if (staging == nullptr) {
            registry_.ReleaseData(cell_);
            cell_ = 0;
            return;
        }
        std::memset(staging, 0, length);
        // Copy what is mapped one page at a time: the range also straddles the
        // end of a mapping, and the pages before it still hold the pixels.
        for (std::size_t offset = 0; offset < length;) {
            const std::size_t to_page = 4096 - ((address + offset) & 4095u);
            const std::size_t chunk = to_page < length - offset ? to_page : length - offset;
            if (std::byte* source =
                    call.Mapped(static_cast<std::uint32_t>(address + offset), chunk)) {
                std::memcpy(staging + offset, source, chunk);
            }
            offset += chunk;
        }
        bytes_ = staging;
        ok_ = true;
    }

    ~StagedRead() {
        if (cell_ != 0) {
            registry_.ReleaseData(cell_);
        }
    }

    StagedRead(const StagedRead&) = delete;
    StagedRead& operator=(const StagedRead&) = delete;

    std::byte* bytes() const { return bytes_; }
    bool ok() const { return ok_; }

private:
    HleRegistry& registry_;
    std::uint32_t cell_ = 0;
    std::byte* bytes_ = nullptr;
    bool ok_ = false;
};

// Largest index value in an element buffer, for every index width. A draw that
// reads a client-side vertex array with an element buffer bound has no other way
// to know how far that array is read: the driver resolves the indices itself.
GlesState::IndexMax ScanIndexMax(const std::byte* data, std::size_t length) {
    GlesState::IndexMax out;
    if (data == nullptr) {
        return out;
    }
    for (std::size_t i = 0; i < length; ++i) {
        const auto value = static_cast<std::uint32_t>(data[i]);
        if (value > out.bytes) {
            out.bytes = value;
        }
    }
    for (std::size_t i = 0; i + 1 < length; i += 2) {
        std::uint16_t value = 0;
        std::memcpy(&value, data + i, sizeof(value));
        if (value > out.shorts) {
            out.shorts = value;
        }
    }
    for (std::size_t i = 0; i + 3 < length; i += 4) {
        std::uint32_t value = 0;
        std::memcpy(&value, data + i, sizeof(value));
        if (value > out.words) {
            out.words = value;
        }
    }
    return out;
}

// Keeps the element-buffer index bounds current. A draw that reads client-side
// vertex arrays needs the largest index the buffer can hold; reading it back from
// the driver at draw time would stall the pipeline, so the bound is taken when
// the buffer is uploaded.
void UpdateIndexMax(const std::shared_ptr<GlesState>& state, unsigned int target,
                    std::size_t length, bool replace, const std::byte* data) {
    if (target != 0x8893) {  // GL_ELEMENT_ARRAY_BUFFER
        return;
    }
    std::uint32_t name = 0;
    {
        const std::lock_guard lock(state->mutex);
        name = state->element_buffer;
    }
    if (name == 0) {
        return;
    }
    const GlesState::IndexMax scan = ScanIndexMax(data, length);
    const std::lock_guard lock(state->mutex);
    GlesState::IndexMax& cell = state->index_max[name];
    if (replace) {
        cell = scan;
        return;
    }
    // A partial update can only raise the bound: an over-estimate keeps the check
    // valid, an under-estimate would let a bad index through unread.
    if (scan.bytes > cell.bytes) cell.bytes = scan.bytes;
    if (scan.shorts > cell.shorts) cell.shorts = scan.shorts;
    if (scan.words > cell.words) cell.words = scan.words;
}

// Bytes one vertex array element occupies in guest memory.
std::size_t AttributeElementSize(unsigned int type) {
    switch (type) {
        case 0x1400:  // GL_BYTE
        case 0x1401:  // GL_UNSIGNED_BYTE
            return 1;
        case 0x1402:  // GL_SHORT
        case 0x1403:  // GL_UNSIGNED_SHORT
        case 0x140b:  // GL_HALF_FLOAT
            return 2;
        case 0x1404:  // GL_INT
        case 0x1405:  // GL_UNSIGNED_INT
        case 0x1406:  // GL_FLOAT
        case 0x8368:  // GL_UNSIGNED_INT_2_10_10_10_REV
        case 0x8d9f:  // GL_INT_2_10_10_10_REV
            return 4;
        default:
            return 0;
    }
}

bool HasClientArrays(const std::shared_ptr<GlesState>& state) {
    const std::lock_guard lock(state->mutex);
    for (const GlesState::ClientArray& array : state->attribs) {
        if (array.enabled && array.buffer == 0) {
            return true;
        }
    }
    return state->legacy_vertex.enabled || state->legacy_color.enabled ||
           state->legacy_normal.enabled || state->legacy_texcoord.enabled;
}

// The driver dereferences a client-side array only when the draw runs, so the
// check belongs here, with the largest vertex index the draw can reach. Only
// arrays without a buffer object need it: with a buffer bound the driver reads
// its own memory.
bool ValidateDrawArrays(HleCall& call, const std::shared_ptr<GlesState>& state,
                        const char* name, std::uint64_t max_index, bool bounded) {
    if (!bounded) {
        return true;
    }
    const auto check = [&](const GlesState::ClientArray& array) {
        if (!array.enabled || array.buffer != 0 || array.size <= 0) {
            return true;
        }
        const std::size_t element = AttributeElementSize(array.type);
        if (element == 0) {
            return true;
        }
        const std::uint64_t stride =
            array.stride > 0 ? static_cast<std::uint64_t>(array.stride)
                             : static_cast<std::uint64_t>(array.size) * element;
        const std::uint64_t end = static_cast<std::uint64_t>(array.pointer) +
                                  max_index * stride +
                                  static_cast<std::uint64_t>(array.size) * element;
        if (end > 0xffffffffull) {
            RejectBuffer(name, array.pointer, 1);
            return false;
        }
        const std::size_t length = static_cast<std::size_t>(end - array.pointer);
        if (call.Mapped(array.pointer, length) == nullptr) {
            RejectBuffer(name, array.pointer, length);
            return false;
        }
        return true;
    };
    const std::lock_guard lock(state->mutex);
    for (const GlesState::ClientArray& array : state->attribs) {
        if (!check(array)) {
            return false;
        }
    }
    return check(state->legacy_vertex) && check(state->legacy_color) &&
           check(state->legacy_normal) && check(state->legacy_texcoord);
}

// Largest index in a guest-side index array, or false when the array cannot be
// read. Only the legacy path (no element buffer) reaches this.
bool ScanClientIndices(HleCall& call, std::uint32_t address, int count, unsigned int type,
                       std::uint64_t* max_index) {
    const std::size_t element = IndexSize(type);
    if (element == 0 || count <= 0) {
        return false;
    }
    const std::size_t length = static_cast<std::size_t>(count) * element;
    const std::byte* indices = call.Mapped(address, length);
    if (indices == nullptr) {
        return false;
    }
    std::uint64_t largest = 0;
    for (int i = 0; i < count; ++i) {
        std::uint32_t value = 0;
        if (element == 1) {
            value = static_cast<unsigned char>(indices[i]);
        } else if (element == 2) {
            std::uint16_t half = 0;
            std::memcpy(&half, indices + static_cast<std::size_t>(i) * 2, sizeof(half));
            value = half;
        } else {
            std::memcpy(&value, indices + static_cast<std::size_t>(i) * 4, sizeof(value));
        }
        if (value > largest) {
            largest = value;
        }
    }
    *max_index = largest;
    return true;
}

// The buffer the container believes is bound to target, in the terms the guest
// uses. Never called while the state mutex is already held elsewhere in a way
// that matters: every caller takes the lock around it.
std::uint32_t TrackedBinding(const GlesState& state, unsigned int target) {
    const auto found = state.bindings.find(target);
    if (found != state.bindings.end()) {
        return found->second;
    }
    switch (target) {
        case 0x8892:  // GL_ARRAY_BUFFER
            return state.array_buffer;
        case 0x8893:  // GL_ELEMENT_ARRAY_BUFFER
            return state.element_buffer;
        case 0x88ef:  // GL_PIXEL_UNPACK_BUFFER
            return state.unpack_buffer;
        case 0x88eb:  // GL_PIXEL_PACK_BUFFER
            return state.pack_buffer;
        default:
            return 0;
    }
}

// The mapping the guest means by glUnmapBuffer(target) or
// glFlushMappedBufferRange(target): the range on the buffer bound to that target
// and, when the engine unmapped without rebinding, the only range still open on
// the target. The caller holds the state mutex.
bool FindMapping(GlesState& state, unsigned int target, GlesState::Mapping* mapping,
                 unsigned int* owner) {
    const std::uint32_t buffer = TrackedBinding(state, target);
    const auto found = state.mappings.find(buffer);
    if (found != state.mappings.end()) {
        *mapping = found->second;
        *owner = found->first;
        return true;
    }
    for (const auto& entry : state.mappings) {
        if (entry.second.target == target) {
            *mapping = entry.second;
            *owner = entry.first;
            return true;
        }
    }
    return false;
}

// Reads a range of the buffer bound to target. glGetBufferSubData is not on
// every driver (this one does not export it), and mapping the same range is the
// portable way to get the bytes out; without it a guest that maps a range for
// reading gets zeros back and works on data it never wrote.
bool ReadBackRange(unsigned int target, std::intptr_t offset, std::intptr_t length, void* out) {
    const auto read_back =
        Gl<void (*)(unsigned int, std::intptr_t, std::intptr_t, void*)>("glGetBufferSubData");
    if (read_back != nullptr) {
        read_back(target, offset, length, out);
    } else {
        const auto map = Gl<void* (*)(unsigned int, std::intptr_t, std::intptr_t, unsigned int)>(
            "glMapBufferRange");
        const auto unmap = Gl<unsigned char (*)(unsigned int)>("glUnmapBuffer");
        if (map == nullptr || unmap == nullptr) {
            return false;
        }
        const auto* source =
            static_cast<const unsigned char*>(map(target, offset, length, 0x0001u /* READ */));
        if (source == nullptr) {
            return false;
        }
        std::memcpy(out, source, static_cast<std::size_t>(length));
        unmap(target);
    }
    // The read is the container's own call: an error it raises must not be left
    // for the guest to read as the answer to its next query.
    const auto take_error = Gl<unsigned int (*)(void)>("glGetError");
    if (take_error != nullptr) {
        take_error();
    }
    return true;
}

// The staging cell of a mapped range reaches the driver through glBufferSubData,
// which writes into the buffer bound to the target at that moment. The engine may
// have rebound the target since it mapped, so the binding is set to the mapped
// buffer for the duration of the upload and put back afterwards.
void UploadMappedRange(HleRegistry& registry, GlesState& state, unsigned int target,
                       const GlesState::Mapping& mapping, std::intptr_t offset,
                       std::intptr_t length) {
    if (!mapping.writable || mapping.cell == 0 || length <= 0 || offset < 0) {
        return;
    }
    if (offset >= mapping.length) {
        return;
    }
    if (offset + length > mapping.length) {
        length = mapping.length - offset;
    }
    const auto upload =
        Gl<void (*)(unsigned int, std::intptr_t, std::intptr_t, const void*)>("glBufferSubData");
    if (upload == nullptr) {
        return;
    }
    const auto* bytes = static_cast<const std::byte*>(registry.Arena().Ptr(mapping.cell));
    if (bytes == nullptr) {
        return;
    }
    std::uint32_t current = 0;
    {
        const std::lock_guard lock(state.mutex);
        current = TrackedBinding(state, target);
    }
    const auto bind = Gl<void (*)(unsigned int, unsigned int)>("glBindBuffer");
    const bool rebind = bind != nullptr && mapping.buffer != 0 && current != mapping.buffer;
    if (rebind) {
        bind(target, mapping.buffer);
    }
    upload(target, mapping.offset + offset, length, bytes + offset);
    if (rebind) {
        bind(target, current);
    }
}

// The pixels argument of an upload call in the form the driver takes it. A bound
// GL_PIXEL_UNPACK_BUFFER makes the argument an offset into that buffer, so no
// guest memory is involved; otherwise the guest range is checked and, when it is
// not entirely mapped, served from a staging copy instead of being dropped.
class UploadPixels {
public:
    UploadPixels(HleRegistry& registry, HleCall& call,
                 const std::shared_ptr<GlesState>& state, std::uint32_t address,
                 std::size_t length, const char* name) {
        std::uint32_t unpack_buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            unpack_buffer = state->unpack_buffer;
        }
        if (unpack_buffer != 0) {
            bytes_ = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
            ok_ = true;
            return;
        }
        staged_ = std::make_unique<StagedRead>(registry, call, address, length);
        if (!staged_->ok()) {
            RejectBuffer(name, address, length);
            return;
        }
        bytes_ = staged_->bytes();
        ok_ = true;
    }

    void* bytes() const { return bytes_; }
    bool ok() const { return ok_; }

private:
    std::unique_ptr<StagedRead> staged_;
    void* bytes_ = nullptr;
    bool ok_ = false;
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

// Extensions are filtered down to the ones whose advertised surface is fully
// supported. GL_OES_framebuffer_object is safe because its entry points are
// forwarded through the GLES 1.x host library below. The list must not be
// empty, though -- engines probe it to pick internal formats for their
// off-screen targets. Without packed_depth_stencil Unity builds a depth-only
// renderbuffer, attaches it at the stencil point and keeps drawing into an
// incomplete framebuffer: every draw then fails with
// GL_INVALID_FRAMEBUFFER_OPERATION and the window presents pure black while all
// the call counters look healthy.
constexpr const char* kSafeExtensions[] = {
    "GL_OES_framebuffer_object",
    "GL_OES_packed_depth_stencil",
    "GL_OES_depth24",
    "GL_OES_stencil8",
    "GL_OES_stencil4",
    "GL_OES_rgb8_rgba8",
    "GL_OES_fbo_render_mipmap",
    "GL_OES_standard_derivatives",
    "GL_EXT_texture_format_BGRA8888",
};

bool ListsExtension(const std::string& driver, const char* name) {
    const std::size_t at = driver.find(name);
    if (at == std::string::npos) {
        return false;
    }
    // A prefix of a longer name is not the same extension.
    const std::size_t end = at + std::strlen(name);
    if (end < driver.size() && driver[end] != ' ') {
        return false;
    }
    return at == 0 || driver[at - 1] == ' ';
}

std::string FilterExtensions(const std::string& driver) {
    std::string out;
    for (const char* name : kSafeExtensions) {
        if (!ListsExtension(driver, name)) {
            continue;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += name;
    }
    return out;
}

// The driver's extension list, read the way the current context allows. ES 3.0
// removed the single GL_EXTENSIONS string: asking for it there raises
// GL_INVALID_ENUM, and the error the container left behind is then read by the
// guest as the failure of whatever it checked next.
std::vector<std::string> DriverExtensions() {
    std::vector<std::string> out;
    const int version = CurrentContextVersion();
    const auto get_string = Gl<const unsigned char* (*)(unsigned int)>("glGetString");
    const auto get_string_i =
        Gl<const unsigned char* (*)(unsigned int, unsigned int)>("glGetStringi");
    const auto get_integer = Gl<void (*)(unsigned int, int*)>("glGetIntegerv");
    const bool indexed = version >= 3 || get_string == nullptr;
    if (indexed) {
        if (get_string_i != nullptr && get_integer != nullptr) {
            int count = 0;
            get_integer(0x821d, &count);  // GL_NUM_EXTENSIONS
            if (count > 0 && count < 4096) {
                for (int i = 0; i < count; ++i) {
                    const unsigned char* name = get_string_i(0x1f03, i);
                    if (name != nullptr) {
                        out.emplace_back(reinterpret_cast<const char*>(name));
                    }
                }
            }
        }
    }
    if (!indexed || (out.empty() && version == 0)) {
        const unsigned char* all = get_string != nullptr ? get_string(0x1f03) : nullptr;
        if (all != nullptr) {
            const char* text = reinterpret_cast<const char*>(all);
            const std::size_t length = std::strlen(text);
            std::size_t at = 0;
            while (at < length) {
                std::size_t end = at;
                while (end < length && text[end] != ' ') {
                    ++end;
                }
                if (end > at) {
                    out.emplace_back(text + at, end - at);
                }
                at = end + 1;
            }
        }
    }
    return out;
}

// What the container tells the guest it supports. On an ES 2.0 context the
// driver lists extensions in one string; on an ES 3.0 context the guest
// enumerates them through GL_NUM_EXTENSIONS and glGetStringi, and that is the
// path Unity 5.3 takes. Answering zero there made it believe the GPU had no
// packed_depth_stencil -- and no other extension at all -- so it fell back to a
// framebuffer the driver rejects every draw against: black window, healthy
// counters.
std::vector<std::string> AdvertisedExtensions() {
    const std::vector<std::string> driver = DriverExtensions();
    std::vector<std::string> out;
    for (const char* name : kSafeExtensions) {
        for (const std::string& have : driver) {
            if (have == name) {
                out.emplace_back(name);
                break;
            }
        }
    }
    return out;
}

// Mechanical handlers, generated by tools/gen_gles.py from the NDK's own GLES
// headers. Included inside the anonymous namespace so Gl<>/FloatArg stay
// private to this file, and so the generated function gets internal linkage.
#include "generated/guest_gles_auto.inc"

}  // namespace

void RegisterGlesFunctions(HleRegistry& registry) {
    auto state = std::make_shared<GlesState>();

    // The mechanical entry points first. HleRegistry::Add replaces an existing
    // name, so the hand-written handlers below -- the ones whose pointers, 64-bit
    // values or return buffers need marshalling -- always win.
    RegisterGlesGeneratedFunctions(registry);

    // ---- strings and errors ------------------------------------------------

    registry.Add("glGetString", [&registry, state](HleCall& call) {
        // The query has to come from the library that matches the current
        // context: on a GLES 1.x context the ES2 wrapper answers for a
        // different API surface, or not at all.
        static const auto es2 = Gl<const unsigned char* (*)(unsigned int)>("glGetString");
        static const auto es1 = Gl1<const unsigned char* (*)(unsigned int)>("glGetString");
        const int context_version = CurrentContextVersion();
        const auto function = (context_version == 1 && es1 != nullptr) ? es1 : es2;
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
		// stays filtered to the subset this container implements, including
        // the OES framebuffer entry points registered below.
        if (name == 0x1f02) {  // GL_VERSION
            // Loaders parse this string to decide which entry points to fetch,
            // so it has to describe the surface this container serves and match
            // GL_MAJOR_VERSION, GL_MINOR_VERSION and the shading language string
            // exactly. The ES 2.0 context path also answers 3.0: engines built
            // against an ES 3 driver call the 3.0 surface unconditionally
            // (Minecraft's screenshot path does exactly that with glReadBuffer),
            // and telling them 2.0 leaves those pointers null.
            //
            // A GLES 1.x context is the exception: it has no shader path, and an
            // engine told it has one takes the ES2 route and dereferences what
            // the context never provided.
            value = context_version == 1 ? "OpenGL ES-CM 1.1 (Armbat)"
                                         : "OpenGL ES 3.0 (Armbat)";
        } else if (name == 0x8b8c) {  // GL_SHADING_LANGUAGE_VERSION
            // Not a valid query on a GLES 1.x context: the driver answers
            // with an error and a null pointer, and the guest has to see the
            // same answer instead of a version it cannot use.
            if (context_version == 1) {
                call.Return(0);
                return;
            }
            // Must agree with GL_VERSION above, or an engine that reads both
            // concludes the shading language cannot express the API it was told
            // about and falls back to a path the container does not serve.
            value = "OpenGL ES GLSL ES 3.00";
        } else if (name == 0x1f03) {  // GL_EXTENSIONS
            value = FilterExtensions(value);
        }
        const std::lock_guard lock(state->mutex);
        const std::uint32_t cell = StringCellAddress(registry, state->strings[name], value);
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
        std::string value;
        if (name == 0x1f03) {  // GL_EXTENSIONS
            // The enumerated list is the same filtered list the single-string
            // query answers with and that GL_NUM_EXTENSIONS counts. Handing the
            // guest the driver's own list would advertise entry points and
            // formats this container does not serve.
            const std::vector<std::string> advertised = AdvertisedExtensions();
            if (index >= advertised.size()) {
                call.Return(0);
                return;
            }
            value = advertised[index];
        } else {
            const unsigned char* text = function(name, index);
            if (text == nullptr) {
                call.Return(0);
                return;
            }
            value = reinterpret_cast<const char*>(text);
        }
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
        const std::size_t length = ValueCountQuery(call.Arg(0)) * sizeof(int);
        int* out = length != 0
                       ? reinterpret_cast<int*>(call.Mapped(call.Arg(1), length))
                       : nullptr;
        if (length != 0 && out == nullptr) {
            RejectBuffer("glGetIntegerv", call.Arg(1), length);
            return;
        }
        // These three answer for the surface the container serves, the way a
        // driver does, and not for the host context the driver happens to run.
        // They have to agree with GL_VERSION and GL_SHADING_LANGUAGE_VERSION
        // above: an engine reads all of them into the caps it validates itself
        // against, and a mismatch sends Unity 5.3 down a path its context does
        // not support (it printed "GPU not supported" and exited on the reverse
        // mismatch).
        const int context_version = CurrentContextVersion();
        switch (call.Arg(0)) {
            case 0x821b:  // GL_MAJOR_VERSION
                *out = context_version == 1 ? 1 : 3;
                return;
            case 0x821c:  // GL_MINOR_VERSION
                *out = 0;
                return;
            case 0x821d:  // GL_NUM_EXTENSIONS
                // The count has to match what glGetStringi enumerates and what
                // the single-string query returns, or an engine that probes the
                // enumerated path sees no extensions at all.
                *out = static_cast<int>(AdvertisedExtensions().size());
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
        const std::size_t length = ValueCountQuery(call.Arg(0)) * sizeof(float);
        float* out = length != 0
                         ? reinterpret_cast<float*>(call.Mapped(call.Arg(1), length))
                         : nullptr;
        if (length != 0 && out == nullptr) {
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
        const std::size_t length = ValueCountQuery(call.Arg(0));
        unsigned char* out =
            length != 0
                ? reinterpret_cast<unsigned char*>(call.Mapped(call.Arg(1), length))
                : nullptr;
        if (length != 0 && out == nullptr) {
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
        const int value = static_cast<int>(call.Arg(1));
        const std::lock_guard lock(state->mutex);
        switch (call.Arg(0)) {
            case 0x0cf5:  // GL_UNPACK_ALIGNMENT
                state->unpack_alignment = value;
                break;
            case 0x0cf2:  // GL_UNPACK_ROW_LENGTH
                state->unpack_row_length = value;
                break;
            case 0x0cf3:  // GL_UNPACK_SKIP_ROWS
                state->unpack_skip_rows = value;
                break;
            case 0x0cf4:  // GL_UNPACK_SKIP_PIXELS
                state->unpack_skip_pixels = value;
                break;
            case 0x806e:  // GL_UNPACK_IMAGE_HEIGHT
                state->unpack_image_height = value;
                break;
            case 0x806d:  // GL_UNPACK_SKIP_IMAGES
                state->unpack_skip_images = value;
                break;
            case 0x0d05:  // GL_PACK_ALIGNMENT
                state->pack_alignment = value;
                break;
            case 0x0d02:  // GL_PACK_ROW_LENGTH
                state->pack_row_length = value;
                break;
            case 0x0d03:  // GL_PACK_SKIP_ROWS
                state->pack_skip_rows = value;
                break;
            case 0x0d04:  // GL_PACK_SKIP_PIXELS
                state->pack_skip_pixels = value;
                break;
            default:
                break;
        }
    });
    // ES 2.0 has a single read buffer, so there is nothing to select there and
    // the call is a no-op. On an ES 3.0 context it is real state: it decides
    // which attachment glReadPixels and glBlitFramebuffer read from, and an
    // engine that selects GL_COLOR_ATTACHMENT0 for a readback gets the back
    // buffer instead when the selection is dropped.
    registry.Add("glReadBuffer", [](HleCall& call) {
        if (CurrentContextVersion() < 3) {
            return;
        }
        static const auto function = Gl<void (*)(unsigned int)>("glReadBuffer");
        if (function != nullptr) {
            function(call.Arg(0));
        }
    });

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
            state->bindings[target] = buffer;
            if (target == 0x8892) {  // GL_ARRAY_BUFFER
                state->array_buffer = buffer;
            } else if (target == 0x8893) {  // GL_ELEMENT_ARRAY_BUFFER
                state->element_buffer = buffer;
            } else if (target == 0x88ef) {  // GL_PIXEL_UNPACK_BUFFER
                state->unpack_buffer = buffer;
            } else if (target == 0x88eb) {  // GL_PIXEL_PACK_BUFFER
                state->pack_buffer = buffer;
            }
        }
        if (function != nullptr) function(target, buffer);
    });
    registry.Add("glBufferData", [&registry, state](HleCall& call) {
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
        // Null is a legal size-only allocation and must stay null instead of
        // being turned into a zero-filled staging block.
        void* data = nullptr;
        if (call.Arg(2) != 0) {
            StagedRead staged(registry, call, call.Arg(2), static_cast<std::size_t>(size));
            if (!staged.ok()) {
                RejectBuffer("glBufferData", call.Arg(2), static_cast<std::size_t>(size));
                return;
            }
            data = staged.bytes();
            function(call.Arg(0), size, data, call.Arg(3));
            UpdateIndexMax(state, call.Arg(0), static_cast<std::size_t>(size), true,
                           static_cast<const std::byte*>(data));
            return;
        }
        function(call.Arg(0), size, nullptr, call.Arg(3));
        UpdateIndexMax(state, call.Arg(0), static_cast<std::size_t>(size), true, nullptr);
    });
    registry.Add("glBufferSubData", [&registry, state](HleCall& call) {
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
        StagedRead staged(registry, call, call.Arg(3), static_cast<std::size_t>(size));
        if (!staged.ok()) {
            RejectBuffer("glBufferSubData", call.Arg(3), static_cast<std::size_t>(size));
            return;
        }
        function(call.Arg(0), offset, size, staged.bytes());
        UpdateIndexMax(state, call.Arg(0), static_cast<std::size_t>(size), false,
                       static_cast<const std::byte*>(staged.bytes()));
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
    // glMapBufferRange hands the guest a pointer into the driver's buffer. That
    // memory is host memory the guest cannot address, so the range is mapped
    // through a staging cell instead: filled from the driver when the guest asked
    // for the old contents, uploaded back on unmap. Answering null here lost
    // everything an engine wrote through the pointer -- dynamic geometry never
    // reached the GPU while every call counter looked healthy.
    registry.Add("glMapBufferRange", [&registry, state](HleCall& call) {
        const unsigned int target = call.Arg(0);
        const std::int64_t offset = static_cast<std::int32_t>(call.Arg(1));
        const std::int64_t length = static_cast<std::int32_t>(call.Arg(2));
        const unsigned int access = call.Arg(3);
        if (offset < 0 || length <= 0 ||
            static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(length) >
                0x7fffffffull ||
            length > static_cast<std::int64_t>(kMaxStagedBytes)) {
            call.Return(0);
            return;
        }
        const std::uint32_t cell = registry.AllocateData(static_cast<std::uint32_t>(length));
        if (cell == 0) {
            call.Return(0);
            return;
        }
        auto* staging = static_cast<std::byte*>(registry.Arena().Ptr(cell));
        if (staging == nullptr) {
            registry.ReleaseData(cell);
            call.Return(0);
            return;
        }
        std::memset(staging, 0, static_cast<std::size_t>(length));
        if ((access & 0x0001u) != 0) {  // GL_MAP_READ_BIT
            ReadBackRange(target, static_cast<std::intptr_t>(offset),
                          static_cast<std::intptr_t>(length), staging);
        }
        std::uint32_t replaced = 0;
        {
            const std::lock_guard lock(state->mutex);
            const std::uint32_t owner = TrackedBinding(*state, target);
            GlesState::Mapping& mapping = state->mappings[owner];
            replaced = mapping.cell;
            mapping.cell = cell;
            mapping.offset = static_cast<std::intptr_t>(offset);
            mapping.length = static_cast<std::intptr_t>(length);
            mapping.buffer = owner;
            mapping.target = target;
            mapping.writable = (access & 0x0002u) != 0;  // GL_MAP_WRITE_BIT
        }
        if (replaced != 0) {
            registry.ReleaseData(replaced);
        }
        call.Return(cell);
    });
    registry.Add("glUnmapBuffer", [&registry, state](HleCall& call) {
        const unsigned int target = call.Arg(0);
        GlesState::Mapping mapping;
        unsigned int owner = 0;
        bool mapped = false;
        {
            const std::lock_guard lock(state->mutex);
            if (FindMapping(*state, target, &mapping, &owner)) {
                state->mappings.erase(owner);
                mapped = true;
            }
        }
        if (!mapped || mapping.cell == 0) {
            call.Return(0);  // the driver answers GL_FALSE for an unmapped target
            return;
        }
        if (mapping.writable) {
            UploadMappedRange(registry, *state, target, mapping, 0, mapping.length);
        }
        registry.ReleaseData(mapping.cell);
        call.Return(1);  // GL_TRUE
    });
    registry.Add("glFlushMappedBufferRange", [&registry, state](HleCall& call) {
        const unsigned int target = call.Arg(0);
        const std::intptr_t offset = static_cast<std::int32_t>(call.Arg(1));
        const std::intptr_t length = static_cast<std::int32_t>(call.Arg(2));
        if (offset < 0 || length <= 0) {
            return;
        }
        GlesState::Mapping mapping;
        unsigned int owner = 0;
        bool mapped = false;
        {
            const std::lock_guard lock(state->mutex);
            mapped = FindMapping(*state, target, &mapping, &owner);
        }
        if (!mapped) {
            return;
        }
        // A flushed range has to reach the driver now: an engine that streams
        // geometry maps once, writes, flushes and draws every frame, and only
        // unmaps when the buffer is recycled. Waiting for the unmap left the
        // driver reading the buffer's previous contents, so every dynamic draw
        // came out of zeroed vertices with nothing in the log to show it.
        UploadMappedRange(registry, *state, target, mapping, offset, length);
    });

    // ---- GL_OES_framebuffer_object (GLES 1.x extension) -------------------
    // These names must go through libGLESv1_CM.so: the guest can be running an
    // ES 1.x context, where the OES extension is the API and the GLES 2.0 core
    // entry points are not a valid substitute. Keep the same guest-buffer
    // checks as the core handlers above before passing pointers to the driver.
    registry.Add("glGenFramebuffersOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, unsigned int*)>("glGenFramebuffersOES");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer out = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glGenFramebuffersOES", call.Arg(1), out, length, count)) {
            return;
        }
        function(count, static_cast<unsigned int*>(out.bytes));
    });
    registry.Add("glDeleteFramebuffersOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, const unsigned int*)>("glDeleteFramebuffersOES");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer names = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glDeleteFramebuffersOES", call.Arg(1), names, length, count)) {
            return;
        }
        function(count, static_cast<const unsigned int*>(names.bytes));
    });
    registry.Add("glBindFramebufferOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int)>("glBindFramebufferOES");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
	});
    registry.Add("glIsFramebufferOES", [](HleCall& call) {
        static const auto function = Gl1<unsigned char (*)(unsigned int)>("glIsFramebufferOES");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glCheckFramebufferStatusOES", [](HleCall& call) {
        static const auto function =
            Gl1<unsigned int (*)(unsigned int)>("glCheckFramebufferStatusOES");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glFramebufferTexture2DOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, unsigned int, unsigned int, int)>(
                "glFramebufferTexture2DOES");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), call.Arg(2), call.Arg(3),
                     static_cast<int>(call.Arg(4)));
        }
    });
    registry.Add("glFramebufferRenderbufferOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, unsigned int, unsigned int)>(
                "glFramebufferRenderbufferOES");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), call.Arg(2), call.Arg(3));
        }
    });
    registry.Add("glGenRenderbuffersOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, unsigned int*)>("glGenRenderbuffersOES");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer out = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glGenRenderbuffersOES", call.Arg(1), out, length, count)) {
            return;
        }
        function(count, static_cast<unsigned int*>(out.bytes));
    });
    registry.Add("glDeleteRenderbuffersOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(int, const unsigned int*)>("glDeleteRenderbuffersOES");
        if (function == nullptr) {
            return;
        }
        const int count = static_cast<int>(call.Arg(0));
        if (count < 0 || count > 65536) {
            return;
        }
        const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
        const Buffer names = GuestBuffer(call, call.Arg(1), length);
        if (!RequireArray("glDeleteRenderbuffersOES", call.Arg(1), names, length, count)) {
            return;
        }
        function(count, static_cast<const unsigned int*>(names.bytes));
    });
    registry.Add("glBindRenderbufferOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int)>("glBindRenderbufferOES");
        if (function != nullptr) function(call.Arg(0), call.Arg(1));
    });
    registry.Add("glIsRenderbufferOES", [](HleCall& call) {
        static const auto function =
            Gl1<unsigned char (*)(unsigned int)>("glIsRenderbufferOES");
        call.Return(function != nullptr ? function(call.Arg(0)) : 0);
    });
    registry.Add("glRenderbufferStorageOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, int, int)>("glRenderbufferStorageOES");
        if (function != nullptr) {
            function(call.Arg(0), call.Arg(1), static_cast<int>(call.Arg(2)),
                     static_cast<int>(call.Arg(3)));
        }
    });
    registry.Add("glGetFramebufferAttachmentParameterivOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, unsigned int, int*)>(
                "glGetFramebufferAttachmentParameterivOES");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(3), sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetFramebufferAttachmentParameterivOES", call.Arg(3), sizeof(int));
            return;
        }
        function(call.Arg(0), call.Arg(1), call.Arg(2), out);
    });
    registry.Add("glGetRenderbufferParameterivOES", [](HleCall& call) {
        static const auto function =
            Gl1<void (*)(unsigned int, unsigned int, int*)>("glGetRenderbufferParameterivOES");
        if (function == nullptr) {
            return;
        }
        int* out = reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)));
        if (out == nullptr) {
            RejectBuffer("glGetRenderbufferParameterivOES", call.Arg(2), sizeof(int));
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
        if (function == nullptr) {
            return;
        }
        function(call.Arg(0));
        // The verdict is taken here, not when the engine asks: a failed shader
        // is often deleted before its status is queried, and then the only
        // trace left is a subsystem that silently turns itself off.
        static const auto get_shaderiv =
            Gl<void (*)(unsigned int, unsigned int, int*)>("glGetShaderiv");
        if (get_shaderiv == nullptr) {
            return;
        }
        int status = 1;
        get_shaderiv(call.Arg(0), 0x8b81 /* GL_COMPILE_STATUS */, &status);
        if (status != 0) {
            return;
        }
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) >= 8) {
            return;
        }
        int type = 0;
        get_shaderiv(call.Arg(0), 0x8b7f /* GL_SHADER_TYPE */, &type);
        char text[1024] = {};
        int written = 0;
        static const auto info_log =
            Gl<void (*)(unsigned int, int, int*, char*)>("glGetShaderInfoLog");
        if (info_log != nullptr) {
            info_log(call.Arg(0), static_cast<int>(sizeof(text)), &written, text);
        }
        ARMBAT_LOG_WARN("Armbat", "gles: compile failed shader=%u type=0x%x: %s", call.Arg(0), type,
                        text);
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
    // The program-binary pair is driver work, not a stub. An engine caches the
    // linked program and reads GL_LINK_STATUS right after loading it, so a
    // handler that silently returns nothing is indistinguishable from a
    // shader that failed to compile. The host driver validates the blob the
    // same way the device does and reports a rejected one through glGetError.
    registry.Add("glProgramBinary", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, unsigned int, const void*, int)>("glProgramBinary");
        if (function == nullptr) {
            return;
        }
        const int length = static_cast<int>(call.Arg(3));
        if (length < 0) {
            return;
        }
        const Buffer binary = GuestBuffer(call, call.Arg(2), static_cast<std::size_t>(length));
        if (!CheckBuffer("glProgramBinary", call.Arg(2), binary,
                         static_cast<std::size_t>(length))) {
            return;
        }
        function(call.Arg(0), call.Arg(1), binary.bytes, length);
    });
    registry.Add("glGetProgramBinary", [](HleCall& call) {
        static const auto function = Gl<void (*)(unsigned int, int, int*, unsigned int*, void*)>(
            "glGetProgramBinary");
        if (function == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(1));
        if (size < 0) {
            return;
        }
        int* length = call.Arg(2) != 0
                          ? reinterpret_cast<int*>(call.Mapped(call.Arg(2), sizeof(int)))
                          : nullptr;
        unsigned int* format =
            call.Arg(3) != 0
                ? reinterpret_cast<unsigned int*>(call.Mapped(call.Arg(3), sizeof(unsigned int)))
                : nullptr;
        const Buffer binary = GuestBuffer(call, call.Arg(4), static_cast<std::size_t>(size));
        if ((call.Arg(2) != 0 && length == nullptr) || (call.Arg(3) != 0 && format == nullptr) ||
            !CheckBuffer("glGetProgramBinary", call.Arg(4), binary,
                         static_cast<std::size_t>(size))) {
            return;
        }
        function(call.Arg(0), size, length, format, binary.bytes);
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
        ARMBAT_LOG_WARN("Armbat", "gles: program %u failed: %s caller=%s", program, log,
                        armbat::DescribeGuestAddress(call.Machine().Reg(14)).c_str());
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
        const unsigned int index = call.Arg(0);
        const int size = static_cast<int>(call.Arg(1));
        const unsigned int type = call.Arg(2);
        const int stride = static_cast<int>(call.Arg(4));
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        // The driver dereferences the array only at the draw, so the first
        // element is checked here and the range the draw reaches is checked by
        // ValidateDrawArrays.
        const std::size_t element = AttributeElementSize(type);
        const std::size_t first =
            element != 0 && size > 0 ? element * static_cast<std::size_t>(size) : 1;
        bool ok = true;
        void* pointer = OffsetOrPointer(call, call.Arg(5), buffer, first,
                                        "glVertexAttribPointer", &ok);
        if (!ok) {
            return;
        }
        if (index < state->attribs.size()) {
            const std::lock_guard lock(state->mutex);
            GlesState::ClientArray& array = state->attribs[index];
            array.pointer = call.Arg(5);
            array.buffer = buffer;
            array.size = size;
            array.type = type;
            array.stride = stride;
        }
        function(index, size, type, static_cast<unsigned char>(call.Arg(3)), stride, pointer);
    });
    registry.Add("glVertexAttribIPointer", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, int, const void*)>(
                "glVertexAttribIPointer");
        if (function == nullptr) {
            return;
        }
        const unsigned int index = call.Arg(0);
        const int size = static_cast<int>(call.Arg(1));
        const unsigned int type = call.Arg(2);
        const int stride = static_cast<int>(call.Arg(3));
        std::uint32_t buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            buffer = state->array_buffer;
        }
        const std::size_t element = AttributeElementSize(type);
        const std::size_t first =
            element != 0 && size > 0 ? element * static_cast<std::size_t>(size) : 1;
        bool ok = true;
        void* pointer =
            OffsetOrPointer(call, call.Arg(4), buffer, first, "glVertexAttribIPointer", &ok);
        if (!ok) {
            return;
        }
        if (index < state->attribs.size()) {
            const std::lock_guard lock(state->mutex);
            GlesState::ClientArray& array = state->attribs[index];
            array.pointer = call.Arg(4);
            array.buffer = buffer;
            array.size = size;
            array.type = type;
            array.stride = stride;
        }
        function(index, size, type, stride, pointer);
    });
    registry.Add("glEnableVertexAttribArray", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int)>("glEnableVertexAttribArray");
        if (function != nullptr) function(call.Arg(0));
        const unsigned int index = call.Arg(0);
        if (index < state->attribs.size()) {
            const std::lock_guard lock(state->mutex);
            state->attribs[index].enabled = true;
        }
    });
    registry.Add("glDisableVertexAttribArray", [state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int)>("glDisableVertexAttribArray");
        if (function != nullptr) function(call.Arg(0));
        const unsigned int index = call.Arg(0);
        if (index < state->attribs.size()) {
            const std::lock_guard lock(state->mutex);
            state->attribs[index].enabled = false;
        }
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
    registry.Add("glDrawArrays", [state](HleCall& call) {
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
        if (count > 0 && HasClientArrays(state)) {
            const std::uint64_t max_index = static_cast<std::uint64_t>(first) +
                                            static_cast<std::uint64_t>(count - 1);
            if (!ValidateDrawArrays(call, state, "glDrawArrays", max_index, true)) {
                return;
            }
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
        // Client-side attributes are read through the indices, so the largest
        // index the draw can reach bounds the range they have to cover. With an
        // element buffer that value comes from the upload-time scan; without
        // one the guest's own index array is read here.
        if (count > 0 && HasClientArrays(state)) {
            bool bounded = false;
            std::uint64_t max_index = 0;
            if (buffer != 0) {
                GlesState::IndexMax bounds;
                bool known = false;
                {
                    const std::lock_guard lock(state->mutex);
                    const auto found = state->index_max.find(buffer);
                    if (found != state->index_max.end()) {
                        bounds = found->second;
                        known = true;
                    }
                }
                if (known) {
                    max_index = index_size == 1 ? bounds.bytes
                                : index_size == 2 ? bounds.shorts
                                                  : bounds.words;
                    bounded = true;
                }
            } else {
                bounded = ScanClientIndices(call, call.Arg(3), count, call.Arg(2), &max_index);
            }
            if (!ValidateDrawArrays(call, state, "glDrawElements", max_index, bounded)) {
                return;
            }
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
    registry.Add("glEnableClientState", [state](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int)>("glEnableClientState");
        if (function != nullptr) {
            function(call.Arg(0));
        }
        const std::lock_guard lock(state->mutex);
        switch (call.Arg(0)) {
            case 0x8074:  // GL_VERTEX_ARRAY
                state->legacy_vertex.enabled = true;
                break;
            case 0x8075:  // GL_NORMAL_ARRAY
                state->legacy_normal.enabled = true;
                break;
            case 0x8076:  // GL_COLOR_ARRAY
                state->legacy_color.enabled = true;
                break;
            case 0x8078:  // GL_TEXTURE_COORD_ARRAY
                state->legacy_texcoord.enabled = true;
                break;
            default:
                break;
        }
    });
    registry.Add("glDisableClientState", [state](HleCall& call) {
        static const auto function = Gl1<void (*)(unsigned int)>("glDisableClientState");
        if (function != nullptr) {
            function(call.Arg(0));
        }
        const std::lock_guard lock(state->mutex);
        switch (call.Arg(0)) {
            case 0x8074:  // GL_VERTEX_ARRAY
                state->legacy_vertex.enabled = false;
                break;
            case 0x8075:  // GL_NORMAL_ARRAY
                state->legacy_normal.enabled = false;
                break;
            case 0x8076:  // GL_COLOR_ARRAY
                state->legacy_color.enabled = false;
                break;
            case 0x8078:  // GL_TEXTURE_COORD_ARRAY
                state->legacy_texcoord.enabled = false;
                break;
            default:
                break;
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
        if (buffer == 0) {
            const std::lock_guard lock(state->mutex);
            state->legacy_vertex.pointer = call.Arg(3);
            state->legacy_vertex.buffer = buffer;
            state->legacy_vertex.size = static_cast<int>(call.Arg(0));
            state->legacy_vertex.type = call.Arg(1);
            state->legacy_vertex.stride = static_cast<int>(call.Arg(2));
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
        if (buffer == 0) {
            const std::lock_guard lock(state->mutex);
            state->legacy_color.pointer = call.Arg(3);
            state->legacy_color.buffer = buffer;
            state->legacy_color.size = static_cast<int>(call.Arg(0));
            state->legacy_color.type = call.Arg(1);
            state->legacy_color.stride = static_cast<int>(call.Arg(2));
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
        if (buffer == 0) {
            const std::lock_guard lock(state->mutex);
            state->legacy_texcoord.pointer = call.Arg(3);
            state->legacy_texcoord.buffer = buffer;
            state->legacy_texcoord.size = static_cast<int>(call.Arg(0));
            state->legacy_texcoord.type = call.Arg(1);
            state->legacy_texcoord.stride = static_cast<int>(call.Arg(2));
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
        if (buffer == 0) {
            const std::lock_guard lock(state->mutex);
            state->legacy_normal.pointer = call.Arg(2);
            state->legacy_normal.buffer = buffer;
            state->legacy_normal.size = 3;
            state->legacy_normal.type = call.Arg(0);
            state->legacy_normal.stride = static_cast<int>(call.Arg(1));
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
    registry.Add("glTexImage2D", [&registry, state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, unsigned int, unsigned int,
                        const void*)>("glTexImage2D");
        if (function == nullptr) {
            return;
        }
        PixelStore store;
        std::uint32_t unpack_buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            store = UnpackStoreLocked(*state);
            unpack_buffer = state->unpack_buffer;
        }
        const std::size_t length =
            ImageSize(call.Arg(3), call.Arg(4), call.Arg(6), call.Arg(7), store.alignment,
                      store.row_length, store.skip_rows, store.skip_pixels);
        const std::size_t checked = length != 0 ? length : 1;
        void* pixels = nullptr;
        if (unpack_buffer != 0) {
            // A bound pixel-unpack buffer turns the argument into an offset; the
            // bytes are already on the GPU and no guest memory is involved.
            pixels = reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.Arg(8)));
        } else {
            StagedRead staged(registry, call, call.Arg(8), checked);
            if (!staged.ok()) {
                RejectBuffer("glTexImage2D", call.Arg(8), checked);
                return;
            }
            pixels = staged.bytes();
            function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                     static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                     static_cast<int>(call.Arg(5)), call.Arg(6), call.Arg(7), pixels);
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), call.Arg(6), call.Arg(7), pixels);
    });
    registry.Add("glTexSubImage2D", [&registry, state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, unsigned int, unsigned int,
                        const void*)>("glTexSubImage2D");
        if (function == nullptr) {
            return;
        }
        PixelStore store;
        std::uint32_t unpack_buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            store = UnpackStoreLocked(*state);
            unpack_buffer = state->unpack_buffer;
        }
        // glTexSubImage2D(target, level, xoffset, yoffset, width, height, format,
        // type, pixels): the size of the source image comes from width and
        // height, which sit two slots later than in glTexImage2D.
        const std::size_t length =
            ImageSize(call.Arg(4), call.Arg(5), call.Arg(6), call.Arg(7), store.alignment,
                      store.row_length, store.skip_rows, store.skip_pixels);
        const std::size_t checked = length != 0 ? length : 1;
        void* pixels = nullptr;
        if (unpack_buffer != 0) {
            pixels = reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.Arg(8)));
        } else {
            // The upload is still served when the range does not fit one mapped
            // run: a sub-rectangle of an atlas, or the tail of a buffer the guest
            // allocated flat, holds the pixels the guest meant to send. The
            // driver only ever reads the staged copy, never the guest range.
            StagedRead staged(registry, call, call.Arg(8), checked);
            if (!staged.ok()) {
                RejectBuffer("glTexSubImage2D", call.Arg(8), checked);
                return;
            }
            pixels = staged.bytes();
            function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                     static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                     static_cast<int>(call.Arg(5)), call.Arg(6), call.Arg(7), pixels);
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), call.Arg(6), call.Arg(7), pixels);
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
    registry.Add("glTexImage3D", [&registry, state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, int, unsigned int,
                        unsigned int, const void*)>("glTexImage3D");
        if (function == nullptr) return;
        PixelStore store;
        std::uint32_t unpack_buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            store = UnpackStoreLocked(*state);
            unpack_buffer = state->unpack_buffer;
        }
        const std::size_t length =
            ImageSize3D(call.Arg(3), call.Arg(4), call.Arg(5), call.Arg(7), call.Arg(8),
                        store.alignment, store.row_length, store.skip_rows, store.skip_pixels,
                        store.image_height, store.skip_images);
        const std::size_t checked = length != 0 ? length : 1;
        void* pixels = nullptr;
        if (unpack_buffer != 0) {
            pixels = reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.Arg(9)));
        } else {
            StagedRead staged(registry, call, call.Arg(9), checked);
            if (!staged.ok()) {
                RejectBuffer("glTexImage3D", call.Arg(9), checked);
                return;
            }
            pixels = staged.bytes();
            function(call.Arg(0), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                     static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                     static_cast<int>(call.Arg(6)), call.Arg(7), call.Arg(8), pixels);
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                 static_cast<int>(call.Arg(6)), call.Arg(7), call.Arg(8), pixels);
    });
    registry.Add("glTexSubImage3D", [&registry, state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, int, int, unsigned int,
                        unsigned int, const void*)>("glTexSubImage3D");
        if (function == nullptr) return;
        PixelStore store;
        std::uint32_t unpack_buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            store = UnpackStoreLocked(*state);
            unpack_buffer = state->unpack_buffer;
        }
        const std::size_t length =
            ImageSize3D(call.Arg(5), call.Arg(6), call.Arg(7), call.Arg(8), call.Arg(9),
                        store.alignment, store.row_length, store.skip_rows, store.skip_pixels,
                        store.image_height, store.skip_images);
        const std::size_t checked = length != 0 ? length : 1;
        void* pixels = nullptr;
        if (unpack_buffer != 0) {
            pixels = reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.Arg(10)));
        } else {
            StagedRead staged(registry, call, call.Arg(10), checked);
            if (!staged.ok()) {
                RejectBuffer("glTexSubImage3D", call.Arg(10), checked);
                return;
            }
            pixels = staged.bytes();
            function(call.Arg(0), static_cast<int>(call.Arg(1)),
                     static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                     static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                     static_cast<int>(call.Arg(6)), static_cast<int>(call.Arg(7)),
                     call.Arg(8), call.Arg(9), pixels);
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                 static_cast<int>(call.Arg(6)), static_cast<int>(call.Arg(7)),
                 call.Arg(8), call.Arg(9), pixels);
    });
    registry.Add("glCompressedTexImage3D", [&registry, state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, unsigned int, int, int, int, int, int,
                        const void*)>("glCompressedTexImage3D");
        if (function == nullptr) return;
        const int size = static_cast<int>(call.Arg(7));
        if (size < 0) return;
        const UploadPixels data(registry, call, state, call.Arg(8),
                                static_cast<std::size_t>(size), "glCompressedTexImage3D");
        if (!data.ok()) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), static_cast<int>(call.Arg(6)), size,
                 data.bytes());
    });
    registry.Add("glCompressedTexSubImage3D", [&registry, state](HleCall& call) {
        static const auto function =
            Gl<void (*)(unsigned int, int, int, int, int, int, int, int, unsigned int,
                        int, const void*)>("glCompressedTexSubImage3D");
        if (function == nullptr) return;
        const int size = static_cast<int>(call.Arg(9));
        if (size < 0) return;
        const UploadPixels data(registry, call, state, call.Arg(10),
                                static_cast<std::size_t>(size), "glCompressedTexSubImage3D");
        if (!data.ok()) return;
        function(call.Arg(0), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)),
                 static_cast<int>(call.Arg(4)), static_cast<int>(call.Arg(5)),
                 static_cast<int>(call.Arg(6)), static_cast<int>(call.Arg(7)),
                 call.Arg(8), size, data.bytes());
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
    // GL_EXT_discard_framebuffer / ES 3.0 invalidation is only advisory: the driver
    // is allowed to throw the contents of the listed attachments away. An engine
    // passes it for a target it will not read again, but the container sits between
    // the engine and the driver and cannot prove that. Unity 5.3 draws its frame
    // into its own target, invalidates it, and composites from that very target in
    // the same frame; forwarding the hint let the driver drop exactly the pixels the
    // composite samples, and the window presented black while every draw counter
    // looked healthy. Keeping the contents costs one tile write-back per target and
    // is always correct.
    registry.Add("glInvalidateFramebuffer", [](HleCall&) {
        // Deliberately does nothing; see the comment above.
    });
    registry.Add("glInvalidateSubFramebuffer", [](HleCall&) {
        // Same as glInvalidateFramebuffer above: advisory, and unsafe to honour.
    });
    // ---- GL sync objects --------------------------------------------------
    //
    // A GLsync is a driver pointer that the guest keeps in its own structures and
    // hands back, so it never sees the pointer itself: the handle table turns it
    // into an index and every call that takes a sync translates it back. The
    // calls are rare here (one capability probe at startup), but a fence the
    // driver was never told about is a fence that never flushes anything.
    registry.Add("glFenceSync", [](HleCall& call) {
        static const auto function = Gl<void* (*)(unsigned int, unsigned int)>("glFenceSync");
        if (function == nullptr) {
            call.Return(0);
            return;
        }
        call.Return(armbat::RegisterHostHandle(function(call.Arg(0), call.Arg(1))));
    });
    registry.Add("glClientWaitSync", [](HleCall& call) {
        static const auto function =
            Gl<unsigned int (*)(void*, unsigned int, std::uint64_t)>("glClientWaitSync");
        void* sync = armbat::LookupHostHandle(call.Arg(0));
        if (function == nullptr || sync == nullptr) {
            call.Return(0x911a);  // GL_ALREADY_SIGNALED: there is no fence to wait for
            return;
        }
        // GLuint64 arrives as an aligned register pair, low word first.
        const std::uint64_t timeout = static_cast<std::uint64_t>(call.Arg(2)) |
                                      (static_cast<std::uint64_t>(call.Arg(3)) << 32);
        call.Return(function(sync, call.Arg(1), timeout));
    });
    registry.Add("glWaitSync", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(void*, unsigned int, std::uint64_t)>("glWaitSync");
        void* sync = armbat::LookupHostHandle(call.Arg(0));
        if (function == nullptr || sync == nullptr) {
            return;
        }
        const std::uint64_t timeout = static_cast<std::uint64_t>(call.Arg(2)) |
                                      (static_cast<std::uint64_t>(call.Arg(3)) << 32);
        function(sync, call.Arg(1), timeout);
    });
    registry.Add("glDeleteSync", [](HleCall& call) {
        static const auto function = Gl<void (*)(void*)>("glDeleteSync");
        void* sync = armbat::LookupHostHandle(call.Arg(0));
        if (function != nullptr && sync != nullptr) {
            function(sync);
        }
    });
    registry.Add("glIsSync", [](HleCall& call) {
        static const auto function = Gl<unsigned char (*)(void*)>("glIsSync");
        void* sync = armbat::LookupHostHandle(call.Arg(0));
        call.Return(function != nullptr && sync != nullptr ? function(sync) : 0);
    });
    registry.Add("glGetSynciv", [](HleCall& call) {
        static const auto function =
            Gl<void (*)(void*, unsigned int, int, int*, int*)>("glGetSynciv");
        void* sync = armbat::LookupHostHandle(call.Arg(0));
        if (function == nullptr || sync == nullptr) {
            return;
        }
        const int size = static_cast<int>(call.Arg(2));
        if (size < 0) {
            return;
        }
        int* length =
            call.Arg(3) != 0 ? reinterpret_cast<int*>(call.Mapped(call.Arg(3), sizeof(int)))
                             : nullptr;
        const std::size_t values_length =
            sizeof(int) * static_cast<std::size_t>(size != 0 ? size : 1);
        int* values = call.Arg(4) != 0
                          ? reinterpret_cast<int*>(call.Mapped(call.Arg(4), values_length))
                          : nullptr;
        function(sync, call.Arg(1), size, length, values);
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
    registry.Add("glCompressedTexImage2D", [&registry, state](HleCall& call) {
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
        const UploadPixels data(registry, call, state, call.Arg(7),
                                static_cast<std::size_t>(size), "glCompressedTexImage2D");
        if (!data.ok()) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), call.Arg(2),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), size, data.bytes());
    });
    registry.Add("glCompressedTexSubImage2D", [&registry, state](HleCall& call) {
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
        const UploadPixels data(registry, call, state, call.Arg(8),
                                static_cast<std::size_t>(size), "glCompressedTexSubImage2D");
        if (!data.ok()) {
            return;
        }
        function(call.Arg(0), static_cast<int>(call.Arg(1)), static_cast<int>(call.Arg(2)),
                 static_cast<int>(call.Arg(3)), static_cast<int>(call.Arg(4)),
                 static_cast<int>(call.Arg(5)), call.Arg(6), size, data.bytes());
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
    // ES 3's object factories all share one shape: a count and a guest array of
    // GLuint names the driver fills. The names are the only thing that crosses
    // the boundary, so the handler validates the range and hands the arena
    // pointer over. Without these the shim exports the entry point, the call
    // succeeds, and the engine binds object zero because no name was written.
    {
        const auto add_name_array = [&registry](const char* gen_name, const char* del_name) {
            const auto gen = Gl<void (*)(int, unsigned int*)>(gen_name);
            const auto del = Gl<void (*)(int, const unsigned int*)>(del_name);
            const std::string gen_id = gen_name;
            const std::string del_id = del_name;
            registry.Add(gen_name, [gen, gen_id](HleCall& call) {
                if (gen == nullptr) {
                    return;
                }
                const int count = static_cast<int>(call.Arg(0));
                if (count < 0 || count > 65536) {
                    return;
                }
                const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
                const Buffer out = GuestBuffer(call, call.Arg(1), length);
                if (!RequireArray(gen_id.c_str(), call.Arg(1), out, length, count)) {
                    return;
                }
                gen(count, static_cast<unsigned int*>(out.bytes));
            });
            registry.Add(del_name, [del, del_id](HleCall& call) {
                if (del == nullptr) {
                    return;
                }
                const int count = static_cast<int>(call.Arg(0));
                if (count < 0 || count > 65536) {
                    return;
                }
                const std::size_t length = static_cast<std::size_t>(count) * sizeof(unsigned int);
                const Buffer names = GuestBuffer(call, call.Arg(1), length);
                if (!RequireArray(del_id.c_str(), call.Arg(1), names, length, count)) {
                    return;
                }
                del(count, static_cast<const unsigned int*>(names.bytes));
            });
        };
        add_name_array("glGenVertexArrays", "glDeleteVertexArrays");
        add_name_array("glGenSamplers", "glDeleteSamplers");
        add_name_array("glGenQueries", "glDeleteQueries");
        add_name_array("glGenTransformFeedbacks", "glDeleteTransformFeedbacks");
        add_name_array("glGenProgramPipelines", "glDeleteProgramPipelines");
    }
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
        PixelStore store;
        std::uint32_t pack_buffer = 0;
        {
            const std::lock_guard lock(state->mutex);
            store = PackStoreLocked(*state);
            pack_buffer = state->pack_buffer;
        }
        const std::size_t length = ImageSize(call.Arg(2), call.Arg(3), call.Arg(4), call.Arg(5),
                                             store.alignment, store.row_length, store.skip_rows,
                                             store.skip_pixels);
        const std::size_t checked = length != 0 ? length : 1;
        void* pixels = nullptr;
        if (pack_buffer != 0) {
            // A bound pixel-pack buffer turns the argument into an offset; the
            // driver writes into its own memory and no guest range is involved.
            pixels = reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.Arg(6)));
        } else if (call.Arg(6) != 0) {
            // The driver writes the whole image; a range it cannot write end to
            // end has to be refused rather than half-filled.
            pixels = call.Mapped(call.Arg(6), checked);
            if (pixels == nullptr) {
                RejectBuffer("glReadPixels", call.Arg(6), checked);
                return;
            }
        }
        function(static_cast<int>(call.Arg(0)), static_cast<int>(call.Arg(1)),
                 static_cast<int>(call.Arg(2)), static_cast<int>(call.Arg(3)), call.Arg(4),
                 call.Arg(5), pixels);
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
