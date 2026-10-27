#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>

#include "armbat/log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
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

// OpenSL ES on Android is a tree of C++-style objects. The guest holds a
// pointer to an object block whose first word points at a table of function
// pointers, and it calls the methods through that table. The table and every
// entry in it must live in guest memory, so the host's own OpenSL ES cannot be
// forwarded the way EGL is: its vtables are host pointers the guest cannot
// execute.
//
// This file builds the objects itself. A vtable is a block of ordinary HLE
// stubs, and the object state (realized, play state, volume, queue callback)
// stays on the host, keyed by the guest address of the block. An interface is a
// second guest block: [vtable][owner], so a method always learns which object
// it was called on without the guest seeing any host pointer.
//
// The player's PCM goes to the platform's own OpenSL ES engine, the same way
// EGL is forwarded to the platform's GLES driver: the guest's objects stay in
// the container, and only the samples cross over. The host engine then consumes
// them at the device rate and calls back, which is what the guest's buffer
// queue callback expects to see, and the platform handles mixing, resampling,
// volume keys and audio focus on its own.

namespace armbat {
namespace {

constexpr std::uint32_t kSlSuccess = 0;
constexpr std::uint32_t kSlParameterInvalid = 2;
constexpr std::uint32_t kSlMemoryFailure = 3;
constexpr std::uint32_t kSlFeatureUnsupported = 12;

constexpr std::uint32_t kObjectStateUnrealized = 1;
constexpr std::uint32_t kObjectStateRealized = 2;
constexpr std::uint32_t kPlayStopped = 1;
constexpr std::uint32_t kPlayPaused = 2;
constexpr std::uint32_t kPlayPlaying = 3;
constexpr std::uint32_t kSlTimeUnknown = 0xFFFFFFFFu;

// Every shim object (SL_IID_*) is a word pointing at a descriptor. The
// generator stamps the descriptor with this magic and the interface's own name,
// which is how the container tells SL_IID_PLAY from SL_IID_VOLUME without a
// private table of addresses that changes with the load address of the shim.
constexpr std::uint32_t kDescriptorMagic = 0x41524254u;  // "ARBT"
constexpr std::uint32_t kDescriptorNameOffset = 4;

// Vtables are made longer than the largest real interface: a slot past the end
// of the struct is never called, and a slot past the end of the *block* would
// be a jump into whatever follows. Forty covers every interface in OpenSL ES
// 1.0.1 plus the Android extensions.
constexpr std::uint32_t kSlotsPerVtable = 40;
constexpr std::uint32_t kBlockBytes = 8;

enum class Kind : std::uint32_t { Engine, OutputMix, Player, Recorder };

// The PCM format the player was configured with. It is kept on the object
// because the output device can only be opened once the engine starts pushing
// samples, not when the player is created.
struct PcmFormat {
    std::uint32_t locator = 0;
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    std::uint32_t bits = 0;
};

// The platform stream a player's samples go to, opened on the first buffer.
//
// The samples are forwarded to the host's own OpenSL ES engine, the same way
// EGL is forwarded: our engine stays in the guest, the PCM goes out through a
// real buffer queue. That is what paces the guest's mixer -- the host queue
// takes a buffer once per device period -- and the platform then does the
// mixing, resampling, volume keys and audio focus that a game expects.
struct AudioDevice {
    SLObjectItf engine_object = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf mix_object = nullptr;
    SLObjectItf player_object = nullptr;
    SLPlayItf play = nullptr;
    SLAndroidSimpleBufferQueueItf queue = nullptr;

    // The host queue holds a couple of buffers at once; the copies have to stay
    // alive until it has played them, and the free slot count is what the
    // guest's Enqueue waits for.
    static constexpr std::uint32_t kQueueDepth = 2;
    std::mutex mutex;
    std::condition_variable room;
    std::deque<std::vector<std::uint8_t>> buffers;
    std::uint32_t in_flight = 0;

