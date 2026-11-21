#include <android/looper.h>
#include <android/sensor.h>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "armbat/a32_machine.h"
#include "armbat/guest_arena.h"
#include "armbat/hle.h"
#include "armbat/log.h"

// The platform sensor API (android/sensor.h).
//
// Motion input is reached through this family and nothing else: libunity imports
// ASensorManager_getInstance and drops its tilt controls when the call answers
// null, which is what left a tilt-driven game without them. The device has the
// sensors, so the container runs the real libandroid implementation behind a
// pump thread of its own and copies every sample into each queue the archive
// enabled the sensor on.
//
// An ASensor* the archive holds is a four-byte cell in the HLE data area whose
// value is an index into the table below, and an ASensorEventQueue* is a handle
// for one of the records below. The archive never dereferences either, so the
// numbering is the container's.

namespace armbat {
namespace {

HleRegistry* g_registry = nullptr;

// The pump's own looper idents: what ALooper_pollOnce answered.
constexpr int kSensorIdent = 1;
constexpr int kCommandIdent = 2;

// The report rate used until the archive asks for one. A tilt control reads at
// 50 Hz; asking the sensor for its own minimum would sample for nothing.
constexpr std::int32_t kDefaultRateUs = 20000;

// A queue nobody drains is not being used; a second of the newest samples is
// enough to catch up when it resumes, and it must not grow without bound while
// the archive is paused.
constexpr std::size_t kQueueCapacity = 256;

// Long enough that an idle game does not wake for anything, short enough that a
// stopped pump is noticed.
constexpr int kPollTimeoutMs = 500;

struct Sensor {
    ASensor const* host = nullptr;
    std::int32_t type = 0;
    // The name the platform gives this sensor in an ASensorEvent, and the same
    // number ASensor_getHandle answers. An event is mapped back to its sensor by
    // that number, so the two answers have to agree.
    std::int32_t handle = 0;
    std::uint32_t token = 0;
    std::uint32_t name = 0;
    std::uint32_t vendor = 0;
};

// One queue the archive created. The pump thread fills it and the archive drains
// it from its own thread, so everything behind the descriptors is guarded.
struct SensorQueue {
    std::uint32_t looper = 0;
    std::uint32_t ident = 0;
    std::uint32_t callback = 0;
    std::uint32_t data = 0;
    int wake_read = -1;
    int wake_write = -1;
    // Table indices this queue enabled. The host sensor stays on while any queue
    // wants it, which is why the state below counts references.
    std::vector<std::uint32_t> enabled;
    // Period this queue asked for, per table index. The host queue carries one
    // period per sensor, so RefreshRate folds these into a single one.
    std::unordered_map<std::uint32_t, std::int32_t> rates;
    std::deque<ASensorEvent> events;
    std::mutex mutex;
};

enum class CommandKind { Enable, Disable, Rate };

// One change of the host queue's state. Only the pump thread touches the host
// queue, so enabling a sensor from the archive's thread is a command it picks up
// on its next wake instead of a second writer.
struct Command {
    CommandKind kind = CommandKind::Enable;
    std::uint32_t index = 0;
    std::int32_t rate = kDefaultRateUs;
};

struct SensorState {
    std::mutex mutex;  // manager, sensors, refs, rates, list_address, queues
    ASensorManager* manager = nullptr;
    std::vector<Sensor> sensors;
    // How many queue subscriptions each sensor has, and the period the host
    // queue currently carries for it: the fastest one any of them asked for.
    std::vector<std::uint32_t> refs;
    std::vector<std::int32_t> rates;
    std::uint32_t list_address = 0;
    std::unordered_map<std::uint32_t, std::shared_ptr<SensorQueue>> queues;
    std::uint32_t next_handle = 1;

    std::mutex command_mutex;
    std::deque<Command> commands;
    int command_read = -1;
    int command_write = -1;

