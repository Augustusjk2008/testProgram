#pragma once

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HW::Registers::FpgaUpdateState {

inline constexpr uint64_t UserBase = 0x170000;
inline constexpr size_t WindowSize = 0x10000;

inline constexpr uint64_t Communication = 0x00;
inline constexpr uint64_t FlashEraseStatus = 0x04;
inline constexpr uint64_t FlashWriteStatus = 0x08;
inline constexpr uint64_t FlashReadStatus = 0x0C;
inline constexpr uint64_t ReadDataCrc = 0x10;
inline constexpr uint64_t WriteDataCrc = 0x14;
inline constexpr uint64_t CrcValidationStatus = 0x18;

} // namespace MB_DDF::HW::Registers::FpgaUpdateState
