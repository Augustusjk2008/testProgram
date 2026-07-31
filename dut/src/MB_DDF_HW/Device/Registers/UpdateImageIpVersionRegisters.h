#pragma once

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HW::Registers::UpdateImageIpVersion {

inline constexpr uint64_t UserBase = 0x160000;
inline constexpr size_t WindowSize = 0x10000;

inline constexpr uint64_t Communication = 0x00;
inline constexpr uint64_t SoftwareType = 0x04;
inline constexpr uint64_t SoftwareStatus = 0x08;
inline constexpr uint64_t ModelSoftwareVersion = 0x0C;
inline constexpr uint64_t ModelSoftwareDate = 0x10;

inline constexpr uint64_t DidoBase = 0x14;
inline constexpr uint64_t DidoVersion = 0x18;
inline constexpr uint64_t DidoDate = 0x1C;
inline constexpr uint64_t DhBase = 0x20;
inline constexpr uint64_t DhVersion = 0x24;
inline constexpr uint64_t DhDate = 0x28;
inline constexpr uint64_t Ad7606Base = 0x2C;
inline constexpr uint64_t Ad7606Version = 0x30;
inline constexpr uint64_t Ad7606Date = 0x34;
inline constexpr uint64_t PwmBase = 0x38;
inline constexpr uint64_t PwmVersion = 0x3C;
inline constexpr uint64_t PwmDate = 0x40;
inline constexpr uint64_t Ads1258Base = 0x44;
inline constexpr uint64_t Ads1258Version = 0x48;
inline constexpr uint64_t Ads1258Date = 0x4C;
inline constexpr uint64_t XadcBase = 0x50;
inline constexpr uint64_t XadcVersion = 0x54;
inline constexpr uint64_t XadcDate = 0x58;
inline constexpr uint64_t ComBase = 0x5C;
inline constexpr uint64_t ComVersion = 0x60;
inline constexpr uint64_t ComDate = 0x64;
inline constexpr uint64_t FpgaUpdateBase = 0x68;
inline constexpr uint64_t FpgaUpdateVersion = 0x6C;
inline constexpr uint64_t FpgaUpdateDate = 0x70;

} // namespace MB_DDF::HW::Registers::UpdateImageIpVersion
