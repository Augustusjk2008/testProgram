#pragma once

#include <cstdint>

namespace MB_DDF::HW::Registers::Ads1258 {

// ADS1258 设备局部字节偏移，连续通道、诊断值和错误计数通过基址与步长计算。
inline constexpr uint64_t Communication = 0, ClearErrors = 0x04, ConfigBase = 0x08,
                          ConfigStride = 4, Config0 = 0x0C, Config1 = 0x10,
                          Sysred = 0x24, WorkCount = 0x30, SpiDivider = 0x38,
                          ActivationThresholdC = 0x4C,
                          ActivationThresholdB = 0x50, Enable1 = 0x5C,
                          Enable2 = 0x60, StateRollback = 0x64,
                          DrdyReadDelay = 0x68, DataBase = 0x80, DataStride = 4,
                          Chip1DiagnosticsBase = 0x100, Chip2DiagnosticsBase = 0x114,
                          DiagnosticStride = 4, ErrorBase = 0x128, ErrorStride = 4;
// 已确认的电气健康采样通道（芯片 1）：C 组、B 组、一次电源 28.5V(1)。
inline constexpr uint64_t CGroupVoltage = 0x80, BGroupVoltage = 0x88,
                          Primary28V5 = 0x8C;
constexpr uint64_t config(unsigned index) {
    return ConfigBase + index * ConfigStride;
}
constexpr uint64_t data(unsigned index) {
    return DataBase + index * DataStride;
}
constexpr uint64_t diagnostic(unsigned chip, unsigned index) {
    return (chip == 0 ? Chip1DiagnosticsBase : Chip2DiagnosticsBase) +
           index * DiagnosticStride;
}
constexpr uint64_t error(unsigned index) {
    return ErrorBase + index * ErrorStride;
}
} // namespace MB_DDF::HW::Registers::Ads1258
