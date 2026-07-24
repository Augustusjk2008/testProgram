#pragma once

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HWTest {

inline constexpr uint64_t kCom3UserOffset = 0xC0000;
inline constexpr size_t kComRegisterWindowSize = 0x40000;
inline constexpr int kCom3EventNumber = 2;

int run_com3_echo();

} // namespace MB_DDF::HWTest