    // Which guest callback to wake when the host consumes a buffer. The guest
    // registers it on its queue interface; the host callback runs on the
    // platform's thread and cannot look into the guest.
    std::uint32_t itf = 0;
    std::uint32_t callback = 0;
    std::uint32_t context = 0;
};

struct ObjectState {
    Kind kind = Kind::Engine;
    bool realized = false;
    std::uint32_t play_state = kPlayStopped;
    std::int32_t volume = 0;
    std::uint32_t muted = 0;
    std::uint32_t queue_callback = 0;
    std::uint32_t queue_context = 0;
    std::uint32_t buffers_queued = 0;
    PcmFormat format;
    std::shared_ptr<AudioDevice> device;
    bool device_failed = false;
    std::vector<std::uint32_t> blocks;
};

HleRegistry* g_registry = nullptr;

// Guest threads reach these objects concurrently: the mixer thread drives the
// player while the application thread realizes and queries it.
std::mutex g_mutex;
std::unordered_map<std::uint32_t, ObjectState> g_objects;
std::unordered_map<std::uint32_t, std::uint32_t> g_block_owner;
std::unordered_map<std::string, std::uint32_t> g_vtables;

void LogOnce(const char* category, const std::string& line) {
    if (LogAllowed(category, 1)) {
        ARMBAT_LOG_INFO("Armbat", "%s", line.c_str());
    }
}

// Defined with the output device, far below the object table that has to drop
// the last reference to one.
void CloseDevice(AudioDevice& device);

// ---- callback thread -----------------------------------------------------
//
// OpenSL ES delivers a buffer-queue callback on its own thread, and an engine
// relies on that: it fills the queue once at startup and refills it from the
// callback. Delivering the callback from inside Enqueue would recurse (the
// callback enqueues again), and delivering it on the thread that filled the
// queue would block the application -- that thread is the engine's main one.
// One container thread therefore runs the callbacks, in the order the buffers
// were consumed, exactly like the platform engine does.
struct CallbackRequest {
    std::uint32_t callback = 0;
    std::uint32_t itf = 0;
    std::uint32_t context = 0;
};

GuestCallRunner g_guest_call;
std::mutex g_callback_mutex;
std::condition_variable g_callback_ready;
std::deque<CallbackRequest> g_callback_queue;
std::thread g_callback_thread;
bool g_callback_stopping = false;

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
        if (runner) {
            runner(request.callback, {request.itf, request.context});
        }
    }
}

// One callback per consumed buffer, like the device: an engine that mixes ahead
// relies on each of its enqueues coming back as a wake-up. The cap only keeps a
// stalled engine from growing the queue without bound.
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

void WriteU32(HleCall& call, std::uint32_t address, std::uint32_t value) {
    if (address != 0) {
        call.CopyOut(address, &value, sizeof(value));
    }
}

// The interface name behind an SLInterfaceID, or false when the guest handed
// over something that is not a shim descriptor.
bool ReadInterfaceId(HleCall& call, std::uint32_t iid, std::string& name) {
    if (iid == 0) {
        return false;
    }
    std::uint32_t magic = 0;
    std::uint32_t name_address = 0;
    if (!call.CopyIn(iid, &magic, sizeof(magic)) || magic != kDescriptorMagic) {
        return false;
    }
    if (!call.CopyIn(iid + kDescriptorNameOffset, &name_address, sizeof(name_address))) {
        return false;
    }
    return name_address != 0 && call.ReadCString(name_address, name, 48);
}

// The vtable layout a requested interface needs. An empty answer means the
// container does not know the method order and must not guess one: a wrong
// table turns the first call into a jump to an unrelated method.
std::string InterfaceKey(const std::string& name) {
    if (name == "SL_IID_OBJECT") return "object";
    if (name == "SL_IID_ENGINE") return "engine";
    if (name == "SL_IID_PLAY") return "play";
    if (name == "SL_IID_BUFFERQUEUE") return "bufferqueue";
    if (name == "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") return "androidbufferqueue";
    if (name == "SL_IID_VOLUME") return "volume";
    if (name == "SL_IID_ANDROIDCONFIGURATION") return "configuration";
    if (name == "SL_IID_OUTPUTMIX") return "outputmix";
    // Everything else the platform exports is optional for an output engine:
    // reverb, effects, recording, prefetch. Refusing them is what an engine
    // without those features does, and callers fall back. They still get a
    // table of no-op stubs so a required request does not fail the whole
    // player.
    if (name.rfind("SL_IID_", 0) == 0 && name != "SL_IID_NULL") return "generic";
    return {};
}

std::uint32_t Stub(const std::string& name) {
    return g_registry != nullptr ? g_registry->Resolve(name) : 0;
}

