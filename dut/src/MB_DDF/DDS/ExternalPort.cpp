#include "MB_DDF/DDS/ExternalPort.h"

#include <utility>

namespace MB_DDF {
namespace DDS {

ExternalPort::ExternalPort(ExternalEndpointRef external_io, MessageCallback callback)
    : external_io_(std::move(external_io)) {
    if (!external_io_) {
        return;
    }

    auto& dds_core = DDSCore::instance();
    publisher = dds_core.create_publisher("external://publisher", external_io_);
    subscriber = dds_core.create_subscriber("external://subscriber", external_io_, callback);
}

bool ExternalPort::write(const void* data, size_t size) {
    return publisher && publisher->write(data, size);
}

size_t ExternalPort::read(void* data, size_t size) {
    return subscriber ? subscriber->read(data, size) : 0;
}

int32_t ExternalPort::read(void* data, size_t size, uint32_t timeout_us) {
    return subscriber ? subscriber->read(data, size, timeout_us) : 0;
}

} // namespace DDS
} // namespace MB_DDF
