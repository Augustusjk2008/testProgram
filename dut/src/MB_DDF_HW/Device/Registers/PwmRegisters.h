#pragma once

#include <cstdint>

namespace MB_DDF::HW::Registers::Pwm {

// PWM_ctrl 设备局部字节偏移；实例基址由 XdmaConfig::user_offset 提供。
inline constexpr uint64_t Communication = 0x00;
inline constexpr uint64_t DutyBase = 0x04, DirectionBase = 0x14, ChannelStride = 0x04;
inline constexpr uint64_t UpdateEnable = 0x24, Enable = 0x28, Carrier = 0x2C, Peak = 0x30,
                          Waveform = 0x34, DutyMode = 0x38;
constexpr uint64_t duty(unsigned channel) {
    return DutyBase + channel * ChannelStride;
}
constexpr uint64_t direction(unsigned channel) {
    return DirectionBase + channel * ChannelStride;
}
} // namespace MB_DDF::HW::Registers::Pwm
