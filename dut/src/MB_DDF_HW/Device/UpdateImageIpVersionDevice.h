#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"

#include <cstdint>

namespace MB_DDF::HW {

/// 单个 FPGA IP 的地址、版本和日期标识。
struct UpdateImageIpVersionComponent {
    uint32_t base_address{};
    uint32_t version{};
    uint32_t date{};
};

/// UPDATE image 的软件和 FPGA IP 版本快照。
struct UpdateImageIpVersionSnapshot {
    uint16_t software_type{};
    uint16_t software_status{};
    uint32_t model_software_version{};
    uint32_t model_software_date{};
    UpdateImageIpVersionComponent dido{};
    UpdateImageIpVersionComponent dh{};
    UpdateImageIpVersionComponent ad7606{};
    UpdateImageIpVersionComponent pwm{};
    UpdateImageIpVersionComponent ads1258{};
    UpdateImageIpVersionComponent xadc{};
    UpdateImageIpVersionComponent com{};
    UpdateImageIpVersionComponent fpga_update{};
};

/// UPDATE image IP version 的只读设备接口；不暴露地址表中的保留写位置。
class UpdateImageIpVersionDevice {
public:
    explicit UpdateImageIpVersionDevice(ITransport& transport) : transport_(transport) {}

    Result<void> check_communication() const;
    Result<UpdateImageIpVersionSnapshot> read_snapshot() const;

private:
    ITransport& transport_;
};

} // namespace MB_DDF::HW
