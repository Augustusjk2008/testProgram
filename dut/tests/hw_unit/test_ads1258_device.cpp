#include <gtest/gtest.h>
#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/Registers/Ads1258Registers.h"
#include "hw_unit/support/RecordingTransport.h"
using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

static_assert(Registers::Ads1258::DrdyReadDelay == 0x68u);
static_assert(Registers::Ads1258::CGroupVoltage == 0x80u);
static_assert(Registers::Ads1258::BGroupVoltage == 0x88u);
static_assert(Registers::Ads1258::Primary28V5 == 0x8Cu);
static_assert(Ads1258Device::adc_input_voltage(0x00FFFFFFu) < 0.0);

TEST(HwAds1258Device, ReadsChannelsAndErrors) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    for (unsigned i = 0; i < 32; ++i) {
        t.preset(0x80 + i * 4, i);
    }
    for (unsigned i = 0; i < 9; ++i) {
        t.preset(0x100 + i * 4, 100 + i);
    }
    Ads1258Device d(t);
    auto r = d.read_snapshot();
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().raw[31], 31u);
    EXPECT_EQ(r.value().errors.chip2_channel_fault, 108u);
}
TEST(HwAds1258Device, ClearsErrorsWithFF) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    Ads1258Device d(t);
    ASSERT_TRUE(d.clear_error_counters());
    EXPECT_EQ(t.accesses().back().value, 0xFFu);
}

TEST(HwAds1258Device, ConvertsChannelsOneThroughThreeWithLinearGain) {
    EXPECT_DOUBLE_EQ(Ads1258Device::adc_input_voltage(0xAB780000u), 4.096);
    EXPECT_DOUBLE_EQ(Ads1258Device::channel_voltage(0, 0x00780000u),
                     4.096 * 16.23);
    EXPECT_DOUBLE_EQ(Ads1258Device::channel_voltage(1, 0xAB780000u),
                     Ads1258Device::channel_voltage(1, 0x00780000u));
    EXPECT_DOUBLE_EQ(Ads1258Device::channel_voltage(2, 0x00780000u),
                     4.096 * 18.6);
    EXPECT_DOUBLE_EQ(Ads1258Device::channel_voltage(3, 0x00780000u),
                     4.096 * 18.6);
}

TEST(HwAds1258Device, UsesNonlinearGainThroughThreeVolts) {
    constexpr uint32_t code_for_three_volts = 0x57E400u;
    const double a = Ads1258Device::adc_input_voltage(code_for_three_volts);
    const double gain = -0.1594 * a * a + 0.843 * a + 15.1;

    EXPECT_NEAR(a, 3.0, 1.0e-12);
    EXPECT_NEAR(Ads1258Device::channel_voltage(4, code_for_three_volts),
                a * gain, 1.0e-12);
}

TEST(HwAds1258Device, UsesLinearHighRangeGainAboveThreeVolts) {
    constexpr uint32_t code_above_three_volts = 0x57E401u;
    const double a = Ads1258Device::adc_input_voltage(code_above_three_volts);

    ASSERT_GT(a, 3.0);
    EXPECT_NEAR(Ads1258Device::channel_voltage(31, code_above_three_volts),
                a * 16.23, 1.0e-12);

    constexpr uint32_t maximum_positive_code = 0x007FFFFFu;
    const double maximum_a =
        Ads1258Device::adc_input_voltage(maximum_positive_code);
    EXPECT_NEAR(Ads1258Device::channel_voltage(0, maximum_positive_code),
                maximum_a * 16.23, 1.0e-12);
    EXPECT_NEAR(Ads1258Device::channel_voltage(3, maximum_positive_code),
                maximum_a * 18.6, 1.0e-12);
}

TEST(HwAds1258Device, SignExtendsNegativeTwentyFourBitSamples) {
    constexpr double one_code =
        Ads1258Device::ReferenceVoltage /
        static_cast<double>(Ads1258Device::PositiveFullScaleCode);

    EXPECT_NEAR(Ads1258Device::adc_input_voltage(0x00FFFFFFu),
                -one_code, 1.0e-15);
    EXPECT_DOUBLE_EQ(Ads1258Device::adc_input_voltage(0xAB880000u),
                     -Ads1258Device::ReferenceVoltage);
    EXPECT_LT(Ads1258Device::adc_input_voltage(0x00800000u),
              -Ads1258Device::ReferenceVoltage);
    EXPECT_LT(Ads1258Device::channel_voltage(2, 0x00FFFFFFu), 0.0);
    EXPECT_LT(Ads1258Device::channel_voltage(0, 0x00FFFFFFu), 0.0);
}

TEST(HwAds1258Device, ConfiguresAndReadsV3DrdyReadDelay) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    Ads1258Device d(t);
    Ads1258Config config{};
    config.drdy_read_delay = 1250;

    ASSERT_TRUE(d.configure(config));
    EXPECT_EQ(t.accesses().back().offset, 0x68u);
    EXPECT_EQ(t.accesses().back().value, 1250u);

    t.clear_accesses();
    const auto readback = d.read_config();
    ASSERT_TRUE(readback);
    EXPECT_EQ(readback.value().drdy_read_delay, 1250u);
    EXPECT_EQ(t.accesses().back().offset, 0x68u);
}