    std::thread pump;
    bool pump_started = false;
    std::atomic<bool> stopping{false};
};

SensorState& State() {
    // Deliberately leaked: the pump thread and a queue destroyed last both reach
    // it, and neither is ordered against the static destructors.
    static SensorState* state = new SensorState();
    return *state;
}

void SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

int CreateWakePipe(int* read_fd, int* write_fd) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return -1;
    }
    SetNonBlocking(fds[0]);
    SetNonBlocking(fds[1]);
    *read_fd = fds[0];
    *write_fd = fds[1];
    return 0;
}

std::uint32_t CopyString(HleRegistry& registry, const char* text) {
    if (text == nullptr) {
        text = "";
    }
    const std::size_t length = std::strlen(text);
    const std::uint32_t size = static_cast<std::uint32_t>(length + 1);
    const std::uint32_t address = registry.AllocateData(size);
    if (address == 0) {
        return 0;
    }
    std::memcpy(registry.Arena().Ptr(address), text, length + 1);
    return address;
}

void Post(Command command) {
    SensorState& state = State();
    {
        const std::lock_guard lock(state.command_mutex);
        state.commands.push_back(command);
    }
    if (state.command_write >= 0) {
        // A full pipe already carries a wake the pump has not taken yet.
        const char byte = 1;
        const ssize_t written = ::write(state.command_write, &byte, 1);
        (void)written;
    }
}

// Copies one host sample into every queue that enabled a sensor of its type.
void Distribute(const ASensorEvent& host_event) {
    SensorState& state = State();
    std::uint32_t index = 0;
    std::int32_t handle = 0;
    std::vector<std::shared_ptr<SensorQueue>> queues;
    {
        const std::lock_guard lock(state.mutex);
        const auto it = std::find_if(state.sensors.begin(), state.sensors.end(),
                                     [&host_event](const Sensor& sensor) {
                                         return sensor.type == host_event.type;
                                     });
        if (it == state.sensors.end()) {
            return;
        }
        index = static_cast<std::uint32_t>(std::distance(state.sensors.begin(), it));
        handle = it->handle;
        queues.reserve(state.queues.size());
        for (const auto& entry : state.queues) {
            queues.push_back(entry.second);
        }
        // The host reports the sample under its own opaque sensor number, and
        // the platform documents that number as the one ASensor_getHandle
        // answers for the sensor. The table keeps the same number, so an archive
        // that maps an event back to its sensor by handle gets what it expects.
        ASensorEvent event = host_event;
        event.sensor = handle;
        for (const auto& queue : queues) {
            const std::lock_guard queue_lock(queue->mutex);
            if (std::find(queue->enabled.begin(), queue->enabled.end(), index) ==
                queue->enabled.end()) {
                continue;
            }
            // The wake descriptor is a level, not a counter: it has to be
            // readable while the queue holds events and quiet once it is empty,
            // which is what the archive's looper polls. One byte marks the empty
            // to non-empty edge, and getEvents clears it when it drains the last
            // event; a byte per sample would leave the descriptor readable with
            // nothing behind it and the looper would spin on it.
            const bool was_empty = queue->events.empty();
            if (queue->events.size() >= kQueueCapacity) {
                queue->events.pop_front();
            }
            queue->events.push_back(event);
            if (was_empty && queue->wake_write >= 0) {
                const char byte = 1;
                const ssize_t written = ::write(queue->wake_write, &byte, 1);
                (void)written;
            }
        }
    }
}

// The descriptor is a level that mirrors the queue: readable while events wait,
// quiet once they are gone. The archive drains the events through getEvents and
// never touches the descriptor, so the wake bytes have to leave with them --
// otherwise a callback source stays readable forever, ALooper_pollAll keeps
// dispatching it, and the frame loop never returns.
void ClearWake(SensorQueue* queue) {
    if (queue->wake_read < 0) {
        return;
    }
    char buffer[64];
    while (::read(queue->wake_read, buffer, sizeof(buffer)) > 0) {
    }
}

void DrainEvents(ASensorEventQueue* queue) {
    ASensorEvent batch[16];
    ssize_t count = 0;
    while ((count = ASensorEventQueue_getEvents(queue, batch, 16)) > 0) {
        for (ssize_t i = 0; i < count; ++i) {
            Distribute(batch[i]);
        }
    }
}

