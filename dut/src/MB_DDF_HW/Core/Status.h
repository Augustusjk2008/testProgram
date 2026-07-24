#pragma once

#include <cerrno>
#include <string>
#include <utility>

namespace MB_DDF::HW {

enum class StatusCode {
    Ok = 0,
    InvalidArgument,
    NotOpen,
    OpenFailed,
    MapFailed,
    IoError,
    Timeout,
    Unsupported,
    Busy,
    BufferTooSmall,
    ProtocolError,
    HardwareFault,
};

struct Status {
    StatusCode code{StatusCode::Ok};
    int errno_value{0};
    std::string message{};

    static Status ok() {
        return {};
    }

    static Status error(StatusCode c, int err, std::string msg) {
        return Status{c, err, std::move(msg)};
    }

    bool is_ok() const {
        return code == StatusCode::Ok;
    }
    explicit operator bool() const {
        return is_ok();
    }
};

} // namespace MB_DDF::HW
