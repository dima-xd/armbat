#include "armbat/read_window.h"

#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace armbat {

std::int64_t ReadWindow::Fill(int fd, std::uint8_t* out, std::uint32_t length,
                              std::uint64_t generation) {
    // One host read feeds the request and, when the file has that much left, the
    // window behind it. A request larger than the window is answered on its own:
    // prefetching past it would be memory nobody asked for, and answering less
    // than the request would hand the guest a short read it never caused.
    std::uint64_t wanted = length;
    if (size_known_ && size_ > position_) {
        wanted = std::max<std::uint64_t>(length, std::min<std::uint64_t>(kBytes, size_ - position_));
    }
    if (window_.size() < wanted) {
        window_.resize(static_cast<std::size_t>(wanted));
    }
    std::size_t done = 0;
    while (done < wanted) {
        const ssize_t got = ::pread(fd, window_.data() + done, wanted - done,
                                    static_cast<off_t>(position_ + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            window_length_ = 0;
            return done == 0 ? -1 : static_cast<std::int64_t>(done);
        }
        if (got == 0) {
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    if (done == 0) {
        window_length_ = 0;
        return 0;
    }
    window_offset_ = position_;
    window_length_ = static_cast<std::uint32_t>(done);
    window_generation_ = generation;
    const std::uint32_t copied =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(length, done));
    std::memcpy(out, window_.data(), copied);
    position_ += copied;
    return static_cast<std::int64_t>(copied);
}

bool ReadWindow::Seek(std::int64_t offset, int whence, std::int64_t* result) {
    std::int64_t target = 0;
    switch (whence) {
        case SEEK_SET:
            target = offset;
            break;
        case SEEK_CUR:
            target = static_cast<std::int64_t>(position_) + offset;
            break;
        case SEEK_END:
            if (!size_known_) {
                return false;
            }
            target = static_cast<std::int64_t>(size_) + offset;
            break;
        default:
            return false;
    }
    if (target < 0) {
        return false;
    }
    position_ = static_cast<std::uint64_t>(target);
    *result = target;
    return true;
}

}  // namespace armbat
