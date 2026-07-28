#include "MB_DDF_HW_Test/HelmDdsTestBridge.h"

#include "MB_DDF_HW_Test/ProductProtocol.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace MB_DDF::HWTest {
namespace {

HelmStreamParameters sweep_parameters() {
    HelmStreamParameters parameters{};
    parameters.waveform = 4;
    parameters.frequency_hz = 1.0;
    parameters.amplitude_deg = 2.0;
    parameters.offset_deg = 3.0;
    parameters.start_phase_radians = 0.0;
    parameters.maximum_frequency_hz = 4.0;
    parameters.sweep_duration_seconds = 10.0;
    parameters.enable_mask = 0x05;
    return parameters;
}

TEST(HelmDdsTestBridgeTest, GeneratesOneSharedWaveformAndZerosDisabledChannels) {
    const auto commands = helm_channel_commands(sweep_parameters(), 1.0);

    EXPECT_TRUE(std::isfinite(commands[0]));
    EXPECT_FLOAT_EQ(commands[0], commands[2]);
    EXPECT_FLOAT_EQ(commands[1], 0.0F);
    EXPECT_FLOAT_EQ(commands[3], 0.0F);
}

TEST(HelmDdsTestBridgeTest, SweepDurationIsConfigurableAndThenOutputsZero) {
    auto parameters = sweep_parameters();
    parameters.sweep_duration_seconds = 2.0;

    const auto at_end = helm_channel_commands(parameters, 2.0);
    const auto after_end = helm_channel_commands(parameters, 2.001);

    EXPECT_TRUE(std::isfinite(at_end[0]));
    EXPECT_EQ(after_end, (std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F}));
}

TEST(HelmDdsTestBridgeTest, EqualSweepFrequenciesReduceToFixedFrequencySine) {
    auto parameters = sweep_parameters();
    parameters.frequency_hz = 2.0;
    parameters.maximum_frequency_hz = 2.0;
    parameters.amplitude_deg = 1.0;
    parameters.offset_deg = 0.0;
    parameters.enable_mask = 1;

    const auto commands = helm_channel_commands(parameters, 0.125);

    EXPECT_NEAR(commands[0], 1.0F, 1.0e-6F);
}

TEST(HelmDdsTestBridgeTest, DdsPayloadCodecsPreserveExactCommandAndFeedbackFields) {
    ProtocolModel::Helm_ins_frame command{};
    command.serial_a = 0x1234;
    command.Q = 0.0F;
    command.temp_imu = 30.0;
    command.temp_ground = 30.0;
    command.ins[0] = 250.0F;
    command.ins[1] = -100.0F;
    command.ins[2] = 1.25F;
    command.ins[3] = 0.0F;
    const auto command_bytes = ProtocolModel::Helm_ins_frameProtocol::packFrame(command);
    static_assert(command_bytes.size() == 27);
    ProtocolModel::Helm_ins_frame decoded_command{};
    ASSERT_TRUE(ProtocolModel::Helm_ins_frameProtocol::unpackFrame(
        command_bytes.data(), command_bytes.size(), decoded_command));
    EXPECT_EQ(decoded_command.serial_a, 0x1234u);
    EXPECT_FLOAT_EQ(decoded_command.ins[0], 250.0F);
    EXPECT_FLOAT_EQ(decoded_command.ins[1], -100.0F);

    ProtocolModel::Helm_fdb_frame feedback{};
    feedback.serial_b = 0x4321;
    feedback.version = 0x4000;
    feedback.fdb[3] = -12.5F;
    feedback.bitGroup1.self_check = 3;
    feedback.bitGroup1.bit4 = 2;
    feedback.bitGroup1.bit = 1;
    feedback.timeout = 1;
    feedback.serial_a = command.serial_a;
    feedback.ins[0] = command.ins[0];
    const auto feedback_bytes = ProtocolModel::Helm_fdb_frameProtocol::packFrame(feedback);
    static_assert(feedback_bytes.size() == 41);
    ProtocolModel::Helm_fdb_frame decoded_feedback{};
    ASSERT_TRUE(ProtocolModel::Helm_fdb_frameProtocol::unpackFrame(
        feedback_bytes.data(), feedback_bytes.size(), decoded_feedback));
    EXPECT_EQ(decoded_feedback.serial_b, 0x4321u);
    EXPECT_FLOAT_EQ(decoded_feedback.fdb[3], -12.5F);
    EXPECT_EQ(decoded_feedback.bitGroup1.self_check, 3u);
    EXPECT_EQ(decoded_feedback.bitGroup1.bit4, 2u);
    EXPECT_EQ(decoded_feedback.timeout, 1u);
    EXPECT_EQ(decoded_feedback.serial_a, 0x1234u);
}

TEST(HelmDdsTestBridgeTest, PacksAtMostFiveCompleteFeedbackSamples) {
    ProductProtocol protocol;
    auto response = protocol.create_message("helm_feedback_response", false);
    std::array<HelmFeedbackSample, 5> samples{};
    constexpr uint64_t first_timestamp_us = 0x0000000200000010ULL;
    for (size_t index = 0; index < samples.size(); ++index) {
        samples[index].timestamp_us = first_timestamp_us + index * 1000;
        samples[index].frame.serial_b = static_cast<uint16_t>(100 + index);
        samples[index].frame.version = 0x4000;
        samples[index].frame.fdb[3] = static_cast<float>(index) + 0.5F;
        samples[index].frame.bitGroup1.self_check = 3;
        samples[index].frame.timeout = static_cast<uint8_t>(index & 1u);
        samples[index].frame.serial_a = static_cast<uint16_t>(90 + index);
        samples[index].frame.ins[0] = static_cast<float>(index) + 1.5F;
    }

    EXPECT_EQ(populate_helm_feedback_batch(samples, response), ProductErrorCode::Ok);
    EXPECT_EQ(response.get_unsigned("sample_count").value_or(0), 5u);
    EXPECT_EQ(response.get_unsigned("first_timestamp_us_low").value_or(0), 0x10u);
    EXPECT_EQ(response.get_unsigned("first_timestamp_us_high").value_or(0), 0x02u);
    EXPECT_EQ(response.get_unsigned("sample[4].delta_us").value_or(0), 4000u);
    EXPECT_EQ(response.get_unsigned("sample[4].serial_b").value_or(0), 104u);
    EXPECT_FLOAT_EQ(response.get_float("sample[4].fdb[3]").value_or(0.0F), 4.5F);
    EXPECT_EQ(response.get_unsigned("sample[4].self_check").value_or(0), 3u);
    EXPECT_EQ(response.get_unsigned("sample[4].serial_a").value_or(0), 94u);
    EXPECT_FLOAT_EQ(response.get_float("sample[4].ins[0]").value_or(0.0F), 5.5F);
}

} // namespace
} // namespace MB_DDF::HWTest
