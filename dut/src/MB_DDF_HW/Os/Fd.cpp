#include "MB_DDF_HW/Os/Fd.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace MB_DDF::HW::Os {

Fd::Fd(int fd) noexcept : fd_(fd) {}

Fd::~Fd() {
    reset();
}

Fd::Fd(Fd&& other) noexcept : fd_(other.release()) {}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

bool Fd::valid() const noexcept {
    return fd_ >= 0;
}

int Fd::get() const noexcept {
    return fd_;
}

int Fd::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void Fd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

Status Fd::set_nonblock() const {
    if (!valid()) {
        return Status::error(StatusCode::NotOpen, 0, "fd is not open");
    }

    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        return Status::error(StatusCode::IoError, errno, "fcntl(F_GETFL) failed");
    }

    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        return Status::error(StatusCode::IoError, errno, "fcntl(F_SETFL) failed");
    }

    return Status::ok();
}

} // namespace MB_DDF::HW::Os
