#pragma once

#include "MB_DDF_HW/Core/Buffer.h"
#include "MB_DDF_HW/Core/Result.h"
#include "MB_DDF_HW/Core/Timeout.h"

namespace MB_DDF::HW {

/// 面向原始字节载荷的通用端点，不规定上层协议格式。
class IByteEndpoint {
public:
    virtual ~IByteEndpoint() = default;
    virtual Result<size_t> send(BufferView data) = 0;
    virtual Result<size_t> receive(MutableBufferView buffer, Timeout timeout) = 0;
    virtual size_t mtu() const = 0;
};
} // namespace MB_DDF::HW
