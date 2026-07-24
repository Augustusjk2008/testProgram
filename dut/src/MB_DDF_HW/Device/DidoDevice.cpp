#include "MB_DDF_HW/Device/DidoDevice.h"
#include "MB_DDF_HW/Device/Registers/DidoRegisters.h"

namespace MB_DDF::HW {
namespace R = Registers::Dido;
Result<void> DidoDevice::check_communication() const {
    return check_device_communication(transport_);
}
Result<void> DidoDevice::set_outputs(uint16_t values, uint16_t updates) {
    for (unsigned i = 0; i < 16; ++i) {
        if (updates & (1u << i)) {
            auto s = transport_.write32(R::output(i), (values & (1u << i)) ? 0xAAAAu : 0xFFFFu);
            if (!s) {
                return s;
            }
        }
    }
    return {};
}
Result<uint16_t> DidoDevice::read_outputs() const {
    uint16_t result = 0;
    for (unsigned i = 0; i < 16; ++i) {
        auto v = transport_.read32(R::output(i));
        if (!v) {
            return v.status();
        }
        const bool high = (v.value() & 1u) != 0;
        // DO0-7 高有效，DO8-15 低有效。
        const bool active = i < 8 ? high : !high;
        result |= static_cast<uint16_t>(active) << i;
    }
    return result;
}
Result<uint16_t> DidoDevice::read_inputs() const {
    uint16_t result = 0;
    for (unsigned i = 0; i < 16; ++i) {
        auto v = transport_.read32(R::input(i));
        if (!v) {
            return v.status();
        }
        // FPGA 对有效输入统一回读 0xAA。
        const bool active = (v.value() & 0xFFu) == 0xAAu;
        result |= static_cast<uint16_t>(active) << i;
    }
    return result;
}
Result<DidoSnapshot> DidoDevice::read_snapshot() const {
    auto o = read_outputs();
    if (!o) {
        return o.status();
    }
    auto i = read_inputs();
    if (!i) {
        return i.status();
    }
    return DidoSnapshot{o.value(), i.value()};
}
} // namespace MB_DDF::HW
