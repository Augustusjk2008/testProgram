#include "MB_DDF_HW/DdsAdapter/ExternalEndpointAdapter.h"

namespace MB_DDF::HW {
bool ExternalEndpointAdapter::send(const uint8_t* d, size_t n) {
    return endpoint_.send({d, n}).is_ok();
}
int32_t ExternalEndpointAdapter::receive(uint8_t* d, size_t n) {
    return receive_impl(d, n, Timeout::poll());
}
int32_t ExternalEndpointAdapter::receive(uint8_t* d, size_t n, uint32_t us) {
    return receive_impl(d, n, Timeout::after_us(us));
}
int32_t ExternalEndpointAdapter::receive_impl(uint8_t* d, size_t n, Timeout t) {
    auto r = endpoint_.receive({d, n}, t);
    if (r) {
        return static_cast<int32_t>(r.value());
    }
    return r.status().code == StatusCode::Timeout ? 0 : -1;
}
size_t ExternalEndpointAdapter::mtu() const {
    return endpoint_.mtu();
}
} // namespace MB_DDF::HW
