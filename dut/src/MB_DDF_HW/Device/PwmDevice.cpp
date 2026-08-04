#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW/Device/Registers/PwmRegisters.h"
#include <cmath>

namespace MB_DDF::HW {
namespace R = Registers::Pwm;

namespace {

Result<PwmDirectionMode> decode_direction_mode(uint32_t value) {
    switch (value & 0xFFFFu) {
    case static_cast<uint16_t>(PwmDirectionMode::PositiveToZero):
        return PwmDirectionMode::PositiveToZero;
    case static_cast<uint16_t>(PwmDirectionMode::PositiveToOne):
        return PwmDirectionMode::PositiveToOne;
    default:
        return Status::error(StatusCode::HardwareFault, 0, "invalid PWM direction mode");
    }
}

} // namespace

Result<void> PwmDevice::check_communication() const {
    return check_device_communication(transport_);
}
Result<void> PwmDevice::configure(const PwmConfig& c) {
    auto a = transport_.write32(R::Carrier, c.carrier_frequency_value);
    if (!a) {
        return a;
    }
    auto b = transport_.write32(R::Peak, c.peak_value);
    if (!b) {
        return b;
    }
    auto waveform = transport_.write32(R::Waveform,
                                       c.waveform == PwmWaveform::Sawtooth ? 0xA001u : 0xA002u);
    if (!waveform) {
        return waveform;
    }
    auto direction_mode = transport_.write32(R::DirectionMode,
                                              static_cast<uint16_t>(c.direction_mode));
    if (!direction_mode) {
        return direction_mode;
    }
    return set_channel_mapping(c.channel_mapping);
}
Result<void> PwmDevice::set_channel_mapping(PwmChannelMapping mapping) {
    return transport_.write32(R::ChannelMapping, mapping.encoded);
}
Result<void> PwmDevice::set_duty_mode_unsigned() {
    return transport_.write32(R::DutyMode, 0xFFFFu);
}
Result<void> PwmDevice::set_update_enabled(bool v) {
    return transport_.write32(R::UpdateEnable, v ? 0xAAAAu : 0xFFFFu);
}
Result<void> PwmDevice::disable_outputs() {
    return transport_.write32(R::Enable, 0xFFFFu);
}
Result<uint32_t> PwmDevice::read_peak_value() const {
    return transport_.read32(R::Peak);
}
Result<void> PwmDevice::apply_outputs(const PwmRawOutputs& o) {
    auto peak = read_peak_value();
    if (!peak) {
        return peak.status();
    }
    for (auto duty : o.duty) {
        if (duty > peak.value()) {
            return Status::error(StatusCode::InvalidArgument, 0, "PWM duty exceeds peak value");
        }
    }
    auto current = transport_.read32(R::Enable);
    if (!current) {
        return current.status();
    }
    // 硬件要求先更新方向，再更新占空比，避免方向切换瞬间输出错误。
    for (unsigned i = 0; i < 4; ++i) {
        auto s =
            transport_.write32(R::direction(i), (o.direction_mask & (1u << i)) ? 0xAAAAu : 0xFFFFu);
        if (!s) {
            return s;
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        auto s = transport_.write32(R::duty(i), o.duty[i]);
        if (!s) {
            return s;
        }
    }
    if ((current.value() & 0xFu) != (o.enable_mask & 0xFu)) {
        uint32_t encoded = 0;
        for (unsigned i = 0; i < 4; ++i) {
            encoded |= ((o.enable_mask & (1u << i)) ? 0xAu : 0xFu) << (i * 4);
        }
        return transport_.write32(R::Enable, encoded);
    }
    return {};
}
Result<void> PwmDevice::apply_normalized_outputs(const PwmNormalizedOutputs& o) {
    auto peak = read_peak_value();
    if (!peak) {
        return peak.status();
    }
    PwmRawOutputs raw{};
    raw.enable_mask = o.enable_mask;
    for (unsigned i = 0; i < 4; ++i) {
        if (o.value[i] < -1.0 || o.value[i] > 1.0) {
            return Status::error(StatusCode::InvalidArgument, 0,
                                 "normalized PWM output is out of range");
        }
        raw.direction_mask |= (o.value[i] >= 0 ? 1u : 0u) << i;
        raw.duty[i] = static_cast<uint32_t>(std::abs(o.value[i]) * peak.value());
    }
    return apply_outputs(raw);
}
Result<PwmState> PwmDevice::read_state() const {
    PwmState s{};
    for (unsigned i = 0; i < 4; ++i) {
        auto d = transport_.read32(R::duty(i));
        if (!d) {
            return d.status();
        }
        s.outputs.duty[i] = d.value();
        auto dir = transport_.read32(R::direction(i));
        if (!dir) {
            return dir.status();
        }
        s.outputs.direction_mask |= (dir.value() & 1u) << i;
    }
    auto e = transport_.read32(R::Enable);
    if (!e) {
        return e.status();
    }
    s.outputs.enable_mask = e.value() & 0xF;
    auto u = transport_.read32(R::UpdateEnable);
    if (!u) {
        return u.status();
    }
    s.update_enabled = (u.value() & 1u) != 0;
    auto c = transport_.read32(R::Carrier);
    if (!c) {
        return c.status();
    }
    s.config.carrier_frequency_value = c.value();
    auto p = transport_.read32(R::Peak);
    if (!p) {
        return p.status();
    }
    s.config.peak_value = p.value();
    auto w = transport_.read32(R::Waveform);
    if (!w) {
        return w.status();
    }
    s.config.waveform = w.value() == 0xA002u ? PwmWaveform::Triangle : PwmWaveform::Sawtooth;
    auto direction_mode = transport_.read32(R::DirectionMode);
    if (!direction_mode) {
        return direction_mode.status();
    }
    auto decoded_direction_mode = decode_direction_mode(direction_mode.value());
    if (!decoded_direction_mode) {
        return decoded_direction_mode.status();
    }
    s.config.direction_mode = decoded_direction_mode.value();
    auto channel_mapping = transport_.read32(R::ChannelMapping);
    if (!channel_mapping) {
        return channel_mapping.status();
    }
    s.config.channel_mapping.encoded = static_cast<uint16_t>(channel_mapping.value());
    return s;
}
} // namespace MB_DDF::HW
