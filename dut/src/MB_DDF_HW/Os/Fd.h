#pragma once

#include "MB_DDF_HW/Core/Status.h"

namespace MB_DDF::HW::Os {

class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept;
    ~Fd();

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;

    bool valid() const noexcept;
    int get() const noexcept;
    int release() noexcept;
    void reset(int fd = -1) noexcept;

    Status set_nonblock() const;

private:
    int fd_{-1};
};

} // namespace MB_DDF::HW::Os
