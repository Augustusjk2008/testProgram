#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/Registers/Ads1258Registers.h"

#include <cmath>
#include <utility>

namespace MB_DDF::HW {
namespace R = Registers::Ads1258;
namespace {
constexpr uint32_t kRaw24Mask = 0x00FFFFFFu;
constexpr double kDiagnosticVoltageDivisor = 0x0C0000u;

int32_t signed24(uint32_t word) {
    const uint32_t raw = word & kRaw24Mask;
    return (raw & 0x00800000u) != 0u
               ? static_cast<int32_t>(raw) - 0x01000000
               : static_cast<int32_t>(raw);
}

bool has_valid_upper_byte(uint32_t word) {
    return (word & 0xFF000000u) == 0u;
}

Status invalid_diagnostic(const char* message) {
    return Status::error(StatusCode::HardwareFault, 0, message);
}

Result<void> begin_reconfiguration(ITransport& transport) {
    auto disabled = transport.write32(R::Enable1, 0xFFFFu);
    if (!disabled) {
        return disabled;
    }
    return transport.write32(R::StateRollback, 0xAAAAu);
}

Result<void> finish_reconfiguration(ITransport& transport,
                                    uint32_t enable1, uint32_t enable2) {
    const auto rollback = transport.write32(R::StateRollback, 0xFFFFu);
    if (!rollback) {
        (void)transport.write32(R::Enable1, 0xFFFFu);
        (void)transport.write32(R::Enable2, 0xFFFFu);
        return rollback;
    }
    const auto restored_enable1 = transport.write32(R::Enable1, enable1);
    const auto restored_enable2 = transport.write32(R::Enable2, enable2);
    if (!restored_enable1 || !restored_enable2) {
        (void)transport.write32(R::Enable1, 0xFFFFu);
        (void)transport.write32(R::Enable2, 0xFFFFu);
        return !restored_enable1 ? restored_enable1 : restored_enable2;
    }
    return {};
}

void best_effort_finish_reconfiguration(ITransport& transport,
                                        uint32_t enable1, uint32_t enable2) {
    (void)finish_reconfiguration(transport, enable1, enable2);
}

std::array<uint32_t, 21> values(const Ads1258Config& c) {
    return {c.write_command,
            c.config0,
            c.config1,
            c.muxsch,
            c.muxdif,
            c.muxsg0,
            c.muxsg1,
            c.sysred,
            c.gpioc,
            c.gpiod,
            c.work_count,
            c.clock_select,
            c.spi_divider,
            c.pwdn_wait,
            c.reset_wait,
            c.reset_low_time,
            c.drdy_timeout,
            c.activation_threshold_c,
            c.activation_threshold_b,
            c.activation_count,
            c.activation_period};
}
Ads1258Config config(const std::array<uint32_t, 21>& v,
                     uint32_t drdy_read_delay) {
    return {v[0],  v[1],  v[2],  v[3],  v[4],  v[5],  v[6],  v[7],  v[8],  v[9], v[10],
            v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19], v[20],
            drdy_read_delay};
}
} // namespace
Result<void> Ads1258Device::check_communication() const {
    return check_device_communication(transport_);
}
Result<void> Ads1258Device::configure(const Ads1258Config& c) {
    const auto saved_enable1 = transport_.read32(R::Enable1);
    if (!saved_enable1) {
        return saved_enable1.status();
    }
    const auto saved_enable2 = transport_.read32(R::Enable2);
    if (!saved_enable2) {
        return saved_enable2.status();
    }
    const auto guarded = begin_reconfiguration(transport_);
    if (!guarded) {
        best_effort_finish_reconfiguration(transport_, 0xFFFFu, 0xFFFFu);
        return guarded;
    }

    const auto v = values(c);
    for (unsigned i = 0; i < v.size(); ++i) {
        auto s = transport_.write32(R::config(i), v[i]);
        if (!s) {
            best_effort_finish_reconfiguration(transport_, 0xFFFFu, 0xFFFFu);
            return s;
        }
    }
    const auto drdy = transport_.write32(R::DrdyReadDelay, c.drdy_read_delay);
    if (!drdy) {
        best_effort_finish_reconfiguration(transport_, 0xFFFFu, 0xFFFFu);
        return drdy;
    }
    return finish_reconfiguration(
        transport_, saved_enable1.value(), saved_enable2.value());
}
Result<void> Ads1258Device::apply_runtime_overrides(
    const Ads1258RuntimeOverrides& overrides) {
    const auto guarded = begin_reconfiguration(transport_);
    if (!guarded) {
        best_effort_finish_reconfiguration(transport_, 0xFFFFu, 0xFFFFu);
        return guarded;
    }
    const std::pair<uint64_t, uint32_t> writes[]{
        {R::Config0, overrides.config0},
        {R::Config1, overrides.config1},
        {R::Sysred, overrides.sysred},
        {R::WorkCount, overrides.work_count},
        {R::SpiDivider, overrides.spi_divider},
        {R::ActivationThresholdC, overrides.activation_threshold_c},
        {R::ActivationThresholdB, overrides.activation_threshold_b},
    };
    for (const auto& [offset, value] : writes) {
        const auto result = transport_.write32(offset, value);
        if (!result) {
            best_effort_finish_reconfiguration(transport_, 0xFFFFu, 0xFFFFu);
            return result;
        }
    }
    return finish_reconfiguration(transport_, 0xAAAAu, 0xAAAAu);
}
Result<void> Ads1258Device::clear_error_counters() {
    return transport_.write32(R::ClearErrors, 0xFFu);
}
Result<Ads1258Config> Ads1258Device::read_config() const {
    std::array<uint32_t, 21> v{};
    for (unsigned i = 0; i < v.size(); ++i) {
        auto value = transport_.read32(R::config(i));
        if (!value) {
            return value.status();
        }
        v[i] = value.value();
    }
    auto drdy_read_delay = transport_.read32(R::DrdyReadDelay);
    if (!drdy_read_delay) {
        return drdy_read_delay.status();
    }
    return config(v, drdy_read_delay.value());
}
Result<Ads1258ErrorCounters> Ads1258Device::read_error_counters() const {
    std::array<uint32_t, 9> v{};
    for (unsigned i = 0; i < 9; ++i) {
        auto r = transport_.read32(R::error(i));
        if (!r) {
            return r.status();
        }
        v[i] = r.value();
    }
    return Ads1258ErrorCounters{v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]};
}
Result<Ads1258Snapshot> Ads1258Device::read_snapshot() const {
    Ads1258Snapshot s{};
    for (unsigned i = 0; i < 32; ++i) {
        auto v = transport_.read32(R::data(i));
        if (!v) {
            return v.status();
        }
        s.raw[i] = v.value();
    }
    for (unsigned chip = 0; chip < s.diagnostics.size(); ++chip) {
        uint32_t values[5]{};
        for (unsigned item = 0; item < 5; ++item) {
            const auto value = transport_.read32(R::diagnostic(chip, item));
            if (!value) {
                return value.status();
            }
            values[item] = value.value();
        }
        s.diagnostics[chip] =
            Ads1258ChipDiagnostics{values[0], values[1], values[2],
                                   values[3], values[4]};
        const auto calibration = decode_diagnostics(s.diagnostics[chip]);
        if (!calibration) {
            return calibration.status();
        }
    }
    auto e = read_error_counters();
    if (!e) {
        return e.status();
    }
    s.errors = e.value();
    return s;
}