void ApplyCommand(ASensorEventQueue* queue, const Command& command) {
    SensorState& state = State();
    ASensor const* sensor = nullptr;
    {
        const std::lock_guard lock(state.mutex);
        if (command.index >= state.sensors.size()) {
            return;
        }
        sensor = state.sensors[command.index].host;
    }
    switch (command.kind) {
        case CommandKind::Enable: {
            const int enabled = ASensorEventQueue_enableSensor(queue, sensor);
            const int rated = ASensorEventQueue_setEventRate(queue, sensor, command.rate);
            if (LogAllowed("sensor-host-enable", 8)) {
                ARMBAT_LOG_INFO("Armbat", "sensors: host enable index %u -> %d, rate %d -> %d",
                                command.index, enabled, command.rate, rated);
            }
            break;
        }
        case CommandKind::Disable:
            ASensorEventQueue_disableSensor(queue, sensor);
            if (LogAllowed("sensor-host-disable", 8)) {
                ARMBAT_LOG_INFO("Armbat", "sensors: host disable index %u", command.index);
            }
            break;
        case CommandKind::Rate:
            ASensorEventQueue_setEventRate(queue, sensor, command.rate);
            break;
    }
}

void DrainCommands(ASensorEventQueue* queue) {
    SensorState& state = State();
    if (state.command_read >= 0) {
        char buffer[64];
        while (::read(state.command_read, buffer, sizeof(buffer)) > 0) {
        }
    }
    std::deque<Command> commands;
    {
        const std::lock_guard lock(state.command_mutex);
        commands.swap(state.commands);
    }
    for (const Command& command : commands) {
        ApplyCommand(queue, command);
    }
}

void SensorPump() {
    pthread_setname_np(pthread_self(), "armbat-sensors");
    SensorState& state = State();
    ASensorManager* manager = nullptr;
    int command_read = -1;
    {
        const std::lock_guard lock(state.mutex);
        manager = state.manager;
        command_read = state.command_read;
    }
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (looper == nullptr || manager == nullptr) {
        ARMBAT_LOG_WARN("Armbat", "sensors: the pump has no looper");
        return;
    }
    ASensorEventQueue* queue =
        ASensorManager_createEventQueue(manager, looper, kSensorIdent, nullptr, nullptr);
    if (queue == nullptr) {
        ARMBAT_LOG_WARN("Armbat", "sensors: the host refused an event queue");
        return;
    }
    if (command_read >= 0) {
        const int added =
            ALooper_addFd(looper, command_read, kCommandIdent, ALOOPER_EVENT_INPUT, nullptr, nullptr);
        if (LogAllowed("sensor-pump", 2)) {
            ARMBAT_LOG_INFO("Armbat", "sensors: pump up, command fd %d addFd=%d", command_read,
                            added);
        }
    }
    for (;;) {
        const int ident = ALooper_pollOnce(kPollTimeoutMs, nullptr, nullptr, nullptr);
        if (ident == kCommandIdent) {
            DrainCommands(queue);
        } else if (ident == kSensorIdent) {
            DrainEvents(queue);
        } else if (ident >= 0) {
            if (LogAllowed("sensor-ident", 8)) {
                ARMBAT_LOG_INFO("Armbat", "sensors: unexpected looper ident %d", ident);
            }
        }
        if (state.stopping.load(std::memory_order_acquire)) {
            break;
        }
    }
    // Whatever the archive left enabled is disabled with the queue, so the
    // sensor stops costing power when the guest is gone.
    ASensorManager_destroyEventQueue(manager, queue);
}

std::shared_ptr<SensorQueue> FindQueue(std::uint32_t handle) {
    SensorState& state = State();
    const std::lock_guard lock(state.mutex);
    const auto it = state.queues.find(handle);
    return it == state.queues.end() ? nullptr : it->second;
}

