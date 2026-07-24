#pragma once

#include <cstdint>

namespace MB_DDF::HW::Registers::Ads1258 {

// ADS1258 设备局部字节偏移，连续通道和错误计数通过基址与步长计算。
inline constexpr uint64_t Communication = 0, ClearErrors = 0x04, ConfigBase = 0x08,
                          ConfigStride = 4, DataBase = 0x80, DataStride = 4, ErrorBase = 0x100,
                          ErrorStride = 4, DrdyReadDelay = 0x68;
// 已确认的电气健康采样通道（芯片 1）：C 组、B 组、一次电源 28.5V(1)。
inline constexpr uint64_t CGroupVoltage = 0x80, BGroupVoltage = 0x88,
                          Primary28V5 = 0x8C;
constexpr uint64_t config(unsigned index) {
    return ConfigBase + index * ConfigStride;
}
constexpr uint64_t data(unsigned index) {
    return DataBase + index * DataStride;
}
constexpr uint64_t error(unsigned index) {
    return ErrorBase + index * ErrorStride;
}
} // namespace MB_DDF::HW::Registers::Ads1258
