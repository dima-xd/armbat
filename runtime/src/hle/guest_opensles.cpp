#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>

#include "armbat/log.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "armbat/guest_arena.h"
#include "armbat/hle.h"

// OpenSL ES is the one platform API the container cannot forward by pointer: the
// guest calls an object through a 32-bit vtable of ARMv7 addresses, while the
// platform's own objects live above the 4 GiB the guest can address. So the
// guest gets a block per object and per interface, and every method of every
// interface is forwarded to the platform object it stands for.
//
// The container therefore keeps no OpenSL ES state of its own: no realized
// flag, no play state, no queue depth, no volume, no pacing. Those were exactly
// the defects of the previous implementation -- a Clear that did not clear, a
// volume that never reached the device, an Enqueue that blocked instead of
// reporting a full queue, an interface that silently answered "success" to
// every unknown method. The platform implements all of it, and the guest was
// written against the platform's behaviour, so the less this file does itself
// the closer the guest gets to a real device.
//
// What is left here is the ABI adapter:
//
//  * a guest block [vtable][self] built from HLE stubs, so the guest can
//    execute the table it holds;
//  * guest SLInterfaceID -> platform SLInterfaceID, and SLDataSource/SLDataSink
//    translated into host structs, because they carry pointers whose width
//    differs between the two ABIs;
//  * guest PCM handed over as it is: the arena is mapped in this process, so
//    Enqueue passes the guest's own buffer and nothing is copied;
//  * callbacks the other way, delivered on a container thread, because the
//    platform calls them from its audio thread and only the container can hand
//    a host thread a guest context.

namespace armbat {
namespace {

// Vtables are longer than the largest real interface: a slot past the end of the
// struct is never called, and a slot past the end of the block would be a jump
// into whatever follows. Forty covers OpenSL ES 1.0.1 plus the Android
// extensions.
constexpr std::uint32_t kSlotsPerVtable = 40;
constexpr std::uint32_t kBlockBytes = 8;

// Every shim object (SL_IID_*) is a word pointing at a descriptor. The generator
// stamps the descriptor with this magic and the interface's own name, which is
// how the container tells SL_IID_PLAY from SL_IID_VOLUME without a private table
// of addresses that changes with the load address of the shim.
constexpr std::uint32_t kDescriptorMagic = 0x41524254u;  // "ARBT"
constexpr std::uint32_t kDescriptorNameOffset = 4;

// The vtable a block is built from. The name is stable, so a block compares its
// key by pointer-sized string identity instead of carrying an enum.
constexpr const char* kKeyObject = "object";
constexpr const char* kKeyEngine = "engine";
constexpr const char* kKeyPlay = "play";
constexpr const char* kKeyRecord = "record";
constexpr const char* kKeyVolume = "volume";
constexpr const char* kKeyBufferQueue = "bufferqueue";
constexpr const char* kKeyAndroidBufferQueue = "androidbufferqueue";
constexpr const char* kKeyConfiguration = "configuration";
constexpr const char* kKeyOutputMix = "outputmix";

// An interface the container can wrap: the guest's name for it, the platform's
// own SLInterfaceID, and the vtable layout. Anything absent from this table is
// answered with SL_RESULT_FEATURE_UNSUPPORTED rather than with a table of
// no-ops, so a caller can still fall back to what it does on a device without
// the feature.
struct InterfaceInfo {
    const char* name;
    SLInterfaceID id;
    const char* key;
};

const InterfaceInfo kInterfaces[] = {
    {"SL_IID_OBJECT", SL_IID_OBJECT, kKeyObject},
    {"SL_IID_ENGINE", SL_IID_ENGINE, kKeyEngine},
    {"SL_IID_PLAY", SL_IID_PLAY, kKeyPlay},
    {"SL_IID_RECORD", SL_IID_RECORD, kKeyRecord},
    {"SL_IID_VOLUME", SL_IID_VOLUME, kKeyVolume},
    {"SL_IID_BUFFERQUEUE", SL_IID_BUFFERQUEUE, kKeyBufferQueue},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
     kKeyAndroidBufferQueue},
    {"SL_IID_ANDROIDCONFIGURATION", SL_IID_ANDROIDCONFIGURATION, kKeyConfiguration},
    {"SL_IID_OUTPUTMIX", SL_IID_OUTPUTMIX, kKeyOutputMix},
};

template <typename T, std::size_t N>
constexpr std::size_t Count(const T (&)[N]) {
    return N;
}

bool KeyIs(const char* key, const char* wanted) {
    return key != nullptr && std::strcmp(key, wanted) == 0;
}

void LogOnce(const char* category, const std::string& line) {
    if (LogAllowed(category, 1)) {
        ARMBAT_LOG_INFO("Armbat", "%s", line.c_str());
    }
}

// The method order of each interface, taken from SLES/OpenSLES.h so a slot can
// never drift: the guest executes whatever address the table holds, and a wrong
// one is a jump to an unrelated method rather than a wrong return value.
struct Slot {
    std::uint32_t index;
    const char* name;
};

struct SlotTable {
    const char* key;
    const Slot* slots;
    std::size_t count;
};

const Slot kObjectSlots[] = {
    {0, "sl.object.Realize"},
    {1, "sl.object.Resume"},
    {2, "sl.object.GetState"},
    {3, "sl.object.GetInterface"},
    {5, "sl.object.AbortAsyncOperation"},
    {6, "sl.object.Destroy"},
    {7, "sl.object.SetPriority"},
    {8, "sl.object.GetPriority"},
    {9, "sl.object.SetLossOfControlInterfaces"},
};

const Slot kEngineSlots[] = {
    {2, "sl.engine.CreateAudioPlayer"},
    {3, "sl.engine.CreateAudioRecorder"},
    {7, "sl.engine.CreateOutputMix"},
    {10, "sl.engine.QueryNumSupportedInterfaces"},
    {12, "sl.engine.QueryNumSupportedExtensions"},
    {13, "sl.engine.QuerySupportedExtension"},
    {14, "sl.engine.IsExtensionSupported"},
};

const Slot kPlaySlots[] = {
    {0, "sl.play.SetPlayState"},
    {1, "sl.play.GetPlayState"},
    {2, "sl.play.GetDuration"},
    {3, "sl.play.GetPosition"},
    {4, "sl.play.RegisterCallback"},
    {5, "sl.play.SetCallbackEventsMask"},
    {6, "sl.play.GetCallbackEventsMask"},
    {7, "sl.play.SetMarkerPosition"},
    {8, "sl.play.ClearMarkerPosition"},
    {9, "sl.play.GetMarkerPosition"},
    {10, "sl.play.SetPositionUpdatePeriod"},
    {11, "sl.play.GetPositionUpdatePeriod"},
};

const Slot kRecordSlots[] = {
    {0, "sl.record.SetRecordState"},
    {1, "sl.record.GetRecordState"},
    {2, "sl.record.SetDurationLimit"},
    {3, "sl.record.GetPosition"},
    {4, "sl.record.RegisterCallback"},
    {5, "sl.record.SetCallbackEventsMask"},
    {6, "sl.record.GetCallbackEventsMask"},
    {7, "sl.record.SetMarkerPosition"},
    {8, "sl.record.ClearMarkerPosition"},
    {9, "sl.record.GetMarkerPosition"},
    {10, "sl.record.SetPositionUpdatePeriod"},
    {11, "sl.record.GetPositionUpdatePeriod"},
};

const Slot kVolumeSlots[] = {
    {0, "sl.volume.SetVolumeLevel"},
    {1, "sl.volume.GetVolumeLevel"},
    {2, "sl.volume.GetMaxVolumeLevel"},
    {3, "sl.volume.SetMute"},
    {4, "sl.volume.GetMute"},
    {5, "sl.volume.EnableStereoPosition"},
    {6, "sl.volume.IsEnabledStereoPosition"},
    {7, "sl.volume.SetStereoPosition"},
    {8, "sl.volume.GetStereoPosition"},
};

// SLBufferQueueItf and SLAndroidSimpleBufferQueueItf agree on their first four
// slots; the platform's Android queue is the one games use.
const Slot kQueueSlots[] = {
    {0, "sl.queue.Enqueue"},
    {1, "sl.queue.Clear"},
    {2, "sl.queue.GetState"},
    {3, "sl.queue.RegisterCallback"},
};

const Slot kConfigurationSlots[] = {
    {0, "sl.configuration.SetConfiguration"},
    {1, "sl.configuration.GetConfiguration"},
    {2, "sl.configuration.AcquireJavaProxy"},
    {3, "sl.configuration.ReleaseJavaProxy"},
};

const Slot kOutputMixSlots[] = {
    {0, "sl.outputmix.GetDestinationOutputDeviceIDs"},
    {1, "sl.outputmix.RegisterDeviceChangeCallback"},
    {2, "sl.outputmix.ReRoute"},
};

const SlotTable kSlotTables[] = {
    {kKeyObject, kObjectSlots, Count(kObjectSlots)},
    {kKeyEngine, kEngineSlots, Count(kEngineSlots)},
    {kKeyPlay, kPlaySlots, Count(kPlaySlots)},
    {kKeyRecord, kRecordSlots, Count(kRecordSlots)},
    {kKeyVolume, kVolumeSlots, Count(kVolumeSlots)},
    {kKeyBufferQueue, kQueueSlots, Count(kQueueSlots)},
    {kKeyAndroidBufferQueue, kQueueSlots, Count(kQueueSlots)},
    {kKeyConfiguration, kConfigurationSlots, Count(kConfigurationSlots)},
    {kKeyOutputMix, kOutputMixSlots, Count(kOutputMixSlots)},
};

enum class Kind : std::uint32_t { Engine, OutputMix, Player, Recorder };

// One guest block: [vtable][object]. `host` is the platform interface pointer
// this block stands for, so a forwarding method needs nothing but the pointer
// the guest called it with. `generation` is what lets a callback notice that its
// block was destroyed and the address is now somebody else's.
struct Block {
    std::uint32_t address = 0;
    std::uint32_t object = 0;
    const char* key = "";
    const void* host = nullptr;
    std::uint32_t callback = 0;
    std::uint32_t context = 0;
    std::uint64_t generation = 0;
};

struct GuestObject {
    std::uint32_t address = 0;
    SLObjectItf host = nullptr;
    Kind kind = Kind::Engine;
    // An interface is handed out once: the platform returns the same pointer for
    // a repeated GetInterface, and an engine that compares them expects that.
    std::unordered_map<std::string, std::shared_ptr<Block>> interfaces;
};

HleRegistry* g_registry = nullptr;

// Guest threads reach these objects concurrently: the mixer thread drives the
// player while the application thread realizes and queries it. The lock only
// guards the maps -- no platform call is made while it is held, so a Destroy
// that blocks cannot stall another thread's audio.
std::mutex g_mutex;
std::unordered_map<std::uint32_t, std::shared_ptr<GuestObject>> g_objects;
std::unordered_map<std::uint32_t, std::shared_ptr<Block>> g_blocks;
std::unordered_map<std::string, std::uint32_t> g_vtables;
std::uint64_t g_generation = 0;

// ---- callbacks -----------------------------------------------------------
//
// The platform delivers play and record callbacks on threads of its own; one
// container thread runs those, in the order the platform produced them, which is
// the order the guest's mixer expects. The buffer queue is the exception and
// runs on the platform's thread -- see RunCallbackHere.
struct CallbackRequest {
    std::uint32_t callback = 0;
    std::uint32_t itf = 0;
    std::uint32_t context = 0;
    std::uint32_t event = 0;
    std::uint64_t generation = 0;
    bool has_event = false;
};

GuestCallRunner g_guest_call;
std::mutex g_callback_mutex;
std::condition_variable g_callback_ready;
std::condition_variable g_callback_idle;
std::deque<CallbackRequest> g_callback_queue;
std::thread g_callback_thread;
bool g_callback_stopping = false;
unsigned g_callbacks_running = 0;

// Whether the guest may still be entered with the block a request names.
bool CallbackStillCurrent(const CallbackRequest& request) {
    const std::lock_guard lock(g_mutex);
    const auto block = g_blocks.find(request.itf);
    return block != g_blocks.end() && block->second->generation == request.generation;
}

// Runs one callback on the thread the platform called it from.
//
// The buffer queue is the one callback that must not be moved off that thread:
// the platform calls it from its audio thread and, before returning to its own
// work, decides whether the head has reached the end of the stream. A refill
// that arrives from another thread is too late, the queue is still empty, and
// the platform raises SL_PLAYEVENT_HEADATEND at every buffer boundary. Measured
// here with the same player twice: refilling inside the callback gave 0
// head-at-end events in 3 s, refilling 3 ms later from another thread gave 23.
// A guest that keeps one buffer in flight -- this one does -- is then told after
// its first buffer that the sound ended, and stops it: every dialogue and sound
// effect was cut after two buffers, while the looping ambiences, which ignore
// the event, played on.
bool RunCallbackHere(const CallbackRequest& request) {
    GuestCallRunner runner;
    {
        const std::lock_guard lock(g_callback_mutex);
        if (g_callback_stopping || !g_guest_call) {
            return false;
        }
        runner = g_guest_call;
        ++g_callbacks_running;
    }
    if (!CallbackStillCurrent(request)) {
        const std::lock_guard lock(g_callback_mutex);
        --g_callbacks_running;
        return false;
    }
    if (request.has_event) {
        runner(request.callback, {request.itf, request.context, request.event});
    } else {
        runner(request.callback, {request.itf, request.context});
    }
    {
        const std::lock_guard lock(g_callback_mutex);
        --g_callbacks_running;
    }
    g_callback_idle.notify_all();
    return true;
}

void CallbackLoop() {
    for (;;) {
        CallbackRequest request;
        GuestCallRunner runner;
        {
            std::unique_lock lock(g_callback_mutex);
            g_callback_ready.wait(
                lock, [] { return g_callback_stopping || !g_callback_queue.empty(); });
            if (g_callback_stopping) {
                return;
            }
            request = g_callback_queue.front();
            g_callback_queue.pop_front();
            runner = g_guest_call;
        }
        // The object may have been destroyed between the platform raising the
        // event and this thread delivering it. The generation tells a block from
        // a later one that happens to reuse the address.
        if (!CallbackStillCurrent(request) || !runner) {
            continue;
        }
        if (request.has_event) {
            runner(request.callback, {request.itf, request.context, request.event});
        } else {
            runner(request.callback, {request.itf, request.context});
        }
    }
}

// One request per event the platform reports. The cap only keeps a stalled guest
// from growing the queue without bound: a guest that falls 64 callbacks behind
// has stopped mixing, and dropping is then the only answer that does not cost
// memory without a limit.
void PostCallback(const CallbackRequest& request) {
    const std::lock_guard lock(g_callback_mutex);
    if (g_callback_stopping || g_callback_queue.size() >= 64) {
        return;
    }
    if (!g_callback_thread.joinable()) {
        g_callback_thread = std::thread(CallbackLoop);
    }
    g_callback_queue.push_back(request);
    g_callback_ready.notify_one();
}

// The block address a trampoline was registered with, and back.
void* BlockContext(std::uint32_t address) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
}