// Builds (once) the guest-side vtable for one interface key. Called with
// g_mutex held.
std::uint32_t VtableFor(const std::string& key) {
    const auto cached = g_vtables.find(key);
    if (cached != g_vtables.end()) {
        return cached->second;
    }
    if (g_registry == nullptr) {
        return 0;
    }
    const std::uint32_t unused = Stub("sl.unused");
    if (unused == 0) {
        return 0;
    }
    std::uint32_t words[kSlotsPerVtable] = {};
    for (std::uint32_t& word : words) {
        word = unused;
    }
    const auto place = [&words](std::uint32_t slot, const std::string& name) {
        if (slot >= kSlotsPerVtable) {
            return;
        }
        const std::uint32_t stub = Stub(name);
        if (stub != 0) {
            words[slot] = stub;
        }
    };

    if (key == "object") {
        place(0, "sl.object.Realize");
        place(2, "sl.object.GetState");
        place(3, "sl.object.GetInterface");
        place(6, "sl.object.Destroy");
        place(8, "sl.object.GetPriority");
    } else if (key == "engine") {
        place(2, "sl.engine.CreateAudioPlayer");
        place(7, "sl.engine.CreateOutputMix");
        place(10, "sl.engine.QueryNumSupportedInterfaces");
        place(11, "sl.engine.QuerySupportedInterfaces");
    } else if (key == "play") {
        place(0, "sl.play.SetPlayState");
        place(1, "sl.play.GetPlayState");
        place(2, "sl.play.GetDuration");
        place(3, "sl.play.GetPosition");
        place(4, "sl.play.RegisterCallback");
        place(5, "sl.play.SetCallbackEventsMask");
        place(6, "sl.play.GetCallbackEventsMask");
        place(9, "sl.play.GetMarkerPosition");
        place(11, "sl.play.GetPositionUpdatePeriod");
    } else if (key == "androidbufferqueue") {
        place(0, "sl.queue.Enqueue");
        place(1, "sl.queue.Clear");
        place(2, "sl.queue.GetState");
        place(3, "sl.queue.RegisterCallback");
    } else if (key == "bufferqueue") {
        place(0, "sl.queue.Enqueue");
        place(1, "sl.queue.Clear");
        place(2, "sl.queue.GetState");
        place(3, "sl.queue.RegisterCallback");
    } else if (key == "volume") {
        place(0, "sl.volume.SetVolumeLevel");
        place(1, "sl.volume.GetVolumeLevel");
        place(2, "sl.volume.GetMaxVolumeLevel");
        place(3, "sl.volume.SetMute");
        place(4, "sl.volume.GetMute");
        place(5, "sl.volume.EnableStereoPosition");
        place(6, "sl.volume.IsEnabledStereoPosition");
        place(7, "sl.volume.SetStereoPosition");
        place(8, "sl.volume.GetStereoPosition");
    } else if (key == "configuration") {
        place(0, "sl.configuration.SetConfiguration");
        place(1, "sl.configuration.GetConfiguration");
        place(2, "sl.configuration.AcquireJavaProxy");
        place(3, "sl.configuration.ReleaseJavaProxy");
    } else if (key == "outputmix") {
        place(0, "sl.outputmix.GetDestinationOutputDeviceIDs");
        place(1, "sl.outputmix.RegisterDeviceChangeCallback");
    }

    const std::uint32_t table_address = g_registry->AllocateData(sizeof(words));
    if (table_address == 0) {
        return 0;
    }
    std::memcpy(g_registry->Arena().Ptr(table_address), words, sizeof(words));
    g_vtables.emplace(key, table_address);
    return table_address;
}

// A guest block the caller can hold: [vtable][owner object]. Called with
// g_mutex held.
std::uint32_t CreateBlock(const std::string& key, std::uint32_t owner) {
    const std::uint32_t table = VtableFor(key);
    if (table == 0 || g_registry == nullptr) {
        return 0;
    }
    const std::uint32_t block = g_registry->AllocateData(kBlockBytes);
    if (block == 0) {
        return 0;
    }
    const std::uint32_t words[2] = {table, owner};
    std::memcpy(g_registry->Arena().Ptr(block), words, sizeof(words));
    g_block_owner[block] = owner;
    const auto object = g_objects.find(owner);
    if (object != g_objects.end()) {
        object->second.blocks.push_back(block);
    }
    return block;
}

// Called with g_mutex held.
std::uint32_t CreateObject(Kind kind) {
    if (g_registry == nullptr) {
        return 0;
    }
    const std::uint32_t table = VtableFor("object");
    if (table == 0) {
        return 0;
    }
    const std::uint32_t block = g_registry->AllocateData(kBlockBytes);
    if (block == 0) {
        return 0;
    }
    const std::uint32_t words[2] = {table, block};
    std::memcpy(g_registry->Arena().Ptr(block), words, sizeof(words));
    ObjectState state;
    state.kind = kind;
    g_objects.emplace(block, std::move(state));
    g_block_owner[block] = block;
    return block;
}

// The object a `self` pointer belongs to. A block the container never handed
// out is not an object: answering "success" to it would hide a guest bug.
ObjectState* Owner(std::uint32_t self) {
    const auto block = g_block_owner.find(self);
    if (block == g_block_owner.end()) {
        return nullptr;
    }
    const auto object = g_objects.find(block->second);
    return object != g_objects.end() ? &object->second : nullptr;
}

void ReleaseObject(std::uint32_t self) {
    std::vector<std::uint32_t> blocks;
    std::shared_ptr<AudioDevice> device;
    {
        const std::lock_guard lock(g_mutex);
        const auto object = g_objects.find(self);
        if (object == g_objects.end()) {
            return;
        }
        blocks = std::move(object->second.blocks);
        device = std::move(object->second.device);
        g_objects.erase(object);
        g_block_owner.erase(self);
        blocks.push_back(self);
        for (const std::uint32_t block : blocks) {
            g_block_owner.erase(block);
        }
    }
    if (device != nullptr) {
        CloseDevice(*device);
    }
    for (const std::uint32_t block : blocks) {
        g_registry->ReleaseData(block);
    }
}

