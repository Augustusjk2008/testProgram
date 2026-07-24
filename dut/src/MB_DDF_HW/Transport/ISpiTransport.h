#pragma once

#include "MB_DDF_HW/Core/Buffer.h"
#include "MB_DDF_HW/Core/Result.h"

#include <cstddef>

namespace MB_DDF::HW {

/// SPI 全双工事务接口。一次 transfer 必须对应一次连续片选周期。
class ISpiTransport {
public:
    virtual ~ISpiTransport() = default;

    virtual Result<void> open() = 0;
    virtual void close() noexcept = 0;
    virtual bool is_open() const = 0;

    /// 同时发送和接收等长字节流；实现不得把事务拆成独立 write/read。
    virtual Result<size_t> transfer(BufferView tx, MutableBufferView rx) = 0;
};

} // namespace MB_DDF::HW
