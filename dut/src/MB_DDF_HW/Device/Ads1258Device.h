#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"
#include <array>
#include <cstddef>

namespace MB_DDF::HW {

/// ADS1258 全部可写配置寄存器的原始值集合。
struct Ads1258Config {
    uint32_t write_command{}, config0{}, config1{}, muxsch{}, muxdif{}, muxsg0{}, muxsg1{},
        sysred{}, gpioc{}, gpiod{}, work_count{}, clock_select{}, spi_divider{}, pwdn_wait{},
        reset_wait{}, reset_low_time{}, drdy_timeout{}, activation_threshold_c{},
        activation_threshold_b{}, activation_count{}, activation_period{},
        drdy_read_delay{};
};

/// FPGA 维护的 ADS1258 采集错误计数。
struct Ads1258ErrorCounters {
    uint32_t acquisition_timeout{}, chip1_no_new_data{}, chip1_overflow{}, chip1_power_fault{},
        chip1_channel_fault{}, chip2_no_new_data{}, chip2_overflow{}, chip2_power_fault{},
        chip2_channel_fault{};
};

/// 单片 ADS1258 的 v4 原始诊断寄存器，顺序对应 OFFSET/VCC/TEMP/GAIN/VREF。
struct Ads1258ChipDiagnostics {
    uint32_t offset{}, vcc{}, temperature{}, gain{}, vref{};
};

/// 单片 ADS1258 诊断值换算结果。VCC/TEMP 用于质量门槛，不直接参与电压公式。
struct Ads1258ChipCalibration {
    double offset_voltage{}, supply_voltage{}, temperature_celsius{}, gain{},
        reference_voltage{};
};

enum class Ads1258TemperatureMode {
    FreeAir,
    ControlledBoard,
};

/// HW_TEST 启动阶段保留的运行时覆盖；阈值沿用当前值，等待最终标定。
struct Ads1258RuntimeOverrides {
    uint32_t config0{0x02u};
    uint32_t config1{0x82u};
    uint32_t sysred{0x3Du};
    uint32_t work_count{0xAAu};
    uint32_t spi_divider{0x20u};
    uint32_t activation_threshold_c{0x21EC35u};
    uint32_t activation_threshold_b{0x21EC35u};
};

/// 两片 ADS1258 的 32 通道原始数据、五类诊断值及错误计数快照。
struct Ads1258Snapshot {
    std::array<uint32_t, 32> raw{};
    Ads1258ErrorCounters errors{};
    std::array<Ads1258ChipDiagnostics, 2> diagnostics{};
};

/// ADS1258 的强类型设备接口；快照保持原始 32 位格式，工程量按全局通道号换算。
class Ads1258Device {
public:
    inline static constexpr double ReferenceVoltage = 4.096;
    inline static constexpr uint32_t PositiveFullScaleCode = 0x780000u;
    inline static constexpr size_t LinearChannelCount = 3;
    inline static constexpr double FirstThreeChannelGain = 18.6;
    inline static constexpr double HighRangeGain = 16.23;
    inline static constexpr double V4SpecialChannelGain = 19.18;

    static constexpr double adc_input_voltage(uint32_t data) noexcept {
        const uint32_t raw_code = data & 0x00FFFFFFu;
        const int32_t signed_code = (raw_code & 0x00800000u) != 0u
                                        ? static_cast<int32_t>(raw_code) - 0x01000000
                                        : static_cast<int32_t>(raw_code);
        return static_cast<double>(signed_code) * ReferenceVoltage /
               static_cast<double>(PositiveFullScaleCode);
    }

    static constexpr double channel_voltage(size_t channel, uint32_t data) noexcept {
        const double a = adc_input_voltage(data);
        if ((channel > 0) && (channel <= LinearChannelCount)) {
            return a * FirstThreeChannelGain;
        }
        if (a <= 3.0) {
            return a * (-0.1594 * a * a + 0.843 * a + 15.1);
        }
        return a * HighRangeGain;
    }

    explicit Ads1258Device(ITransport& t) : transport_(t) {}
    Result<void> check_communication() const;
    Result<void> configure(const Ads1258Config&);
    Result<void> apply_runtime_overrides(const Ads1258RuntimeOverrides&);
    Result<void> clear_error_counters();
    Result<Ads1258Snapshot> read_snapshot() const;
    Result<Ads1258Config> read_config() const;
    Result<Ads1258ErrorCounters> read_error_counters() const;
    static Result<Ads1258ChipCalibration> decode_diagnostics(
        const Ads1258ChipDiagnostics&,
        Ads1258TemperatureMode mode = Ads1258TemperatureMode::FreeAir);
    static Result<double> calibrated_channel_voltage(
        size_t global_channel, const Ads1258Snapshot&,
        Ads1258TemperatureMode mode = Ads1258TemperatureMode::FreeAir);

private:
    ITransport& transport_;
};
} // namespace MB_DDF::HW
