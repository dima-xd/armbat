#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace armbat {

// One finger of a touch event, in the guest's own window pixels.
//
// The numbers are already in the archive's space: the container undoes the
// scale of the guest frame before it queues the event, so a game that was
// written for a 1920x1080 screen sees 1920x1080 here even though the panel is
// larger and the guest's window is scaled up to fill it.
struct InputPointer {
    std::int32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float raw_x = 0.0f;
    float raw_y = 0.0f;
    float pressure = 1.0f;
    float size = 0.0f;
    float touch_major = 0.0f;
    float touch_minor = 0.0f;
};

// A platform input event, in the shape the AInputEvent family answers.
//
// A native activity never looks inside an AInputEvent: it passes the pointer
// back to AMotionEvent_getX and friends. The container therefore keeps the
// event in this form and hands the guest a small structure of its own that the
// import implementations read.
struct InputEvent {
    std::int32_t type = 2;          // AINPUT_EVENT_TYPE_MOTION
    std::int32_t source = 0x1002;   // AINPUT_SOURCE_TOUCHSCREEN
    std::int32_t device_id = 0;
    std::int32_t action = 0;
    std::int32_t meta_state = 0;
    std::int32_t flags = 0;
    std::int32_t edge_flags = 0;
    std::int32_t key_code = 0;
    std::int32_t repeat_count = 0;
    std::int32_t scan_code = 0;
    std::int64_t down_time = 0;
    std::int64_t event_time = 0;
    std::vector<InputPointer> pointers;
};

// The queue between the host's input and the guest's native activity.
//
// The host's touch arrives on the UI thread while the archive reads on its own
// loop, so this is the one piece both sides touch. A pipe carries the wakeup:
// AInputQueue_attachLooper registers its read end with the guest's looper, and
// the guest's ALooper_pollAll returns LOOPER_ID_INPUT for the same reason it
// does on a device -- a readable descriptor, not a busy wait.
class GuestInputQueue {
public:
    static GuestInputQueue& Instance();

    // Platform side: queue an event and wake the guest.
    void Push(InputEvent event);

    // Guest side: the next event, or false when none is waiting.
    bool Pop(InputEvent* event);

    // The guest's looper source. Attach returns the read end of the pipe, or
    // -1 when descriptors are unavailable; Detach closes both ends.
    int Attach();
    void Detach(int read_fd);

    // Read end currently registered with the guest's looper, or -1.
    int ReadFd();

private:
    GuestInputQueue() = default;

    void Wake();
    void Drain();

    std::mutex mutex_;
    std::deque<InputEvent> pending_;
    int read_fd_ = -1;
    int write_fd_ = -1;
};

}  // namespace armbat
