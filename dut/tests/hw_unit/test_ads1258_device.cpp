#include <gtest/gtest.h>
#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/Registers/Ads1258Registers.h"
#include "hw_unit/support/RecordingTransport.h"
using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

static_assert(Registers::Ads1258::DrdyReadDelay == 0x68u);
static_assert(Registers::Ads1258::Config0 == 0x0Cu);
static_assert(Registers::Ads1258::WorkCount == 0x30u);
static_assert(Registers::Ads1258::CGroupVoltage == 0x80u);
static_assert(Registers::Ads1258::BGroupVoltage == 0x88u);
static_assert(Registers::Ads1258::Primary28V5 == 0x8Cu);
static_assert(Registers::Ads1258::Enable1 == 0x5Cu);
static_assert(Registers::Ads1258::Enable2 == 0x60u);
static_assert(Registers::Ads1258::StateRollback == 0x64u);
static_assert(Registers::Ads1258::Chip1DiagnosticsBase == 0x100u);
static_assert(Registers::Ads1258::Chip2DiagnosticsBase == 0x114u);
static_assert(Registers::Ads1258::ErrorBase == 0x128u);
static_assert(Ads1258Device::adc_input_voltage(0x00FFFFFFu) < 0.0);

TEST(HwAds1258Device, ReadsChannelsAndErrors) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    for (unsigned i = 0; i < 32; ++i) {
        t.preset(0x80 + i * 4, i);
    }
    for (unsigned chip = 0; chip < 2; ++chip) {
        for (unsigned item = 0; item < 5; ++item) {
            t.preset(Registers::Ads1258::diagnostic(chip, item),
                     10u * chip + item + 1u);
        }
    }
    for (unsigned i = 0; i < 9; ++i) {
        t.preset(0x128 + i * 4, 100 + i);
    }
    Ads1258Device d(t);
    auto r = d.read_snapshot();
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().raw[31], 31u);
    EXPECT_EQ(r.value().diagnostics[0].offset, 1u);
    EXPECT_EQ(r.value().diagnostics[0].vref, 5u);
    EXPECT_EQ(r.value().diagnostics[1].offset, 11u);
    EXPECT_EQ(r.value().diagnostics[1].vref, 15u);
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

TEST(HwAds1258Device, ConfiguresAndReadsV4DrdyReadDelay) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(Registers::Ads1258::Enable1, 0xAAAAu);
    t.preset(Registers::Ads1258::Enable2, 0xFFFFu);
    Ads1258Device d(t);
    Ads1258Config config{};
    config.drdy_read_delay = 1250;

    ASSERT_TRUE(d.configure(config));
    const auto& accesses = t.accesses();
    ASSERT_EQ(accesses.size(), 29u);
    EXPECT_FALSE(accesses[0].write);
    EXPECT_EQ(accesses[0].offset, Registers::Ads1258::Enable1);
    EXPECT_FALSE(accesses[1].write);
    EXPECT_EQ(accesses[1].offset, Registers::Ads1258::Enable2);
    EXPECT_EQ(accesses[2].offset, Registers::Ads1258::Enable1);
    EXPECT_EQ(accesses[2].value, 0xFFFFu);
    EXPECT_EQ(accesses[3].offset, Registers::Ads1258::StateRollback);
    EXPECT_EQ(accesses[3].value, 0xAAAAu);
    EXPECT_EQ(accesses[25].offset, Registers::Ads1258::DrdyReadDelay);
    EXPECT_EQ(accesses[25].value, 1250u);
    EXPECT_EQ(accesses[26].offset, Registers::Ads1258::StateRollback);
    EXPECT_EQ(accesses[26].value, 0xFFFFu);
    EXPECT_EQ(accesses[27].offset, Registers::Ads1258::Enable1);
    EXPECT_EQ(accesses[27].value, 0xAAAAu);
    EXPECT_EQ(accesses[28].offset, Registers::Ads1258::Enable2);
    EXPECT_EQ(accesses[28].value, 0xFFFFu);

    t.clear_accesses();
    const auto readback = d.read_config();
    ASSERT_TRUE(readback);
    EXPECT_EQ(readback.value().drdy_read_delay, 1250u);
    EXPECT_EQ(t.accesses().back().offset, 0x68u);
}

