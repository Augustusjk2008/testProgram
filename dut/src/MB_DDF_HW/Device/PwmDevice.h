#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"
#include <array>
#include <cstdint>

namespace MB_DDF::HW {

/// PWM 载波波形类型。
enum class PwmWaveform {
    Sawtooth,
    Triangle
};

/// 有符号占空比下，正负占空比对应方向输出的模式。
enum class PwmDirectionMode : uint16_t {
    PositiveToZero = 0xAAAA,
    PositiveToOne = 0xBBBB,
};

/// 四个逻辑舵机通道到实际 PWM/方向通道的 4-bit 编码。
struct PwmChannelMapping {
    uint16_t encoded{0x3210};
};

/// 直接对应硬件寄存器的 PWM 输出值。
struct PwmRawOutputs {
    std::array<uint32_t, 4> duty{};
    uint8_t direction_mask{0};
    uint8_t enable_mask{0};
};

/// 范围为 [-1.0, 1.0] 的归一化 PWM 输出值。
struct PwmNormalizedOutputs {
    std::array<double, 4> value{};
    uint8_t enable_mask{0};
};

/// PWM 载波配置。
struct PwmConfig {
    uint32_t carrier_frequency_value{0};
    uint32_t peak_value{0};
    PwmWaveform waveform{PwmWaveform::Sawtooth};
    PwmDirectionMode direction_mode{PwmDirectionMode::PositiveToZero};
    PwmChannelMapping channel_mapping{};
};

/// PWM 配置和当前输出状态快照。
struct PwmState {
    PwmConfig config{};
    PwmRawOutputs outputs{};
    bool update_enabled{false};
};

/// PWM_ctrl 的强类型设备接口；不拥有 Transport。
class PwmDevice {
public:
    explicit PwmDevice(ITransport& transport) : transport_(transport) {}
    Result<void> check_communication() const;
    Result<void> configure(const PwmConfig& config);
    Result<void> set_duty_mode_unsigned();
    Result<void> set_update_enabled(bool enabled);
    Result<void> disable_outputs();
    Result<void> apply_outputs(const PwmRawOutputs& outputs);
    Result<void> apply_normalized_outputs(const PwmNormalizedOutputs& outputs);
    Result<PwmState> read_state() const;
    Result<uint32_t> read_peak_value() const;

private:
    ITransport& transport_;
};
} // namespace MB_DDF::HW
