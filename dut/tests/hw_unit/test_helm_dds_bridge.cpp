#include "MB_DDF_HW_Test/HelmDdsTestBridge.h"

#include "HelmControl/ProtocolModel/helm_command_contract.h"
#include "MB_DDF_HW_Test/ProductProtocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

HelmStreamParameters running_parameters() {
    HelmStreamParameters parameters{};
    parameters.waveform = 3;
    parameters.frequency_hz = 1.0;
    parameters.amplitude_deg = 0.0;
    parameters.offset_deg = 12.5;
    parameters.start_phase_radians = 0.0;
    parameters.maximum_frequency_hz = 1.0;
    parameters.sweep_duration_seconds = 10.0;
    parameters.enable_mask = 0x0F;
    return parameters;
}

class RecordingHelmDdsEndpoint final : public IHelmDdsEndpoint {
public:
    enum class CallKind {
        Open,
        Publish,
        Close,
    };

    struct Call {
        CallKind kind;
        std::vector<char> payload;
    };

    explicit RecordingHelmDdsEndpoint(size_t fail_publish_number = 0)
        : fail_publish_number_(fail_publish_number) {}

    bool open(FeedbackCallback callback, std::string* error) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback_ = std::move(callback);
            calls_.push_back({CallKind::Open, {}});
        }
        if (error != nullptr) error->clear();
        calls_ready_.notify_all();
        return true;
    }

    bool publish_command(std::span<const char> bytes,
                         std::string* error) override {
        bool published = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calls_.push_back(
                {CallKind::Publish, std::vector<char>(bytes.begin(), bytes.end())});
            ++publish_count_;
            published = publish_count_ != fail_publish_number_;
        }
        if (error != nullptr) {
            *error = published ? "" : "intentional publish failure";
        }
        calls_ready_.notify_all();
        return published;
    }

    void close() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calls_.push_back({CallKind::Close, {}});
        }
        calls_ready_.notify_all();
    }

    bool wait_for_publish_count(size_t expected_count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return calls_ready_.wait_for(lock, std::chrono::seconds(1), [&] {
            return publish_count_ >= expected_count;
        });
    }

    std::vector<Call> calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    std::vector<Call> published_calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Call> result;
        for (const auto& call : calls_) {
            if (call.kind == CallKind::Publish) result.push_back(call);
        }
        return result;
    }

private:
    const size_t fail_publish_number_;
    mutable std::mutex mutex_;
    std::condition_variable calls_ready_;
    FeedbackCallback callback_{};
    size_t publish_count_{0};
    std::vector<Call> calls_;
};

ProtocolModel::Helm_ins_frame unpack_command(
    const RecordingHelmDdsEndpoint::Call& call) {
    EXPECT_EQ(call.kind, RecordingHelmDdsEndpoint::CallKind::Publish);
    EXPECT_EQ(call.payload.size(), 27u);
    ProtocolModel::Helm_ins_frame frame{};
    if (call.payload.size() == 27u) {
        EXPECT_TRUE(ProtocolModel::Helm_ins_frameProtocol::unpackFrame(
            call.payload.data(), call.payload.size(), frame));
    }
    return frame;
}

bool all_commands_are_zero(const ProtocolModel::Helm_ins_frame& frame) {
    return std::all_of(std::begin(frame.ins), std::end(frame.ins),
                       [](float command) { return command == 0.0F; });
}

void expect_unlock_byte(const RecordingHelmDdsEndpoint::Call& call) {
    ASSERT_EQ(call.payload.size(), 27u);
    EXPECT_EQ(static_cast<uint8_t>(call.payload[26]), 0xFFu);
}

TEST(HelmDdsTestBridgeTest, GeneratesOneSharedWaveformAndZerosDisabledChannels) {
    const auto commands = helm_channel_commands(sweep_parameters(), 1.0);

    EXPECT_TRUE(std::isfinite(commands[0]));
    EXPECT_FLOAT_EQ(commands[0], commands[2]);
    EXPECT_FLOAT_EQ(commands[1], 0.0F);
    EXPECT_FLOAT_EQ(commands[3], 0.0F);
}

TEST(HelmDdsTestBridgeTest, StartPublishesNeutralUnlockedFirstFrame) {
    auto endpoint = std::make_unique<RecordingHelmDdsEndpoint>();
    auto* recording_endpoint = endpoint.get();
    HelmDdsTestBridge bridge(std::move(endpoint));

    ASSERT_EQ(bridge.start(running_parameters()), ProductErrorCode::Ok);
    ASSERT_TRUE(recording_endpoint->wait_for_publish_count(1));

    const auto calls = recording_endpoint->calls();
    ASSERT_GE(calls.size(), 2u);
    EXPECT_EQ(calls.front().kind, RecordingHelmDdsEndpoint::CallKind::Open);
    EXPECT_EQ(calls[1].kind, RecordingHelmDdsEndpoint::CallKind::Publish);
    const auto published = recording_endpoint->published_calls();
    ASSERT_FALSE(published.empty());
    const auto first_frame = unpack_command(published.front());
    EXPECT_TRUE(all_commands_are_zero(first_frame));
    expect_unlock_byte(published.front());

    EXPECT_EQ(bridge.stop(), ProductErrorCode::Ok);
}

