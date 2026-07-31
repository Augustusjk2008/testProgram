#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"

#include <cstdint>

namespace MB_DDF::HW {

/// FPGA 更新流程的只读状态快照。
struct FpgaUpdateStateSnapshot {
    uint16_t flash_erase_status{};
    uint16_t flash_write_status{};
    uint16_t flash_read_status{};
    uint32_t read_data_crc{};
    uint32_t write_data_crc{};
    uint8_t crc_validation_status{};
};

/// FPGA update state 的只读设备接口；不提供擦除、写入或触发能力。
class FpgaUpdateStateDevice {
public:
    explicit FpgaUpdateStateDevice(ITransport& transport) : transport_(transport) {}

    Result<void> check_communication() const;
    Result<FpgaUpdateStateSnapshot> read_snapshot() const;

private:
    ITransport& transport_;
};

} // namespace MB_DDF::HW
