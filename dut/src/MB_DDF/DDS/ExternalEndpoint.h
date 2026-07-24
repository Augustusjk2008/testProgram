#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace MB_DDF {
namespace DDS {

class ExternalEndpoint {
public:
    virtual ~ExternalEndpoint() = default;

    virtual bool send(const uint8_t* data, size_t size) = 0;
    virtual int32_t receive(uint8_t* data, size_t capacity) = 0;
    virtual int32_t receive(uint8_t* data, size_t capacity, uint32_t timeout_us) = 0;
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
