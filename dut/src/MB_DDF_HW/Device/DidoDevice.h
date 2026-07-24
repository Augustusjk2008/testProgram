#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"

namespace MB_DDF::HW {

/// 位图中的 1 表示业务有效，不表示固定物理电平。
struct DidoSnapshot {
    uint16_t outputs_active{0}, inputs_active{0};
};
struct DidoOutputUpdate {
    uint16_t value_mask{0}, update_mask{0};
};

/// DIDO_ctrl 的强类型设备接口；内部负责高/低有效极性转换。
class DidoDevice {
public:
    explicit DidoDevice(ITransport& t) : transport_(t) {}
    Result<void> check_communication() const;
    Result<void> set_outputs(uint16_t value_mask, uint16_t update_mask);
    Result<uint16_t> read_outputs() const;
    Result<uint16_t> read_inputs() const;
    Result<DidoSnapshot> read_snapshot() const;

private:
    ITransport& transport_;
};
} // namespace MB_DDF::HW
