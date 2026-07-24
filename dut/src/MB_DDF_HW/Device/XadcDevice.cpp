#include "MB_DDF_HW/Device/XadcDevice.h"

#include "MB_DDF_HW/Device/Registers/XadcRegisters.h"

namespace MB_DDF::HW {

Result<XadcVoltageSample> XadcDevice::read_value_yx() const {
    const auto data = transport_.read32(Registers::Xadc::ValueYx);
    if (!data) {
        return data.status();
    }
    return XadcVoltageSample{adc_code(data.value()),
                             value_yx_voltage(data.value())};
}

Result<double> XadcDevice::read_temperature_celsius() const {
    const auto data = transport_.read32(Registers::Xadc::Temperature);
    if (!data) {
        return data.status();
    }
    return temperature_celsius(data.value());
}

Result<XadcElectricalHealthSample> XadcDevice::read_electrical_health() const {
    const auto external = transport_.read32(Registers::Xadc::External3V3);
    if (!external) {
        return external.status();
    }
    const auto core = transport_.read32(Registers::Xadc::VccInt);
    if (!core) {
        return core.status();
    }
    const auto assist = transport_.read32(Registers::Xadc::VccAux);
    if (!assist) {
        return assist.status();
    }
    const auto js5v = transport_.read32(Registers::Xadc::Js5V);
    if (!js5v) {
        return js5v.status();
    }
    const auto dyt5v = transport_.read32(Registers::Xadc::Dyt5V);
    if (!dyt5v) {
        return dyt5v.status();
    }
    const auto power24v = transport_.read32(Registers::Xadc::Power24V);
    if (!power24v) {
        return power24v.status();
    }
    return XadcElectricalHealthSample{
        unipolar_voltage(external.value(), External3V3Gain),
        unipolar_voltage(core.value(), InternalSupplyGain),
        unipolar_voltage(assist.value(), InternalSupplyGain),
        unipolar_voltage(js5v.value(), Js5VGain),
        unipolar_voltage(dyt5v.value(), Dyt5VGain),
        unipolar_voltage(power24v.value(), Power24VGain)};
}

} // namespace MB_DDF::HW