// ---- object interface -----------------------------------------------------

void ObjectRealize(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    if (ObjectState* state = Owner(call.Arg(0))) {
        state->realized = true;
    }
    call.Return(kSlSuccess);
}

void ObjectGetState(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    const ObjectState* state = Owner(call.Arg(0));
    if (state == nullptr) {
        call.Return(kSlParameterInvalid);
        return;
    }
    WriteU32(call, call.Arg(1), state->realized ? kObjectStateRealized : kObjectStateUnrealized);
    call.Return(kSlSuccess);
}

void ObjectGetInterface(HleCall& call) {
    const std::uint32_t self = call.Arg(0);
    const std::uint32_t iid = call.Arg(1);
    const std::uint32_t out = call.Arg(2);
    if (self == 0 || out == 0) {
        call.Return(kSlParameterInvalid);
        return;
    }
    std::string name;
    if (!ReadInterfaceId(call, iid, name)) {
        call.Return(kSlFeatureUnsupported);
        return;
    }
    const std::string key = InterfaceKey(name);
    if (key.empty()) {
        LogOnce(("opensles-missing:" + name).c_str(), "opensles: no interface " + name);
        call.Return(kSlFeatureUnsupported);
        return;
    }
    if (key == "object") {
        // SL_IID_OBJECT is the object itself, not a second block.
        WriteU32(call, out, self);
        call.Return(kSlSuccess);
        return;
    }

    std::uint32_t block = 0;
    {
        const std::lock_guard lock(g_mutex);
        if (Owner(self) == nullptr) {
            call.Return(kSlParameterInvalid);
            return;
        }
        block = CreateBlock(key, self);
    }
    if (block == 0) {
        call.Return(kSlMemoryFailure);
        return;
    }
    WriteU32(call, out, block);
    LogOnce(("opensles-iface:" + name).c_str(), "opensles: interface " + name + " -> 0x" +
                                                       std::to_string(block));
    call.Return(kSlSuccess);
}

void ObjectDestroy(HleCall& call) {
    ReleaseObject(call.Arg(0));
}

void ObjectGetPriority(HleCall& call) {
    WriteU32(call, call.Arg(1), 0);
    WriteU32(call, call.Arg(2), 0);
    call.Return(kSlSuccess);
}

// ---- engine interface -----------------------------------------------------

void EngineCreateOutputMix(HleCall& call) {
    const std::uint32_t out = call.Arg(1);
    if (out == 0) {
        call.Return(kSlParameterInvalid);
        return;
    }
    std::uint32_t block = 0;
    {
        const std::lock_guard lock(g_mutex);
        if (Owner(call.Arg(0)) == nullptr) {
            call.Return(kSlParameterInvalid);
            return;
        }
        block = CreateObject(Kind::OutputMix);
    }
    if (block == 0) {
        call.Return(kSlMemoryFailure);
        return;
    }
    WriteU32(call, out, block);
    LogOnce("opensles-mix", "opensles: output mix created at 0x" + std::to_string(block));
    call.Return(kSlSuccess);
}

bool ReadPcmFormat(HleCall& call, std::uint32_t source, PcmFormat* format) {
    std::uint32_t locator_pointer = 0;
    std::uint32_t format_pointer = 0;
    if (!call.CopyIn(source, &locator_pointer, sizeof(locator_pointer)) ||
        !call.CopyIn(source + 4, &format_pointer, sizeof(format_pointer))) {
        return false;
    }
    if (locator_pointer != 0) {
        call.CopyIn(locator_pointer, &format->locator, sizeof(format->locator));
    }
    if (format_pointer == 0) {
        return false;
    }
    // SLDataFormat_PCM and SLAndroidDataFormat_PCM_EX agree on the leading
    // fields: formatType, numChannels, samplesPerSec (milliHertz), bitsPerSample.
    std::uint32_t fields[4] = {};
    if (!call.CopyIn(format_pointer, fields, sizeof(fields))) {
        return false;
    }
    format->channels = fields[1];
    format->rate = fields[2] / 1000;
    format->bits = fields[3];
    return true;
}

// ---- output device -------------------------------------------------------

