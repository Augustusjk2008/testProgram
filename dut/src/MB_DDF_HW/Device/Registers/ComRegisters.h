#pragma once

#include <cstdint>

namespace MB_DDF::HW::Registers::Com {

// COM 地址表本身使用字节偏移，因此这里不再乘以 4。
inline constexpr uint64_t ReceiveRam = 0x00000, SendRam = 0x10000, Format = 0x20000,
                          SendHeader = 0x20004, SendTail = 0x20008, ReceiveHeader = 0x2000C,
                          ReceiveTail = 0x20010, Loopback = 0x20014, InterruptMode = 0x20015,
                          InterruptPulse = 0x20018, SendLength = 0x2001C, ReceiveLength = 0x2001E,
                          ReceiveTimeout = 0x20020, SendEndDelay = 0x20024, SendBaud = 0x20028,
                          ReceiveBaud = 0x2002A, Control = 0x30000, Error = 0x30004,
                          PulseEnable = 0x30008, ReceivedBytes = 0x3000C;
} // namespace MB_DDF::HW::Registers::Com
