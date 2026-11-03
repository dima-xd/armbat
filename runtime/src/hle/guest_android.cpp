#include <android/log.h>

#include "armbat/log.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "armbat/a32_machine.h"
#include "armbat/guest_apk.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_input.h"
#include "armbat/handles.h"
#include "armbat/hle.h"
#include "armbat/jni_bridge.h"

// The Android platform libraries an ARMv7 native activity imports: assets,
// configuration, window, looper, input and EGL. The Java side of the platform
// runs on the host, so these are served here instead of by the framework.
//
// EGL is forwarded to the host implementation: handles are 32-bit on both sides
// and the only data EGL exchanges are int arrays, which are copied across the
// address-space boundary.

namespace armbat {
namespace {

GuestApk* g_apk = nullptr;

void Log(const std::string& line) {
    // Per-call platform logging is a trace, not a feature. An engine asks for
    // EGL config attributes thousands of times while it picks a configuration
    // and again on every frame, and the guest's own log shares the container's
    // per-process log quota, so an unbounded trace silences exactly the
    // messages being looked for. Each call name gets a small budget: the first
    // turn of every path stays visible and the total stays bounded.
    static std::mutex mutex;
    static std::unordered_map<std::string, unsigned> counts;
    const std::string name = line.substr(0, line.find('('));
    {
        const std::lock_guard lock(mutex);
        if (++counts[name] > 8) {
            return;
        }
    }
    ARMBAT_LOG_TEXT(LogLevel::Debug, "Armbat", line);
}

// The host window behind a guest handle and one of its dimensions. The window
// belongs to the framework view; the guest only ever learns how big it is.
int WindowExtent(std::uint32_t handle, bool want_height) {
    using Query = int (*)(void*);
    static const Query get_width = [] {
        void* library = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        return library != nullptr
                   ? reinterpret_cast<Query>(::dlsym(library, "ANativeWindow_getWidth"))
                   : nullptr;
    }();
    static const Query get_height = [] {
        void* library = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        return library != nullptr
                   ? reinterpret_cast<Query>(::dlsym(library, "ANativeWindow_getHeight"))
                   : nullptr;
    }();
    void* window = LookupHostHandle(handle);
    const Query query = want_height ? get_height : get_width;
    return (window != nullptr && query != nullptr) ? query(window) : 0;
}

// ---- assets ---------------------------------------------------------------

// AAsset has no fields the guest may touch; the guest only ever holds the
// pointer. The container keeps everything behind it.
struct Asset {
    std::vector<std::uint8_t> data;
    std::uint64_t position = 0;
    bool closed = false;
    // AAsset_getBuffer answers with a pointer into the guest's address space
    // that stays valid until the asset is closed, so the copy is made once and
    // released with the handle. Handing out a fresh block per call exhausts the
    // HLE data area after a few dozen assets -- which is what happened when the
    // engine started reading its resource packs.
    std::uint32_t buffer_cell = 0;
};

std::vector<Asset> g_assets;
// Every asset entry point can be reached from any guest thread at once: the
// engine reads resource packs on a pool of its own. The vectors below are shared
// state, and an unsynchronised push_back reallocating under another thread's
// read is a use-after-free of the host heap, not a guest error.
std::mutex g_asset_mutex;

// A handle that leaves the container is a table index plus one, never zero: the
// guest compares what the platform returned with null, and zero is how it spells
// failure. Slot zero is also the first one the reuse in AAssetManager_open hands
// out, so a raw index made the engine lose whole files at random. The vanilla
// resource pack's manifest was one of them: the open answered zero, the engine
// concluded the pack had no manifest, and the pack stack it builds afterwards
// dereferenced that empty slot.
constexpr std::uint32_t kFirstAssetHandle = 1;

// The slot behind a guest handle, for the asset and the asset directory tables
// alike. A null handle, or one the table does not hold, becomes an index past
// the end so every existing range check keeps working.
std::size_t HandleSlot(std::uint32_t handle) {
    return handle < kFirstAssetHandle ? static_cast<std::size_t>(-1)
                                      : handle - kFirstAssetHandle;
}

// AAssetDir is a name list plus the guest cell the next name is written into:
// the platform owns the returned string until the next call.
struct AssetDir {
    std::vector<std::string> names;
    std::size_t next = 0;
    std::uint32_t cell = 0;
};

constexpr std::uint32_t kAssetDirCell = 512;

std::vector<AssetDir> g_asset_dirs;

// ---- looper ---------------------------------------------------------------
//
// A looper belongs to the thread that prepared it, and only that thread polls
// the sources registered on it. The single global list this replaces let every
// thread see every source: with the input queue among them, several threads
// woke on one touch and raced for the event, so the engine's input thread lost
// touches to whichever thread reached AInputQueue_getEvent first. A wake was
// worse still -- it answered the caller's own ident (1, which is Unity's
// LOOPER_ID_INPUT) instead of ALOOPER_POLL_WAKE (-1), so every wait another
// thread cut short looked like a touch that had already been consumed.

struct LooperSource {
    std::uint32_t fd = 0;
    std::uint32_t ident = 0;
    std::uint32_t events = 0;
    std::uint32_t callback = 0;
    std::uint32_t data = 0;
};

struct Looper {
    std::uint32_t handle = 0;
    int wake_read = -1;
    int wake_write = -1;
    // One thread waits on the sources and another may attach or detach while it
    // does, so the list is guarded; the wait itself copies it and unlocks.
    std::mutex mutex;
    std::vector<LooperSource> sources;
};

struct LooperTable {
    std::mutex mutex;
    std::unordered_map<std::uint32_t, std::unique_ptr<Looper>> by_handle;
    std::uint32_t next_handle = 1;
};

LooperTable& Loopers() {
    // Deliberately leaked: loopers outlive every other static in the container
    // and a thread may still poll one while the process is unwinding.
    static LooperTable* table = new LooperTable();
    return *table;
}

// The looper of the calling thread, or null before ALooper_prepare.
thread_local Looper* g_thread_looper = nullptr;

// android/looper.h: the answers ALooper_pollOnce and ALooper_pollAll return.
// A loop that switches on these must not mistake a wake for an event: Unity
// reads 1 as LOOPER_ID_INPUT.
constexpr std::int32_t kLooperWake = -1;
constexpr std::int32_t kLooperCallback = -2;
constexpr std::int32_t kLooperTimeout = -3;
constexpr std::int32_t kLooperError = -4;

Looper* LooperForHandle(std::uint32_t handle) {
    if (handle == 0) {
        return g_thread_looper;
    }
    LooperTable& table = Loopers();
    const std::lock_guard lock(table.mutex);
    const auto it = table.by_handle.find(handle);
    return it == table.by_handle.end() ? nullptr : it->second.get();
}

Looper* CreateThreadLooper() {
    LooperTable& table = Loopers();
    std::uint32_t handle = 0;
    {
        const std::lock_guard lock(table.mutex);
        handle = table.next_handle++;
        auto looper = std::make_unique<Looper>();
        looper->handle = handle;
        // The wake pipe is a source of the looper the way it is on a device: a
        // thread asleep in poll() has to leave the wait the moment another
        // thread wakes it, not when its timeout runs out.
        int fds[2] = {-1, -1};
        if (::pipe(fds) == 0) {
            looper->wake_read = fds[0];
            looper->wake_write = fds[1];
            for (const int fd : fds) {
                const int flags = ::fcntl(fd, F_GETFL, 0);
                if (flags >= 0) {
                    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                }
            }
        }
        table.by_handle.emplace(handle, std::move(looper));
    }
    g_thread_looper = LooperForHandle(handle);
    return g_thread_looper;
}

// One descriptor is one source: re-attaching the same queue replaces the entry
// instead of handing one descriptor to two wakes.
void LooperAddSource(Looper& looper, const LooperSource& source) {
    const std::lock_guard lock(looper.mutex);
    for (LooperSource& existing : looper.sources) {
        if (existing.fd == source.fd) {
            existing = source;
            return;
        }
    }
    looper.sources.push_back(source);
}

bool LooperRemoveSource(Looper& looper, std::uint32_t fd) {
    const std::lock_guard lock(looper.mutex);
    const auto end = std::remove_if(looper.sources.begin(), looper.sources.end(),
                                    [fd](const LooperSource& source) {
                                        return source.fd == fd;
                                    });
    if (end == looper.sources.end()) {
        return false;
    }
    looper.sources.erase(end, looper.sources.end());
    return true;
}

// The three out parameters a wait fills in. They describe the source that
// fired, and a loop that dispatches through the cookie reads them instead of
// switching on the ident.
void WritePollResult(HleCall& call, std::int32_t fd, std::int32_t events,
                     std::uint32_t data) {
    if (call.Arg(1) != 0) {
        call.CopyOut(call.Arg(1), &fd, sizeof(fd));
    }
    if (call.Arg(2) != 0) {
        call.CopyOut(call.Arg(2), &events, sizeof(events));
    }
    if (call.Arg(3) != 0) {
        call.CopyOut(call.Arg(3), &data, sizeof(data));
    }
}

// ---- input -----------------------------------------------------------------
//
// A native activity never looks inside an AInputEvent: the platform keeps the
// event in native memory, hands the archive a pointer, and the archive asks the
// AMotionEvent_* family about it. Here the event lives in the guest, in a slot
// of the HLE data area, and the readers decode that layout.

struct GuestInputEvent {
    std::int32_t type;
    std::int32_t source;
    std::int32_t device_id;
    std::int32_t action;
    std::int32_t meta_state;
    std::int32_t flags;
    std::int32_t edge_flags;
    std::int32_t key_code;
    std::int32_t repeat_count;
    std::int32_t scan_code;
    std::int32_t pointer_count;
    std::int32_t reserved;
    // The platform reports both in nanoseconds since boot; the host event
    // carries milliseconds, so the reader scales them.
    std::int64_t down_time;
    std::int64_t event_time;
};
static_assert(sizeof(GuestInputEvent) == 64, "the guest layout is 32-bit");

struct GuestInputPointer {
    std::int32_t id;
    float x;
    float y;
    float raw_x;
    float raw_y;
    float pressure;
    float size;
    float touch_major;
    float touch_minor;
    std::int32_t tool_type;
};
static_assert(sizeof(GuestInputPointer) == 40, "the guest layout is 32-bit");

constexpr std::uint32_t kInputPointers = 16;
constexpr std::uint32_t kInputEventBytes =
    sizeof(GuestInputEvent) + kInputPointers * sizeof(GuestInputPointer);
// A handful of events in flight is all a single consumer ever holds; a queue
// that grows past them is not being drained at all.
constexpr std::uint32_t kInputEventSlots = 8;

struct InputSlot {
    std::uint32_t address = 0;
    bool busy = false;
};

std::vector<InputSlot> g_input_slots;
std::mutex g_input_mutex;

// One wait on this thread's looper. ALOOPER_POLL_CALLBACK means "a source ran,
// the wait is not over": ALooper_pollAll drains those and only then answers.
std::int32_t PollLooperOnce(HleCall& call, std::int32_t timeout) {
    Looper* looper = g_thread_looper;
    if (looper == nullptr) {
        looper = CreateThreadLooper();
    }
    if (looper == nullptr) {
        call.Return(static_cast<std::uint32_t>(kLooperError));
        return kLooperError;
    }
    // The wait and the dispatch read one copy of the source list, so a source
    // attached or detached while this thread is asleep cannot shift the index
    // that names the descriptor which fired.
    std::vector<LooperSource> sources;
    {
        const std::lock_guard lock(looper->mutex);
        sources = looper->sources;
    }
    std::vector<struct pollfd> fds;
    fds.reserve(sources.size() + 1);
    fds.push_back(pollfd{looper->wake_read, POLLIN, 0});
    for (const LooperSource& source : sources) {
        fds.push_back(pollfd{static_cast<int>(source.fd), static_cast<short>(source.events), 0});
    }
    const int ready = ::poll(fds.data(), fds.size(), timeout);
    if (ready < 0) {
        if (errno == EINTR) {
            // The container's wakeup signal interrupted the wait because a
            // guest signal is queued on this thread; the handler runs at the
            // next safe point. That is how a parked thread is reached at all,
            // so it cannot be reported as a looper failure: the platform
            // answers ALOOPER_POLL_WAKE for an interrupted wait, and a loop
            // that treats an error as fatal would stop reading the queue for
            // the rest of the session.
            WritePollResult(call, -1, 0, 0);
            call.Return(static_cast<std::uint32_t>(kLooperWake));
            return kLooperWake;
        }
        call.Return(static_cast<std::uint32_t>(kLooperError));
        return kLooperError;
    }
    if (ready == 0) {
        call.Return(static_cast<std::uint32_t>(kLooperTimeout));
        return kLooperTimeout;
    }
    if (fds[0].revents != 0) {
        char buffer[64];
        while (::read(looper->wake_read, buffer, sizeof(buffer)) > 0) {
        }
        WritePollResult(call, -1, 0, 0);
        call.Return(static_cast<std::uint32_t>(kLooperWake));
        return kLooperWake;
    }
    for (std::size_t i = 1; i < fds.size(); ++i) {
        if (fds[i].revents == 0) {
            continue;
        }
        // The callback may add or remove sources, so the entry comes from the
        // copy this wait polled and the set is rebuilt by the next wait.
        const LooperSource source = sources[i - 1];
        if (source.callback != 0) {
            const std::uint32_t fd = source.fd;
            const std::uint32_t revents = static_cast<std::uint32_t>(fds[i].revents);
            const std::uint32_t data = source.data;
            call.Machine().CallNested(source.callback, {fd, revents, data});
            call.Return(static_cast<std::uint32_t>(kLooperCallback));
            return kLooperCallback;
        }
        WritePollResult(call, static_cast<std::int32_t>(source.fd),
                        static_cast<std::int32_t>(fds[i].revents), source.data);
        call.Return(source.ident);
        return static_cast<std::int32_t>(source.ident);
    }
    call.Return(static_cast<std::uint32_t>(kLooperTimeout));
    return kLooperTimeout;
}

std::uint32_t StoreInputEvent(HleRegistry& registry, const InputEvent& event) {
    const std::lock_guard lock(g_input_mutex);
    InputSlot* slot = nullptr;
    for (InputSlot& candidate : g_input_slots) {
        if (!candidate.busy) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        if (g_input_slots.size() >= kInputEventSlots) {
            return 0;
        }
        g_input_slots.push_back(InputSlot{});
        slot = &g_input_slots.back();
        slot->address = registry.AllocateData(kInputEventBytes);
        if (slot->address == 0) {
            g_input_slots.pop_back();
            return 0;
        }
    }
    auto* memory = reinterpret_cast<std::uint8_t*>(registry.Arena().Ptr(slot->address));
    if (memory == nullptr) {
        return 0;
    }
    GuestInputEvent header{};
    header.type = event.type;
    header.source = event.source;
    header.device_id = event.device_id;
    header.action = event.action;
    header.meta_state = event.meta_state;
    header.flags = event.flags;
    header.edge_flags = event.edge_flags;
    header.key_code = event.key_code;
    header.repeat_count = event.repeat_count;
    header.scan_code = event.scan_code;
    header.pointer_count = static_cast<std::int32_t>(std::min<std::size_t>(
        event.pointers.size(), kInputPointers));
    header.down_time = event.down_time;
    header.event_time = event.event_time;
    std::memcpy(memory, &header, sizeof(header));
    for (std::uint32_t index = 0; index < kInputPointers; ++index) {
        GuestInputPointer pointer{};
        if (index < event.pointers.size()) {
            const InputPointer& source = event.pointers[index];
            pointer.id = source.id;
            pointer.x = source.x;
            pointer.y = source.y;
            pointer.raw_x = source.raw_x;
            pointer.raw_y = source.raw_y;
            pointer.pressure = source.pressure;
            pointer.size = source.size;
            pointer.touch_major = source.touch_major;
            pointer.touch_minor = source.touch_minor;
            pointer.tool_type = source.tool_type;
        }
        std::memcpy(memory + sizeof(header) + index * sizeof(pointer), &pointer, sizeof(pointer));
    }
    slot->busy = true;
    return slot->address;
}

void ReleaseInputEvent(std::uint32_t address) {
    if (address == 0) {
        return;
    }
    const std::lock_guard lock(g_input_mutex);
    for (InputSlot& slot : g_input_slots) {
        if (slot.address == address) {
            slot.busy = false;
            return;
        }
    }
}

const GuestInputEvent* InputEventAt(HleCall& call, std::uint32_t address) {
    if (address == 0) {
        return nullptr;
    }
    return reinterpret_cast<const GuestInputEvent*>(
        call.Mapped(address, sizeof(GuestInputEvent)));
}

const GuestInputEvent* MotionEventAt(HleCall& call, std::uint32_t address) {
    const GuestInputEvent* event = InputEventAt(call, address);
    return event != nullptr && event->type == 2 ? event : nullptr;
}

const GuestInputEvent* KeyEventAt(HleCall& call, std::uint32_t address) {
    const GuestInputEvent* event = InputEventAt(call, address);
    return event != nullptr && event->type == 1 ? event : nullptr;
}

const GuestInputPointer* InputPointerAt(HleCall& call, std::uint32_t address,
                                        std::uint32_t index) {
    const GuestInputEvent* event = MotionEventAt(call, address);
    if (event == nullptr || index >= kInputPointers ||
        index >= static_cast<std::uint32_t>(event->pointer_count)) {
        return nullptr;
    }
    const auto* pointers = reinterpret_cast<const GuestInputPointer*>(call.Mapped(
        address + sizeof(GuestInputEvent), sizeof(GuestInputPointer) * kInputPointers));
    return pointers != nullptr ? &pointers[index] : nullptr;
}

void ReturnFloat(HleCall& call, float value) {
    call.Return(std::bit_cast<std::uint32_t>(value));
}

// ---- EGL ------------------------------------------------------------------

void* EglSymbol(const char* name) {
    static void* library = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    return library != nullptr ? dlsym(library, name) : nullptr;
}

std::vector<std::int32_t> ReadInts(HleCall& call, std::uint32_t address, std::size_t limit) {
    std::vector<std::int32_t> values(limit, 0);
    if (address == 0) {
        return {};
    }
    call.CopyIn(address, values.data(), values.size() * sizeof(std::int32_t));
    while (!values.empty() && values.back() == 0) {
        values.pop_back();
    }
    values.push_back(0);
    return values;
}

}  // namespace

void SetGuestApk(GuestApk* apk) { g_apk = apk; }

void RegisterAndroidFunctions(HleRegistry& registry) {
    // ---- AAssetManager ----------------------------------------------------
    //
    // The archive turns the Java AssetManager into the handle it keeps for the
    // life of the process. The container reads assets straight out of the APK
    // and never looks at the handle, so any non-null value is the honest
    // answer -- and null is not a failure an archive can recover from: Wwise
    // aborts its whole sound engine when the AssetManager cannot be set.
    registry.Add("AAssetManager_fromJava", [](HleCall& call) { call.Return(1); });
    registry.Add("AAssetManager_open", [](HleCall& call) {
        std::string name;
        call.ReadCString(call.Arg(1), name, 4096);
        // Reading an entry is a pread plus an inflate of the whole file, and it
        // runs on whichever thread opened it. Nothing in it is shared state: the
        // archive index is immutable once the file is open, and pread leaves the
        // descriptor's offset alone. Doing it under the asset table's mutex made
        // every other thread's asset read wait for the biggest inflate in
        // flight, which is exactly what a world load does on several threads.
        Asset asset;
        std::string error;
        if (g_apk == nullptr || !g_apk->ReadAsset(name, &asset.data, &error)) {
            // Distinct names only: the engine probes the same optional files
            // (import locks, saved manifests, upgrade reports) in every pack, so
            // a repeat is noise while a name never seen before is a fact.
            static std::mutex missing_mutex;
            static std::set<std::string> missing;
            bool first = false;
            {
                const std::lock_guard lock(missing_mutex);
                first = missing.size() < 200 && missing.insert(name).second;
            }
            if (first) {
                Log("asset: cannot open " + name + (error.empty() ? "" : ": " + error));
            }
            call.Return(0);
            return;
        }
        const std::lock_guard asset_lock(g_asset_mutex);
        // A closed handle is a free slot. The engine opens thousands of assets
        // and keeps every one's whole contents, so without reuse the container
        // ends up holding the archive unpacked in memory -- which is both a
        // false memory report to the guest and a reason for the platform to kill
        // the process.
        std::size_t slot = g_assets.size();
        for (std::size_t i = 0; i < g_assets.size(); ++i) {
            if (g_assets[i].closed) {
                slot = i;
                break;
            }
        }
        if (slot == g_assets.size()) {
            g_assets.push_back(std::move(asset));
        } else {
            g_assets[slot] = std::move(asset);
        }
        // The counterpart of the line below: an asset the engine did read, so a
        // missing one is tellable from a file the log never mentions at all.
        if (LogAllowed("asset-opened", 60)) {
            Log("asset: opened " + name + " (" + std::to_string(g_assets[slot].data.size()) +
                " bytes)");
        }
        call.Return(static_cast<std::uint32_t>(slot) + kFirstAssetHandle);
    });
    registry.Add("AAsset_getLength", [](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        call.Return(index < g_assets.size() ? static_cast<std::uint32_t>(g_assets[index].data.size()) : 0);
    });
    registry.Add("AAsset_getLength64", [](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        const std::uint64_t size = index < g_assets.size() ? g_assets[index].data.size() : 0;
        call.ReturnPair(static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size >> 32));
    });
    registry.Add("AAsset_getRemainingLength64", [](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        const std::uint64_t left = index < g_assets.size()
                                       ? g_assets[index].data.size() - g_assets[index].position
                                       : 0;
        call.ReturnPair(static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(left >> 32));
    });
    registry.Add("AAsset_read", [](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        if (index >= g_assets.size()) {
            call.Return(0);
            return;
        }
        Asset& asset = g_assets[index];
        const std::uint32_t wanted = call.Arg(2);
        const std::size_t left = asset.data.size() - static_cast<std::size_t>(asset.position);
        const std::size_t count = std::min<std::size_t>(wanted, left);
        if (count > 0) {
            call.CopyOut(call.Arg(1), asset.data.data() + asset.position, count);
            asset.position += count;
        }
        call.Return(static_cast<std::uint32_t>(count));
    });
    registry.Add("AAsset_seek64", [](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        if (index >= g_assets.size()) {
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        Asset& asset = g_assets[index];
        std::int64_t offset = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(call.Arg(1)) |
            (static_cast<std::uint64_t>(call.Arg(2)) << 32));
        const std::uint32_t whence = call.Arg(3);
        std::int64_t base = 0;
        if (whence == SEEK_CUR) {
            base = static_cast<std::int64_t>(asset.position);
        } else if (whence == SEEK_END) {
            base = static_cast<std::int64_t>(asset.data.size());
        }
        offset += base;
        if (offset < 0) {
            offset = 0;
        }
        asset.position = static_cast<std::uint64_t>(offset);
        call.ReturnPair(static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(offset >> 32));
    });
    registry.Add("AAsset_getBuffer", [&registry](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        if (index >= g_assets.size() || g_assets[index].data.empty()) {
            call.Return(0);
            return;
        }
        Asset& asset = g_assets[index];
        if (asset.buffer_cell == 0) {
            asset.buffer_cell = registry.AllocateData(
                static_cast<std::uint32_t>(asset.data.size()));
            if (asset.buffer_cell == 0) {
                call.Return(0);
                return;
            }
            call.CopyOut(asset.buffer_cell, asset.data.data(), asset.data.size());
        }
        call.Return(asset.buffer_cell);
    });
    registry.Add("AAsset_close", [&registry](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        if (index < g_assets.size()) {
            Asset& asset = g_assets[index];
            if (asset.buffer_cell != 0) {
                registry.ReleaseData(asset.buffer_cell);
                asset.buffer_cell = 0;
            }
            asset.closed = true;
        }
    });
    registry.Add("AAssetManager_openDir", [](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        std::string name;
        call.ReadCString(call.Arg(1), name, 4096);
        AssetDir dir;
        dir.names = g_apk != nullptr ? g_apk->ListAssets(name) : std::vector<std::string>();
        // The platform never fails this call: AAssetManager_openDir wraps
        // AssetManager2::OpenDir in a handler even when the directory does not
        // exist, so the guest gets a valid iterator whose first name is null.
        // Answering null for an empty listing is something an application cannot
        // tell apart from a missing directory.
        g_asset_dirs.push_back(std::move(dir));
        call.Return(static_cast<std::uint32_t>(g_asset_dirs.size()));
    });
    registry.Add("AAssetDir_getNextFileName", [&registry](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        if (index >= g_asset_dirs.size()) {
            call.Return(0);
            return;
        }
        AssetDir& dir = g_asset_dirs[index];
        if (dir.next >= dir.names.size()) {
            call.Return(0);
            return;
        }
        const std::string& name = dir.names[dir.next++];
        if (dir.cell == 0) {
            dir.cell = registry.AllocateData(kAssetDirCell);
        }
        if (dir.cell == 0) {
            call.Return(0);
            return;
        }
        const std::size_t length = std::min<std::size_t>(name.size(), kAssetDirCell - 1);
        call.CopyOut(dir.cell, name.data(), length);
        const char terminator = 0;
        call.CopyOut(dir.cell + static_cast<std::uint32_t>(length), &terminator, 1);
        call.Return(dir.cell);
    });
    registry.Add("AAssetDir_close", [&registry](HleCall& call) {
        const std::lock_guard asset_lock(g_asset_mutex);
        const std::size_t index = HandleSlot(call.Arg(0));
        if (index >= g_asset_dirs.size()) {
            return;
        }
        AssetDir& dir = g_asset_dirs[index];
        if (dir.cell != 0) {
            registry.ReleaseData(dir.cell);
            dir.cell = 0;
        }
        dir.names.clear();
        dir.next = 0;
    });

