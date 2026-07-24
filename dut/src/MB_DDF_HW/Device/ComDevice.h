#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"
#include "MB_DDF_HW/Endpoint/IByteEndpoint.h"
#include <array>

namespace MB_DDF::HW {

/// COM 接收中断为自动回落脉冲或软件清除电平。
enum class ComInterruptMode {
    Pulse,
    Level
};

/// COM 字节格式及接收控制寄存器的原始值。
struct ComFrameFormat {
    uint8_t byte_format{0xB0};
    uint8_t receive_control{0x21};
};

/// 帧长字段、帧头和帧尾布局。
struct ComFrameLayout {
    uint8_t send_length_bytes{1}, receive_length_bytes{1};
    std::array<uint8_t, 4> send_header{0x55, 0xAA, 0x00, 0x00};
    std::array<uint8_t, 4> send_tail{};
    std::array<uint8_t, 4> receive_header{0x55, 0xAA, 0x00, 0x00};
    std::array<uint8_t, 4> receive_tail{};
    uint8_t send_header_length{2}, send_tail_length{0}, receive_header_length{2},
        receive_tail_length{0};
};

/// COM 完整配置；默认值来自寄存器规范，但不会自动写入硬件。
struct ComConfig {
    ComFrameFormat format{};
    ComFrameLayout frame{};
    bool receive_enabled{true};
    bool byte_timeout_returns_idle{true};
    bool loopback{false};
    ComInterruptMode interrupt_mode{ComInterruptMode::Pulse};
    uint16_t interrupt_pulse_counter{5};
    uint32_t receive_timeout_counter{312499};
    uint16_t baudrate_counter{0x00CA};
};

/// 基于片上 RAM 和 XDMA event fd 的 COM 字节端点。
class ComDevice final : public IByteEndpoint {
public:
    explicit ComDevice(ITransport& t) : transport_(t) {}
    /// 返回 COM1 线协议默认值；调用方仍须显式调用 configure() 写入硬件。
    static ComConfig default_config();
    Result<void> check_communication() const;
    Result<void> configure(const ComConfig&);
    Result<ComConfig> read_config() const;
    Result<size_t> send(BufferView) override;
    Result<size_t> receive(MutableBufferView, Timeout) override;
    size_t mtu() const override {
        return 65536;
    }
    Result<uint32_t> read_error_status() const;
    Result<void> clear_error_status();
    /// 触发破坏性软件复位；部分 FPGA 会锁存到下一次 PCIe/AXI 全局复位，正常收发勿用。
    Result<void> reset();
    Result<void> enable_receive();

private:
    ITransport& transport_;
    /// 当前软件配置是否启用了 COM 内部回环。
    bool loopback_enabled_{false};
    /// event 缺失时只允许最近一次回环发送触发一次 RAM 降级读取。
    bool loopback_receive_pending_{false};
};
} // namespace MB_DDF::HW
