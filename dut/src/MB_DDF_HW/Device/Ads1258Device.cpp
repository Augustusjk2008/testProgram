#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/Registers/Ads1258Registers.h"

namespace MB_DDF::HW {
namespace R = Registers::Ads1258;
namespace {
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
    const auto v = values(c);
    for (unsigned i = 0; i < v.size(); ++i) {
        auto s = transport_.write32(R::config(i), v[i]);
        if (!s) {
            return s;
        }
    }
    return transport_.write32(R::DrdyReadDelay, c.drdy_read_delay);
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
    auto e = read_error_counters();
    if (!e) {
        return e.status();
    }
    s.errors = e.value();
    return s;
}
} // namespace MB_DDF::HW
