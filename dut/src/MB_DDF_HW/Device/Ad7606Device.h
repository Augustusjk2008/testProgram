#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"
#include <array>

namespace MB_DDF::HW {

/// AD7606 采集时序和滤波配置，字段值直接对应寄存器。
struct Ad7606Config {
    bool acquisition_enabled{false};
    bool filter_enabled{false};
    uint8_t oversampling{0}, clock_period{24}, conversion_low_cycles{3}, conversion_wait_cycles{35},
        reset_cycles{5};
    uint16_t acquisition_count{3904};
    /// 从低到高的四位 nibble 分别表示 AD7606 逻辑通道 1 至 8 的硬件通道映射。
    uint32_t channel_mapping{0x76543210u};
};

/// 八通道原始采样快照；低 16 位按有符号数解释。
struct Ad7606Snapshot {
    std::array<int16_t, 8> raw{};
};

/// AD7606 配置与采样组合快照。
struct Ad7606State {
    Ad7606Config config{};
    Ad7606Snapshot snapshot{};
};

/// AD7606_HELM 的强类型设备接口；读取立即返回，不轮询。
class Ad7606Device {
public:
    explicit Ad7606Device(ITransport& t) : transport_(t) {}
    Result<void> check_communication() const;
    Result<void> configure(const Ad7606Config&);
    Result<void> set_acquisition_enabled(bool);
    Result<void> set_filter_enabled(bool);
    Result<void> reset();
    Result<Ad7606Snapshot> read_snapshot() const;
    Result<Ad7606State> read_state() const;

private:
    ITransport& transport_;
};
} // namespace MB_DDF::HW