// Releases the host objects. The platform's queue callback stops arriving once
// the player is destroyed, so no guest callback can be posted after this.
void CloseDevice(AudioDevice& device) {
    if (device.player_object != nullptr) {
        if (device.play != nullptr) {
            (*device.play)->SetPlayState(device.play, SL_PLAYSTATE_STOPPED);
        }
        (*device.player_object)->Destroy(device.player_object);
        device.player_object = nullptr;
        device.play = nullptr;
        device.queue = nullptr;
    }
    if (device.mix_object != nullptr) {
        (*device.mix_object)->Destroy(device.mix_object);
        device.mix_object = nullptr;
    }
    if (device.engine_object != nullptr) {
        (*device.engine_object)->Destroy(device.engine_object);
        device.engine_object = nullptr;
        device.engine = nullptr;
    }
    const std::lock_guard lock(device.mutex);
    device.buffers.clear();
    device.in_flight = 0;
    device.callback = 0;
}

// Runs on the platform's own audio thread: one buffer has been played and the
// guest's queue has room again. That is the callback an engine's mixer waits
// for, so it is handed to the container thread the guest can be entered from.
void HostQueueCallback(SLAndroidSimpleBufferQueueItf queue, void* context) {
    auto* device = static_cast<AudioDevice*>(context);
    CallbackRequest request;
    {
        const std::lock_guard lock(device->mutex);
        if (!device->buffers.empty()) {
            device->buffers.pop_front();
        }
        if (device->in_flight > 0) {
            --device->in_flight;
        }
        request.callback = device->callback;
        request.itf = device->itf;
        request.context = device->context;
        device->room.notify_one();
    }
    if (request.callback != 0) {
        PostCallback(request);
    }
}

// Interleaved signed 16-bit PCM, which is what every Android engine hands to
// OpenSL ES. Anything else is left silent rather than played as noise.
std::shared_ptr<AudioDevice> OpenDevice(const PcmFormat& format) {
    if (format.bits != 16 || format.channels == 0 || format.channels > 2 || format.rate < 8000) {
        LogOnce("opensles-format", "opensles: no output for " + std::to_string(format.rate) +
                                       " Hz " + std::to_string(format.channels) + " ch " +
                                       std::to_string(format.bits) + "-bit");
        return nullptr;
    }
    auto device = std::make_shared<AudioDevice>();
    // The HLE mirrors the platform's own API surface, deprecated entry points
    // included, so this one warning is expected rather than actionable.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const SLresult created =
        slCreateEngine(&device->engine_object, 0, nullptr, 0, nullptr, nullptr);
#pragma clang diagnostic pop
    if (created != SL_RESULT_SUCCESS || device->engine_object == nullptr) {
        LogOnce("opensles-device", "opensles: no output engine");
        return nullptr;
    }
    (*device->engine_object)->Realize(device->engine_object, SL_BOOLEAN_FALSE);
    (*device->engine_object)->GetInterface(device->engine_object, SL_IID_ENGINE, &device->engine);
    if (device->engine == nullptr ||
        (*device->engine)->CreateOutputMix(device->engine, &device->mix_object, 0, nullptr,
                                           nullptr) != SL_RESULT_SUCCESS ||
        device->mix_object == nullptr) {
        LogOnce("opensles-device", "opensles: no output mix");
        CloseDevice(*device);
        return nullptr;
    }
    (*device->mix_object)->Realize(device->mix_object, SL_BOOLEAN_FALSE);

    SLDataLocator_AndroidSimpleBufferQueue locator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, AudioDevice::kQueueDepth};
    SLDataFormat_PCM pcm = {};
    pcm.formatType = SL_DATAFORMAT_PCM;
    pcm.numChannels = format.channels;
    pcm.samplesPerSec = format.rate * 1000;
    pcm.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
    pcm.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
    pcm.channelMask = format.channels == 1 ? SL_SPEAKER_FRONT_CENTER
                                           : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT);
    pcm.endianness = SL_BYTEORDER_LITTLEENDIAN;
    SLDataSource source = {&locator, &pcm};
    SLDataLocator_OutputMix output = {SL_DATALOCATOR_OUTPUTMIX, device->mix_object};
    SLDataSink sink = {&output, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean required[] = {SL_BOOLEAN_TRUE};
    if ((*device->engine)->CreateAudioPlayer(device->engine, &device->player_object, &source, &sink,
                                             1, ids, required) != SL_RESULT_SUCCESS ||
        device->player_object == nullptr) {
        LogOnce("opensles-device", "opensles: output player refused");
        CloseDevice(*device);
        return nullptr;
    }
    (*device->player_object)->Realize(device->player_object, SL_BOOLEAN_FALSE);
    (*device->player_object)->GetInterface(device->player_object, SL_IID_PLAY, &device->play);
    (*device->player_object)
        ->GetInterface(device->player_object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &device->queue);
    if (device->play == nullptr || device->queue == nullptr) {
        LogOnce("opensles-device", "opensles: output player incomplete");
        CloseDevice(*device);
        return nullptr;
    }
    (*device->queue)->RegisterCallback(device->queue, HostQueueCallback, device.get());
    (*device->play)->SetPlayState(device->play, SL_PLAYSTATE_PLAYING);
    LogOnce("opensles-device", "opensles: output device " + std::to_string(format.rate) + " Hz " +
                                   std::to_string(format.channels) + " ch 16-bit opened");
    return device;
}

