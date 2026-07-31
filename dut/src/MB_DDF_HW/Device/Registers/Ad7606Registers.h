#pragma once

#include <cstdint>

namespace MB_DDF::HW::Registers::Ad7606 {

// AD7606_HELM 设备局部字节偏移，已按地址表中的寄存器编号乘以 4。
inline constexpr uint64_t Communication = 0, ChannelBase = 0x04, ChannelStride = 4,
                          AcquisitionEnable = 0x24, FilterEnable = 0x28, Oversampling = 0x2C,
                          ClockPeriod = 0x30, ConversionLow = 0x34, ConversionWait = 0x38,
                          ResetCycles = 0x3C, AcquisitionCount = 0x40, ChannelMapping = 0x44;
constexpr uint64_t channel(unsigned index) {
    return ChannelBase + index * ChannelStride;
}
} // namespace MB_DDF::HW::Registers::Ad7606
