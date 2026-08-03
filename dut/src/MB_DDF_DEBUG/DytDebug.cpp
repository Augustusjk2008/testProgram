#include "MB_DDF_DEBUG/DytDebug.h"

#include "MB_DDF/Debug/Logger.h"
#include "MB_DDF_DEBUG/frame_a.h"
#include "MB_DDF_DEBUG/frame_b.h"
#include "MB_DDF_HW/Transport/XdmaTransport.h"

#include <array>
#include <cstdint>
#include <string>

namespace MB_DDF::DytDebug {
namespace {

constexpr uint16_t kCom1BaudrateCounter = 135;
constexpr uint64_t kReplyInterval = 4;

HW::Result<void> send_a_frame(HW::IByteEndpoint& endpoint,
                              ProtocolModel::Main_to_dyt_frame_a& frame) {
    const auto payload =
        ProtocolModel::Main_to_dyt_frame_aProtocol::packFrame(frame);
    const auto* bytes = reinterpret_cast<const uint8_t*>(payload.data());
    const auto sent = endpoint.send({bytes, payload.size()});
    if (!sent) {
        return sent.status();
    }
    if (sent.value() != payload.size()) {
        return HW::Status::error(HW::StatusCode::IoError, 0,
                                 "COM1 sent only part of frame A");
    }
    return {};
}

int report_hardware_error(const char* operation, const HW::Status& status,
                          int exit_code) {
    LOG_ERROR << "[DYT-DEBUG] " << operation << "失败：" << status.message
              << "，errno=" << status.errno_value;
    return exit_code;
}

} // namespace

HW::XdmaConfig make_com1_transport_config() {
    return {"/dev/xdma0", 0x40000, 0x40000, -1, -1, 0};
}

HW::ComConfig make_com1_config() {
    auto config = HW::ComDevice::default_config();
    config.baudrate_counter = kCom1BaudrateCounter;
    return config;
}

HW::Result<LoopStats> run_frame_loop(
    HW::IByteEndpoint& endpoint,
    Tools::SelfDescribingLogWriter& writer,
    uint64_t max_b_frames) {
    constexpr size_t frame_size =
        ProtocolModel::Dyt_to_main_frame_bProtocol::FRAME_SIZE;
    if (!writer.isOpen()) {
        return HW::Status::error(HW::StatusCode::InvalidArgument, 0,
                                 "self-describing log is not open");
    }
    if (endpoint.mtu() < frame_size) {
        return HW::Status::error(HW::StatusCode::BufferTooSmall, 0,
                                 "COM1 MTU is smaller than frame B");
    }

    LoopStats stats;
    ProtocolModel::Main_to_dyt_frame_a outgoing;
    auto sent = send_a_frame(endpoint, outgoing);
    if (!sent) {
        return sent.status();
    }
    ++stats.sent_a_frames;

    std::array<uint8_t, frame_size> received{};
    while (max_b_frames == 0 || stats.received_b_frames < max_b_frames) {
        const auto result = endpoint.receive(
            {received.data(), received.size()}, HW::Timeout::forever());
        if (!result) {
            if (result.status().code == HW::StatusCode::ProtocolError) {
                LOG_WARN << "[DYT-DEBUG] 丢弃无效 B 帧并继续接收："
                         << result.status().message;
                continue;
            }
            return result.status();
        }
        if (result.value() == 0) {
            continue;
        }
        if (result.value() != frame_size) {
            LOG_WARN << "[DYT-DEBUG] 忽略长度不匹配的 B 帧：actual="
                     << result.value() << "，expected=" << frame_size;
            continue;
        }

        ProtocolModel::Dyt_to_main_frame_b frame;
        if (!ProtocolModel::Dyt_to_main_frame_bProtocol::unpackFrame(
                reinterpret_cast<const char*>(received.data()),
                result.value(), frame)) {
            LOG_WARN << "[DYT-DEBUG] 忽略无法解析的 B 帧";
            continue;
        }
        if (!writer.appendRecord(&frame, static_cast<uint32_t>(sizeof(frame)))) {
            return HW::Status::error(HW::StatusCode::IoError, 0,
                                     "failed to append frame B to self-describing log");
        }
        writer.flush();
        ++stats.received_b_frames;

        if (stats.received_b_frames % kReplyInterval == 0) {
            ++outgoing.frameCounter;
            sent = send_a_frame(endpoint, outgoing);
            if (!sent) {
                return sent.status();
            }
            ++stats.sent_a_frames;
        }
    }
    return stats;
}

int run_dyt_debug(const std::string& log_path) {
    HW::XdmaTransport transport(make_com1_transport_config());
    const auto opened = transport.open();
    if (!opened) {
        return report_hardware_error("打开 COM1 XDMA Transport", opened.status(), 4);
    }

    HW::ComDevice com1(transport);
    const auto configured = com1.configure(make_com1_config());
    if (!configured) {
        return report_hardware_error("配置 COM1 921600", configured.status(), 5);
    }
    const auto cleared = com1.clear_error_status();
    if (!cleared) {
        return report_hardware_error("清除 COM1 错误状态", cleared.status(), 6);
    }
    const auto enabled = com1.enable_receive();
    if (!enabled) {
        return report_hardware_error("启动 COM1 接收", enabled.status(), 7);
    }

    Tools::SelfDescribingLogWriter writer;
    const auto schema =
        ProtocolModel::Dyt_to_main_frame_bProtocol::buildSchema();
    if (!writer.open(log_path, schema)) {
        LOG_ERROR << "[DYT-DEBUG] 打开自描述日志失败：" << log_path;
        return 8;
    }

    LOG_INFO << "[DYT-DEBUG] COM1 已配置为 921600，baudrate_counter="
             << kCom1BaudrateCounter << "，B 帧日志=" << log_path;
    const auto result = run_frame_loop(com1, writer);
    if (!result) {
        return report_hardware_error("导引头收发循环", result.status(), 9);
    }
    return 0;
}

} // namespace MB_DDF::DytDebug