// The table index behind an ASensor* the archive passed, or -1 when the handle
// was never handed out (a freed sensor, or a pointer that is not ours).
int SensorIndex(HleCall& call, std::uint32_t address) {
    SensorState& state = State();
    std::uint32_t index = 0;
    if (!call.CopyIn(address, &index, sizeof(index))) {
        return -1;
    }
    const std::lock_guard lock(state.mutex);
    if (index >= state.sensors.size()) {
        return -1;
    }
    return static_cast<int>(index);
}

bool AcquireSensor(std::uint32_t index, std::int32_t rate) {
    SensorState& state = State();
    std::int32_t effective = kDefaultRateUs;
    bool first = false;
    {
        const std::lock_guard lock(state.mutex);
        if (index >= state.refs.size()) {
            return false;
        }
        first = state.refs[index]++ == 0;
        // Only the subscription that turns the sensor on sets the host period;
        // a later one is folded in by RefreshRate, which has to see the old
        // value to notice that it changed.
        if (first && rate > 0) {
            state.rates[index] = rate;
        }
        effective = state.rates[index];
    }
    if (first) {
        Post(Command{CommandKind::Enable, index, effective});
    }
    return true;
}

void ReleaseSensor(std::uint32_t index) {
    SensorState& state = State();
    bool last = false;
    {
        const std::lock_guard lock(state.mutex);
        if (index >= state.refs.size() || state.refs[index] == 0) {
            return;
        }
        last = --state.refs[index] == 0;
    }
    if (last) {
        Post(Command{CommandKind::Disable, index, 0});
    }
}

// Folds the periods the queues asked for into the one the host queue carries.
// The host has a single period per sensor, so the fastest request wins -- a
// queue that asked for 20 ms must not be slowed down by one that asked for
// 100 ms, and a queue that leaves must not keep the others at the old rate.
void RefreshRate(std::uint32_t index) {
    SensorState& state = State();
    std::int32_t effective = kDefaultRateUs;
    std::int32_t previous = kDefaultRateUs;
    bool active = false;
    {
        const std::lock_guard lock(state.mutex);
        for (const auto& entry : state.queues) {
            const std::shared_ptr<SensorQueue>& queue = entry.second;
            const std::lock_guard queue_lock(queue->mutex);
            if (std::find(queue->enabled.begin(), queue->enabled.end(), index) ==
                queue->enabled.end()) {
                continue;
            }
            const auto rate = queue->rates.find(index);
            if (rate == queue->rates.end()) {
                continue;
            }
            if (rate->second < effective) {
                effective = rate->second;
            }
        }
        previous = state.rates[index];
        state.rates[index] = effective;
        active = state.refs[index] > 0;
    }
    // A sensor nobody enabled takes no rate: the enable command carries the
    // period, and a later enable starts from this value again.
    if (active && effective != previous) {
        Post(Command{CommandKind::Rate, index, effective});
    }
}

std::uint32_t DefaultSensor(std::uint32_t type) {
    SensorState& state = State();
    const std::lock_guard lock(state.mutex);
    for (const Sensor& sensor : state.sensors) {
        if (static_cast<std::uint32_t>(sensor.type) == type) {
            return sensor.token;
        }
    }
    return 0;
}

