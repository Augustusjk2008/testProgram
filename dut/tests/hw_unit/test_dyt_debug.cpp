#include <gtest/gtest.h>

#include "MB_DDF/Tools/SelfDescribingLogReader.h"
#include "MB_DDF_DEBUG/SourceDebug.h"
#include "MB_DDF_DEBUG/dyt_frame_a.h"
#include "MB_DDF_DEBUG/dyt_frame_b.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace SourceDebug = MB_DDF::SourceDebug;
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

TEST(SourceDebugConfiguration, ProjectsGeneratedFrameEnvelopeToCom1) {
    const auto transport = SourceDebug::make_com1_transport_config();
    EXPECT_EQ(transport.device_path, "/dev/xdma0");
    EXPECT_EQ(transport.user_offset, 0x40000u);
    EXPECT_EQ(transport.map_length, 0x40000u);
    EXPECT_EQ(transport.h2c_channel, -1);
    EXPECT_EQ(transport.c2h_channel, -1);
    EXPECT_EQ(transport.event_number, 0);

    const ProtocolModel::Main_to_dyt_frame_a frame_a;
    const ProtocolModel::Dyt_to_main_frame_b frame_b;
    ASSERT_EQ(frame_a.frameLength,
              ProtocolModel::Main_to_dyt_frame_aProtocol::FRAME_SIZE);
    ASSERT_EQ(frame_b.frameLength,
              ProtocolModel::Dyt_to_main_frame_bProtocol::FRAME_SIZE);

    const std::array<uint8_t, 4> expected_send_header{
        frame_a.frameHeaderLow, frame_a.frameHeaderHigh, 0, 0};
    const std::array<uint8_t, 4> expected_receive_header{
        frame_b.frameHeaderLow, frame_b.frameHeaderHigh, 0, 0};
    const std::array<uint8_t, 4> no_fixed_tail{};
    const auto configured = SourceDebug::make_com1_config();
    EXPECT_EQ(configured.format.byte_format, 0xB0u);
    EXPECT_EQ(configured.format.receive_control, 0x21u);
    EXPECT_EQ(configured.frame.send_header, expected_send_header);
    EXPECT_EQ(configured.frame.receive_header, expected_receive_header);
    EXPECT_EQ(configured.frame.send_header_length, 2u);
    EXPECT_EQ(configured.frame.receive_header_length, 2u);
    EXPECT_EQ(configured.frame.send_length_bytes, 1u);
    EXPECT_EQ(configured.frame.receive_length_bytes, 1u);
    EXPECT_EQ(configured.frame.send_tail, no_fixed_tail);
    EXPECT_EQ(configured.frame.receive_tail, no_fixed_tail);
    EXPECT_EQ(configured.frame.send_tail_length, 0u);
    EXPECT_EQ(configured.frame.receive_tail_length, 0u);
    EXPECT_TRUE(configured.receive_enabled);
    EXPECT_TRUE(configured.byte_timeout_returns_idle);
    EXPECT_FALSE(configured.loopback);
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

    const auto result = SourceDebug::run_frame_loop(endpoint, writer, 4);
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

    const auto result = SourceDebug::run_frame_loop(endpoint, writer, 4);
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
