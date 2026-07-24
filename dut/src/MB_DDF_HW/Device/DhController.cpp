#include "MB_DDF_HW/Device/DhController.h"
#include "MB_DDF_HW/Device/Registers/DhRegisters.h"
#include <vector>

namespace MB_DDF::HW {
namespace R = Registers::Dh;
namespace {
Result<bool> decode_battery_activated(uint32_t value) {
    switch ((value >> 8u) & 0xFFu) {
    case 0xAAu:
        return true;
    case 0xFFu:
        return false;
    default:
        return Status::error(StatusCode::HardwareFault, 0,
                             "invalid DH battery activation readback");
    }
}
} // namespace
Result<void> DhController::check_communication() const {
    return check_device_communication(transport_);
}
Result<void> DhController::configure_timebase(const DhTimebase& t) {
    if (t.unit_milliseconds == 0) {
        return Status::error(StatusCode::InvalidArgument, 0, "timebase unit must be nonzero");
    }
    return transport_.write32(R::Timebase, t.counter_value);
}
Result<void> DhController::set_pulse_width_ticks(uint8_t c, uint16_t t) {
    if (c >= 48) {
        return Status::error(StatusCode::InvalidArgument, 0, "DH channel is out of range");
    }
    return transport_.write32(R::pulse(c), t);
}
Result<void> DhController::set_pulse_width_ms(uint8_t c, uint32_t ms, uint32_t unit) {
    if (unit == 0 || ms % unit != 0 || ms / unit > 0xFFFFu) {
        return Status::error(StatusCode::InvalidArgument, 0, "DH pulse width is not representable");
    }
    return set_pulse_width_ticks(c, static_cast<uint16_t>(ms / unit));
}
Result<void> DhController::set_fire_mode(DhFireMode m) {
    return transport_.write32(R::FireMode, m == DhFireMode::Multiple ? 0xAAAAu : 0xBBBBu);
}
Result<void> DhController::set_repeat_mode(DhRepeatMode m) {
    return transport_.write32(R::RepeatMode, m == DhRepeatMode::Once ? 0xAAAAu : 0xBBBBu);
}
Result<void> DhController::set_fire_enabled(bool enabled) {
    return transport_.write32(R::FireEnable, enabled ? 0xAAAAu : 0xFFFFu);
}
Result<void> DhController::set_return_enabled(bool enabled) {
    return transport_.write32(R::ReturnEnable, enabled ? 0xA000u : 0x00A0u);
}
Result<void> DhController::set_pulse_config_enabled(bool enabled) {
    return transport_.write32(R::PulseConfigEnable, enabled ? 0xAAAAu : 0xFFFFu);
}
Result<bool> DhController::read_fire_enabled() const {
    const auto value = transport_.read32(R::FireEnable);
    if (!value) {
        return value.status();
    }
    switch (value.value() & 0xFFFFu) {
    case 0xAAAAu:
        return true;
    case 0xFFFFu:
        return false;
    default:
        return Status::error(StatusCode::HardwareFault, 0,
                             "invalid DH fire enable readback");
    }
}
Result<bool> DhController::read_return_enabled() const {
    const auto value = transport_.read32(R::ReturnEnable);
    if (!value) {
        return value.status();
    }
    switch (value.value() & 0xFFu) {
    case 0xAAu:
        return true;
    case 0xFFu:
        return false;
    default:
        return Status::error(StatusCode::HardwareFault, 0,
                             "invalid DH return enable readback");
    }
}
Result<std::array<uint8_t, 23>> DhController::read_channel_statuses() const {
    std::array<uint8_t, 23> statuses{};
    for (unsigned channel = 0; channel < statuses.size(); ++channel) {
        const auto value = transport_.read32(R::feedback(channel));
        if (!value) {
            return value.status();
        }
        switch (value.value() & 0xFFFFu) {
        case 0xFFFFu:
            statuses[channel] = 0;
            break;
        case 0xAAAAu:
            statuses[channel] = 1;
            break;
        case 0xBBBBu:
            statuses[channel] = 2;
            break;
        default:
            return Status::error(StatusCode::HardwareFault, 0,
                                 "invalid DH channel status readback");
        }
    }
    return statuses;
}
Result<void> DhController::fire(uint8_t c) {
    if (c >= 48) {
        return Status::error(StatusCode::InvalidArgument, 0, "DH channel is out of range");
    }

    // 48 路命令按每 16 路使用 B/C/D 前缀编码。
    const uint16_t command = c < 16   ? static_cast<uint16_t>(0xB000u + c)
                             : c < 32 ? static_cast<uint16_t>(0xC000u + c - 16)
                                      : static_cast<uint16_t>(0xD000u + c - 32);
    return transport_.write32(R::fire(c), command);
}
Result<void> DhController::fire_multiple(std::span<const uint8_t> channels) {
    std::vector<uint8_t> unique;
    for (auto c : channels) {
        if (c >= 48) {
            return Status::error(StatusCode::InvalidArgument, 0, "DH channel is out of range");
        }
        bool seen = false;
        for (auto u : unique) {
            seen |= u == c;
        }
        if (!seen) {
            unique.push_back(c);
        }
    }

    // 每批编码四个槽位。origin_v3 允许通道重复；末批不足四路时重复
    // 最后一个有效通道，兼容实板对含 0xFF 尾批不响应的行为。
    for (size_t base = 0; base < unique.size(); base += 4) {
        uint32_t packed = 0;
        for (size_t i = 0; i < 4; ++i) {
            const size_t channel_index =
                base + i < unique.size() ? base + i : unique.size() - 1;
            packed |= static_cast<uint32_t>(unique[channel_index]) << (8 * i);
        }
        auto s = transport_.write32(R::MultiChannels, packed);
        if (!s) {
            return s;
        }
        s = transport_.write32(R::MultiTrigger, 0xAAAAu);
        if (!s) {
            return s;
        }
    }
    return {};
}
Result<uint64_t> DhController::read_feedback_mask() const {
    uint64_t mask = 0;
    for (unsigned i = 0; i < 48; ++i) {
        auto v = transport_.read32(R::feedback(i));
        if (!v) {
            return v.status();
        }
        if ((v.value() & 0xFFu) == 0xAAu) {
            mask |= uint64_t{1} << i;
        }
    }
    return mask;
}
Result<bool> DhController::read_battery_activated() const {
    const auto value = transport_.read32(R::BatteryStatus);
    if (!value) {
        return value.status();
    }
    return decode_battery_activated(value.value());
}
Result<DhFeedback> DhController::read_feedback() const {
    auto m = read_feedback_mask();
    if (!m) {
        return m.status();
    }
    const auto battery = transport_.read32(R::BatteryStatus);
    if (!battery) {
        return battery.status();
    }
    const auto activated = decode_battery_activated(battery.value());
    if (!activated) {
        return activated.status();
    }
    return DhFeedback{m.value(), (battery.value() & 0xFFu) == 0xAAu,
                      activated.value()};
}
Result<std::array<uint16_t, 48>> DhController::read_pulse_widths(unsigned count) const {
    if (count > 48) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "DH pulse width read count is out of range");
    }
    std::array<uint16_t, 48> values{};
    for (unsigned i = 0; i < count; ++i) {
        auto v = transport_.read32(R::pulse(i));
        if (!v) {
            return v.status();
        }
        values[i] = v.value();
    }
    return values;
}
} // namespace MB_DDF::HW
