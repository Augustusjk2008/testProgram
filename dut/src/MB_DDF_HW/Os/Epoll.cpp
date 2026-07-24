#include "MB_DDF_HW/Os/Epoll.h"
#include <cerrno>
#include <chrono>
#include <limits>
#include <sys/epoll.h>

namespace MB_DDF::HW::Os {
Result<void> Epoll::open(int event_fd) {
    close();
    if (event_fd < 0) {
        return Status::error(StatusCode::InvalidArgument, 0, "event fd is invalid");
    }
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        return Status::error(StatusCode::OpenFailed, errno, "epoll_create1 failed");
    }
    fd_.reset(fd);
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = event_fd;
    if (::epoll_ctl(fd_.get(), EPOLL_CTL_ADD, event_fd, &event) != 0) {
        const int error = errno;
        close();
        return Status::error(StatusCode::IoError, error, "epoll_ctl failed");
    }
    return {};
}
void Epoll::close() noexcept {
    fd_.reset();
}
bool Epoll::is_open() const noexcept {
    return fd_.valid();
}
Result<int> Epoll::wait(Timeout timeout) const {
    if (!fd_.valid()) {
        return Status::error(StatusCode::NotOpen, 0, "epoll is not open");
    }
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::microseconds(timeout.microseconds);
    for (;;) {
        int wait_ms = -1;
        if (!timeout.infinite) {
            const auto now = Clock::now();
            if (timeout.microseconds == 0 || now >= deadline) {
                wait_ms = 0;
            } else {
                const auto us =
                    std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
                const auto ms = (us + 999) / 1000;
                wait_ms = static_cast<int>(
                    ms > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : ms);
            }
        }
        epoll_event event{};
        const int result = ::epoll_wait(fd_.get(), &event, 1, wait_ms);
        if (result >= 0) {
            return result;
        }
        if (errno != EINTR) {
            return Status::error(StatusCode::IoError, errno, "epoll_wait failed");
        }
        if (!timeout.infinite && Clock::now() >= deadline) {
            return 0;
        }
    }
}
} // namespace MB_DDF::HW::Os
