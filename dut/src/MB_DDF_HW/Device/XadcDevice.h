#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"

#include <cstdint>

namespace MB_DDF::HW {

/// XADC 单路电压采样；adc_code 为 Data[15:4]，voltage 为板级定标后的伏特值。
struct XadcVoltageSample {
    uint16_t adc_code{};
    double voltage{};
};

struct XadcElectricalHealthSample {
    double external_voltage{};
    double core_voltage{};
    double assist_voltage{};
    double js_5v_voltage{};
    double dyt_5v_voltage{};
    double power_24v_voltage{};
};

/// XADC 只读采样接口。该 IP 的 offset 0 是复位写寄存器，不使用通用通信签名校验。
class XadcDevice {
public:
    inline static constexpr double TemperatureScale = 503.975;
    inline static constexpr double TemperatureOffset = 273.15;
    inline static constexpr double Js5VGain = 10.09;
    inline static constexpr double ValueYxGain = 10.09;
    inline static constexpr double External3V3Gain = 10.09;
    inline static constexpr double InternalSupplyGain = 3.0;
    inline static constexpr double Dyt5VGain = 10.09;
    inline static constexpr double Power24VGain = 35.09;

    explicit XadcDevice(ITransport& transport) : transport_(transport) {}

    static constexpr uint16_t adc_code(uint32_t data) noexcept {
        return static_cast<uint16_t>((data >> 4u) & 0x0FFFu);
    }

    static constexpr double value_yx_voltage(uint32_t data) noexcept {
        return static_cast<double>(adc_code(data)) * ValueYxGain / 4096.0;
    }

    static constexpr double js_5v_voltage(uint32_t data) noexcept {
        return static_cast<double>(adc_code(data)) * Js5VGain / 4096.0;
    }

    static constexpr double unipolar_voltage(uint32_t data,
                                             double gain) noexcept {
        return static_cast<double>(adc_code(data)) * gain / 4096.0;
    }

    static constexpr double temperature_celsius(uint32_t data) noexcept {
        return static_cast<double>(adc_code(data)) * TemperatureScale / 4096.0 -
               TemperatureOffset;
    }

    Result<XadcVoltageSample> read_value_yx() const;
    Result<double> read_temperature_celsius() const;
    Result<XadcElectricalHealthSample> read_electrical_health() const;

private:
    ITransport& transport_;
};

} // namespace MB_DDF::HW
