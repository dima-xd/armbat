#include "armbat/guest_input.h"

#include <fcntl.h>
#include <unistd.h>

#include <android/log.h>

#include "armbat/log.h"
#include <cerrno>
#include <cstring>
#include <utility>

namespace armbat {

GuestInputQueue& GuestInputQueue::Instance() {
    static GuestInputQueue queue;
    return queue;
}

void GuestInputQueue::Push(InputEvent event) {
    const std::lock_guard lock(mutex_);
    pending_.push_back(std::move(event));
    Wake();
}

bool GuestInputQueue::Pop(InputEvent* event) {
    const std::lock_guard lock(mutex_);
    if (pending_.empty()) {
        // The descriptor stays readable while a byte is unread; once the queue
        // is empty it must not stay readable or the guest's loop would spin.
        Drain();
        return false;
    }
    *event = std::move(pending_.front());
    pending_.pop_front();
    if (pending_.empty()) {
        Drain();
    }
    return true;
}

int GuestInputQueue::Attach() {
    const std::lock_guard lock(mutex_);
    if (read_fd_ >= 0) {
        return read_fd_;
    }
    int ends[2] = {-1, -1};
    if (::pipe(ends) != 0) {
        ARMBAT_LOG_ERROR("Armbat", "input: pipe failed: %s",
                            std::strerror(errno));
        return -1;
    }
    for (const int fd : ends) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    read_fd_ = ends[0];
    write_fd_ = ends[1];
    ARMBAT_LOG_INFO("Armbat", "input: queue attached fd=%d", read_fd_);
    return read_fd_;
}

void GuestInputQueue::Detach(int read_fd) {
    const std::lock_guard lock(mutex_);
    if (read_fd_ < 0 || read_fd != read_fd_) {
        return;
    }
    ::close(read_fd_);
    ::close(write_fd_);
    read_fd_ = -1;
    write_fd_ = -1;
    pending_.clear();
}

int GuestInputQueue::ReadFd() {
    const std::lock_guard lock(mutex_);
    return read_fd_;
}

void GuestInputQueue::Wake() {
    if (write_fd_ < 0) {
        return;
    }
    const char byte = 1;
    // A full pipe already means the guest is awake; a short write is a success.
    if (::write(write_fd_, &byte, 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ARMBAT_LOG_WARN("Armbat", "input: wake failed: %s",
                            std::strerror(errno));
    }
}

void GuestInputQueue::Drain() {
    if (read_fd_ < 0) {
        return;
    }
    char buffer[64];
    while (::read(read_fd_, buffer, sizeof(buffer)) > 0) {
    }
}

}  // namespace armbat