std::uint32_t ContextBlock(const void* context) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(context));
}

void WriteU32(HleCall& call, std::uint32_t address, std::uint32_t value) {
    if (address != 0) {
        call.CopyOut(address, &value, sizeof(value));
    }
}

// SLmillibel and SLpermille are 16-bit; a 4-byte write past such an out
// parameter once clobbered the caller's next stack slot.
void WriteI16(HleCall& call, std::uint32_t address, std::int16_t value) {
    if (address != 0) {
        call.CopyOut(address, &value, sizeof(value));
    }
}

// A guest address of a 32-bit out parameter, or nullptr. The arena is host
// memory, so a translated structure or scalar can be written in place -- except
// that an unmapped address has to be refused rather than handed to the platform.
std::uint32_t* OutU32(HleCall& call, std::uint32_t address) {
    if (address == 0) {
        return nullptr;
    }
    return reinterpret_cast<std::uint32_t*>(call.Mapped(address, sizeof(std::uint32_t)));
}

// ---- interface identity --------------------------------------------------

const InterfaceInfo* FindInterface(const std::string& name) {
    for (const InterfaceInfo& info : kInterfaces) {
        if (name == info.name) {
            return &info;
        }
    }
    return nullptr;
}

// The name behind a shim descriptor, or false when the guest handed over
// something that is not one (its own copy of the OpenSL headers, for instance).
bool ReadShimName(HleCall& call, std::uint32_t iid, std::string& name) {
    std::uint32_t magic = 0;
    std::uint32_t name_address = 0;
    if (!call.CopyIn(iid, &magic, sizeof(magic)) || magic != kDescriptorMagic) {
        return false;
    }
    if (!call.CopyIn(iid + kDescriptorNameOffset, &name_address, sizeof(name_address)) ||
        name_address == 0) {
        return false;
    }
    return call.ReadCString(name_address, name, 48);
}

// The UUID itself. `SLInterfaceID_` is 16 bytes with no padding, and the layout
// has not changed since OpenSL ES 1.0, so a guest that brings its own copy of the
// constants finds the same entry.
const InterfaceInfo* FindInterfaceByUuid(HleCall& call, std::uint32_t iid) {
    std::uint8_t bytes[16] = {};
    if (!call.CopyIn(iid, bytes, sizeof(bytes))) {
        return nullptr;
    }
    for (const InterfaceInfo& info : kInterfaces) {
        if (std::memcmp(bytes, info.id, sizeof(bytes)) == 0) {
            return &info;
        }
    }
    return nullptr;
}

const InterfaceInfo* ResolveInterface(HleCall& call, std::uint32_t iid, std::string* name) {
    if (iid == 0) {
        return nullptr;
    }
    std::string shim;
    if (ReadShimName(call, iid, shim)) {
        if (name != nullptr) {
            *name = shim;
        }
        return FindInterface(shim);
    }
    if (name != nullptr) {
        *name = "unknown";
    }
    return FindInterfaceByUuid(call, iid);
}

