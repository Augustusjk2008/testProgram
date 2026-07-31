#include "MB_DDF_HW/Device/UpdateImageIpVersionDevice.h"

#include "MB_DDF_HW/Device/Registers/UpdateImageIpVersionRegisters.h"

namespace MB_DDF::HW {
namespace R = Registers::UpdateImageIpVersion;

namespace {

Result<UpdateImageIpVersionComponent> read_component(ITransport& transport, uint64_t base_offset) {
    auto base_address = transport.read32(base_offset);
    if (!base_address) {
        return base_address.status();
    }
    auto version = transport.read32(base_offset + sizeof(uint32_t));
    if (!version) {
        return version.status();
    }
    auto date = transport.read32(base_offset + 2 * sizeof(uint32_t));
    if (!date) {
        return date.status();
    }
    return UpdateImageIpVersionComponent{base_address.value(), version.value(), date.value()};
}

} // namespace

Result<void> UpdateImageIpVersionDevice::check_communication() const {
    return check_device_communication(transport_);
}

Result<UpdateImageIpVersionSnapshot> UpdateImageIpVersionDevice::read_snapshot() const {
    auto software_type = transport_.read32(R::SoftwareType);
    if (!software_type) {
        return software_type.status();
    }
    auto software_status = transport_.read32(R::SoftwareStatus);
    if (!software_status) {
        return software_status.status();
    }
    auto model_software_version = transport_.read32(R::ModelSoftwareVersion);
    if (!model_software_version) {
        return model_software_version.status();
    }
    auto model_software_date = transport_.read32(R::ModelSoftwareDate);
    if (!model_software_date) {
        return model_software_date.status();
    }

    auto dido = read_component(transport_, R::DidoBase);
    if (!dido) {
        return dido.status();
    }
    auto dh = read_component(transport_, R::DhBase);
    if (!dh) {
        return dh.status();
    }
    auto ad7606 = read_component(transport_, R::Ad7606Base);
    if (!ad7606) {
        return ad7606.status();
    }
    auto pwm = read_component(transport_, R::PwmBase);
    if (!pwm) {
        return pwm.status();
    }
    auto ads1258 = read_component(transport_, R::Ads1258Base);
    if (!ads1258) {
        return ads1258.status();
    }
    auto xadc = read_component(transport_, R::XadcBase);
    if (!xadc) {
        return xadc.status();
    }
    auto com = read_component(transport_, R::ComBase);
    if (!com) {
        return com.status();
    }
    auto fpga_update = read_component(transport_, R::FpgaUpdateBase);
    if (!fpga_update) {
        return fpga_update.status();
    }

    return UpdateImageIpVersionSnapshot{
        static_cast<uint16_t>(software_type.value()),
        static_cast<uint16_t>(software_status.value()),
        model_software_version.value(),
        model_software_date.value(),
        dido.value(),
        dh.value(),
        ad7606.value(),
        pwm.value(),
        ads1258.value(),
        xadc.value(),
        com.value(),
        fpga_update.value(),
    };
}

} // namespace MB_DDF::HW