// Runs without g_mutex: a full host queue makes this wait for the device, and
// the object map has to stay available to the engine's control thread meanwhile.
void EnqueueToDevice(AudioDevice* device, const void* data, std::size_t bytes) {
    if (device == nullptr || device->queue == nullptr || data == nullptr || bytes == 0) {
        return;
    }
    const auto* first = static_cast<const std::uint8_t*>(data);
    const auto* last = first + bytes;
    std::unique_lock lock(device->mutex);
    const bool room = device->room.wait_for(lock, std::chrono::milliseconds(100), [device] {
        return device->in_flight < AudioDevice::kQueueDepth;
    });
    if (!room) {
        // The device stopped draining: drop the buffer instead of freezing the
        // guest's mixer thread, the way a full queue drops it on real hardware.
        return;
    }
    device->buffers.emplace_back(first, last);
    const SLresult result =
        (*device->queue)->Enqueue(device->queue, device->buffers.back().data(), bytes);
    if (result != SL_RESULT_SUCCESS) {
        device->buffers.pop_back();
        return;
    }
    ++device->in_flight;
}

void EngineCreateAudioPlayer(HleCall& call) {
    const std::uint32_t out = call.Arg(1);
    if (out == 0) {
        call.Return(kSlParameterInvalid);
        return;
    }
    std::uint32_t block = 0;
    {
        const std::lock_guard lock(g_mutex);
        if (Owner(call.Arg(0)) == nullptr) {
            call.Return(kSlParameterInvalid);
            return;
        }
        block = CreateObject(Kind::Player);
    }
    if (block == 0) {
        call.Return(kSlMemoryFailure);
        return;
    }
    WriteU32(call, out, block);

    PcmFormat format;
    if (ReadPcmFormat(call, call.Arg(2), &format)) {
        LogOnce("opensles-player-format",
                "opensles: player " + std::to_string(format.rate) + " Hz " +
                    std::to_string(format.channels) + " ch " + std::to_string(format.bits) +
                    "-bit locator=0x" + std::to_string(format.locator));
        const std::lock_guard lock(g_mutex);
        if (ObjectState* state = Owner(block)) {
            state->format = format;
        }
    }
    LogOnce("opensles-player", "opensles: player created at 0x" + std::to_string(block));
    call.Return(kSlSuccess);
}

void EngineQueryNumSupportedInterfaces(HleCall& call) {
    WriteU32(call, call.Arg(1), 0);
    call.Return(kSlSuccess);
}

void EngineQuerySupportedInterfaces(HleCall& call) {
    call.Return(kSlFeatureUnsupported);
}

// ---- play interface -------------------------------------------------------

void PlaySetPlayState(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    ObjectState* state = Owner(call.Arg(0));
    if (state == nullptr) {
        call.Return(kSlParameterInvalid);
        return;
    }
    state->play_state = call.Arg(1);
    // The host engine owns the device now, so stopping the guest's player has
    // to stop the samples too; otherwise a paused game keeps playing.
    if (state->device != nullptr && state->device->play != nullptr) {
        const SLuint32 target = state->play_state == kPlayPlaying ? SL_PLAYSTATE_PLAYING
                                : state->play_state == kPlayPaused ? SL_PLAYSTATE_PAUSED
                                                                   : SL_PLAYSTATE_STOPPED;
        (*state->device->play)->SetPlayState(state->device->play, target);
    }
    if (state->play_state == kPlayPlaying) {
        LogOnce("opensles-playing", "opensles: player set to playing");
    }
    call.Return(kSlSuccess);
}

void PlayGetPlayState(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    const ObjectState* state = Owner(call.Arg(0));
    if (state == nullptr) {
        call.Return(kSlParameterInvalid);
        return;
    }
    WriteU32(call, call.Arg(1), state->play_state);
    call.Return(kSlSuccess);
}

void PlayGetDuration(HleCall& call) {
    WriteU32(call, call.Arg(1), kSlTimeUnknown);
    call.Return(kSlSuccess);
}

void PlayGetPosition(HleCall& call) {
    WriteU32(call, call.Arg(1), 0);
    call.Return(kSlSuccess);
}

void PlayRegisterCallback(HleCall& call) {
    call.Return(kSlSuccess);
}

void PlaySetCallbackEventsMask(HleCall& call) {
    call.Return(kSlSuccess);
}

void PlayGetCallbackEventsMask(HleCall& call) {
    WriteU32(call, call.Arg(1), 0);
    call.Return(kSlSuccess);
}

void PlayGetMarkerPosition(HleCall& call) {
    WriteU32(call, call.Arg(1), 0);
    call.Return(kSlSuccess);
}

