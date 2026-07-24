#include "MB_DDF_HW/Device/Ad7606Device.h"
#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Device/DhController.h"
#include "MB_DDF_HW/Device/DidoDevice.h"
#include "MB_DDF_HW/Device/FlashDevice.h"
#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW/Device/XadcDevice.h"
#include "MB_DDF_HW/Device/Registers/ComRegisters.h"
#include "MB_DDF_HW/Device/Registers/FlashRegisters.h"
#include "MB_DDF_HW/Device/Registers/XadcRegisters.h"
#include "MB_DDF_HW/Transport/XdmaTransport.h"
#ifdef MB_DDF_HW_SMOKE_WITH_ADAPTER
#include "MB_DDF_HW/DdsAdapter/ComExternalEndpoint.h"
#endif
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
using namespace MB_DDF::HW;
namespace {

// Smoke 默认只读；Flash 只读状态/分频，只有 --com-loopback 会修改并恢复 COM 配置。
bool ok(const Result<void>& result, const char* action) {
    if (result) {
        return true;
    }
    std::cerr << action << ": " << result.status().message << '\n';
    return false;
}
template <class T> bool ok(const Result<T>& result, const char* action) {
    if (result) {
        return true;
    }
    std::cerr << action << ": " << result.status().message << '\n';
    return false;
}
int loopback(unsigned index, uint32_t timeout_us) {
    const uint64_t offsets[] = {0x40000, 0x80000, 0xC0000, 0x100000};
    XdmaTransport transport(
        {"/dev/xdma0", offsets[index], 0x40000, -1, -1, static_cast<int>(index)});
    if (!ok(transport.open(), "open COM")) {
        return 1;
    }
    ComDevice com(transport);
    auto saved = com.read_config();
    if (!ok(saved, "read COM config")) {
        return 1;
    }
    auto config = saved.value();
    config.loopback = true;
    config.receive_enabled = true;
    // COM 中断直接连接 XDMA usr_irq_req，必须保持电平直至软件写 0x82 清除。
    config.interrupt_mode = ComInterruptMode::Level;
    // Control.D7 的软件复位在实际 FPGA 中为锁存式操作，正常收发流程不能调用 reset()；
    // 直接配置、清错并使能接收，与原始 RS422 驱动保持一致。
    if (!ok(com.configure(config), "configure COM loopback") ||
        !ok(com.clear_error_status(), "clear COM errors") ||
        !ok(com.enable_receive(), "enable COM receive")) {
        com.configure(saved.value());
        return 1;
    }
    const std::array<uint8_t, 16> payload = {0x4D, 0x42, 0x5F, 0x44, 0x44,
                                             0x46, 0x5F, 0x48, 0x57, 0x5F,
                                             0x4C, 0x4F, 0x4F, 0x50, static_cast<uint8_t>(index),
                                             0xA5};
    std::array<uint8_t, 64> received{};
    bool sent = false;
    int32_t received_size = -1;
#ifdef MB_DDF_HW_SMOKE_WITH_ADAPTER
    ComExternalEndpoint endpoint(com);
    sent = endpoint.send(payload.data(), payload.size());
    if (sent) {
        received_size = endpoint.receive(received.data(), received.size(), timeout_us);
    }
#else
    auto sent_result = com.send({payload.data(), payload.size()});
    sent = ok(sent_result, "send COM loopback");
    if (sent) {
        auto received_result =
            com.receive({received.data(), received.size()}, Timeout::after_us(timeout_us));
        if (ok(received_result, "receive COM loopback")) {
            received_size = static_cast<int32_t>(received_result.value());
        }
    }
#endif
    if (!sent) {
        com.configure(saved.value());
        return 1;
    }
    bool matched = received_size == static_cast<int32_t>(payload.size()) &&
                   std::memcmp(payload.data(), received.data(), payload.size()) == 0;
    std::array<uint8_t, 64> duplicate_check{};
    auto no_duplicate =
        com.receive({duplicate_check.data(), duplicate_check.size()}, Timeout::poll());
    matched &= no_duplicate && no_duplicate.value() == 0;
    if (!matched) {
        std::cerr << "COM" << (index + 1) << " first=" << received_size << ", second="
                  << (no_duplicate ? std::to_string(no_duplicate.value()) : "error") << '\n';
        const auto control = transport.read32(Registers::Com::Control);
        const auto error = transport.read32(Registers::Com::Error);
        const auto format = transport.read32(Registers::Com::Format);
        const auto modes = transport.read32(Registers::Com::Loopback);
        const auto lengths = transport.read32(Registers::Com::SendLength);
        const auto received_bytes = transport.read32(Registers::Com::ReceivedBytes);
        const auto send_word0 = transport.read32(Registers::Com::SendRam);
        const auto send_word1 = transport.read32(Registers::Com::SendRam + 4);
        const auto send_word2 = transport.read32(Registers::Com::SendRam + 8);
        const auto send_word3 = transport.read32(Registers::Com::SendRam + 12);
        const auto send_word4 = transport.read32(Registers::Com::SendRam + 16);
        const auto receive_word0 = transport.read32(Registers::Com::ReceiveRam);
        std::cerr << std::hex << std::setfill('0');
        if (control) std::cerr << " control=0x" << std::setw(8) << control.value();
        if (error) std::cerr << " error=0x" << std::setw(8) << error.value();
        if (format) std::cerr << " format=0x" << std::setw(8) << format.value();
        if (modes) std::cerr << " modes=0x" << std::setw(8) << modes.value();
        if (lengths) std::cerr << " lengths=0x" << std::setw(8) << lengths.value();
        if (received_bytes) {
            std::cerr << " received_bytes=0x" << std::setw(8) << received_bytes.value();
        }
        if (send_word0) std::cerr << " tx0=0x" << std::setw(8) << send_word0.value();
        if (send_word1) std::cerr << " tx1=0x" << std::setw(8) << send_word1.value();
        if (send_word2) std::cerr << " tx2=0x" << std::setw(8) << send_word2.value();
        if (send_word3) std::cerr << " tx3=0x" << std::setw(8) << send_word3.value();
        if (send_word4) std::cerr << " tx4=0x" << std::setw(8) << send_word4.value();
        if (receive_word0) std::cerr << " rx0=0x" << std::setw(8) << receive_word0.value();
        std::cerr << std::dec << '\n';
    }
    auto restored = com.configure(saved.value());
    if (!ok(restored, "restore COM config")) {
        return 1;
    }
    if (!matched) {
        std::cerr << "COM" << (index + 1) << " loopback mismatch\n";
        return 1;
    }
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    bool do_loopback = false;
    std::string selected = "all";
    uint32_t timeout_us = 1000000;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--read-only") {
            continue;
        }
        if (arg == "--com-loopback") {
            do_loopback = true;
        } else if (arg == "--com-index" && i + 1 < argc) {
            selected = argv[++i];
        } else if (arg == "--timeout-us" && i + 1 < argc) {
            timeout_us = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 2;
        }
    }
    XdmaTransport pwm_t({"/dev/xdma0", 0, 0x10000});
    XdmaTransport ad7606_t({"/dev/xdma0", 0x10000, 0x10000});
    XdmaTransport ads1258_t({"/dev/xdma0", 0x20000, 0x10000});
    XdmaTransport dh_t({"/dev/xdma0", 0x30000, 0x10000});
    XdmaTransport dido_t({"/dev/xdma0", 0x140000, 0x10000});
    XdmaTransport xadc_t(
        {"/dev/xdma0", Registers::Xadc::UserBase,
         Registers::Xadc::WindowSize});
    XdmaTransport flash_t(
        {"/dev/xdma0", Registers::Flash::UserBase, 0x10000});
    for (auto* transport :
         {&pwm_t, &ad7606_t, &ads1258_t, &dh_t, &dido_t, &xadc_t, &flash_t}) {
        if (!ok(transport->open(), "open device")) {
            return 1;
        }
    }
    PwmDevice pwm(pwm_t);
    Ad7606Device ad7606(ad7606_t);
    Ads1258Device ads1258(ads1258_t);
    DhController dh(dh_t);
    DidoDevice dido(dido_t);
    XadcDevice xadc(xadc_t);
    FlashDevice flash(flash_t);
    if (!ok(pwm.check_communication(), "PWM communication") ||
        !ok(ad7606.check_communication(), "AD7606 communication") ||
        !ok(ads1258.check_communication(), "ADS1258 communication") ||
        !ok(dh.check_communication(), "DH communication") ||
        !ok(dido.check_communication(), "DIDO communication") ||
        !ok(flash.check_communication(), "Flash transport")) {
        return 1;
    }
    if (!ok(pwm.read_state(), "PWM state") || !ok(ad7606.read_snapshot(), "AD7606 snapshot") ||
        !ok(ads1258.read_snapshot(), "ADS1258 snapshot") ||
        !ok(dh.read_feedback(), "DH feedback") || !ok(dido.read_snapshot(), "DIDO snapshot") ||
        !ok(xadc.read_value_yx(), "XADC value_YX") ||
        !ok(flash.read_controller_status(), "Flash controller status") ||
        !ok(flash.read_clock_divider(), "Flash clock divider")) {
        return 1;
    }
    const uint64_t com_offsets[] = {0x40000, 0x80000, 0xC0000, 0x100000};
    for (unsigned i = 0; i < 4; ++i) {
        XdmaTransport transport(
            {"/dev/xdma0", com_offsets[i], 0x40000, -1, -1, static_cast<int>(i)});
        if (!ok(transport.open(), "open COM")) {
            return 1;
        }
        ComDevice com(transport);
        if (!ok(com.check_communication(), "COM communication") ||
            !ok(com.read_config(), "COM config") || !ok(com.read_error_status(), "COM status")) {
            return 1;
        }
    }
    if (do_loopback) {
        std::vector<unsigned> indices;
        if (selected == "all") {
            indices = {0, 1, 2, 3};
        } else {
            const unsigned index = static_cast<unsigned>(std::stoul(selected));
            if (index > 3) {
                std::cerr << "COM index must be 0..3 or all\n";
                return 2;
            }
            indices.push_back(index);
        }
        for (auto index : indices) {
            if (loopback(index, timeout_us) != 0) {
                return 1;
            }
        }
    }
    std::cout << "MB_DDF_HW smoke passed\n";
    return 0;
}