Result<Ads1258ChipCalibration> Ads1258Device::decode_diagnostics(
    const Ads1258ChipDiagnostics& raw, Ads1258TemperatureMode mode) {
    const uint32_t words[]{raw.offset, raw.vcc, raw.temperature, raw.gain,
                           raw.vref};
    for (const auto word : words) {
        if (!has_valid_upper_byte(word)) {
            return invalid_diagnostic("ADS1258 diagnostic upper byte is not zero");
        }
    }

    Ads1258ChipCalibration result{};
    result.reference_voltage =
        static_cast<double>(raw.vref & kRaw24Mask) /
        kDiagnosticVoltageDivisor;
    result.supply_voltage =
        static_cast<double>(raw.vcc & kRaw24Mask) /
        kDiagnosticVoltageDivisor;
    result.gain = static_cast<double>(raw.gain & kRaw24Mask) /
                  static_cast<double>(PositiveFullScaleCode);
    result.offset_voltage = static_cast<double>(signed24(raw.offset)) *
                            result.reference_voltage /
                            static_cast<double>(PositiveFullScaleCode);
    const double temperature_coefficient =
        mode == Ads1258TemperatureMode::FreeAir ? 394.0 : 563.0;
    result.temperature_celsius =
        (static_cast<double>(raw.temperature & kRaw24Mask) *
             result.reference_voltage /
             static_cast<double>(PositiveFullScaleCode) *
             1'000'000.0 -
         168'000.0) /
            temperature_coefficient +
        25.0;

    if ((raw.temperature & kRaw24Mask) == 0u ||
        !(result.reference_voltage > 0.0) ||
        !(result.supply_voltage > 0.0) || !(result.gain > 0.0) ||
        !std::isfinite(result.reference_voltage) ||
        !std::isfinite(result.supply_voltage) ||
        !std::isfinite(result.gain) ||
        !std::isfinite(result.offset_voltage) ||
        !std::isfinite(result.temperature_celsius)) {
        return invalid_diagnostic("ADS1258 diagnostic values are invalid");
    }
    return result;
}

Result<double> Ads1258Device::calibrated_channel_voltage(
    size_t global_channel, const Ads1258Snapshot& snapshot,
    Ads1258TemperatureMode mode) {
    if (global_channel >= snapshot.raw.size()) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "ADS1258 channel is out of range");
    }
    const uint32_t sample = snapshot.raw[global_channel];
    if (!has_valid_upper_byte(sample)) {
        return invalid_diagnostic("ADS1258 sample upper byte is not zero");
    }
    const auto calibration =
        decode_diagnostics(snapshot.diagnostics[global_channel / 16], mode);
    if (!calibration) {
        return calibration.status();
    }
    const auto& chip = calibration.value();
    const double sample_voltage = static_cast<double>(signed24(sample)) *
                                  chip.reference_voltage /
                                  static_cast<double>(PositiveFullScaleCode);
    const double corrected =
        (sample_voltage - chip.offset_voltage) / chip.gain;
    if (!std::isfinite(corrected)) {
        return invalid_diagnostic("ADS1258 corrected voltage is not finite");
    }

    double result = 0.0;
    if (global_channel >= 1 && global_channel <= 3) {
        constexpr double external_biases[]{0.0, 0.500, 0.325, 0.500};
        result = (corrected - external_biases[global_channel]) *
                 V4SpecialChannelGain;
    } else if (corrected >= 3.0) {
        result = corrected * HighRangeGain;
    } else if (corrected >= 0.0) {
        result = corrected *
                 (-0.1594 * corrected * corrected + 0.843 * corrected +
                  15.1);
    } else {
        return invalid_diagnostic(
            "ADS1258 corrected voltage is outside the v4 fit domain");
    }
    if (!std::isfinite(result)) {
        return invalid_diagnostic("ADS1258 calibrated voltage is not finite");
    }
    return result;
}
} // namespace MB_DDF::HW