TEST(HwAds1258Device, AppliesRuntimeOverridesWithRequiredRollbackSequence) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    Ads1258Device d(t);
    Ads1258RuntimeOverrides overrides{};

    ASSERT_TRUE(d.apply_runtime_overrides(overrides));

    const auto& accesses = t.accesses();
    ASSERT_EQ(accesses.size(), 12u);
    const uint64_t expected_offsets[]{
        Registers::Ads1258::Enable1,
        Registers::Ads1258::StateRollback,
        Registers::Ads1258::Config0,
        Registers::Ads1258::Config1,
        Registers::Ads1258::Sysred,
        Registers::Ads1258::WorkCount,
        Registers::Ads1258::SpiDivider,
        Registers::Ads1258::ActivationThresholdC,
        Registers::Ads1258::ActivationThresholdB,
        Registers::Ads1258::StateRollback,
        Registers::Ads1258::Enable1,
        Registers::Ads1258::Enable2,
    };
    const uint32_t expected_values[]{
        0xFFFFu, 0xAAAAu, 0x02u, 0x82u, 0x3Du, 0xAAu, 0x20u,
        0x21EC35u, 0x21EC35u, 0xFFFFu, 0xAAAAu, 0xAAAAu,
    };
    for (size_t index = 0; index < accesses.size(); ++index) {
        EXPECT_TRUE(accesses[index].write) << index;
        EXPECT_EQ(accesses[index].offset, expected_offsets[index]) << index;
        EXPECT_EQ(accesses[index].value, expected_values[index]) << index;
    }
}

TEST(HwAds1258Device, LeavesBothConvertersDisabledWhenRuntimeOverrideFails) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.fail_next_write_at(Registers::Ads1258::WorkCount);
    Ads1258Device d(t);

    const auto result = d.apply_runtime_overrides({});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::IoError);
    const auto& accesses = t.accesses();
    ASSERT_GE(accesses.size(), 3u);
    const auto cleanup = accesses.end() - 3;
    EXPECT_EQ(cleanup[0].offset, Registers::Ads1258::StateRollback);
    EXPECT_EQ(cleanup[0].value, 0xFFFFu);
    EXPECT_EQ(cleanup[1].offset, Registers::Ads1258::Enable1);
    EXPECT_EQ(cleanup[1].value, 0xFFFFu);
    EXPECT_EQ(cleanup[2].offset, Registers::Ads1258::Enable2);
    EXPECT_EQ(cleanup[2].value, 0xFFFFu);
}

TEST(HwAds1258Device, LeavesBothConvertersDisabledWhenFullConfigureFails) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(Registers::Ads1258::Enable1, 0xAAAAu);
    t.preset(Registers::Ads1258::Enable2, 0xAAAAu);
    t.fail_next_write_at(Registers::Ads1258::Config0);
    Ads1258Device d(t);

    const auto result = d.configure({});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::IoError);
    const auto& accesses = t.accesses();
    ASSERT_GE(accesses.size(), 3u);
    const auto cleanup = accesses.end() - 3;
    EXPECT_EQ(cleanup[0].offset, Registers::Ads1258::StateRollback);
    EXPECT_EQ(cleanup[0].value, 0xFFFFu);
    EXPECT_EQ(cleanup[1].offset, Registers::Ads1258::Enable1);
    EXPECT_EQ(cleanup[1].value, 0xFFFFu);
    EXPECT_EQ(cleanup[2].offset, Registers::Ads1258::Enable2);
    EXPECT_EQ(cleanup[2].value, 0xFFFFu);
}

TEST(HwAds1258Device, DecodesAllFiveDiagnosticsAndTemperatureModes) {
    const Ads1258ChipDiagnostics diagnostics{
        0x180000u, 0x3C0000u, 0x050000u, 0x3C0000u, 0x3C0000u};

    const auto free_air = Ads1258Device::decode_diagnostics(
        diagnostics, Ads1258TemperatureMode::FreeAir);
    const auto controlled_board = Ads1258Device::decode_diagnostics(
        diagnostics, Ads1258TemperatureMode::ControlledBoard);

    ASSERT_TRUE(free_air);
    ASSERT_TRUE(controlled_board);
    EXPECT_NEAR(free_air.value().offset_voltage, 1.0, 1.0e-12);
    EXPECT_NEAR(free_air.value().supply_voltage, 5.0, 1.0e-12);
    EXPECT_NEAR(free_air.value().gain, 0.5, 1.0e-12);
    EXPECT_NEAR(free_air.value().reference_voltage, 5.0, 1.0e-12);
    EXPECT_NEAR(free_air.value().temperature_celsius, 127.368866328257, 1.0e-9);
    EXPECT_NEAR(controlled_board.value().temperature_celsius,
                96.6400236826525, 1.0e-9);
}

