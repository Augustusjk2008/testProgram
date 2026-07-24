#pragma once

#include "MB_DDF_HW/Core/Result.h"
#include "MB_DDF_HW/Core/Timeout.h"
#include "MB_DDF_HW/Os/Fd.h"

namespace MB_DDF::HW::Os {

/// 单 event fd 的 epoll RAII 包装，支持轮询、有限等待和永久等待。
class Epoll {
public:
    Result<void> open(int event_fd);
    void close() noexcept;
    bool is_open() const noexcept;
    Result<int> wait(Timeout timeout) const;

private:
    Fd fd_;
};
} // namespace MB_DDF::HW::Os