TEST(HelmDdsTestBridgeTest, RunningFramesKeepUnlockByte) {
    auto endpoint = std::make_unique<RecordingHelmDdsEndpoint>();
    auto* recording_endpoint = endpoint.get();
    HelmDdsTestBridge bridge(std::move(endpoint));
    const auto parameters = running_parameters();

    ASSERT_EQ(bridge.start(parameters), ProductErrorCode::Ok);
    ASSERT_TRUE(recording_endpoint->wait_for_publish_count(2));
    ASSERT_EQ(bridge.stop(), ProductErrorCode::Ok);

    const auto published = recording_endpoint->published_calls();
    ASSERT_GE(published.size(), 2u);
    const auto running_frame = unpack_command(published[1]);
    for (const float command : running_frame.ins) {
        EXPECT_FLOAT_EQ(command, static_cast<float>(parameters.offset_deg));
    }
    for (const auto& call : published) {
        expect_unlock_byte(call);
    }
}

TEST(HelmDdsTestBridgeTest, StopPublishesNeutralUnlockedFrameBeforeClose) {
    auto endpoint = std::make_unique<RecordingHelmDdsEndpoint>();
    auto* recording_endpoint = endpoint.get();
    HelmDdsTestBridge bridge(std::move(endpoint));

    ASSERT_EQ(bridge.start(running_parameters()), ProductErrorCode::Ok);
    ASSERT_TRUE(recording_endpoint->wait_for_publish_count(2));
    ASSERT_EQ(bridge.stop(), ProductErrorCode::Ok);

    const auto calls = recording_endpoint->calls();
    ASSERT_GE(calls.size(), 2u);
    ASSERT_EQ(calls.back().kind, RecordingHelmDdsEndpoint::CallKind::Close);
    ASSERT_EQ(calls[calls.size() - 2u].kind,
              RecordingHelmDdsEndpoint::CallKind::Publish);
    const auto terminal_frame = unpack_command(calls[calls.size() - 2u]);
    EXPECT_TRUE(all_commands_are_zero(terminal_frame));
    expect_unlock_byte(calls[calls.size() - 2u]);
    EXPECT_FALSE(bridge.active());
}

TEST(HelmDdsTestBridgeTest,
     FirstPublishFailureMakesStartFailAndLeavesBridgeInactive) {
    auto endpoint = std::make_unique<RecordingHelmDdsEndpoint>(1u);
    auto* recording_endpoint = endpoint.get();
    HelmDdsTestBridge bridge(std::move(endpoint));

    EXPECT_EQ(bridge.start(running_parameters()), ProductErrorCode::HelmDdsFailed);
    EXPECT_FALSE(bridge.active());
    ASSERT_TRUE(recording_endpoint->wait_for_publish_count(1));

    const auto published = recording_endpoint->published_calls();
    ASSERT_FALSE(published.empty());
    EXPECT_EQ(published.front().payload.size(), 27u);
}

TEST(HelmDdsTestBridgeTest,
     RuntimePublishFailureRemainsObservableUntilExplicitStop) {
    auto endpoint = std::make_unique<RecordingHelmDdsEndpoint>(2u);
    auto* recording_endpoint = endpoint.get();
    HelmDdsTestBridge bridge(std::move(endpoint));

    ASSERT_EQ(bridge.start(running_parameters()), ProductErrorCode::Ok);
    ASSERT_TRUE(recording_endpoint->wait_for_publish_count(2));

    ProductMessage response;
    const auto result = bridge.poll_feedback(response);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, ProductErrorCode::HelmDdsFailed);
    EXPECT_TRUE(bridge.active());
    EXPECT_EQ(bridge.stop(), ProductErrorCode::Ok);
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
    command.helm_unlock = ProtocolModel::kHelmUnlockRequested;
    const auto command_bytes = ProtocolModel::Helm_ins_frameProtocol::packFrame(command);
    static_assert(command_bytes.size() == 27);
    EXPECT_EQ(static_cast<uint8_t>(command_bytes[ProtocolModel::kHelmUnlockByteOffset]),
              ProtocolModel::kHelmUnlockRequested);
    ProtocolModel::Helm_ins_frame decoded_command{};
    ASSERT_TRUE(ProtocolModel::Helm_ins_frameProtocol::unpackFrame(
        command_bytes.data(), command_bytes.size(), decoded_command));
    EXPECT_EQ(decoded_command.serial_a, 0x1234u);
    EXPECT_FLOAT_EQ(decoded_command.ins[0], 250.0F);
    EXPECT_FLOAT_EQ(decoded_command.ins[1], -100.0F);
    EXPECT_EQ(decoded_command.helm_unlock, ProtocolModel::kHelmUnlockRequested);

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
