#pragma once

#include "MB_DDF_HW/Core/Buffer.h"
#include "MB_DDF_HW/Core/Result.h"
#include "MB_DDF_HW/Core/Timeout.h"
#include "MB_DDF_HW/Transport/RegisterAccessor.h"

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HW {

class ITransport : public RegisterAccessor {
public:
    virtual ~ITransport() = default;

    virtual Result<void> open() = 0;
    virtual void close() noexcept = 0;
    virtual bool is_open() const = 0;

    virtual Result<int> event_fd() const {
        return Status::error(StatusCode::Unsupported, 0, "event fd is not supported");
    }

    /// 0 表示超时，正值只表示事件已就绪，不保证是驱动事件计数。
    virtual Result<int> wait_event(Timeout timeout) = 0;

    virtual Result<size_t> dma_write(int channel, BufferView data, uint64_t device_offset) {
        (void)channel;
        (void)data;
        (void)device_offset;
        return Status::error(StatusCode::Unsupported, 0, "dma_write is not supported");
    }

    virtual Result<size_t> dma_read(int channel, MutableBufferView buffer, uint64_t device_offset) {
        (void)channel;
        (void)buffer;
        (void)device_offset;
        return Status::error(StatusCode::Unsupported, 0, "dma_read is not supported");
    }
};

} // namespace MB_DDF::HW
