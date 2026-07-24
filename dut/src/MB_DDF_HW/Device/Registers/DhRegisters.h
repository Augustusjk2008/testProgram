#pragma once

#include <cstdint>

namespace MB_DDF::HW::Registers::Dh {

// DH_ctrl 设备局部字节偏移；48 路通道通过统一步长计算。
inline constexpr uint64_t Communication = 0, FireCommandBase = 0x44, PulseWidthBase = 0x104,
                          FeedbackBase = 0x44, Timebase = 0x1C4, FireMode = 0x1C8,
                          MultiChannels = 0x1CC, MultiTrigger = 0x1D0, RepeatMode = 0x1D4,
                          BatteryStatus = 0x1D8, FireEnable = 0x1DC,
                          ReturnEnable = 0x04, PulseConfigEnable = 0x1E0;
inline constexpr uint64_t Stride = 4;
constexpr uint64_t fire(unsigned index) {
    return FireCommandBase + index * Stride;
}
constexpr uint64_t pulse(unsigned index) {
    return PulseWidthBase + index * Stride;
}
constexpr uint64_t feedback(unsigned index) {
    return FeedbackBase + index * Stride;
}
} // namespace MB_DDF::HW::Registers::Dh