    // ---- AConfiguration ---------------------------------------------------
    registry.Add("AConfiguration_new", [&registry](HleCall& call) {
        call.Return(registry.AllocateData(8));
    });
    registry.Add("AConfiguration_delete", [](HleCall&) {});
    registry.Add("AConfiguration_fromAssetManager", [](HleCall& call) { call.Return(0); });
    for (const char* name : {"AConfiguration_getLanguage", "AConfiguration_getCountry"}) {
        const bool language = std::strcmp(name, "AConfiguration_getLanguage") == 0;
        registry.Add(name, [language](HleCall& call) {
            const char* text = language ? "en" : "US";
            call.CopyOut(call.Arg(1), text, 2);
        });
    }

    // ---- window and activity ---------------------------------------------
    registry.Add("ANativeWindow_setBuffersGeometry", [](HleCall& call) { call.Return(0); });
    // The only way a Java-based engine (Unity above all) turns the Surface the
    // framework handed it into the window EGL draws on. The guest calls it with
    // one of the bridge's object handles, so the host reference has to come back
    // out through the bridge first. Without this the engine gets a null window,
    // never creates an EGL surface and sits on an idle thread -- what looked
    // like a black screen with no error anywhere.
    registry.Add("ANativeWindow_fromSurface", [](HleCall& call) {
        using FromSurface = void* (*)(JNIEnv*, jobject);
        static const auto from_surface = [] {
            void* library = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
            return library != nullptr
                       ? reinterpret_cast<FromSurface>(::dlsym(library, "ANativeWindow_fromSurface"))
                       : nullptr;
        }();
        JniBridge* bridge = CurrentJniBridge();
        JNIEnv* env = bridge != nullptr ? bridge->HostEnvironment() : nullptr;
        jobject surface = bridge != nullptr ? bridge->ResolveGuestObject(call.Arg(1)) : nullptr;
        void* window = (env != nullptr && surface != nullptr && from_surface != nullptr)
                           ? from_surface(env, surface)
                           : nullptr;
        // One host window is one handle: an engine that asks for the window of
        // the same surface twice must not grow the table.
        const std::uint32_t handle = RegisterHostHandle(window);
        Log("ANativeWindow_fromSurface(handle=" + std::to_string(call.Arg(1)) + ") -> " +
            std::to_string(handle) + " host=" +
            std::to_string(reinterpret_cast<std::uintptr_t>(window)));
        call.Return(handle);
    });
    // The window outlives the guest's reference: it belongs to the view, and the
    // container keeps it for as long as the guest's surface exists.
    registry.Add("ANativeWindow_acquire", [](HleCall&) {});
    registry.Add("ANativeWindow_release", [](HleCall&) {});
    // The size of the window the framework handed over. An engine asks for it
    // before its first frame to size the framebuffer and the viewport; a zero
    // answer describes an empty surface.
    registry.Add("ANativeWindow_getWidth", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(WindowExtent(call.Arg(0), false)));
    });
    registry.Add("ANativeWindow_getHeight", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(WindowExtent(call.Arg(0), true)));
    });
    registry.Add("ANativeActivity_finish", [](HleCall&) { Log("guest requested finish"); });

    // Audio (OpenSL ES) has its own file: its objects need guest-side vtables,
    // which is a different kind of work from the flat calls above.
    RegisterOpenslesFunctions(registry);

    // ---- looper -----------------------------------------------------------
    registry.Add("ALooper_prepare", [](HleCall& call) {
        if (g_thread_looper == nullptr) {
            CreateThreadLooper();
        }
        call.Return(g_thread_looper != nullptr ? g_thread_looper->handle : 0u);
    });
    registry.Add("ALooper_forThread", [](HleCall& call) {
        call.Return(g_thread_looper != nullptr ? g_thread_looper->handle : 0u);
    });
    registry.Add("ALooper_acquire", [](HleCall&) {});
    registry.Add("ALooper_release", [](HleCall&) {});
    // A wake belongs to one looper: the byte goes into that looper's pipe and
    // its own poll answers ALOOPER_POLL_WAKE, which is what the platform
    // promises. Answering an ident instead turns every cut-short wait into a
    // phantom event that drains somebody else's queue.
    registry.Add("ALooper_wake", [](HleCall& call) {
        Looper* looper = LooperForHandle(call.Arg(0));
        if (looper == nullptr || looper->wake_write < 0) {
            return;
        }
        const char byte = 1;
        // A full pipe already carries a wake the reader has not taken yet.
        const ssize_t written = ::write(looper->wake_write, &byte, 1);
        (void)written;
    });
    registry.Add("ALooper_addFd", [](HleCall& call) {
        Looper* looper = LooperForHandle(call.Arg(0));
        if (looper == nullptr) {
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        const LooperSource source{call.Arg(1), call.Arg(2), call.Arg(3), call.Arg(4),
                                  call.Arg(5)};
        LooperAddSource(*looper, source);
        ARMBAT_LOG_INFO("Armbat",
                            "looper: addFd fd=%d ident=%d events=0x%x callback=0x%08x data=0x%08x",
                            static_cast<int>(source.fd), static_cast<int>(source.ident),
                            static_cast<unsigned>(source.events), source.callback, source.data);
        call.Return(1);
    });
    registry.Add("ALooper_removeFd", [](HleCall& call) {
        Looper* looper = LooperForHandle(call.Arg(0));
        const bool removed = looper != nullptr && LooperRemoveSource(*looper, call.Arg(1));
        call.Return(removed ? 1u : static_cast<std::uint32_t>(-1));
    });
    registry.Add("ALooper_pollOnce", [](HleCall& call) {
        call.Return(static_cast<std::uint32_t>(
            PollLooperOnce(call, static_cast<std::int32_t>(call.Arg(0)))));
    });
    registry.Add("ALooper_pollAll", [](HleCall& call) {
        std::int32_t result = PollLooperOnce(call, static_cast<std::int32_t>(call.Arg(0)));
        while (result == kLooperCallback) {
            result = PollLooperOnce(call, 0);
        }
        call.Return(static_cast<std::uint32_t>(result));
    });

    // ---- input ------------------------------------------------------------
    //
    // The container feeds the queue from the host's touch stream
    // (GuestNatives.touch); the archive drains it from its own loop. The wake
    // pipe is what makes ALooper_pollAll return LOOPER_ID_INPUT the way a
    // device's input queue does -- without it the loop sleeps through touches.
    registry.Add("AInputQueue_attachLooper", [](HleCall& call) {
        const int fd = GuestInputQueue::Instance().Attach();
        if (fd < 0) {
            return;
        }
        // The queue goes on the looper the caller names, exactly as on a device.
        Looper* looper = LooperForHandle(call.Arg(1));
        if (looper == nullptr) {
            looper = g_thread_looper != nullptr ? g_thread_looper : CreateThreadLooper();
        }
        if (looper == nullptr) {
            return;
        }
        // A queue has one consumer: the platform attaches it to one looper, and
        // a later attach moves it instead of leaving two readers behind. Two
        // readers split one gesture between them -- one takes the down, the
        // other the move -- and an archive waiting for the rest of that gesture
        // then waits for a touch that was already delivered to somebody else.
        const LooperSource source{static_cast<std::uint32_t>(fd), call.Arg(2),
                                  static_cast<std::uint32_t>(POLLIN), call.Arg(3), call.Arg(4)};
        {
            const std::lock_guard lock(Loopers().mutex);
            for (auto& entry : Loopers().by_handle) {
                if (entry.second.get() != looper) {
                    LooperRemoveSource(*entry.second, static_cast<std::uint32_t>(fd));
                }
            }
            LooperAddSource(*looper, source);
        }
        ARMBAT_LOG_INFO("Armbat",
                            "input: attachLooper looper=%u ident=%d callback=0x%08x data=0x%08x",
                            static_cast<unsigned>(looper->handle),
                            static_cast<int>(source.ident), source.callback, source.data);
    });
    registry.Add("AInputQueue_detachLooper", [](HleCall&) {
        const int fd = GuestInputQueue::Instance().ReadFd();
        if (fd < 0) {
            return;
        }
        {
            // One queue, one owner: the descriptor leaves every looper it was
            // attached to, so a re-attach cannot leave a stale reader behind.
            LooperTable& table = Loopers();
            const std::lock_guard lock(table.mutex);
            for (auto& entry : table.by_handle) {
                LooperRemoveSource(*entry.second, static_cast<std::uint32_t>(fd));
            }
        }
        GuestInputQueue::Instance().Detach(fd);
    });
    registry.Add("AInputQueue_getEvent", [&registry](HleCall& call) {
        InputEvent event;
        if (!GuestInputQueue::Instance().Pop(&event)) {
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        const std::uint32_t address = StoreInputEvent(registry, event);
        if (address == 0 || !call.CopyOut(call.Arg(1), &address, sizeof(address))) {
            ReleaseInputEvent(address);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(0);
    });
    // The IME lives in the host and never sees a guest event, so nothing is
    // dispatched for it: the archive handles every event itself.
    registry.Add("AInputQueue_preDispatchEvent", [](HleCall& call) { call.Return(0); });
    registry.Add("AInputQueue_finishEvent", [](HleCall& call) {
        ReleaseInputEvent(call.Arg(1));
    });

    registry.Add("AInputEvent_getType", [](HleCall& call) {
        const GuestInputEvent* event = InputEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->type) : 0u);
    });
    registry.Add("AInputEvent_getSource", [](HleCall& call) {
        const GuestInputEvent* event = InputEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->source) : 0u);
    });
    registry.Add("AInputEvent_getDeviceId", [](HleCall& call) {
        const GuestInputEvent* event = InputEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->device_id) : 0u);
    });
    registry.Add("AMotionEvent_getAction", [](HleCall& call) {
        const GuestInputEvent* event = MotionEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->action) : 0u);
    });
    registry.Add("AMotionEvent_getPointerCount", [](HleCall& call) {
        const GuestInputEvent* event = MotionEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->pointer_count) : 0u);
    });
    registry.Add("AMotionEvent_getPointerId", [](HleCall& call) {
        const GuestInputPointer* pointer = InputPointerAt(call, call.Arg(0), call.Arg(1));
        call.Return(pointer != nullptr ? static_cast<std::uint32_t>(pointer->id) : 0u);
    });
    const auto add_motion_float = [&registry](const char* name,
                                              float GuestInputPointer::*field) {
        registry.Add(name, [field](HleCall& call) {
            const GuestInputPointer* pointer = InputPointerAt(call, call.Arg(0), call.Arg(1));
            ReturnFloat(call, pointer != nullptr ? pointer->*field : 0.0f);
        });
    };
    add_motion_float("AMotionEvent_getX", &GuestInputPointer::x);
    add_motion_float("AMotionEvent_getY", &GuestInputPointer::y);
    add_motion_float("AMotionEvent_getRawX", &GuestInputPointer::raw_x);
    add_motion_float("AMotionEvent_getRawY", &GuestInputPointer::raw_y);
    add_motion_float("AMotionEvent_getPressure", &GuestInputPointer::pressure);
    add_motion_float("AMotionEvent_getSize", &GuestInputPointer::size);
    add_motion_float("AMotionEvent_getTouchMajor", &GuestInputPointer::touch_major);
    add_motion_float("AMotionEvent_getTouchMinor", &GuestInputPointer::touch_minor);
    add_motion_float("AMotionEvent_getToolMajor", &GuestInputPointer::touch_major);
    add_motion_float("AMotionEvent_getToolMinor", &GuestInputPointer::touch_minor);
    registry.Add("AMotionEvent_getToolType", [](HleCall& call) {
        const GuestInputPointer* pointer = InputPointerAt(call, call.Arg(0), call.Arg(1));
        call.Return(pointer != nullptr ? static_cast<std::uint32_t>(pointer->tool_type) : 0u);
    });
    // The container queues complete events: the host's historical samples are
    // already part of the move stream, so no event carries history of its own.
    registry.Add("AMotionEvent_getHistorySize", [](HleCall& call) { call.Return(0); });
    // Nanoseconds since boot, which is what the platform answers. The host's
    // events carry milliseconds, and a zero here made every frame look
    // simultaneous to a game that measures the time between two events.
    const auto add_motion_time = [](std::int64_t GuestInputEvent::*field) {
        return [field](HleCall& call) {
            const GuestInputEvent* event = MotionEventAt(call, call.Arg(0));
            const std::int64_t nanos = event != nullptr ? event->*field * 1000000 : 0;
            call.ReturnPair(static_cast<std::uint32_t>(nanos & 0xffffffffu),
                            static_cast<std::uint32_t>(nanos >> 32));
        };
    };
    registry.Add("AMotionEvent_getEventTime",
                 add_motion_time(&GuestInputEvent::event_time));
    registry.Add("AMotionEvent_getDownTime",
                 add_motion_time(&GuestInputEvent::down_time));
    // The platform keeps a short history of samples inside one move event; this
    // container queues one sample per event, so the history size is zero. An
    // archive that asks for a historical sample anyway (index 0 is the one a
    // batch-less stream implies) must get the sample the event carries, not a
    // zero: a zero coordinate puts the touch in the corner of the screen and
    // every hit test misses.
    const auto add_historical_float = [&registry](const char* name,
                                                  float GuestInputPointer::*field) {
        registry.Add(name, [field](HleCall& call) {
            const GuestInputPointer* pointer = InputPointerAt(call, call.Arg(0), call.Arg(1));
            ReturnFloat(call, pointer != nullptr ? pointer->*field : 0.0f);
        });
    };
    add_historical_float("AMotionEvent_getHistoricalX", &GuestInputPointer::x);
    add_historical_float("AMotionEvent_getHistoricalY", &GuestInputPointer::y);
    add_historical_float("AMotionEvent_getHistoricalRawX", &GuestInputPointer::raw_x);
    add_historical_float("AMotionEvent_getHistoricalRawY", &GuestInputPointer::raw_y);
    add_historical_float("AMotionEvent_getHistoricalPressure", &GuestInputPointer::pressure);
    add_historical_float("AMotionEvent_getHistoricalSize", &GuestInputPointer::size);
    registry.Add("AMotionEvent_getHistoricalEventTime",
                 add_motion_time(&GuestInputEvent::event_time));
    registry.Add("AMotionEvent_getMetaState", [](HleCall& call) {
        const GuestInputEvent* event = MotionEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->meta_state) : 0u);
    });
    registry.Add("AMotionEvent_getFlags", [](HleCall& call) {
        const GuestInputEvent* event = MotionEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->flags) : 0u);
    });
    registry.Add("AMotionEvent_getEdgeFlags", [](HleCall& call) {
        const GuestInputEvent* event = MotionEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->edge_flags) : 0u);
    });
    // The action index is the high byte of the packed action; an event that
    // names no pointer answers index 0, as the platform does.
    registry.Add("AMotionEvent_getActionIndex", [](HleCall& call) {
        const GuestInputEvent* event = MotionEventAt(call, call.Arg(0));
        call.Return(event != nullptr
                        ? static_cast<std::uint32_t>((event->action >> 8) & 0xff)
                        : 0u);
    });
    registry.Add("AMotionEvent_findPointerIndex", [](HleCall& call) {
        const GuestInputEvent* event = MotionEventAt(call, call.Arg(0));
        const auto wanted = static_cast<std::int32_t>(call.Arg(1));
        if (event != nullptr) {
            for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(event->pointer_count);
                 ++index) {
                const GuestInputPointer* pointer = InputPointerAt(call, call.Arg(0), index);
                if (pointer != nullptr && pointer->id == wanted) {
                    call.Return(index);
                    return;
                }
            }
        }
        call.Return(static_cast<std::uint32_t>(-1));
    });
    // The guest's window covers the whole virtual screen, so no event is ever
    // offset from it; only the precision is the identity of the units.
    for (const char* name : {"AMotionEvent_getXOffset", "AMotionEvent_getYOffset"}) {
        registry.Add(name, [](HleCall& call) { ReturnFloat(call, 0.0f); });
    }
    for (const char* name : {"AMotionEvent_getXPrecision", "AMotionEvent_getYPrecision"}) {
        registry.Add(name, [](HleCall& call) { ReturnFloat(call, 1.0f); });
    }
    // AMOTION_EVENT_ORIENTATION_0: the touch surface is upright, which is what
    // the guest's virtual screen is, whatever the device's rotation.
    registry.Add("AMotionEvent_getOrientation", [](HleCall& call) { call.Return(0); });
    registry.Add("AMotionEvent_getAxisValue", [](HleCall& call) {
        const GuestInputPointer* pointer = InputPointerAt(call, call.Arg(0), call.Arg(2));
        if (pointer == nullptr) {
            ReturnFloat(call, 0.0f);
            return;
        }
        float value = 0.0f;
        switch (static_cast<int>(call.Arg(1))) {
            case 0: value = pointer->x; break;            // AMOTION_EVENT_AXIS_X
            case 1: value = pointer->y; break;            // AMOTION_EVENT_AXIS_Y
            case 2: value = pointer->pressure; break;     // AMOTION_EVENT_AXIS_PRESSURE
            case 3: value = pointer->size; break;         // AMOTION_EVENT_AXIS_SIZE
            case 4: value = pointer->touch_major; break;  // AMOTION_EVENT_AXIS_TOUCH_MAJOR
            case 5: value = pointer->touch_minor; break;  // AMOTION_EVENT_AXIS_TOUCH_MINOR
            case 6: value = pointer->touch_major; break;  // AMOTION_EVENT_AXIS_TOOL_MAJOR
            case 7: value = pointer->touch_minor; break;  // AMOTION_EVENT_AXIS_TOOL_MINOR
            default: break;
        }
        ReturnFloat(call, value);
    });
    registry.Add("AKeyEvent_getAction", [](HleCall& call) {
        const GuestInputEvent* event = KeyEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->action) : 0u);
    });
    registry.Add("AKeyEvent_getKeyCode", [](HleCall& call) {
        const GuestInputEvent* event = KeyEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->key_code) : 0u);
    });
    registry.Add("AKeyEvent_getMetaState", [](HleCall& call) {
        const GuestInputEvent* event = KeyEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->meta_state) : 0u);
    });
    registry.Add("AKeyEvent_getRepeatCount", [](HleCall& call) {
        const GuestInputEvent* event = KeyEventAt(call, call.Arg(0));
        call.Return(event != nullptr ? static_cast<std::uint32_t>(event->repeat_count) : 0u);
    });

    // ---- EGL --------------------------------------------------------------
    //
    // EGL objects are host pointers but only 32 bits wide in the guest, so every
    // display, config, context and surface crosses the boundary through the
    // shared handle table. Attribute lists are int arrays and are copied.
    registry.Add("eglGetDisplay", [](HleCall& call) {
        using Function = void* (*)(void*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglGetDisplay"));
        void* display =
            function != nullptr
                ? function(reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.Arg(0))))
                : nullptr;
        const std::uint32_t handle = RegisterHostHandle(display);
        Log("eglGetDisplay(" + std::to_string(call.Arg(0)) + ") -> handle " +
            std::to_string(handle) + " host " + std::to_string(reinterpret_cast<std::uintptr_t>(display)));
        call.Return(handle);
    });
    registry.Add("eglInitialize", [](HleCall& call) {
        using Function = int (*)(void*, int*, int*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglInitialize"));
        void* display = LookupHostHandle(call.Arg(0));
        int major = 0;
        int minor = 0;
        const int result =
            (function != nullptr && display != nullptr) ? function(display, &major, &minor) : 0;
        if (call.Arg(1) != 0) {
            call.CopyOut(call.Arg(1), &major, sizeof(major));
        }
        if (call.Arg(2) != 0) {
            call.CopyOut(call.Arg(2), &minor, sizeof(minor));
        }
        Log("eglInitialize(display=" + std::to_string(call.Arg(0)) + ") -> " +
            std::to_string(result) + " " + std::to_string(major) + "." + std::to_string(minor));
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglChooseConfig", [](HleCall& call) {
        using Function = int (*)(void*, const int*, void**, int, int*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglChooseConfig"));
        void* display = LookupHostHandle(call.Arg(0));
        const std::vector<std::int32_t> attribs = ReadInts(call, call.Arg(1), 64);
        const int count = static_cast<int>(call.Arg(3));
        // Ask for a full page of configs even when the caller only wants the
        // count (config_size == 0); some implementations report zero matches
        // when the output array is too small to hold them.
        const int capacity = std::max(count, 64);
        std::vector<void*> configs(static_cast<std::size_t>(capacity), nullptr);
        int found = 0;
        const int result =
            (function != nullptr && display != nullptr)
                ? function(display, attribs.data(), configs.data(), capacity, &found)
                : 0;
        if (result != 0 && call.Arg(2) != 0 && count > 0) {
            const int produced = std::min(found, count);
            std::vector<std::uint32_t> handles(static_cast<std::size_t>(produced), 0);
            for (int i = 0; i < produced; ++i) {
                handles[static_cast<std::size_t>(i)] =
                    RegisterHostHandle(configs[static_cast<std::size_t>(i)]);
            }
            call.CopyOut(call.Arg(2), handles.data(), handles.size() * sizeof(std::uint32_t));
        }
        if (call.Arg(4) != 0) {
            call.CopyOut(call.Arg(4), &found, sizeof(found));
        }
        std::string line = "eglChooseConfig(display=" + std::to_string(call.Arg(0)) + " count=" +
                           std::to_string(count) + ") -> " + std::to_string(result) + " found=" +
                           std::to_string(found) + " err=";
        {
            using GetError = int (*)(void);
            const auto get_error = reinterpret_cast<GetError>(EglSymbol("eglGetError"));
            line += std::to_string(get_error != nullptr ? get_error() : -1);
        }
        line += " attribs:";
        for (const std::int32_t value : attribs) {
            line += " " + std::to_string(value);
        }
        Log(line);
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglGetConfigAttrib", [](HleCall& call) {
        using Function = int (*)(void*, void*, int, int*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglGetConfigAttrib"));
        void* config = LookupHostHandle(call.Arg(1));
        int value = 0;
        const int result =
            (function != nullptr && config != nullptr)
                ? function(LookupHostHandle(call.Arg(0)), config, static_cast<int>(call.Arg(2)),
                           &value)
                : 0;
        if (call.Arg(3) != 0) {
            call.CopyOut(call.Arg(3), &value, sizeof(value));
        }
        Log("eglGetConfigAttrib(config=" + std::to_string(call.Arg(1)) + " attr=" +
            std::to_string(call.Arg(2)) + ") -> " + std::to_string(result) + " value=" +
            std::to_string(value));
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglCreateContext", [](HleCall& call) {
        using Function = void* (*)(void*, void*, void*, const int*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglCreateContext"));
        std::vector<std::int32_t> attribs = ReadInts(call, call.Arg(3), 64);
        // The container forwards GLES 2.0 only: ask for a 2.0 context even when
        // the guest would prefer a newer one, so the driver never hands back a
        // context whose ES3 entry points we cannot serve.
        for (std::size_t i = 0; i + 1 < attribs.size(); i += 2) {
            if (attribs[i] == 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */ && attribs[i + 1] > 2) {
                attribs[i + 1] = 2;
            }
        }
        std::string attrib_text;
        for (const std::int32_t value : attribs) {
            attrib_text += " " + std::to_string(value);
        }
        void* context = function != nullptr
                            ? function(LookupHostHandle(call.Arg(0)),
                                       LookupHostHandle(call.Arg(1)),
                                       LookupHostHandle(call.Arg(2)), attribs.data())
                            : nullptr;
        const std::uint32_t handle = RegisterHostHandle(context);
        Log("eglCreateContext(display=" + std::to_string(call.Arg(0)) + " config=" +
            std::to_string(call.Arg(1)) + " share=" + std::to_string(call.Arg(2)) +
            ") -> " + std::to_string(handle) + " attribs:" + attrib_text);
        call.Return(handle);
    });
    registry.Add("eglCreateWindowSurface", [](HleCall& call) {
        using Function = void* (*)(void*, void*, void*, const int*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglCreateWindowSurface"));
        const std::vector<std::int32_t> attribs = ReadInts(call, call.Arg(3), 64);
        // The third argument is the guest's ANativeWindow handle, not the host
        // window pointer: the container registered that pointer when the
        // framework created the surface.
        void* surface = function != nullptr
                            ? function(LookupHostHandle(call.Arg(0)),
                                       LookupHostHandle(call.Arg(1)),
                                       LookupHostHandle(call.Arg(2)), attribs.data())
                            : nullptr;
        const std::uint32_t handle = RegisterHostHandle(surface);
        Log("eglCreateWindowSurface(display=" + std::to_string(call.Arg(0)) + " config=" +
            std::to_string(call.Arg(1)) + " window=" + std::to_string(call.Arg(2)) + ") -> " +
            std::to_string(handle));
        call.Return(handle);
    });
    registry.Add("eglCreatePbufferSurface", [](HleCall& call) {
        using Function = void* (*)(void*, void*, const int*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglCreatePbufferSurface"));
        const std::vector<std::int32_t> attribs = ReadInts(call, call.Arg(2), 64);
        void* surface = function != nullptr
                            ? function(LookupHostHandle(call.Arg(0)),
                                       LookupHostHandle(call.Arg(1)), attribs.data())
                            : nullptr;
        call.Return(RegisterHostHandle(surface));
    });
    registry.Add("eglMakeCurrent", [](HleCall& call) {
        using Function = int (*)(void*, void*, void*, void*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglMakeCurrent"));
        const int result = function != nullptr
                               ? function(LookupHostHandle(call.Arg(0)),
                                          LookupHostHandle(call.Arg(1)),
                                          LookupHostHandle(call.Arg(2)),
                                          LookupHostHandle(call.Arg(3)))
                               : 0;
        Log("eglMakeCurrent(display=" + std::to_string(call.Arg(0)) + " draw=" +
            std::to_string(call.Arg(1)) + " read=" + std::to_string(call.Arg(2)) + " context=" +
            std::to_string(call.Arg(3)) + ") -> " + std::to_string(result));
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglSwapBuffers", [](HleCall& call) {
        using Function = int (*)(void*, void*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglSwapBuffers"));
        const int result = function != nullptr
                               ? function(LookupHostHandle(call.Arg(0)),
                                          LookupHostHandle(call.Arg(1)))
                               : 0;
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglSwapInterval", [](HleCall& call) {
        using Function = int (*)(void*, int);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglSwapInterval"));
        const int result = function != nullptr
                               ? function(LookupHostHandle(call.Arg(0)),
                                          static_cast<int>(call.Arg(1)))
                               : 0;
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglDestroyContext", [](HleCall& call) {
        using Function = int (*)(void*, void*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglDestroyContext"));
        const int result = function != nullptr
                               ? function(LookupHostHandle(call.Arg(0)),
                                          LookupHostHandle(call.Arg(1)))
                               : 0;
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglDestroySurface", [](HleCall& call) {
        using Function = int (*)(void*, void*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglDestroySurface"));
        const int result = function != nullptr
                               ? function(LookupHostHandle(call.Arg(0)),
                                          LookupHostHandle(call.Arg(1)))
                               : 0;
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglTerminate", [](HleCall& call) {
        using Function = int (*)(void*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglTerminate"));
        const int result =
            function != nullptr ? function(LookupHostHandle(call.Arg(0))) : 0;
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglGetError", [](HleCall& call) {
        using Function = int (*)(void);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglGetError"));
        call.Return(static_cast<std::uint32_t>(function != nullptr ? function() : 0));
    });
    registry.Add("eglQuerySurface", [](HleCall& call) {
        using Function = int (*)(void*, void*, int, int*);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglQuerySurface"));
        int value = 0;
        const int result = function != nullptr
                               ? function(LookupHostHandle(call.Arg(0)),
                                          LookupHostHandle(call.Arg(1)),
                                          static_cast<int>(call.Arg(2)), &value)
                               : 0;
        if (call.Arg(3) != 0) {
            call.CopyOut(call.Arg(3), &value, sizeof(value));
        }
        call.Return(static_cast<std::uint32_t>(result));
    });
    registry.Add("eglGetCurrentContext", [](HleCall& call) {
        using Function = void* (*)(void);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglGetCurrentContext"));
        call.Return(RegisterHostHandle(function != nullptr ? function() : nullptr));
    });
    registry.Add("eglGetCurrentSurface", [](HleCall& call) {
        using Function = void* (*)(int);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglGetCurrentSurface"));
        call.Return(RegisterHostHandle(
            function != nullptr ? function(static_cast<int>(call.Arg(0))) : nullptr));
    });
    registry.Add("eglGetCurrentDisplay", [](HleCall& call) {
        using Function = void* (*)(void);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglGetCurrentDisplay"));
        call.Return(RegisterHostHandle(function != nullptr ? function() : nullptr));
    });
    for (const char* name : {"eglReleaseThread", "eglWaitClient", "eglWaitGL", "eglWaitNative"}) {
        registry.Add(name, [](HleCall& call) { call.Return(1); });
    }
    registry.Add("eglBindAPI", [](HleCall& call) { call.Return(1); });
    registry.Add("eglQueryAPI", [](HleCall& call) { call.Return(0x30a0u); });  // EGL_OPENGL_ES_API
    registry.Add("eglQueryString", [&registry](HleCall& call) {
        using Function = const char* (*)(void*, int);
        const auto function = reinterpret_cast<Function>(EglSymbol("eglQueryString"));
        if (function == nullptr) {
            call.Return(0);
            return;
        }
        const int attribute = static_cast<int>(call.Arg(1));
        const char* text = function(LookupHostHandle(call.Arg(0)), attribute);
        if (text == nullptr) {
            call.Return(0);
            return;
        }
        // The string lives in the host library, so it has to be copied where the
        // guest can reach it. Keep one cell per attribute: callers do hold on to
        // EGL_EXTENSIONS and the like.
        struct CachedString {
            std::uint32_t cell = 0;
            std::string text;
        };
        static std::unordered_map<int, CachedString> cache;
        CachedString& entry = cache[attribute];
        const std::size_t length = std::strlen(text);
        if (entry.cell == 0 || entry.text != std::string(text, length)) {
            if (entry.cell != 0) {
                registry.ReleaseData(entry.cell);
            }
            entry.text.assign(text, length);
            entry.cell = registry.AllocateData(static_cast<std::uint32_t>(length + 1));
            if (entry.cell == 0) {
                call.Return(0);
                return;
            }
            call.CopyOut(entry.cell, entry.text.c_str(), entry.text.size() + 1);
        }
        Log("eglQueryString(attr=" + std::to_string(attribute) + "): " +
            entry.text.substr(0, 120));
        call.Return(entry.cell);
    });
    registry.Add("eglGetProcAddress", [&registry](HleCall& call) {
        std::string name;
        if (!call.ReadCString(call.Arg(0), name, 256)) {
            call.Return(0);
            return;
        }
        const std::uint32_t address = registry.Resolve(name);
        // A null here is a function the guest then cannot call, and the only
        // other trace of it is the silence that follows.
        static std::mutex seen_mutex;
        static std::set<std::string> seen;
        bool first = false;
        {
            const std::lock_guard lock(seen_mutex);
            first = seen.size() < 200 && seen.insert(name).second;
        }
        if (first) {
            Log("eglGetProcAddress(" + name + ") -> " +
                (address != 0 ? std::to_string(address) : std::string("null")));
        }
        call.Return(address);
    });

    RegisterGlesFunctions(registry);
}

}  // namespace armbat