bool EnsureSensors() {
    if (g_registry == nullptr) {
        return false;
    }
    HleRegistry& registry = *g_registry;
    SensorState& state = State();
    const std::lock_guard lock(state.mutex);
    if (state.manager != nullptr) {
        return true;
    }

    // The manager belongs to this process, not to the guest: the guest's package
    // does not exist on the host, and the platform attributes sensor access to
    // the process making the call. getInstance() is the one form that asks for
    // the calling process's own attribution.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    ASensorManager* manager = ASensorManager_getInstance();
#pragma GCC diagnostic pop
    if (manager == nullptr) {
        ARMBAT_LOG_WARN("Armbat", "sensors: no sensor manager on the host");
        return false;
    }
    ASensorList host_list = nullptr;
    const int host_count = ASensorManager_getSensorList(manager, &host_list);
    if (host_count <= 0 || host_list == nullptr) {
        ARMBAT_LOG_WARN("Armbat", "sensors: the host exposes none");
        return false;
    }
    for (int i = 0; i < host_count; ++i) {
        ASensor const* host = host_list[i];
        if (host == nullptr) {
            continue;
        }
        Sensor sensor;
        sensor.host = host;
        sensor.type = ASensor_getType(host);
        sensor.handle = ASensor_getHandle(host);
        sensor.token = registry.AllocateData(4);
        if (sensor.token == 0) {
            return false;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(state.sensors.size());
        std::memcpy(registry.Arena().Ptr(sensor.token), &index, sizeof(index));
        sensor.name = CopyString(registry, ASensor_getName(host));
        sensor.vendor = CopyString(registry, ASensor_getVendor(host));
        state.sensors.push_back(sensor);
    }
    if (state.sensors.empty()) {
        return false;
    }
    const std::uint32_t bytes =
        static_cast<std::uint32_t>(state.sensors.size() * sizeof(std::uint32_t));
    state.list_address = registry.AllocateData(bytes);
    if (state.list_address == 0) {
        return false;
    }
    auto* cells = reinterpret_cast<std::uint32_t*>(registry.Arena().Ptr(state.list_address));
    for (std::size_t i = 0; i < state.sensors.size(); ++i) {
        cells[i] = state.sensors[i].token;
    }
    state.refs.assign(state.sensors.size(), 0);
    state.rates.assign(state.sensors.size(), kDefaultRateUs);
    state.manager = manager;

    if (!state.pump_started) {
        CreateWakePipe(&state.command_read, &state.command_write);
        state.pump_started = true;
        state.pump = std::thread(SensorPump);
    }
    ARMBAT_LOG_INFO("Armbat", "sensors: %zu from the host", state.sensors.size());
    return true;
}

}  // namespace

