#pragma once

#include <cstdint>

namespace MB_DDF::HW::Registers::Dido {

// DIDO_ctrl 设备局部字节偏移。
inline constexpr uint64_t Communication = 0, OutputBase = 0x04, InputBase = 0x80, Stride = 4;
constexpr uint64_t output(unsigned index) {
    return OutputBase + index * Stride;
}
constexpr uint64_t input(unsigned index) {
    return InputBase + index * Stride;
}
} // namespace MB_DDF::HW::Registers::Dido
