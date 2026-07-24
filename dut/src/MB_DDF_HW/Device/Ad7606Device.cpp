#include "MB_DDF_HW/Device/Ad7606Device.h"
#include "MB_DDF_HW/Device/Registers/Ad7606Registers.h"

namespace MB_DDF::HW {
namespace R = Registers::Ad7606;
Result<void> Ad7606Device::check_communication() const {
    return check_device_communication(transport_);
}
Result<void> Ad7606Device::set_acquisition_enabled(bool v) {
    return transport_.write32(R::AcquisitionEnable, v ? 0xAAAAu : 0xFFFFu);
}
Result<void> Ad7606Device::set_filter_enabled(bool v) {
    return transport_.write32(R::FilterEnable, v ? 0xAAAAu : 0xFFFFu);
}
Result<void> Ad7606Device::reset() {
    auto s = set_acquisition_enabled(false);
    if (!s) {
        return s;
    }
    return set_acquisition_enabled(true);
}
Result<void> Ad7606Device::configure(const Ad7606Config& c) {
    auto s = set_acquisition_enabled(c.acquisition_enabled);
    if (!s) {
        return s;
    }
    s = set_filter_enabled(c.filter_enabled);
    if (!s) {
        return s;
    }
    const uint32_t values[] = {c.oversampling,           c.clock_period, c.conversion_low_cycles,
                               c.conversion_wait_cycles, c.reset_cycles, c.acquisition_count};
    const uint64_t offsets[] = {R::Oversampling,   R::ClockPeriod, R::ConversionLow,
                                R::ConversionWait, R::ResetCycles, R::AcquisitionCount};
    for (unsigned i = 0; i < 6; ++i) {
        s = transport_.write32(offsets[i], values[i]);
        if (!s) {
            return s;
        }
    }
    return {};
}
Result<Ad7606Snapshot> Ad7606Device::read_snapshot() const {
    Ad7606Snapshot s{};
    for (unsigned i = 0; i < 8; ++i) {
        auto v = transport_.read32(R::channel(i));
        if (!v) {
            return v.status();
        }
        s.raw[i] = static_cast<int16_t>(v.value() & 0xFFFFu);
    }
    return s;
}
Result<Ad7606State> Ad7606Device::read_state() const {
    Ad7606State s{};
    auto snap = read_snapshot();
    if (!snap) {
        return snap.status();
    }
    s.snapshot = snap.value();
    auto a = transport_.read32(R::AcquisitionEnable);
    if (!a) {
        return a.status();
    }
    s.config.acquisition_enabled = a.value() & 1;
    auto f = transport_.read32(R::FilterEnable);
    if (!f) {
        return f.status();
    }
    s.config.filter_enabled = f.value() & 1;
    auto o = transport_.read32(R::Oversampling);
    if (!o) {
        return o.status();
    }
    s.config.oversampling = o.value();
    auto c = transport_.read32(R::ClockPeriod);
    if (!c) {
        return c.status();
    }
    s.config.clock_period = c.value();
    auto l = transport_.read32(R::ConversionLow);
    if (!l) {
        return l.status();
    }
    s.config.conversion_low_cycles = l.value();
    auto w = transport_.read32(R::ConversionWait);
    if (!w) {
        return w.status();
    }
    s.config.conversion_wait_cycles = w.value();
    auto r = transport_.read32(R::ResetCycles);
    if (!r) {
        return r.status();
    }
    s.config.reset_cycles = r.value();
    auto n = transport_.read32(R::AcquisitionCount);
    if (!n) {
        return n.status();
    }
    s.config.acquisition_count = n.value();
    return s;
}
} // namespace MB_DDF::HW