TEST(HwAds1258Device, CalibratesSpecialChannelsWithDiagnosticsAndExternalBias) {
    Ads1258Snapshot snapshot{};
    snapshot.diagnostics[0] = Ads1258ChipDiagnostics{
        0x180000u, 0x3C0000u, 0x040831u, 0x3C0000u, 0x3C0000u};
    snapshot.diagnostics[1] = Ads1258ChipDiagnostics{
        0u, 0x3C0000u, 0x040831u, 0x780000u, 0x3C0000u};
    snapshot.raw[1] = 0x300000u;
    snapshot.raw[2] = 0x300000u;
    snapshot.raw[3] = 0x300000u;

    const auto channel1 =
        Ads1258Device::calibrated_channel_voltage(1, snapshot);
    const auto channel2 =
        Ads1258Device::calibrated_channel_voltage(2, snapshot);
    const auto channel3 =
        Ads1258Device::calibrated_channel_voltage(3, snapshot);
    ASSERT_TRUE(channel1);
    ASSERT_TRUE(channel2);
    ASSERT_TRUE(channel3);
    EXPECT_NEAR(channel1.value(), 28.77, 1.0e-9);
    EXPECT_NEAR(channel2.value(), 32.1265, 1.0e-9);
    EXPECT_NEAR(channel3.value(), 28.77, 1.0e-9);
}

TEST(HwAds1258Device, UsesEachChipsDiagnosticsForGeneralChannels) {
    Ads1258Snapshot snapshot{};
    snapshot.diagnostics[0] = Ads1258ChipDiagnostics{
        0u, 0x3C0000u, 0x040831u, 0x780000u, 0x3C0000u};
    snapshot.diagnostics[1] = Ads1258ChipDiagnostics{
        0u, 0x3C0000u, 0x040831u, 0x3C0000u, 0x3C0000u};
    snapshot.raw[4] = 0x180000u;
    snapshot.raw[20] = 0x180000u;

    const auto chip1 = Ads1258Device::calibrated_channel_voltage(4, snapshot);
    const auto chip2 = Ads1258Device::calibrated_channel_voltage(20, snapshot);
    ASSERT_TRUE(chip1);
    ASSERT_TRUE(chip2);
    EXPECT_NEAR(chip1.value(), 15.7836, 1.0e-9);
    EXPECT_NEAR(chip2.value(), 32.2968, 1.0e-9);
}

TEST(HwAds1258Device, RejectsInvalidDiagnosticQuality) {
    Ads1258Snapshot snapshot{};
    snapshot.diagnostics[0] = Ads1258ChipDiagnostics{
        0u, 0u, 0x040831u, 0x780000u, 0x3C0000u};
    snapshot.raw[0] = 0x180000u;

    const auto result =
        Ads1258Device::calibrated_channel_voltage(0, snapshot);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::HardwareFault);

    snapshot.diagnostics[0] = Ads1258ChipDiagnostics{
        0u, 0x3C0000u, 0u, 0x780000u, 0x3C0000u};
    const auto zero_temperature =
        Ads1258Device::calibrated_channel_voltage(0, snapshot);
    ASSERT_FALSE(zero_temperature);
    EXPECT_EQ(zero_temperature.status().code, StatusCode::HardwareFault);

    snapshot.diagnostics[0] = Ads1258ChipDiagnostics{
        0u, 0x3C0000u, 0x01040831u, 0x780000u, 0x3C0000u};
    const auto invalid_temperature =
        Ads1258Device::calibrated_channel_voltage(0, snapshot);
    ASSERT_FALSE(invalid_temperature);
    EXPECT_EQ(invalid_temperature.status().code, StatusCode::HardwareFault);
}
