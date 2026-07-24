#include "MB_DDF_HW_Test/ComEchoRunner.h"

#include "MB_DDF/Debug/Logger.h"
#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Transport/XdmaTransport.h"

#include <array>
#include <csignal>

namespace MB_DDF::HWTest {
namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void request_stop(int) noexcept {
    g_stop_requested = 1;
}

void log_status(const char* operation, const HW::Status& status) {
    LOG_ERROR << "[COM3-ECHO] " << operation << "失败：状态码="
              << static_cast<int>(status.code) << "，errno=" << status.errno_value
              << "，说明=" << status.message;
}

} // namespace

int run_com3_echo() {
    HW::XdmaTransport transport(
        {"/dev/xdma0", kCom3UserOffset, kComRegisterWindowSize, -1, -1,
         kCom3EventNumber});
    const auto opened = transport.open();
    if (!opened) {
        log_status("打开 COM3 XDMA Transport", opened.status());
        return 5;
    }

    HW::ComDevice device(transport);
    auto config = HW::ComDevice::default_config();
    config.receive_enabled = true;
    config.loopback = false;
    config.interrupt_mode = HW::ComInterruptMode::Level;
    const auto configured = device.configure(config);
    if (!configured) {
        log_status("配置 COM3 614400/8E1", configured.status());
        transport.close();
        return 5;
    }
    const auto cleared = device.clear_error_status();
    if (!cleared) {
        log_status("清除 COM3 接收错误", cleared.status());
        transport.close();
        return 5;
    }
    const auto enabled = device.enable_receive();
    if (!enabled) {
        log_status("使能 COM3 接收", enabled.status());
        transport.close();
        return 5;
    }

    g_stop_requested = 0;
    const auto previous_int = std::signal(SIGINT, request_stop);
    if (previous_int == SIG_ERR) {
        LOG_ERROR << "[COM3-ECHO] 安装 SIGINT 处理器失败";
        transport.close();
        return 5;
    }
    const auto previous_term = std::signal(SIGTERM, request_stop);
    if (previous_term == SIG_ERR) {
        LOG_ERROR << "[COM3-ECHO] 安装 SIGTERM 处理器失败";
        (void)std::signal(SIGINT, previous_int);
        transport.close();
        return 5;
    }
    LOG_INFO << "[COM3-ECHO] 已启动：/dev/xdma0，偏移=0xC0000，event=2，614400/8E1";

    std::array<uint8_t, 255> payload{};
    int result = 0;
    while (g_stop_requested == 0) {
        const auto received =
            device.receive({payload.data(), payload.size()}, HW::Timeout::after_us(100000));
        if (!received) {
            if (received.status().code == HW::StatusCode::ProtocolError) {
                log_status("丢弃无效 COM3 帧", received.status());
                continue;
            }
            log_status("接收 COM3 payload", received.status());
            result = 5;
            break;
        }
        if (received.value() == 0) {
            continue;
        }
        const auto sent = device.send({payload.data(), received.value()});
        if (!sent || sent.value() != received.value()) {
            if (!sent) {
                log_status("发送 COM3 回显 payload", sent.status());
            } else {
                LOG_ERROR << "[COM3-ECHO] 回显长度不一致：接收=" << received.value()
                          << "，发送=" << sent.value();
            }
            result = 5;
            break;
        }
    }

    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
    transport.close();
    LOG_INFO << "[COM3-ECHO] 已正常退出";
    return result;
}

} // namespace MB_DDF::HWTest
