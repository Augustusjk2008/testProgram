#pragma once

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HW::Registers::Flash {

// 全局窗口基址由板级集成信息确认；origin_v3 提供下列局部寄存器偏移。
inline constexpr uint64_t UserBase = 0x160000;

// FLASH IP 设备局部字节偏移。
inline constexpr uint64_t ReadRamBase = 0x000;
inline constexpr uint64_t WriteRamBase = 0x100;
inline constexpr uint64_t Command = 0x200;
inline constexpr uint64_t FlashAddress = 0x204;
inline constexpr uint64_t ByteCounts = 0x208;
inline constexpr uint64_t TriggerStatus = 0x20C;
inline constexpr uint64_t ClearDone = 0x210;
inline constexpr uint64_t ClockDivider = 0x300;

inline constexpr uint64_t RamStride = sizeof(uint32_t);
// 地址表没有显式给出 RAM 深度；该值只作为防止访问越过相邻区域的软件安全上限。
inline constexpr size_t RamBytes = 0x100;
inline constexpr uint32_t TriggerValue = 0xA5;
inline constexpr uint32_t ClearDoneValue = 1;
// D16 是 bit16；其完成电平由具体命令阶段决定。
inline constexpr uint32_t CompletionMask = 1u << 16;

constexpr uint64_t read_ram_word(size_t index) {
    return ReadRamBase + index * RamStride;
}

constexpr uint64_t write_ram_word(size_t index) {
    return WriteRamBase + index * RamStride;
}

} // namespace MB_DDF::HW::Registers::Flash