std::uint32_t Stub(const char* name) {
    return g_registry != nullptr ? g_registry->Resolve(name) : 0;
}

// Builds (once) the guest-side table for one interface key. Called with g_mutex
// held.
std::uint32_t VtableFor(const char* key) {
    const auto cached = g_vtables.find(key);
    if (cached != g_vtables.end()) {
        return cached->second;
    }
    if (g_registry == nullptr) {
        return 0;
    }
    const std::uint32_t unsupported = Stub("sl.unsupported");
    if (unsupported == 0) {
        return 0;
    }
    // A slot the container does not forward answers FEATURE_UNSUPPORTED. Saying
    // "success" to a method with out-parameters nobody wrote is how a caller ends
    // up trusting garbage.
    std::uint32_t words[kSlotsPerVtable] = {};
    for (std::uint32_t& word : words) {
        word = unsupported;
    }
    for (const SlotTable& table : kSlotTables) {
        if (!KeyIs(table.key, key)) {
            continue;
        }
        for (std::size_t i = 0; i < table.count; ++i) {
            const Slot& slot = table.slots[i];
            if (slot.index >= kSlotsPerVtable) {
                continue;
            }
            const std::uint32_t stub = Stub(slot.name);
            if (stub != 0) {
                words[slot.index] = stub;
            }
        }
    }
    const std::uint32_t address = g_registry->AllocateData(sizeof(words));
    if (address == 0) {
        return 0;
    }
    std::memcpy(g_registry->Arena().Ptr(address), words, sizeof(words));
    g_vtables.emplace(key, address);
    return address;
}

// A guest block the caller can hold: [vtable][object]. Called with g_mutex held.
std::uint32_t AppendBlock(std::uint32_t object, const char* key, const void* host) {
    const std::uint32_t table = VtableFor(key);
    if (table == 0 || g_registry == nullptr) {
        return 0;
    }
    const std::uint32_t address = g_registry->AllocateData(kBlockBytes);
    if (address == 0) {
        return 0;
    }
    const std::uint32_t words[2] = {table, object};
    std::memcpy(g_registry->Arena().Ptr(address), words, sizeof(words));
    auto block = std::make_shared<Block>();
    block->address = address;
    block->object = object;
    block->key = key;
    block->host = host;
    block->generation = ++g_generation;
    g_blocks.emplace(address, std::move(block));
    return address;
}

// Called with g_mutex held.
std::uint32_t CreateObject(Kind kind, SLObjectItf host) {
    if (g_registry == nullptr) {
        return 0;
    }
    const std::uint32_t table = VtableFor(kKeyObject);
    if (table == 0) {
        return 0;
    }
    const std::uint32_t address = g_registry->AllocateData(kBlockBytes);
    if (address == 0) {
        return 0;
    }
    // The object block points at itself, so a method called on it knows which
    // object it belongs to.
    const std::uint32_t words[2] = {table, address};
    std::memcpy(g_registry->Arena().Ptr(address), words, sizeof(words));
    auto block = std::make_shared<Block>();
    block->address = address;
    block->object = address;
    block->key = kKeyObject;
    block->host = host;
    block->generation = ++g_generation;
    auto object = std::make_shared<GuestObject>();
    object->address = address;
    object->host = host;
    object->kind = kind;
    g_blocks.emplace(address, std::move(block));
    g_objects.emplace(address, std::move(object));
    return address;
}

// Called with g_mutex held.
std::uint32_t AddInterface(const std::shared_ptr<GuestObject>& object, const char* key,
                           const void* host) {
    const std::uint32_t address = AppendBlock(object->address, key, host);
    if (address == 0) {
        return 0;
    }
    object->interfaces.emplace(key, g_blocks.at(address));
    return address;
}

std::shared_ptr<Block> Lookup(std::uint32_t self) {
    const std::lock_guard lock(g_mutex);
    const auto block = g_blocks.find(self);
    return block != g_blocks.end() ? block->second : nullptr;
}

// The object a call was made on: the object block itself, or the object an
// interface block belongs to.
std::shared_ptr<GuestObject> ResolveObject(std::uint32_t self) {
    const std::lock_guard lock(g_mutex);
    const auto direct = g_objects.find(self);
    if (direct != g_objects.end()) {
        return direct->second;
    }
    const auto block = g_blocks.find(self);
    if (block == g_blocks.end()) {
        return nullptr;
    }
    const auto object = g_objects.find(block->second->object);
    return object != g_objects.end() ? object->second : nullptr;
}

template <typename Interface>
Interface InterfaceOf(std::uint32_t self, const char* key) {
    const std::shared_ptr<Block> block = Lookup(self);
    if (block == nullptr || !KeyIs(block->key, key)) {
        return nullptr;
    }
    // The platform's interface handles carry const; the block stores the pointer
    // as an opaque address and the callee only reads through it.
    return reinterpret_cast<Interface>(const_cast<void*>(block->host));
}

// ---- platform callbacks --------------------------------------------------
//
// The platform hands back its own interface pointer and the context registered
// with it, which is the guest block address. The guest is then entered with its
// own interface pointer and its own context, so a callback looks exactly like
// the one a device delivers.

bool ClaimCallback(std::uint32_t self, CallbackRequest* request) {
    const std::lock_guard lock(g_mutex);
    const auto block = g_blocks.find(self);
    if (block == g_blocks.end() || block->second->callback == 0) {
        return false;
    }
    request->callback = block->second->callback;
    request->context = block->second->context;
    request->itf = self;
    request->generation = block->second->generation;
    return true;
}

void QueueCallback(SLAndroidSimpleBufferQueueItf queue, void* context) {
    (void)queue;
    CallbackRequest request;
    if (ClaimCallback(ContextBlock(context), &request)) {
        RunCallbackHere(request);
    }
}

void PlayCallback(SLPlayItf caller, void* context, SLuint32 event) {
    (void)caller;
    CallbackRequest request;
    if (ClaimCallback(ContextBlock(context), &request)) {
        request.event = event;
        request.has_event = true;
        PostCallback(request);
    }
}

void RecordCallback(SLRecordItf caller, void* context, SLuint32 event) {
    (void)caller;
    CallbackRequest request;
    if (ClaimCallback(ContextBlock(context), &request)) {
        request.event = event;
        request.has_event = true;
        PostCallback(request);
    }
}

void MixCallback(SLOutputMixItf caller, void* context) {
    (void)caller;
    CallbackRequest request;
    if (ClaimCallback(ContextBlock(context), &request)) {
        PostCallback(request);
    }
}

// ---- object interface ----------------------------------------------------