void PlayGetPositionUpdatePeriod(HleCall& call) {
    WriteU32(call, call.Arg(1), 1000);
    call.Return(kSlSuccess);
}

// ---- buffer queue ---------------------------------------------------------
//
// The samples leave the guest here. The host engine consumes them one device
// period at a time and reports each one through HostQueueCallback, which is
// what the guest's own queue callback then sees.

void QueueEnqueue(HleCall& call) {
    std::shared_ptr<AudioDevice> device;
    const std::uint32_t itf = call.Arg(0);
    const std::uint32_t address = call.Arg(1);
    const std::uint32_t size = call.Arg(2);
    std::uint32_t callback = 0;
    std::uint32_t context = 0;
    {
        const std::lock_guard lock(g_mutex);
        ObjectState* state = Owner(itf);
        if (state == nullptr) {
            call.Return(kSlParameterInvalid);
            return;
        }
        ++state->buffers_queued;
        if (state->device == nullptr && !state->device_failed) {
            state->device = OpenDevice(state->format);
            state->device_failed = state->device == nullptr;
        }
        device = state->device;
        callback = state->queue_callback;
        context = state->queue_context;
        LogOnce("opensles-enqueue", "opensles: first buffer of " + std::to_string(size) +
                                        " bytes accepted");
    }
    if (device != nullptr) {
        {
            const std::lock_guard lock(device->mutex);
            device->itf = itf;
            device->callback = callback;
            device->context = context;
        }
        if (const void* data = call.Mapped(address, size)) {
            EnqueueToDevice(device.get(), data, size);
        }
    }
    call.Return(kSlSuccess);
}

void QueueClear(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    if (ObjectState* state = Owner(call.Arg(0))) {
        state->buffers_queued = 0;
    }
    call.Return(kSlSuccess);
}

// The engine uses the state to know how much of its audio is still queued, so
// it reports the host queue's depth rather than an always-empty queue.
void QueueGetState(HleCall& call) {
    const std::uint32_t out = call.Arg(1);
    std::uint32_t count = 0;
    std::uint32_t index = 0;
    {
        const std::lock_guard lock(g_mutex);
        const ObjectState* state = Owner(call.Arg(0));
        if (state != nullptr) {
            index = state->buffers_queued;
            if (state->device != nullptr) {
                const std::lock_guard device_lock(state->device->mutex);
                count = state->device->in_flight;
            }
        }
    }
    if (out != 0) {
        const std::uint32_t fields[2] = {count, index};
        call.CopyOut(out, fields, sizeof(fields));
    }
    call.Return(kSlSuccess);
}

void QueueRegisterCallback(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    ObjectState* state = Owner(call.Arg(0));
    if (state == nullptr) {
        call.Return(kSlParameterInvalid);
        return;
    }
    state->queue_callback = call.Arg(1);
    state->queue_context = call.Arg(2);
    if (state->device != nullptr) {
        const std::lock_guard device_lock(state->device->mutex);
        state->device->itf = call.Arg(0);
        state->device->callback = state->queue_callback;
        state->device->context = state->queue_context;
    }
    LogOnce("opensles-callback", "opensles: buffer queue callback registered");
    call.Return(kSlSuccess);
}

// ---- volume interface -----------------------------------------------------

void VolumeSetLevel(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    if (ObjectState* state = Owner(call.Arg(0))) {
        state->volume = static_cast<std::int32_t>(call.Arg(1));
    }
    call.Return(kSlSuccess);
}

void VolumeGetLevel(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    const ObjectState* state = Owner(call.Arg(0));
    WriteU32(call, call.Arg(1), state != nullptr ? static_cast<std::uint32_t>(state->volume) : 0);
    call.Return(kSlSuccess);
}

void VolumeGetMaxLevel(HleCall& call) {
    WriteU32(call, call.Arg(1), 0);
    call.Return(kSlSuccess);
}

void VolumeSetMute(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    if (ObjectState* state = Owner(call.Arg(0))) {
        state->muted = call.Arg(1);
    }
    call.Return(kSlSuccess);
}

void VolumeGetMute(HleCall& call) {
    const std::lock_guard lock(g_mutex);
    const ObjectState* state = Owner(call.Arg(0));
    WriteU32(call, call.Arg(1), state != nullptr ? state->muted : 0);
    call.Return(kSlSuccess);
}

void VolumeEnableStereoPosition(HleCall& call) {
    call.Return(kSlSuccess);
}

void VolumeIsEnabledStereoPosition(HleCall& call) {
    // Stereo positioning is not implemented; saying so lets the caller keep
    // using its own panning instead of trusting a value that never applies.
    WriteU32(call, call.Arg(1), 0);
    call.Return(kSlSuccess);
}

void VolumeSetStereoPosition(HleCall& call) {
    call.Return(kSlSuccess);
}

