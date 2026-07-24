#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/ExternalEndpoint.h"
#include "MB_DDF/DDS/Publisher.h"
#include "MB_DDF/DDS/Subscriber.h"

namespace MB_DDF {
namespace DDS {

class ExternalPort {
public:
    explicit ExternalPort(ExternalEndpointRef external_io, MessageCallback callback = nullptr);

    bool write(const void* data, size_t size);
    size_t read(void* data, size_t size);
    int32_t read(void* data, size_t size, uint32_t timeout_us);

    ExternalEndpointRef external() const { return external_io_; }

    std::shared_ptr<Publisher> publisher;
    std::shared_ptr<Subscriber> subscriber;

private:
    ExternalEndpointRef external_io_;
};

} // namespace DDS
} // namespace MB_DDF
