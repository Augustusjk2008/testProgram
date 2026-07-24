#include "MB_DDF_HW/Os/Poll.h"

#include <cerrno>
#include <climits>
#include <poll.h>

namespace MB_DDF::HW::Os::Poll {
namespace {

int to_poll_timeout_ms(Timeout timeout) {
    if (timeout.infinite) {
        return -1;
    }

    if (timeout.microseconds == 0) {
        return 0;
    }

    const uint64_t rounded_ms = (static_cast<uint64_t>(timeout.microseconds) + 999u) / 1000u;
    if (rounded_ms > static_cast<uint64_t>(INT_MAX)) {
        return INT_MAX;
    }
    return static_cast<int>(rounded_ms);
}

} // namespace

Result<int> wait_readable(int fd, Timeout timeout) {
    if (fd < 0) {
        return Status::error(StatusCode::IoError, EINVAL, "poll fd is invalid");
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    const int ret = ::poll(&pfd, 1, to_poll_timeout_ms(timeout));
    if (ret < 0) {
        return Status::error(StatusCode::IoError, errno, "poll failed");
    }

    if (ret == 0) {
        return 0;
    }

    if ((pfd.revents & POLLIN) != 0) {
        return 1;
    }

    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return Status::error(StatusCode::IoError, 0, "poll reported fd error");
    }

    return 0;
}

} // namespace MB_DDF::HW::Os::Poll
