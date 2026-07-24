#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"
#include <array>
#include <span>

namespace MB_DDF::HW {

/// 点火通道选择模式和重复模式。
enum class DhFireMode {
    Multiple,
    Single
};
enum class DhRepeatMode {
    Once,
    Repeatable
};
struct DhFeedback {
    uint64_t channel_mask{0};
    bool battery_fired{false};
    bool battery_activated{false};
};
struct DhTimebase {
    uint32_t counter_value{0};
    uint32_t unit_milliseconds{0};
};

/// DH_ctrl 点火控制接口；调用方负责业务级安全确认和并发协调。
class DhController {
public:
    explicit DhController(ITransport& t) : transport_(t) {}
    Result<void> check_communication() const;
    Result<void> configure_timebase(const DhTimebase&);
    Result<void> set_pulse_width_ticks(uint8_t, uint16_t);
    Result<void> set_pulse_width_ms(uint8_t, uint32_t, uint32_t);
    Result<void> set_fire_mode(DhFireMode);
    Result<void> set_repeat_mode(DhRepeatMode);
    Result<void> set_fire_enabled(bool enabled);
    Result<void> set_return_enabled(bool enabled);
    Result<void> set_pulse_config_enabled(bool enabled);
    Result<bool> read_fire_enabled() const;
    Result<bool> read_return_enabled() const;
    Result<std::array<uint8_t, 23>> read_channel_statuses() const;
    Result<void> fire(uint8_t);
    Result<void> fire_multiple(std::span<const uint8_t>);
    Result<uint64_t> read_feedback_mask() const;
    Result<bool> read_battery_activated() const;
    Result<DhFeedback> read_feedback() const;
    Result<std::array<uint16_t, 48>> read_pulse_widths(unsigned count = 48) const;

private:
    ITransport& transport_;
};
} // namespace MB_DDF::HW
