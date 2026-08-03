#include <gtest/gtest.h>

#include "MB_DDF/Tools/SelfDescribingLogReader.h"
#include "MB_DDF_DEBUG/DytDebug.h"
#include "MB_DDF_DEBUG/frame_a.h"
#include "MB_DDF_DEBUG/frame_b.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace Dyt = MB_DDF::DytDebug;
namespace HW = MB_DDF::HW;
namespace Tools = MB_DDF::Tools;

class ScriptedEndpoint final : public HW::IByteEndpoint {
public:
    HW::Result<size_t> send(HW::BufferView data) override {
        sent.emplace_back(data.data, data.data + data.size);
        return data.size;
    }

    HW::Result<size_t> receive(HW::MutableBufferView buffer, HW::Timeout) override {
        ++receive_calls;
        if (!receive_failures.empty()) {
            auto status = std::move(receive_failures.front());
            receive_failures.pop_front();
            return status;
        }
        if (incoming.empty()) {
            return HW::Status::error(HW::StatusCode::IoError, 0,
                                     "scripted receive queue is empty");
        }
        auto bytes = std::move(incoming.front());
        incoming.pop_front();
        if (bytes.size() > buffer.size) {
            return HW::Status::error(HW::StatusCode::BufferTooSmall, 0,
                                     "scripted receive buffer is too small");
        }
        std::copy(bytes.begin(), bytes.end(), buffer.data);
        return bytes.size();
    }

    size_t mtu() const override {
        return 65536;
    }

    std::deque<std::vector<uint8_t>> incoming;
    std::deque<HW::Status> receive_failures;
    std::vector<std::vector<uint8_t>> sent;
    size_t receive_calls{0};
};

std::vector<uint8_t> packed_b_frame(uint16_t counter) {
    ProtocolModel::Dyt_to_main_frame_b frame;
    frame.frameCounter = counter;
    frame.spinFrequency = static_cast<double>(counter);
    const auto packed = ProtocolModel::Dyt_to_main_frame_bProtocol::packFrame(frame);
    return {reinterpret_cast<const uint8_t*>(packed.data()),
            reinterpret_cast<const uint8_t*>(packed.data()) + packed.size()};
}

std::filesystem::path unique_log_path() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("mb_ddf_dyt_debug_" + std::to_string(suffix) + ".sdlog");
}

void expect_same_except_baud(const HW::ComConfig& actual,
                             const HW::ComConfig& defaults) {
    EXPECT_EQ(actual.format.byte_format, defaults.format.byte_format);
    EXPECT_EQ(actual.format.receive_control, defaults.format.receive_control);
    EXPECT_EQ(actual.frame.send_length_bytes, defaults.frame.send_length_bytes);
    EXPECT_EQ(actual.frame.receive_length_bytes, defaults.frame.receive_length_bytes);
    EXPECT_EQ(actual.frame.send_header, defaults.frame.send_header);
    EXPECT_EQ(actual.frame.send_tail, defaults.frame.send_tail);
    EXPECT_EQ(actual.frame.receive_header, defaults.frame.receive_header);
    EXPECT_EQ(actual.frame.receive_tail, defaults.frame.receive_tail);
    EXPECT_EQ(actual.frame.send_header_length, defaults.frame.send_header_length);
    EXPECT_EQ(actual.frame.send_tail_length, defaults.frame.send_tail_length);
    EXPECT_EQ(actual.frame.receive_header_length, defaults.frame.receive_header_length);
    EXPECT_EQ(actual.frame.receive_tail_length, defaults.frame.receive_tail_length);
    EXPECT_EQ(actual.receive_enabled, defaults.receive_enabled);
    EXPECT_EQ(actual.byte_timeout_returns_idle, defaults.byte_timeout_returns_idle);
    EXPECT_EQ(actual.loopback, defaults.loopback);
    EXPECT_EQ(actual.interrupt_mode, defaults.interrupt_mode);
    EXPECT_EQ(actual.interrupt_pulse_counter, defaults.interrupt_pulse_counter);
    EXPECT_EQ(actual.receive_timeout_counter, defaults.receive_timeout_counter);
}

