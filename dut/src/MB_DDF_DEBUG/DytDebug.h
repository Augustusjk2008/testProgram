#pragma once

#include "MB_DDF/Tools/SelfDescribingLog.h"
#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Endpoint/IByteEndpoint.h"
#include "MB_DDF_HW/Transport/TransportConfig.h"

#include <cstdint>
#include <string>

namespace MB_DDF::DytDebug {

struct LoopStats {
    uint64_t received_b_frames{0};
    uint64_t sent_a_frames{0};
};

/// COM1 的 XDMA 映射：user BAR 0x40000、256 KiB 窗口、event 0。
HW::XdmaConfig make_com1_transport_config();

/// 保留 COM 默认配置，仅把 125 MHz 时钟下的波特率计数器改为 135。
HW::ComConfig make_com1_config();

/// 先发送一帧 A，持续接收并记录有效 B 帧，每收到四帧再发送一帧 A。
/// max_b_frames 为 0 时持续运行；非 0 仅供可控调用和测试。
HW::Result<LoopStats> run_frame_loop(
    HW::IByteEndpoint& endpoint,
    Tools::SelfDescribingLogWriter& writer,
    uint64_t max_b_frames = 0);

/// 打开并初始化 COM1，然后将 B 帧追加到指定自描述日志文件。
int run_dyt_debug(const std::string& log_path = "dyt_frame_b.sdlog");

} // namespace MB_DDF::DytDebug