void VolumeGetStereoPosition(HleCall& call) {
    WriteU32(call, call.Arg(1), 0);
    call.Return(kSlSuccess);
}

// ---- Android configuration ------------------------------------------------

void ConfigSetConfiguration(HleCall& call) {
    call.Return(kSlSuccess);
}

void ConfigGetConfiguration(HleCall& call) {
    const std::uint32_t size_address = call.Arg(2);
    const std::uint32_t value = call.Arg(3);
    std::uint32_t size = 0;
    if (size_address != 0) {
        call.CopyIn(size_address, &size, sizeof(size));
    }
    if (value != 0 && size >= sizeof(std::uint32_t)) {
        WriteU32(call, value, 0);
        WriteU32(call, size_address, sizeof(std::uint32_t));
    } else if (size_address != 0) {
        WriteU32(call, size_address, sizeof(std::uint32_t));
    }
    call.Return(kSlSuccess);
}

void ConfigAcquireJavaProxy(HleCall& call) {
    call.Return(kSlFeatureUnsupported);
}

void ConfigReleaseJavaProxy(HleCall& call) {
    call.Return(kSlSuccess);
}

// ---- output mix -----------------------------------------------------------

void MixGetDestinationOutputDeviceIDs(HleCall& call) {
    WriteU32(call, call.Arg(1), 0);
    call.Return(kSlSuccess);
}

void MixRegisterDeviceChangeCallback(HleCall& call) {
    call.Return(kSlSuccess);
}

// ---- entry point ----------------------------------------------------------

void SlCreateEngine(HleCall& call) {
    const std::uint32_t out = call.Arg(0);
    if (out == 0) {
        call.Return(kSlParameterInvalid);
        return;
    }
    std::uint32_t block = 0;
    {
        const std::lock_guard lock(g_mutex);
        block = CreateObject(Kind::Engine);
    }
    if (block == 0) {
        call.Return(kSlMemoryFailure);
        return;
    }
    WriteU32(call, out, block);
    LogOnce("opensles-engine", "opensles: engine created at 0x" + std::to_string(block));
    call.Return(kSlSuccess);
}

}  // namespace

void SetGuestCallRunner(GuestCallRunner runner) {
    const std::lock_guard lock(g_callback_mutex);
    g_guest_call = std::move(runner);
}

void StopGuestCallThread() {
    std::thread thread;
    bool joined = false;
    {
        const std::lock_guard lock(g_callback_mutex);
        g_callback_queue.clear();
        if (!g_callback_thread.joinable()) {
            return;
        }
        if (g_callback_thread.get_id() == std::this_thread::get_id()) {
            // The callback itself is asking: joining would deadlock, and the
            // thread has nothing left to run once the process is going away.
            g_callback_thread.detach();
        } else {
            g_callback_stopping = true;
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

    // A slot the interface has but the container does not model. Success is the
    // honest answer for a setter and the only safe one for a method whose
    // out-parameters the caller may not read.
    registry.Add("sl.unused", [](HleCall& call) { call.Return(kSlSuccess); });

    registry.Add("sl.object.Realize", ObjectRealize);
    registry.Add("sl.object.GetState", ObjectGetState);
    registry.Add("sl.object.GetInterface", ObjectGetInterface);
    registry.Add("sl.object.Destroy", ObjectDestroy);
    registry.Add("sl.object.GetPriority", ObjectGetPriority);

    registry.Add("sl.engine.CreateAudioPlayer", EngineCreateAudioPlayer);
    registry.Add("sl.engine.CreateOutputMix", EngineCreateOutputMix);
    registry.Add("sl.engine.QueryNumSupportedInterfaces", EngineQueryNumSupportedInterfaces);
    registry.Add("sl.engine.QuerySupportedInterfaces", EngineQuerySupportedInterfaces);

    registry.Add("sl.play.SetPlayState", PlaySetPlayState);
    registry.Add("sl.play.GetPlayState", PlayGetPlayState);
    registry.Add("sl.play.GetDuration", PlayGetDuration);
    registry.Add("sl.play.GetPosition", PlayGetPosition);
    registry.Add("sl.play.RegisterCallback", PlayRegisterCallback);
    registry.Add("sl.play.SetCallbackEventsMask", PlaySetCallbackEventsMask);
    registry.Add("sl.play.GetCallbackEventsMask", PlayGetCallbackEventsMask);
    registry.Add("sl.play.GetMarkerPosition", PlayGetMarkerPosition);
    registry.Add("sl.play.GetPositionUpdatePeriod", PlayGetPositionUpdatePeriod);

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

    registry.Add("sl.outputmix.GetDestinationOutputDeviceIDs", MixGetDestinationOutputDeviceIDs);
    registry.Add("sl.outputmix.RegisterDeviceChangeCallback", MixRegisterDeviceChangeCallback);
}

}  // namespace armbat
