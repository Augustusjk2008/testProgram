#pragma once

#include "MB_DDF_HW/Transport/ITransport.h"
#include <sstream>

namespace MB_DDF::HW {

/// 读取设备统一通信测试寄存器，并校验 FPGA 固定签名。
inline Result<void> check_device_communication(const ITransport& transport) {
    auto value = transport.read32(0);
    if (!value) {
        return value.status();
    }
    if (value.value() != 0xAAAABBBBu) {
        std::ostringstream message;
        message << "communication test mismatch: 0x" << std::hex << value.value();
        return Status::error(StatusCode::HardwareFault, 0, message.str());
    }
    return {};
}
} // namespace MB_DDF::HW
