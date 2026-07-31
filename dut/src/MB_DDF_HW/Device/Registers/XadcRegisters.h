#pragma once

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HW::Registers::Xadc {

// 全局窗口基址由板级集成信息确认；origin_v4 提供下列局部寄存器偏移。
inline constexpr uint64_t UserBase = 0x150000;
inline constexpr size_t WindowSize = 0x10000;

// XADC IP 设备局部字节偏移。
inline constexpr uint64_t Status = 0x004;
inline constexpr uint64_t Temperature = 0x200;
inline constexpr uint64_t VccInt = 0x204;
inline constexpr uint64_t VccAux = 0x208;
inline constexpr uint64_t Js5V = 0x240;
inline constexpr uint64_t External3V3 = 0x244;
inline constexpr uint64_t Power24V = 0x248;
inline constexpr uint64_t ValueYx = 0x260;
inline constexpr uint64_t Dyt5V = 0x264;

} // namespace MB_DDF::HW::Registers::Xadc