void ObjectRealize(HleCall& call) {
    const std::shared_ptr<GuestObject> object = ResolveObject(call.Arg(0));
    if (object == nullptr || object->host == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const std::uint32_t async = call.Arg(1);
    if (async != 0) {
        // The platform would report completion through an object callback, and
        // that callback carries six arguments this container does not deliver.
        LogOnce("opensles-async", "opensles: async realize not reported to the guest");
    }
    call.Return((*object->host)
                    ->Realize(object->host,
                              async != 0 ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE));
}

void ObjectResume(HleCall& call) {
    const std::shared_ptr<GuestObject> object = ResolveObject(call.Arg(0));
    if (object == nullptr || object->host == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const std::uint32_t async = call.Arg(1);
    call.Return((*object->host)->Resume(object->host,
                                        async != 0 ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE));
}

void ObjectGetState(HleCall& call) {
    const std::shared_ptr<GuestObject> object = ResolveObject(call.Arg(0));
    std::uint32_t* state = OutU32(call, call.Arg(1));
    if (object == nullptr || object->host == nullptr || state == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*object->host)->GetState(object->host, state));
}

void ObjectGetInterface(HleCall& call) {
    const std::uint32_t self = call.Arg(0);
    const std::uint32_t out = call.Arg(2);
    if (out == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    std::string name;
    const InterfaceInfo* info = ResolveInterface(call, call.Arg(1), &name);
    if (info == nullptr) {
        LogOnce(("opensles-missing:" + name).c_str(), "opensles: no interface " + name);
        call.Return(SL_RESULT_FEATURE_UNSUPPORTED);
        return;
    }
    const std::shared_ptr<GuestObject> object = ResolveObject(self);
    if (object == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (KeyIs(info->key, kKeyObject)) {
        // SL_IID_OBJECT is the object itself, not a second block.
        WriteU32(call, out, object->address);
        call.Return(SL_RESULT_SUCCESS);
        return;
    }
    {
        const std::lock_guard lock(g_mutex);
        const auto block = object->interfaces.find(info->key);
        if (block != object->interfaces.end()) {
            WriteU32(call, out, block->second->address);
            call.Return(SL_RESULT_SUCCESS);
            return;
        }
    }
    // Asked without the lock: a realize in progress elsewhere may block here, and
    // the object map has to stay available meanwhile.
    void* host_interface = nullptr;
    const SLresult result =
        (*object->host)->GetInterface(object->host, info->id, &host_interface);
    if (result != SL_RESULT_SUCCESS || host_interface == nullptr) {
        LogOnce(("opensles-refused:" + name).c_str(),
                "opensles: " + name + " refused with " + std::to_string(result));
        call.Return(result);
        return;
    }
    std::uint32_t address = 0;
    {
        const std::lock_guard lock(g_mutex);
        const auto object_now = g_objects.find(object->address);
        if (object_now == g_objects.end()) {
            // Destroyed while the platform was answering.
            call.Return(SL_RESULT_PARAMETER_INVALID);
            return;
        }
        const auto block = object_now->second->interfaces.find(info->key);
        address = block != object_now->second->interfaces.end()
                      ? block->second->address
                      : AddInterface(object_now->second, info->key, host_interface);
    }
    if (address == 0) {
        call.Return(SL_RESULT_MEMORY_FAILURE);
        return;
    }
    WriteU32(call, out, address);
    LogOnce(("opensles-interface:" + name).c_str(), "opensles: " + name + " forwarded");
    call.Return(SL_RESULT_SUCCESS);
}

void ObjectAbortAsyncOperation(HleCall& call) {
    const std::shared_ptr<GuestObject> object = ResolveObject(call.Arg(0));
    if (object != nullptr && object->host != nullptr) {
        (*object->host)->AbortAsyncOperation(object->host);
    }
    call.Return(SL_RESULT_SUCCESS);
}

void ObjectDestroy(HleCall& call) {
    std::shared_ptr<GuestObject> object;
    std::vector<std::uint32_t> blocks;
    {
        const std::lock_guard lock(g_mutex);
        const auto found = g_objects.find(call.Arg(0));
        if (found == g_objects.end()) {
            call.Return(SL_RESULT_SUCCESS);
            return;
        }
        object = found->second;
        g_objects.erase(found);
        // Forgotten before the platform destroys the object: a callback that
        // arrives during Destroy finds no block and is dropped, and a guest that
        // keeps calling through a stale pointer gets PARAMETER_INVALID instead of
        // a jump into reused memory.
        blocks.push_back(object->address);
        g_blocks.erase(object->address);
        for (const auto& entry : object->interfaces) {
            blocks.push_back(entry.second->address);
            g_blocks.erase(entry.second->address);
        }
    }
    if (object->host != nullptr) {
        (*object->host)->Destroy(object->host);
    }
    if (g_registry != nullptr) {
        for (const std::uint32_t block : blocks) {
            g_registry->ReleaseData(block);
        }
    }
    call.Return(SL_RESULT_SUCCESS);
}

void ObjectSetPriority(HleCall& call) {
    const std::shared_ptr<GuestObject> object = ResolveObject(call.Arg(0));
    if (object == nullptr || object->host == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*object->host)
                    ->SetPriority(object->host, static_cast<SLint32>(call.Arg(1)),
                                  call.Arg(2) != 0 ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE));
}

void ObjectGetPriority(HleCall& call) {
    const std::shared_ptr<GuestObject> object = ResolveObject(call.Arg(0));
    SLint32* priority = nullptr;
    SLboolean* preemptable = nullptr;
    if (call.Arg(1) != 0) {
        priority = reinterpret_cast<SLint32*>(call.Mapped(call.Arg(1), 4));
    }
    if (call.Arg(2) != 0) {
        preemptable = reinterpret_cast<SLboolean*>(call.Mapped(call.Arg(2), 4));
    }
    if (object == nullptr || object->host == nullptr || priority == nullptr ||
        preemptable == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*object->host)->GetPriority(object->host, priority, preemptable));
}

void ObjectSetLossOfControlInterfaces(HleCall& call) {
    const std::shared_ptr<GuestObject> object = ResolveObject(call.Arg(0));
    if (object == nullptr || object->host == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const auto count = static_cast<std::int16_t>(call.Arg(1));
    std::vector<SLInterfaceID> ids;
    if (count > 0) {
        const std::uint32_t array = call.Arg(2);
        if (array == 0) {
            call.Return(SL_RESULT_PARAMETER_INVALID);
            return;
        }
        for (std::int16_t i = 0; i < count; ++i) {
            std::uint32_t guest_id = 0;
            const std::uint32_t offset = static_cast<std::uint32_t>(i) * 4;
            if (!call.CopyIn(array + offset, &guest_id, sizeof(guest_id))) {
                call.Return(SL_RESULT_PARAMETER_INVALID);
                return;
            }
            const InterfaceInfo* info = ResolveInterface(call, guest_id, nullptr);
            if (info == nullptr) {
                call.Return(SL_RESULT_FEATURE_UNSUPPORTED);
                return;
            }
            ids.push_back(info->id);
        }
    }
    call.Return((*object->host)
                    ->SetLossOfControlInterfaces(
                        object->host, count, ids.data(),
                        call.Arg(3) != 0 ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE));
}

// ---- engine interface ----------------------------------------------------

// Guest SLInterfaceID array -> the platform's. A name the container cannot wrap
// is dropped rather than failing the whole object: the platform cannot hand over
// a vtable for an interface nobody here can translate, and refusing the player
// would leave the game silent. The guest's GetInterface for it still answers
// FEATURE_UNSUPPORTED, which is what a device without that interface answers.
bool TranslateInterfaces(HleCall& call, std::uint32_t count, std::uint32_t ids_address,
                         std::uint32_t required_address, std::vector<SLInterfaceID>* ids,
                         std::vector<SLboolean>* required) {
    ids->clear();
    required->clear();
    if (count == 0) {
        return true;
    }
    if (ids_address == 0) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t guest_id = 0;
        if (!call.CopyIn(ids_address + i * 4, &guest_id, sizeof(guest_id))) {
            return false;
        }
        std::string name;
        const InterfaceInfo* info = ResolveInterface(call, guest_id, &name);
        if (info == nullptr) {
            LogOnce(("opensles-unwrapped:" + name).c_str(),
                    "opensles: interface " + name + " not requested from the platform");
            continue;
        }
        ids->push_back(info->id);
        std::uint32_t flag = 0;
        if (required_address != 0 &&
            !call.CopyIn(required_address + i * 4, &flag, sizeof(flag))) {
            return false;
        }
        required->push_back(flag != 0 ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE);
    }
    return true;
}

// SLDataSource and SLDataSink are the same two-word record: a locator and a
// format. Both point at guest structures whose pointers are 32 bits while the
// platform's are 64, so they cannot be moved across as bytes: each locator kind
// is rebuilt here, and an object reference (the output mix) is translated from a
// guest block into the platform object it stands for.
struct TranslatedData {
    SLDataLocator_AndroidSimpleBufferQueue simple{};
    SLDataLocator_AndroidBufferQueue android{};
    SLDataLocator_BufferQueue plain{};
    SLDataLocator_Address address{};
    SLDataLocator_IODevice device{};
    SLDataLocator_OutputMix mix{};
    SLDataFormat_PCM pcm{};
    SLAndroidDataFormat_PCM_EX pcm_ex{};
};

bool TranslateFormat(HleCall& call, std::uint32_t guest, TranslatedData* data, void** out) {
    std::uint32_t type = 0;
    if (!call.CopyIn(guest, &type, sizeof(type))) {
        return false;
    }
    if (type == SL_DATAFORMAT_PCM) {
        if (!call.CopyIn(guest, &data->pcm, sizeof(data->pcm))) {
            return false;
        }
        *out = &data->pcm;
        return true;
    }
    if (type == SL_ANDROID_DATAFORMAT_PCM_EX) {
        if (!call.CopyIn(guest, &data->pcm_ex, sizeof(data->pcm_ex))) {
            return false;
        }
        *out = &data->pcm_ex;
        return true;
    }
    return false;
}

bool TranslateData(HleCall& call, std::uint32_t guest, bool is_sink, TranslatedData* data,
                   void** locator, void** format) {
    *locator = nullptr;
    *format = nullptr;
    if (guest == 0) {
        return false;
    }
    std::uint32_t locator_address = 0;
    std::uint32_t format_address = 0;
    if (!call.CopyIn(guest, &locator_address, sizeof(locator_address)) ||
        !call.CopyIn(guest + 4, &format_address, sizeof(format_address))) {
        return false;
    }
    if (locator_address == 0) {
        return false;
    }
    std::uint32_t type = 0;
    if (!call.CopyIn(locator_address, &type, sizeof(type))) {
        return false;
    }
    switch (type) {
        case SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE:
        case SL_DATALOCATOR_ANDROIDBUFFERQUEUE:
        case SL_DATALOCATOR_BUFFERQUEUE: {
            std::uint32_t buffers = 0;
            if (!call.CopyIn(locator_address + 4, &buffers, sizeof(buffers))) {
                return false;
            }
            if (type == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) {
                data->simple.locatorType = type;
                data->simple.numBuffers = buffers;
                *locator = &data->simple;
            } else if (type == SL_DATALOCATOR_ANDROIDBUFFERQUEUE) {
                data->android.locatorType = type;
                data->android.numBuffers = buffers;
                *locator = &data->android;
            } else {
                data->plain.locatorType = type;
                data->plain.numBuffers = buffers;
                *locator = &data->plain;
            }
            break;
        }
        case SL_DATALOCATOR_ADDRESS: {
            std::uint32_t address = 0;
            std::uint32_t length = 0;
            if (!call.CopyIn(locator_address + 4, &address, sizeof(address)) ||
                !call.CopyIn(locator_address + 8, &length, sizeof(length))) {
                return false;
            }
            // The buffer lives in the arena, which is host memory.
            std::byte* buffer = address == 0 ? nullptr : call.Mapped(address, length);
            if (address != 0 && buffer == nullptr) {
                return false;
            }
            data->address.locatorType = type;
            data->address.pAddress = buffer;
            data->address.length = length;
            *locator = &data->address;
            break;
        }
        case SL_DATALOCATOR_IODEVICE: {
            std::uint32_t device_type = 0;
            std::uint32_t device_id = 0;
            if (!call.CopyIn(locator_address + 4, &device_type, sizeof(device_type)) ||
                !call.CopyIn(locator_address + 8, &device_id, sizeof(device_id))) {
                return false;
            }
            // A device object is only meaningful to the platform for its own
            // output devices, which the guest cannot reach; the microphone
            // locator carries none.
            data->device.locatorType = type;
            data->device.deviceType = device_type;
            data->device.deviceID = device_id;
            data->device.device = nullptr;
            *locator = &data->device;
            break;
        }
        case SL_DATALOCATOR_OUTPUTMIX: {
            if (!is_sink) {
                return false;
            }
            std::uint32_t handle = 0;
            if (!call.CopyIn(locator_address + 4, &handle, sizeof(handle))) {
                return false;
            }
            const std::shared_ptr<GuestObject> mix = ResolveObject(handle);
            if (mix == nullptr || mix->kind != Kind::OutputMix || mix->host == nullptr) {
                return false;
            }
            data->mix.locatorType = type;
            data->mix.outputMix = mix->host;
            *locator = &data->mix;
            break;
        }
        default:
            return false;
    }
    if (format_address == 0) {
        return true;
    }
    return TranslateFormat(call, format_address, data, format);
}

// The platform created an object; the guest gets a block that stands for it.
SLresult AdoptObject(Kind kind, SLObjectItf host, HleCall& call, std::uint32_t out) {
    std::uint32_t block = 0;
    {
        const std::lock_guard lock(g_mutex);
        block = CreateObject(kind, host);
    }
    if (block == 0) {
        (*host)->Destroy(host);
        return SL_RESULT_MEMORY_FAILURE;
    }
    WriteU32(call, out, block);
    return SL_RESULT_SUCCESS;
}

void EngineCreateOutputMix(HleCall& call) {
    const SLEngineItf engine = InterfaceOf<SLEngineItf>(call.Arg(0), kKeyEngine);
    const std::uint32_t out = call.Arg(1);
    if (engine == nullptr || out == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    std::vector<SLInterfaceID> ids;
    std::vector<SLboolean> required;
    if (!TranslateInterfaces(call, call.Arg(2), call.Arg(3), call.Arg(4), &ids, &required)) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    SLObjectItf host = nullptr;
    const SLresult result =
        (*engine)->CreateOutputMix(engine, &host, static_cast<SLuint32>(ids.size()),
                                   ids.data(), required.data());
    if (result != SL_RESULT_SUCCESS || host == nullptr) {
        call.Return(result);
        return;
    }
    const SLresult adopted = AdoptObject(Kind::OutputMix, host, call, out);
    if (adopted == SL_RESULT_SUCCESS) {
        LogOnce("opensles-mix", "opensles: output mix forwarded to the platform");
    }
    call.Return(adopted);
}

void EngineCreateAudioPlayer(HleCall& call) {
    const SLEngineItf engine = InterfaceOf<SLEngineItf>(call.Arg(0), kKeyEngine);
    const std::uint32_t out = call.Arg(1);
    if (engine == nullptr || out == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    TranslatedData source_data;
    TranslatedData sink_data;
    SLDataSource source{};
    SLDataSink sink{};
    if (!TranslateData(call, call.Arg(2), false, &source_data, &source.pLocator,
                       &source.pFormat) ||
        !TranslateData(call, call.Arg(3), true, &sink_data, &sink.pLocator, &sink.pFormat)) {
        LogOnce("opensles-player-data", "opensles: player source or sink not understood");
        call.Return(SL_RESULT_FEATURE_UNSUPPORTED);
        return;
    }
    std::vector<SLInterfaceID> ids;
    std::vector<SLboolean> required;
    if (!TranslateInterfaces(call, call.Arg(4), call.Arg(5), call.Arg(6), &ids, &required)) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    SLObjectItf host = nullptr;
    const SLresult result =
        (*engine)->CreateAudioPlayer(engine, &host, &source, &sink,
                                     static_cast<SLuint32>(ids.size()), ids.data(),
                                     required.data());
    if (result != SL_RESULT_SUCCESS || host == nullptr) {
        LogOnce("opensles-player-refused",
                "opensles: platform refused the player with " + std::to_string(result));
        call.Return(result);
        return;
    }
    if (LogAllowed("opensles-player", 1)) {
        // The leading fields of SLDataFormat_PCM and SLAndroidDataFormat_PCM_EX
        // agree, so either one reads as the same format here.
        const auto* pcm = static_cast<const SLDataFormat_PCM*>(source.pFormat);
        std::uint32_t buffers = 0;
        if (source.pLocator == &source_data.simple) {
            buffers = source_data.simple.numBuffers;
        } else if (source.pLocator == &source_data.android) {
            buffers = source_data.android.numBuffers;
        } else if (source.pLocator == &source_data.plain) {
            buffers = source_data.plain.numBuffers;
        }
        if (pcm != nullptr) {
            ARMBAT_LOG_INFO("Armbat",
                            "opensles: platform player %u Hz %u ch %u-bit, %u buffers, "
                            "%u interfaces",
                            static_cast<unsigned>(pcm->samplesPerSec / 1000),
                            static_cast<unsigned>(pcm->numChannels),
                            static_cast<unsigned>(pcm->bitsPerSample),
                            static_cast<unsigned>(buffers),
                            static_cast<unsigned>(ids.size()));
        }
    }
    call.Return(AdoptObject(Kind::Player, host, call, out));
}

void EngineCreateAudioRecorder(HleCall& call) {
    const SLEngineItf engine = InterfaceOf<SLEngineItf>(call.Arg(0), kKeyEngine);
    const std::uint32_t out = call.Arg(1);
    if (engine == nullptr || out == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    TranslatedData source_data;
    TranslatedData sink_data;
    SLDataSource source{};
    SLDataSink sink{};
    if (!TranslateData(call, call.Arg(2), false, &source_data, &source.pLocator,
                       &source.pFormat) ||
        !TranslateData(call, call.Arg(3), true, &sink_data, &sink.pLocator, &sink.pFormat)) {
        call.Return(SL_RESULT_FEATURE_UNSUPPORTED);
        return;
    }
    std::vector<SLInterfaceID> ids;
    std::vector<SLboolean> required;
    if (!TranslateInterfaces(call, call.Arg(4), call.Arg(5), call.Arg(6), &ids, &required)) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    SLObjectItf host = nullptr;
    const SLresult result =
        (*engine)->CreateAudioRecorder(engine, &host, &source, &sink,
                                       static_cast<SLuint32>(ids.size()), ids.data(),
                                       required.data());
    if (result != SL_RESULT_SUCCESS || host == nullptr) {
        call.Return(result);
        return;
    }
    const SLresult adopted = AdoptObject(Kind::Recorder, host, call, out);
    if (adopted == SL_RESULT_SUCCESS) {
        LogOnce("opensles-recorder", "opensles: recorder forwarded to the platform");
    }
    call.Return(adopted);
}

void EngineQueryNumSupportedInterfaces(HleCall& call) {
    const SLEngineItf engine = InterfaceOf<SLEngineItf>(call.Arg(0), kKeyEngine);
    std::uint32_t* count = OutU32(call, call.Arg(2));
    if (engine == nullptr || count == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*engine)->QueryNumSupportedInterfaces(engine, call.Arg(1), count));
}

// The platform would write its own SLInterfaceID pointers into guest memory,
// where a slot is 32 bits wide. The guest cannot use them, so the honest answer
// is that this query is not available.
void EngineQuerySupportedInterfaces(HleCall& call) {
    call.Return(SL_RESULT_FEATURE_UNSUPPORTED);
}

void EngineQueryNumSupportedExtensions(HleCall& call) {
    const SLEngineItf engine = InterfaceOf<SLEngineItf>(call.Arg(0), kKeyEngine);
    std::uint32_t* count = OutU32(call, call.Arg(1));
    if (engine == nullptr || count == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*engine)->QueryNumSupportedExtensions(engine, count));
}

void EngineQuerySupportedExtension(HleCall& call) {
    const SLEngineItf engine = InterfaceOf<SLEngineItf>(call.Arg(0), kKeyEngine);
    const std::uint32_t name_address = call.Arg(2);
    const std::uint32_t length_address = call.Arg(3);
    if (engine == nullptr || name_address == 0 || length_address == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    SLint16 length = 0;
    if (!call.CopyIn(length_address, &length, sizeof(length))) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    auto* name = reinterpret_cast<SLchar*>(call.Mapped(name_address, static_cast<std::size_t>(length)));
    if (name == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const SLresult result = (*engine)->QuerySupportedExtension(engine, call.Arg(1), name, &length);
    if (result == SL_RESULT_SUCCESS) {
        call.CopyOut(length_address, &length, sizeof(length));
    }
    call.Return(result);
}

void EngineIsExtensionSupported(HleCall& call) {
    const SLEngineItf engine = InterfaceOf<SLEngineItf>(call.Arg(0), kKeyEngine);
    if (engine == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    std::string name;
    if (!call.ReadCString(call.Arg(1), name, 64)) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    SLboolean supported = SL_BOOLEAN_FALSE;
    const SLresult result =
        (*engine)->IsExtensionSupported(engine, reinterpret_cast<const SLchar*>(name.c_str()),
                                        &supported);
    if (result == SL_RESULT_SUCCESS) {
        WriteU32(call, call.Arg(2), supported);
    }
    call.Return(result);
}

// ---- play and record -----------------------------------------------------
//
// Both interfaces are forwarded as they are. What the container keeps is only
// the guest callback: the platform calls ours, and ours has to know which guest
// function and context to enter.

// The guest unregisters by passing a null callback. Our trampoline already drops
// events whose block has no callback, so the platform never has to be told.
bool ClearCallback(std::uint32_t self) {
    const std::lock_guard lock(g_mutex);
    const auto block = g_blocks.find(self);
    if (block == g_blocks.end()) {
        return false;
    }
    block->second->callback = 0;
    block->second->context = 0;
    return true;
}

bool SetCallback(std::uint32_t self, std::uint32_t callback, std::uint32_t context) {
    const std::lock_guard lock(g_mutex);
    const auto block = g_blocks.find(self);
    if (block == g_blocks.end()) {
        return false;
    }
    block->second->callback = callback;
    block->second->context = context;
    return true;
}

void PlaySetPlayState(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    if (play == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->SetPlayState(play, call.Arg(1)));
}

void PlayGetPlayState(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    std::uint32_t* state = OutU32(call, call.Arg(1));
    if (play == nullptr || state == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->GetPlayState(play, state));
}

void PlayGetDuration(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    std::uint32_t* duration = OutU32(call, call.Arg(1));
    if (play == nullptr || duration == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->GetDuration(play, duration));
}

void PlayGetPosition(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    std::uint32_t* position = OutU32(call, call.Arg(1));
    if (play == nullptr || position == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->GetPosition(play, position));
}

void PlayRegisterCallback(HleCall& call) {
    const std::uint32_t self = call.Arg(0);
    const SLPlayItf play = InterfaceOf<SLPlayItf>(self, kKeyPlay);
    if (play == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (call.Arg(1) == 0) {
        call.Return(ClearCallback(self) ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (!SetCallback(self, call.Arg(1), call.Arg(2))) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->RegisterCallback(play, &PlayCallback, BlockContext(self)));
}

void PlaySetCallbackEventsMask(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    if (play == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->SetCallbackEventsMask(play, call.Arg(1)));
}

void PlayGetCallbackEventsMask(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    std::uint32_t* mask = OutU32(call, call.Arg(1));
    if (play == nullptr || mask == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->GetCallbackEventsMask(play, mask));
}

void PlaySetMarkerPosition(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    if (play == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->SetMarkerPosition(play, call.Arg(1)));
}

void PlayClearMarkerPosition(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    if (play == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->ClearMarkerPosition(play));
}

void PlayGetMarkerPosition(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    std::uint32_t* position = OutU32(call, call.Arg(1));
    if (play == nullptr || position == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->GetMarkerPosition(play, position));
}

void PlaySetPositionUpdatePeriod(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    if (play == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->SetPositionUpdatePeriod(play, call.Arg(1)));
}

void PlayGetPositionUpdatePeriod(HleCall& call) {
    const SLPlayItf play = InterfaceOf<SLPlayItf>(call.Arg(0), kKeyPlay);
    std::uint32_t* period = OutU32(call, call.Arg(1));
    if (play == nullptr || period == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*play)->GetPositionUpdatePeriod(play, period));
}

void RecordSetRecordState(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    if (record == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->SetRecordState(record, call.Arg(1)));
}

void RecordGetRecordState(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    std::uint32_t* state = OutU32(call, call.Arg(1));
    if (record == nullptr || state == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->GetRecordState(record, state));
}

void RecordSetDurationLimit(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    if (record == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->SetDurationLimit(record, call.Arg(1)));
}

void RecordGetPosition(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    std::uint32_t* position = OutU32(call, call.Arg(1));
    if (record == nullptr || position == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->GetPosition(record, position));
}

void RecordRegisterCallback(HleCall& call) {
    const std::uint32_t self = call.Arg(0);
    const SLRecordItf record = InterfaceOf<SLRecordItf>(self, kKeyRecord);
    if (record == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (call.Arg(1) == 0) {
        call.Return(ClearCallback(self) ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (!SetCallback(self, call.Arg(1), call.Arg(2))) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->RegisterCallback(record, &RecordCallback, BlockContext(self)));
}

void RecordSetCallbackEventsMask(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    if (record == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->SetCallbackEventsMask(record, call.Arg(1)));
}

void RecordGetCallbackEventsMask(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    std::uint32_t* mask = OutU32(call, call.Arg(1));
    if (record == nullptr || mask == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->GetCallbackEventsMask(record, mask));
}

void RecordSetMarkerPosition(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    if (record == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->SetMarkerPosition(record, call.Arg(1)));
}

void RecordClearMarkerPosition(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    if (record == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->ClearMarkerPosition(record));
}

void RecordGetMarkerPosition(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    std::uint32_t* position = OutU32(call, call.Arg(1));
    if (record == nullptr || position == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->GetMarkerPosition(record, position));
}

void RecordSetPositionUpdatePeriod(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    if (record == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->SetPositionUpdatePeriod(record, call.Arg(1)));
}

void RecordGetPositionUpdatePeriod(HleCall& call) {
    const SLRecordItf record = InterfaceOf<SLRecordItf>(call.Arg(0), kKeyRecord);
    std::uint32_t* period = OutU32(call, call.Arg(1));
    if (record == nullptr || period == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*record)->GetPositionUpdatePeriod(record, period));
}

// ---- buffer queue --------------------------------------------------------
//
// The samples leave the guest here and nothing is copied: the arena is mapped in
// this process, so the platform reads the guest's own PCM, and the guest is told
// a buffer is done through the callback it registered -- which is what paces its
// mixer. Queue depth, Clear and the "queue full" answer are the platform's.

void QueueEnqueue(HleCall& call) {
    const std::uint32_t self = call.Arg(0);
    const std::uint32_t size = call.Arg(2);
    const std::shared_ptr<Block> block = Lookup(self);
    if (block == nullptr || size == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    std::byte* data = call.Mapped(call.Arg(1), size);
    if (data == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (KeyIs(block->key, kKeyAndroidBufferQueue)) {
        const auto queue = static_cast<SLAndroidSimpleBufferQueueItf>(block->host);
        const SLresult result = (*queue)->Enqueue(queue, data, size);
        if (LogAllowed("opensles-enqueue", 1)) {
            ARMBAT_LOG_INFO("Armbat", "opensles: first buffer of %u bytes, result %u",
                            static_cast<unsigned>(size), static_cast<unsigned>(result));
        }
        call.Return(result);
        return;
    }
    if (KeyIs(block->key, kKeyBufferQueue)) {
        const auto queue = static_cast<SLBufferQueueItf>(block->host);
        call.Return((*queue)->Enqueue(queue, data, size));
        return;
    }
    call.Return(SL_RESULT_PARAMETER_INVALID);
}

void QueueClear(HleCall& call) {
    const std::uint32_t self = call.Arg(0);
    const std::shared_ptr<Block> block = Lookup(self);
    if (block == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (KeyIs(block->key, kKeyAndroidBufferQueue)) {
        const auto queue = static_cast<SLAndroidSimpleBufferQueueItf>(block->host);
        call.Return((*queue)->Clear(queue));
        return;
    }
    if (KeyIs(block->key, kKeyBufferQueue)) {
        const auto queue = static_cast<SLBufferQueueItf>(block->host);
        call.Return((*queue)->Clear(queue));
        return;
    }
    call.Return(SL_RESULT_PARAMETER_INVALID);
}

void QueueGetState(HleCall& call) {
    const std::uint32_t out = call.Arg(1);
    const std::shared_ptr<Block> block = Lookup(call.Arg(0));
    if (block == nullptr || out == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    // Both state structures are two 32-bit words; only the member names differ.
    if (KeyIs(block->key, kKeyAndroidBufferQueue)) {
        SLAndroidSimpleBufferQueueState state{};
        const auto queue = static_cast<SLAndroidSimpleBufferQueueItf>(block->host);
        const SLresult result = (*queue)->GetState(queue, &state);
        if (result == SL_RESULT_SUCCESS) {
            call.CopyOut(out, &state, sizeof(state));
        }
        call.Return(result);
        return;
    }
    if (KeyIs(block->key, kKeyBufferQueue)) {
        SLBufferQueueState state{};
        const auto queue = static_cast<SLBufferQueueItf>(block->host);
        const SLresult result = (*queue)->GetState(queue, &state);
        if (result == SL_RESULT_SUCCESS) {
            call.CopyOut(out, &state, sizeof(state));
        }
        call.Return(result);
        return;
    }
    call.Return(SL_RESULT_PARAMETER_INVALID);
}

void QueueRegisterCallback(HleCall& call) {
    const std::uint32_t self = call.Arg(0);
    const std::shared_ptr<Block> block = Lookup(self);
    if (block == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (call.Arg(1) == 0) {
        call.Return(ClearCallback(self) ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (!SetCallback(self, call.Arg(1), call.Arg(2))) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (KeyIs(block->key, kKeyAndroidBufferQueue)) {
        const auto queue = static_cast<SLAndroidSimpleBufferQueueItf>(block->host);
        call.Return(
            (*queue)->RegisterCallback(queue, &QueueCallback, BlockContext(self)));
        return;
    }
    if (KeyIs(block->key, kKeyBufferQueue)) {
        const auto queue = static_cast<SLBufferQueueItf>(block->host);
        call.Return((*queue)->RegisterCallback(
            queue,
            // The plain queue's callback takes the plain interface pointer; the
            // guest is entered with whatever pointer it registered against, so
            // the first argument is not used here either.
            [](SLBufferQueueItf caller, void* context) {
                (void)caller;
                CallbackRequest request;
                if (ClaimCallback(ContextBlock(context), &request)) {
                    RunCallbackHere(request);
                }
            },
            BlockContext(self)));
        return;
    }
    call.Return(SL_RESULT_PARAMETER_INVALID);
}

// ---- volume interface ----------------------------------------------------

// SLmillibel and SLpermille arrive in a register with their upper bits
// unspecified: only the low 16 bits are the value.
void VolumeSetLevel(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    if (volume == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*volume)->SetVolumeLevel(
        volume, static_cast<SLmillibel>(static_cast<std::int16_t>(call.Arg(1)))));
}

void VolumeGetLevel(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    SLmillibel level = 0;
    if (volume == nullptr || call.Arg(1) == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const SLresult result = (*volume)->GetVolumeLevel(volume, &level);
    if (result == SL_RESULT_SUCCESS) {
        WriteI16(call, call.Arg(1), level);
    }
    call.Return(result);
}

void VolumeGetMaxLevel(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    SLmillibel level = 0;
    if (volume == nullptr || call.Arg(1) == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const SLresult result = (*volume)->GetMaxVolumeLevel(volume, &level);
    if (result == SL_RESULT_SUCCESS) {
        WriteI16(call, call.Arg(1), level);
    }
    call.Return(result);
}

void VolumeSetMute(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    if (volume == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*volume)->SetMute(volume, call.Arg(1) != 0 ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE));
}

void VolumeGetMute(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    SLboolean mute = SL_BOOLEAN_FALSE;
    if (volume == nullptr || call.Arg(1) == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const SLresult result = (*volume)->GetMute(volume, &mute);
    if (result == SL_RESULT_SUCCESS) {
        WriteU32(call, call.Arg(1), mute);
    }
    call.Return(result);
}

void VolumeEnableStereoPosition(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    if (volume == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*volume)->EnableStereoPosition(
        volume, call.Arg(1) != 0 ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE));
}

void VolumeIsEnabledStereoPosition(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    SLboolean enabled = SL_BOOLEAN_FALSE;
    if (volume == nullptr || call.Arg(1) == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const SLresult result = (*volume)->IsEnabledStereoPosition(volume, &enabled);
    if (result == SL_RESULT_SUCCESS) {
        WriteU32(call, call.Arg(1), enabled);
    }
    call.Return(result);
}

void VolumeSetStereoPosition(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    if (volume == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*volume)->SetStereoPosition(
        volume, static_cast<SLpermille>(static_cast<std::int16_t>(call.Arg(1)))));
}

void VolumeGetStereoPosition(HleCall& call) {
    const SLVolumeItf volume = InterfaceOf<SLVolumeItf>(call.Arg(0), kKeyVolume);
    SLpermille position = 0;
    if (volume == nullptr || call.Arg(1) == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const SLresult result = (*volume)->GetStereoPosition(volume, &position);
    if (result == SL_RESULT_SUCCESS) {
        WriteI16(call, call.Arg(1), position);
    }
    call.Return(result);
}

// ---- Android configuration ----------------------------------------------
//
// The configuration keys are strings and the values are plain integers, or a
// small structure of them, so both are pointed at directly. The platform
// requires SetConfiguration before Realize, and that ordering is now the guest's
// own: the call goes straight through.

bool ReadConfigKey(HleCall& call, std::uint32_t address, std::string& key) {
    return address != 0 && call.ReadCString(address, key, 64);
}

void ConfigSetConfiguration(HleCall& call) {
    const SLAndroidConfigurationItf config =
        InterfaceOf<SLAndroidConfigurationItf>(call.Arg(0), kKeyConfiguration);
    std::string key;
    if (config == nullptr || !ReadConfigKey(call, call.Arg(1), key)) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const std::uint32_t size = call.Arg(3);
    const std::byte* value =
        call.Arg(2) == 0 ? nullptr : call.Mapped(call.Arg(2), size);
    if (call.Arg(2) != 0 && value == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*config)->SetConfiguration(
        config, reinterpret_cast<const SLchar*>(key.c_str()), value, size));
}

void ConfigGetConfiguration(HleCall& call) {
    const SLAndroidConfigurationItf config =
        InterfaceOf<SLAndroidConfigurationItf>(call.Arg(0), kKeyConfiguration);
    std::string key;
    auto* size = reinterpret_cast<SLuint32*>(call.Mapped(call.Arg(2), sizeof(SLuint32)));
    if (config == nullptr || !ReadConfigKey(call, call.Arg(1), key) || size == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    // The caller declares how much room it has; the platform writes at most that
    // much, so the range is checked against the caller's own size.
    void* value = call.Arg(3) == 0 ? nullptr : call.Mapped(call.Arg(3), *size);
    if (call.Arg(3) != 0 && value == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*config)->GetConfiguration(
        config, reinterpret_cast<const SLchar*>(key.c_str()), size, value));
}

// A Java proxy is a Java object handle, which has no meaning in this container:
// answering with one would hand the guest a pointer into the host heap.
void ConfigAcquireJavaProxy(HleCall& call) {
    call.Return(SL_RESULT_FEATURE_UNSUPPORTED);
}

void ConfigReleaseJavaProxy(HleCall& call) {
    call.Return(SL_RESULT_FEATURE_UNSUPPORTED);
}

// ---- output mix ----------------------------------------------------------

void MixGetDestinationOutputDeviceIDs(HleCall& call) {
    const SLOutputMixItf mix = InterfaceOf<SLOutputMixItf>(call.Arg(0), kKeyOutputMix);
    auto* count = reinterpret_cast<SLint32*>(call.Mapped(call.Arg(1), sizeof(SLint32)));
    if (mix == nullptr || count == nullptr || *count < 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    SLuint32* devices = nullptr;
    if (call.Arg(2) != 0) {
        devices = reinterpret_cast<SLuint32*>(
            call.Mapped(call.Arg(2), static_cast<std::size_t>(*count) * sizeof(SLuint32)));
        if (devices == nullptr) {
            call.Return(SL_RESULT_PARAMETER_INVALID);
            return;
        }
    }
    call.Return((*mix)->GetDestinationOutputDeviceIDs(mix, count, devices));
}

void MixRegisterDeviceChangeCallback(HleCall& call) {
    const std::uint32_t self = call.Arg(0);
    const SLOutputMixItf mix = InterfaceOf<SLOutputMixItf>(self, kKeyOutputMix);
    if (mix == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (call.Arg(1) == 0) {
        call.Return(ClearCallback(self) ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (!SetCallback(self, call.Arg(1), call.Arg(2))) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    call.Return((*mix)->RegisterDeviceChangeCallback(mix, &MixCallback, BlockContext(self)));
}

void MixReRoute(HleCall& call) {
    const SLOutputMixItf mix = InterfaceOf<SLOutputMixItf>(call.Arg(0), kKeyOutputMix);
    if (mix == nullptr) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const auto count = static_cast<SLint32>(call.Arg(1));
    SLuint32* devices = nullptr;
    if (count > 0) {
        devices = reinterpret_cast<SLuint32*>(
            call.Mapped(call.Arg(2), static_cast<std::size_t>(count) * sizeof(SLuint32)));
        if (devices == nullptr) {
            call.Return(SL_RESULT_PARAMETER_INVALID);
            return;
        }
    }
    call.Return((*mix)->ReRoute(mix, count, devices));
}

// ---- entry point ---------------------------------------------------------

void SlCreateEngine(HleCall& call) {
    const std::uint32_t out = call.Arg(0);
    if (out == 0) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    const std::uint32_t num_options = call.Arg(1);
    const void* options = nullptr;
    if (num_options != 0) {
        // SLEngineOption is (feature, data): no pointers, so it crosses as it is.
        options = call.Mapped(call.Arg(2), static_cast<std::size_t>(num_options) * 8);
        if (options == nullptr) {
            call.Return(SL_RESULT_PARAMETER_INVALID);
            return;
        }
    }
    std::vector<SLInterfaceID> ids;
    std::vector<SLboolean> required;
    if (!TranslateInterfaces(call, call.Arg(3), call.Arg(4), call.Arg(5), &ids, &required)) {
        call.Return(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    SLObjectItf host = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const SLresult result =
        slCreateEngine(&host, static_cast<SLuint32>(num_options),
                       static_cast<const SLEngineOption*>(options),
                       static_cast<SLuint32>(ids.size()), ids.data(), required.data());
#pragma clang diagnostic pop
    if (result != SL_RESULT_SUCCESS || host == nullptr) {
        call.Return(result);
        return;
    }
    const SLresult adopted = AdoptObject(Kind::Engine, host, call, out);
    if (adopted == SL_RESULT_SUCCESS) {
        LogOnce("opensles-engine", "opensles: engine created by the platform");
    }
    call.Return(adopted);
}

}  // namespace

void SetGuestCallRunner(GuestCallRunner runner) {
    std::unique_lock lock(g_callback_mutex);
    const bool closing = !runner;
    g_guest_call = std::move(runner);
    if (closing) {
        // A queue callback may be inside guest code on a platform thread right
        // now; the arena it runs on is about to go away.
        g_callback_idle.wait(lock, [] { return g_callbacks_running == 0; });
    }
}

void StopGuestCallThread() {
    std::thread thread;
    bool joined = false;
    {
        const std::lock_guard lock(g_callback_mutex);
        g_callback_queue.clear();
        // Set before the joinable check: after teardown nothing may start a new
        // thread or queue another callback, including a detached one that is
        // still finishing its last event.
        g_callback_stopping = true;
        if (!g_callback_thread.joinable()) {
            return;
        }
        if (g_callback_thread.get_id() == std::this_thread::get_id()) {
            // The callback itself is asking: joining would deadlock, and the
            // thread has nothing left to run once the process is going away.
            g_callback_thread.detach();
        } else {
            thread = std::move(g_callback_thread);
            joined = true;
        }
        g_callback_ready.notify_all();
    }
    if (joined) {
        thread.join();
        const std::lock_guard lock(g_callback_mutex);
        g_callback_stopping = false;
    }
}

void RegisterOpenslesFunctions(HleRegistry& registry) {
    g_registry = &registry;

    registry.Add("slCreateEngine", SlCreateEngine);

    // A slot the container does not forward. FEATURE_UNSUPPORTED is what the
    // platform itself answers for a method it does not implement, and it keeps a
    // caller from trusting out-parameters nobody wrote.
    registry.Add("sl.unsupported",
                 [](HleCall& call) { call.Return(SL_RESULT_FEATURE_UNSUPPORTED); });

    registry.Add("sl.object.Realize", ObjectRealize);
    registry.Add("sl.object.Resume", ObjectResume);
    registry.Add("sl.object.GetState", ObjectGetState);
    registry.Add("sl.object.GetInterface", ObjectGetInterface);
    registry.Add("sl.object.AbortAsyncOperation", ObjectAbortAsyncOperation);
    registry.Add("sl.object.Destroy", ObjectDestroy);
    registry.Add("sl.object.SetPriority", ObjectSetPriority);
    registry.Add("sl.object.GetPriority", ObjectGetPriority);
    registry.Add("sl.object.SetLossOfControlInterfaces", ObjectSetLossOfControlInterfaces);

    registry.Add("sl.engine.CreateAudioPlayer", EngineCreateAudioPlayer);
    registry.Add("sl.engine.CreateAudioRecorder", EngineCreateAudioRecorder);
    registry.Add("sl.engine.CreateOutputMix", EngineCreateOutputMix);
    registry.Add("sl.engine.QueryNumSupportedInterfaces", EngineQueryNumSupportedInterfaces);
    registry.Add("sl.engine.QuerySupportedInterfaces", EngineQuerySupportedInterfaces);
    registry.Add("sl.engine.QueryNumSupportedExtensions", EngineQueryNumSupportedExtensions);
    registry.Add("sl.engine.QuerySupportedExtension", EngineQuerySupportedExtension);
    registry.Add("sl.engine.IsExtensionSupported", EngineIsExtensionSupported);

    registry.Add("sl.play.SetPlayState", PlaySetPlayState);
    registry.Add("sl.play.GetPlayState", PlayGetPlayState);
    registry.Add("sl.play.GetDuration", PlayGetDuration);
    registry.Add("sl.play.GetPosition", PlayGetPosition);
    registry.Add("sl.play.RegisterCallback", PlayRegisterCallback);
    registry.Add("sl.play.SetCallbackEventsMask", PlaySetCallbackEventsMask);
    registry.Add("sl.play.GetCallbackEventsMask", PlayGetCallbackEventsMask);
    registry.Add("sl.play.SetMarkerPosition", PlaySetMarkerPosition);
    registry.Add("sl.play.ClearMarkerPosition", PlayClearMarkerPosition);
    registry.Add("sl.play.GetMarkerPosition", PlayGetMarkerPosition);
    registry.Add("sl.play.SetPositionUpdatePeriod", PlaySetPositionUpdatePeriod);
    registry.Add("sl.play.GetPositionUpdatePeriod", PlayGetPositionUpdatePeriod);

    registry.Add("sl.record.SetRecordState", RecordSetRecordState);
    registry.Add("sl.record.GetRecordState", RecordGetRecordState);
    registry.Add("sl.record.SetDurationLimit", RecordSetDurationLimit);
    registry.Add("sl.record.GetPosition", RecordGetPosition);
    registry.Add("sl.record.RegisterCallback", RecordRegisterCallback);
    registry.Add("sl.record.SetCallbackEventsMask", RecordSetCallbackEventsMask);
    registry.Add("sl.record.GetCallbackEventsMask", RecordGetCallbackEventsMask);
    registry.Add("sl.record.SetMarkerPosition", RecordSetMarkerPosition);
    registry.Add("sl.record.ClearMarkerPosition", RecordClearMarkerPosition);
    registry.Add("sl.record.GetMarkerPosition", RecordGetMarkerPosition);
    registry.Add("sl.record.SetPositionUpdatePeriod", RecordSetPositionUpdatePeriod);
    registry.Add("sl.record.GetPositionUpdatePeriod", RecordGetPositionUpdatePeriod);

    registry.Add("sl.queue.Enqueue", QueueEnqueue);
    registry.Add("sl.queue.Clear", QueueClear);
    registry.Add("sl.queue.GetState", QueueGetState);
    registry.Add("sl.queue.RegisterCallback", QueueRegisterCallback);

    registry.Add("sl.volume.SetVolumeLevel", VolumeSetLevel);
    registry.Add("sl.volume.GetVolumeLevel", VolumeGetLevel);
    registry.Add("sl.volume.GetMaxVolumeLevel", VolumeGetMaxLevel);
    registry.Add("sl.volume.SetMute", VolumeSetMute);
    registry.Add("sl.volume.GetMute", VolumeGetMute);
    registry.Add("sl.volume.EnableStereoPosition", VolumeEnableStereoPosition);
    registry.Add("sl.volume.IsEnabledStereoPosition", VolumeIsEnabledStereoPosition);
    registry.Add("sl.volume.SetStereoPosition", VolumeSetStereoPosition);
    registry.Add("sl.volume.GetStereoPosition", VolumeGetStereoPosition);

    registry.Add("sl.configuration.SetConfiguration", ConfigSetConfiguration);
    registry.Add("sl.configuration.GetConfiguration", ConfigGetConfiguration);
    registry.Add("sl.configuration.AcquireJavaProxy", ConfigAcquireJavaProxy);
    registry.Add("sl.configuration.ReleaseJavaProxy", ConfigReleaseJavaProxy);

    registry.Add("sl.outputmix.GetDestinationOutputDeviceIDs",
                 MixGetDestinationOutputDeviceIDs);
    registry.Add("sl.outputmix.RegisterDeviceChangeCallback",
                 MixRegisterDeviceChangeCallback);
    registry.Add("sl.outputmix.ReRoute", MixReRoute);
}

}  // namespace armbat