void RegisterSensorFunctions(HleRegistry& registry) {
    g_registry = &registry;

    // A null manager is how every sensor call answers "not on this device", so
    // the archive disables its motion input and keeps working.
    registry.Add("ASensorManager_getInstance", [](HleCall& call) {
        call.Return(EnsureSensors() ? 1u : 0u);
    });
    registry.Add("ASensorManager_getInstanceForPackage", [](HleCall& call) {
        call.Return(EnsureSensors() ? 1u : 0u);
    });
    registry.Add("ASensorManager_getSensorList", [](HleCall& call) {
        if (!EnsureSensors()) {
            call.Return(0);
            return;
        }
        SensorState& state = State();
        const std::lock_guard lock(state.mutex);
        // The list is a guest array of the handles, and its address is what the
        // caller's out-parameter receives.
        if (!call.CopyOut(call.Arg(1), &state.list_address, sizeof(state.list_address))) {
            call.Return(0);
            return;
        }
        call.Return(static_cast<std::uint32_t>(state.sensors.size()));
    });
    registry.Add("ASensorManager_getDefaultSensor", [](HleCall& call) {
        if (!EnsureSensors()) {
            call.Return(0);
            return;
        }
        const std::uint32_t token = DefaultSensor(call.Arg(1));
        if (LogAllowed("sensor-default", 16)) {
            ARMBAT_LOG_INFO("Armbat", "sensors: default for type %u -> 0x%08x", call.Arg(1),
                            token);
        }
        call.Return(token);
    });
    registry.Add("ASensorManager_getDefaultSensorByType", [](HleCall& call) {
        if (!EnsureSensors()) {
            call.Return(0);
            return;
        }
        call.Return(DefaultSensor(call.Arg(1)));
    });
    registry.Add("ASensorManager_getDefaultSensorEx", [](HleCall& call) {
        if (!EnsureSensors()) {
            call.Return(0);
            return;
        }
        call.Return(DefaultSensor(call.Arg(1)));
    });

    registry.Add("ASensorManager_createEventQueue", [](HleCall& call) {
        if (!EnsureSensors()) {
            call.Return(0);
            return;
        }
        SensorState& state = State();
        auto queue = std::make_shared<SensorQueue>();
        queue->looper = call.Arg(1);
        queue->ident = call.Arg(2);
        queue->callback = call.Arg(3);
        queue->data = call.Arg(4);
        CreateWakePipe(&queue->wake_read, &queue->wake_write);
        std::uint32_t handle = 0;
        {
            const std::lock_guard lock(state.mutex);
            handle = state.next_handle++;
            state.queues[handle] = queue;
        }
        // The archive polls its own looper, so the queue has to be a source of
        // it, exactly as the platform registers its own descriptor: without this
        // the samples arrive in a queue nobody is woken to read.
        if (queue->wake_read >= 0) {
            AttachGuestLooperSource(queue->looper, static_cast<std::uint32_t>(queue->wake_read),
                                    queue->ident, ALOOPER_EVENT_INPUT, queue->callback,
                                    queue->data);
        }
        ARMBAT_LOG_INFO("Armbat", "sensors: queue %u on looper %u ident %d cb=0x%08x data=0x%08x",
                        handle, queue->looper, static_cast<int>(queue->ident), queue->callback,
                        queue->data);
        call.Return(handle);
    });
    registry.Add("ASensorManager_destroyEventQueue", [](HleCall& call) {
        SensorState& state = State();
        std::shared_ptr<SensorQueue> queue;
        {
            const std::lock_guard lock(state.mutex);
            const auto it = state.queues.find(call.Arg(1));
            if (it == state.queues.end()) {
                // The platform answers an error for a queue it never handed out,
                // and the caller reads that answer from r0.
                call.Return(static_cast<std::uint32_t>(-EINVAL));
                return;
            }
            queue = it->second;
            state.queues.erase(it);
        }
        ARMBAT_LOG_INFO("Armbat", "sensors: destroy queue %u", call.Arg(1));
        std::vector<std::uint32_t> enabled;
        {
            const std::lock_guard lock(queue->mutex);
            enabled.swap(queue->enabled);
        }
        for (const std::uint32_t index : enabled) {
            ReleaseSensor(index);
            // What is left is other queues still using the sensor: their periods
            // decide the one the host keeps.
            RefreshRate(index);
        }
        if (queue->wake_read >= 0) {
            DetachGuestLooperSource(static_cast<std::uint32_t>(queue->wake_read));
            ::close(queue->wake_read);
            queue->wake_read = -1;
        }
        if (queue->wake_write >= 0) {
            ::close(queue->wake_write);
            queue->wake_write = -1;
        }
        call.Return(0);
    });

    registry.Add("ASensorEventQueue_enableSensor", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(1));
        const std::shared_ptr<SensorQueue> queue = FindQueue(call.Arg(0));
        if (index < 0 || queue == nullptr) {
            call.Return(static_cast<std::uint32_t>(-EINVAL));
            return;
        }
        const std::uint32_t slot = static_cast<std::uint32_t>(index);
        std::int32_t period = kDefaultRateUs;
        bool added = false;
        {
            const std::lock_guard lock(queue->mutex);
            if (std::find(queue->enabled.begin(), queue->enabled.end(), slot) ==
                queue->enabled.end()) {
                queue->enabled.push_back(slot);
                added = true;
            }
            const auto rate = queue->rates.find(slot);
            if (rate == queue->rates.end()) {
                queue->rates[slot] = kDefaultRateUs;
            } else {
                period = rate->second;
            }
        }
        // Only a subscription that did not exist yet counts: the platform keeps
        // one reference per queue, and a repeated enable must not leave the
        // sensor on after the queue that asked for it is gone.
        if (!added) {
            call.Return(0);
            return;
        }
        if (!AcquireSensor(slot, period)) {
            const std::lock_guard lock(queue->mutex);
            const auto end = std::remove(queue->enabled.begin(), queue->enabled.end(), slot);
            queue->enabled.erase(end, queue->enabled.end());
            call.Return(static_cast<std::uint32_t>(-EINVAL));
            return;
        }
        // Another queue may already hold this sensor at a different period; the
        // host queue can carry only one, so it is recomputed for all of them.
        RefreshRate(slot);
        if (LogAllowed("sensor-enable", 16)) {
            SensorState& state = State();
            const std::lock_guard lock(state.mutex);
            ARMBAT_LOG_INFO("Armbat", "sensors: enable type %d (min delay %d us)",
                            state.sensors[slot].type,
                            ASensor_getMinDelay(state.sensors[slot].host));
        }
        call.Return(0);
    });
    registry.Add("ASensorEventQueue_disableSensor", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(1));
        const std::shared_ptr<SensorQueue> queue = FindQueue(call.Arg(0));
        if (index < 0 || queue == nullptr) {
            call.Return(static_cast<std::uint32_t>(-EINVAL));
            return;
        }
        const std::uint32_t slot = static_cast<std::uint32_t>(index);
        bool removed = false;
        {
            const std::lock_guard lock(queue->mutex);
            const auto end = std::remove(queue->enabled.begin(), queue->enabled.end(), slot);
            removed = end != queue->enabled.end();
            queue->enabled.erase(end, queue->enabled.end());
            queue->rates.erase(slot);
        }
        // A sensor this queue never enabled holds no reference of its own, so
        // releasing one here would stop a sensor another queue is still using.
        if (!removed) {
            call.Return(0);
            return;
        }
        ReleaseSensor(slot);
        RefreshRate(slot);
        if (LogAllowed("sensor-disable", 16)) {
            ARMBAT_LOG_INFO("Armbat", "sensors: disable queue %u slot %u", call.Arg(0), slot);
        }
        call.Return(0);
    });
    registry.Add("ASensorEventQueue_setEventRate", [](HleCall& call) {
        const std::shared_ptr<SensorQueue> queue = FindQueue(call.Arg(0));
        const int index = SensorIndex(call, call.Arg(1));
        const std::int32_t rate = static_cast<std::int32_t>(call.Arg(2));
        if (queue == nullptr || index < 0 || rate <= 0) {
            call.Return(static_cast<std::uint32_t>(-EINVAL));
            return;
        }
        const std::uint32_t slot = static_cast<std::uint32_t>(index);
        bool enabled = false;
        {
            const std::lock_guard lock(queue->mutex);
            // The period belongs to this queue, not to the sensor: two queues
            // may want the same sensor at different rates.
            queue->rates[slot] = rate;
            enabled = std::find(queue->enabled.begin(), queue->enabled.end(), slot) !=
                      queue->enabled.end();
        }
        if (enabled) {
            RefreshRate(slot);
        }
        if (LogAllowed("sensor-rate", 16)) {
            ARMBAT_LOG_INFO("Armbat", "sensors: rate queue %u slot %d -> %d", call.Arg(0), index,
                            rate);
        }
        call.Return(0);
    });
    registry.Add("ASensorEventQueue_hasEvents", [](HleCall& call) {
        const std::shared_ptr<SensorQueue> queue = FindQueue(call.Arg(0));
        if (queue == nullptr) {
            call.Return(static_cast<std::uint32_t>(-EINVAL));
            return;
        }
        const std::lock_guard lock(queue->mutex);
        // The platform polls the descriptor and answers -1, not 0, when it is
        // not readable: an archive written against the device sees that answer
        // and keeps asking, so the container repeats it.
        call.Return(queue->events.empty() ? static_cast<std::uint32_t>(-1) : 1u);
    });
    registry.Add("ASensorEventQueue_getEvents", [](HleCall& call) {
        const std::shared_ptr<SensorQueue> queue = FindQueue(call.Arg(0));
        const std::uint32_t destination = call.Arg(1);
        const std::uint32_t limit = call.Arg(2);
        if (queue == nullptr || destination == 0 || limit == 0) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(queue->mutex);
        const std::size_t available = std::min<std::size_t>(limit, queue->events.size());
        if (available == 0) {
            // An empty queue must not leave its descriptor readable, or the
            // looper behind it is woken for nothing.
            ClearWake(queue.get());
            call.Return(0);
            return;
        }
        // The layout is identical on both sides: four 32-bit fields, the 64-bit
        // timestamp at offset 16, sixteen floats and the tail, with no padding
        // difference between the guest's ARMv7 and the host's ARM64.
        static_assert(sizeof(ASensorEvent) == 104, "ASensorEvent crosses both address spaces");
        std::size_t copied = 0;
        for (; copied < available; ++copied) {
            const std::uint32_t offset = static_cast<std::uint32_t>(copied * sizeof(ASensorEvent));
            if (!call.CopyOut(destination + offset, &queue->events[copied],
                              sizeof(ASensorEvent))) {
                break;
            }
        }
        // What did not fit stays for the next call.
        queue->events.erase(queue->events.begin(),
                            queue->events.begin() + static_cast<std::ptrdiff_t>(copied));
        if (queue->events.empty()) {
            ClearWake(queue.get());
        }
        call.Return(static_cast<std::uint32_t>(copied));
    });

    registry.Add("ASensor_getName", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(0));
        if (index < 0) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(State().mutex);
        call.Return(State().sensors[index].name);
    });
    registry.Add("ASensor_getVendor", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(0));
        if (index < 0) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(State().mutex);
        call.Return(State().sensors[index].vendor);
    });
    registry.Add("ASensor_getType", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(0));
        if (index < 0) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(State().mutex);
        call.Return(static_cast<std::uint32_t>(State().sensors[index].type));
    });
    // The number an event carries in its sensor field. The archive maps samples
    // back to the sensor they came from with it, so both answers have to be the
    // platform's own handle rather than a number of the container's choosing.
    registry.Add("ASensor_getHandle", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(0));
        if (index < 0) {
            call.Return(static_cast<std::uint32_t>(-EINVAL));
            return;
        }
        const std::lock_guard lock(State().mutex);
        call.Return(static_cast<std::uint32_t>(State().sensors[index].handle));
    });
    registry.Add("ASensor_getMinDelay", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(0));
        if (index < 0) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(State().mutex);
        call.Return(static_cast<std::uint32_t>(
            ASensor_getMinDelay(State().sensors[index].host)));
    });
    registry.Add("ASensor_getResolution", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(0));
        if (index < 0) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(State().mutex);
        const float resolution = ASensor_getResolution(State().sensors[index].host);
        call.Return(std::bit_cast<std::uint32_t>(resolution));
    });
    registry.Add("ASensor_getReportingMode", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(0));
        if (index < 0) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(State().mutex);
        call.Return(static_cast<std::uint32_t>(
            ASensor_getReportingMode(State().sensors[index].host)));
    });
    registry.Add("ASensor_isWakeUpSensor", [](HleCall& call) {
        const int index = SensorIndex(call, call.Arg(0));
        if (index < 0) {
            call.Return(0);
            return;
        }
        const std::lock_guard lock(State().mutex);
        call.Return(ASensor_isWakeUpSensor(State().sensors[index].host) ? 1u : 0u);
    });
}

void StopGuestSensors() {
    SensorState& state = State();
    if (state.pump_started) {
        state.stopping.store(true, std::memory_order_release);
        if (state.pump.joinable()) {
            if (state.pump.get_id() == std::this_thread::get_id()) {
                // The pump itself is asking; it leaves with the process.
                state.pump.detach();
            } else {
                state.pump.join();
            }
        }
        state.pump_started = false;
    }
    // The table's handles and strings live in the arena that is about to go
    // away, so a container started later in this process builds its own.
    const std::lock_guard lock(state.mutex);
    state.manager = nullptr;
    state.sensors.clear();
    state.refs.clear();
    state.rates.clear();
    state.list_address = 0;
    state.queues.clear();
    state.stopping.store(false, std::memory_order_release);
    if (state.command_read >= 0) {
        ::close(state.command_read);
        state.command_read = -1;
    }
    if (state.command_write >= 0) {
        ::close(state.command_write);
        state.command_write = -1;
    }
    {
        const std::lock_guard command_lock(state.command_mutex);
        state.commands.clear();
    }
    g_registry = nullptr;
}

}  // namespace armbat
