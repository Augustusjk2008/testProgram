#pragma once

#include "MB_DDF/DDS/ExternalEndpoint.h"
#include "MB_DDF_HW/Endpoint/IByteEndpoint.h"
#include <functional>

namespace MB_DDF::HW {

/// 使用调用方回调桥接非 COM 设备和 DDS，不解释 payload 内容。
class CallbackExternalEndpoint final : public MB_DDF::DDS::ExternalEndpoint {
public:
    using SendCallback = std::function<Result<size_t>(BufferView)>;
    using ReceiveCallback = std::function<Result<size_t>(MutableBufferView, Timeout)>;
    using MtuCallback = std::function<size_t()>;
    CallbackExternalEndpoint(SendCallback send, ReceiveCallback receive, MtuCallback mtu)
        : send_(std::move(send)), receive_(std::move(receive)), mtu_(std::move(mtu)) {}
    bool send(const uint8_t* data, size_t size) override {
        return send_({data, size}).is_ok();
    }
    int32_t receive(uint8_t* data, size_t size) override {
        return receive_impl(data, size, Timeout::poll());
    }
    int32_t receive(uint8_t* data, size_t size, uint32_t timeout_us) override {
        return receive_impl(data, size, Timeout::after_us(timeout_us));
    }
    size_t mtu() const override {
        return mtu_();
    }

private:
    int32_t receive_impl(uint8_t* data, size_t size, Timeout timeout) {
        auto result = receive_({data, size}, timeout);
        if (result) {
            return static_cast<int32_t>(result.value());
        }
        return result.status().code == StatusCode::Timeout ? 0 : -1;
    }
    SendCallback send_;
    ReceiveCallback receive_;
    MtuCallback mtu_;
};
} // namespace MB_DDF::HW
