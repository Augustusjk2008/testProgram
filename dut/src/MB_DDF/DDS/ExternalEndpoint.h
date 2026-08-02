#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace MB_DDF {
namespace DDS {

class ExternalEndpoint {
public:
    virtual ~ExternalEndpoint() = default;

    /**
     * 发送一个完整外部帧。size 不得超过 mtu()；实现必须保持一帧一报文的边界。
     * DomainGateway 会串行化同一端点的 send，但 receive 可能与 send 并发。
     */
    virtual bool send(const uint8_t* data, size_t size) = 0;

    /**
     * 接收一个完整外部帧。正返回值必须位于 [1, capacity]；0 表示当前无帧，负值表示错误。
     * 流式链路需要在实现内部组帧，不能向上层交付半帧。
     */
    virtual int32_t receive(uint8_t* data, size_t capacity) = 0;

    /**
     * 带超时接收，timeout_us 到期后必须有界返回，以保证 Gateway 能正常 stop()。
     * 返回值约定与无超时重载相同。
     */
    virtual int32_t receive(uint8_t* data, size_t capacity, uint32_t timeout_us) = 0;

    /**
     * 返回稳定、非零且不超过 DomainGateway 安全上限的完整外部帧 MTU。Gateway 不做分片。
     */
    virtual size_t mtu() const = 0;

    virtual int control(uint32_t command, void* argument, size_t argument_size) {
        (void)command;
        (void)argument;
        (void)argument_size;
        return -1;
    }
};

using ExternalEndpointRef = std::shared_ptr<ExternalEndpoint>;

} // namespace DDS
} // namespace MB_DDF
