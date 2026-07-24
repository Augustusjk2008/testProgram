#include <gtest/gtest.h>

#include "MB_DDF_HW/Device/Registers/XadcRegisters.h"
#include "MB_DDF_HW/Device/XadcDevice.h"
#include "hw_unit/support/RecordingTransport.h"

using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

namespace {

static_assert(Registers::Xadc::UserBase == 0x150000u);
static_assert(Registers::Xadc::Temperature == 0x200u);
static_assert(Registers::Xadc::Js5V == 0x240u);
static_assert(Registers::Xadc::ValueYx == 0x260u);
static_assert(XadcDevice::Js5VGain == 10.09);

} // namespace

TEST(HwXadcDevice, ReadsValueYxFromVauxEight) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.preset(Registers::Xadc::ValueYx, 0xCAFE800Fu);
    XadcDevice device(transport);

    const auto result = device.read_value_yx();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().adc_code, 0x800u);
    EXPECT_DOUBLE_EQ(result.value().voltage, 5.045);
    ASSERT_EQ(transport.accesses().size(), 1u);
    EXPECT_FALSE(transport.accesses()[0].write);
    EXPECT_EQ(transport.accesses()[0].offset, 0x260u);
    EXPECT_EQ(transport.accesses()[0].width, 32u);
}

TEST(HwXadcDevice, UsesOnlyDataBitsFifteenThroughFour) {
    EXPECT_EQ(XadcDevice::adc_code(0xCAFE800Fu), 0x800u);
    EXPECT_EQ(XadcDevice::adc_code(0x00000010u), 1u);
    EXPECT_EQ(XadcDevice::adc_code(0x0000FFFFu), 0x0FFFu);
    EXPECT_DOUBLE_EQ(XadcDevice::value_yx_voltage(0x00000010u),
                     10.09 / 4096.0);
    EXPECT_DOUBLE_EQ(XadcDevice::value_yx_voltage(0x0000FFF0u),
                     4095.0 * 10.09 / 4096.0);
    EXPECT_DOUBLE_EQ(XadcDevice::js_5v_voltage(0xCAFE800Fu), 5.045);
    EXPECT_NEAR(XadcDevice::temperature_celsius(0xCAFE800Fu),
                2048.0 * 503.975 / 4096.0 - 273.15, 1.0e-12);
}

TEST(HwXadcDevice, ReadsK7TemperatureFromTemperatureRegister) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.preset(Registers::Xadc::Temperature, 0xCAFE800Fu);
    XadcDevice device(transport);

    const auto result = device.read_temperature_celsius();

    ASSERT_TRUE(result);
    EXPECT_NEAR(result.value(), 2048.0 * 503.975 / 4096.0 - 273.15,
                1.0e-12);
    ASSERT_EQ(transport.accesses().size(), 1u);
    EXPECT_EQ(transport.accesses()[0].offset, 0x200u);
}

TEST(HwXadcDevice, PropagatesTransportReadFailure) {
    RecordingTransport transport;
    XadcDevice device(transport);

    const auto result = device.read_value_yx();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::NotOpen);
    EXPECT_TRUE(transport.accesses().empty());
}
