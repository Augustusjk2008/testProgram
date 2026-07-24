#pragma once

#include "MB_DDF/DDS/ExternalEndpoint.h"
#include "MB_DDF_HW/Endpoint/IByteEndpoint.h"

namespace MB_DDF::HW {

/// 将硬件字节端点适配为 DDS ExternalEndpoint。
class ExternalEndpointAdapter : public MB_DDF::DDS::ExternalEndpoint {
public:
    explicit ExternalEndpointAdapter(IByteEndpoint& endpoint) : endpoint_(endpoint) {}
    bool send(const uint8_t*, size_t) override;
    int32_t receive(uint8_t*, size_t) override;
    int32_t receive(uint8_t*, size_t, uint32_t) override;
    size_t mtu() const override;

private:
    int32_t receive_impl(uint8_t*, size_t, Timeout);
    IByteEndpoint& endpoint_;
};

} // namespace MB_DDF::HW