TEST(DytDebugConfiguration, UsesCom1AndChangesOnlyBaudrateCounter) {
    const auto transport = Dyt::make_com1_transport_config();
    EXPECT_EQ(transport.device_path, "/dev/xdma0");
    EXPECT_EQ(transport.user_offset, 0x40000u);
    EXPECT_EQ(transport.map_length, 0x40000u);
    EXPECT_EQ(transport.h2c_channel, -1);
    EXPECT_EQ(transport.c2h_channel, -1);
    EXPECT_EQ(transport.event_number, 0);

    const auto defaults = HW::ComDevice::default_config();
    const auto configured = Dyt::make_com1_config();
    expect_same_except_baud(configured, defaults);
    EXPECT_EQ(configured.baudrate_counter, 135u);
}

TEST(DytDebugLoop, SendsInitiallyThenAfterFourValidBFramesAndLogsEveryFrame) {
    ScriptedEndpoint endpoint;
    endpoint.incoming.push_back(std::vector<uint8_t>(
        ProtocolModel::Dyt_to_main_frame_bProtocol::FRAME_SIZE - 1, 0));
    for (uint16_t counter = 1; counter <= 4; ++counter) {
        endpoint.incoming.push_back(packed_b_frame(counter));
    }

    const auto log_path = unique_log_path();
    std::error_code ignored;
    std::filesystem::remove(log_path, ignored);

    Tools::SelfDescribingLogWriter writer;
    ASSERT_TRUE(writer.open(
        log_path.string(), ProtocolModel::Dyt_to_main_frame_bProtocol::buildSchema()));

    const auto result = Dyt::run_frame_loop(endpoint, writer, 4);
    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(result.value().received_b_frames, 4u);
    EXPECT_EQ(result.value().sent_a_frames, 2u);
    EXPECT_EQ(endpoint.receive_calls, 5u);

    ASSERT_EQ(endpoint.sent.size(), 2u);
    ProtocolModel::Main_to_dyt_frame_a initial_a;
    ProtocolModel::Main_to_dyt_frame_a fourth_b_reply;
    ASSERT_TRUE(ProtocolModel::Main_to_dyt_frame_aProtocol::unpackFrame(
        reinterpret_cast<const char*>(endpoint.sent[0].data()), endpoint.sent[0].size(),
        initial_a));
    ASSERT_TRUE(ProtocolModel::Main_to_dyt_frame_aProtocol::unpackFrame(
        reinterpret_cast<const char*>(endpoint.sent[1].data()), endpoint.sent[1].size(),
        fourth_b_reply));
    EXPECT_EQ(initial_a.frameCounter, 0u);
    EXPECT_EQ(fourth_b_reply.frameCounter, 1u);

    writer.close();
    Tools::SelfDescribingLogReader reader;
    ASSERT_TRUE(reader.open(log_path.string()));
    ASSERT_EQ(reader.recordCount(), 4u);
    std::vector<uint8_t> fourth_record;
    ASSERT_TRUE(reader.readRecord(3, fourth_record));
    ASSERT_GE(fourth_record.size(), 3u);
    EXPECT_EQ(fourth_record[1], 4u);
    EXPECT_EQ(fourth_record[2], 0u);
    reader.close();
    EXPECT_TRUE(std::filesystem::remove(log_path, ignored));
}

TEST(DytDebugLoop, ContinuesAfterRecoverableProtocolError) {
    ScriptedEndpoint endpoint;
    endpoint.receive_failures.push_back(HW::Status::error(
        HW::StatusCode::ProtocolError, 0, "discarded bad frame"));
    for (uint16_t counter = 1; counter <= 4; ++counter) {
        endpoint.incoming.push_back(packed_b_frame(counter));
    }

    const auto log_path = unique_log_path();
    std::error_code ignored;
    std::filesystem::remove(log_path, ignored);
    Tools::SelfDescribingLogWriter writer;
    ASSERT_TRUE(writer.open(
        log_path.string(), ProtocolModel::Dyt_to_main_frame_bProtocol::buildSchema()));

    const auto result = Dyt::run_frame_loop(endpoint, writer, 4);
    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(result.value().received_b_frames, 4u);
    EXPECT_EQ(result.value().sent_a_frames, 2u);
    EXPECT_EQ(endpoint.receive_calls, 5u);

    writer.close();
    Tools::SelfDescribingLogReader reader;
    ASSERT_TRUE(reader.open(log_path.string()));
    EXPECT_EQ(reader.recordCount(), 4u);
    reader.close();
    EXPECT_TRUE(std::filesystem::remove(log_path, ignored));
}

} // namespace
