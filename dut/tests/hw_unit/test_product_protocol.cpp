#include "MB_DDF_HW_Test/ProductProtocol.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using MB_DDF::HWTest::ProductErrorCode;
using MB_DDF::HWTest::ProductMessage;
using MB_DDF::HWTest::ProductProtocol;

namespace {

ProductMessage request(ProductProtocol& protocol, std::string_view name, uint16_t sequence) {
    auto message = protocol.create_message(name, false);
    EXPECT_TRUE(message);
    EXPECT_TRUE(message.set_unsigned("seq", sequence));
    return message;
}

} // namespace

TEST(ProductProtocolTest, ParsesBothSupportedDataSegmentLengths) {
    ProductProtocol protocol;

    auto short_request = request(protocol, "system_status_request", 0x1234);
    ASSERT_EQ(short_request.bytes().size(), 48u);
    auto parsed_short = protocol.parse_request(short_request.bytes());
    ASSERT_TRUE(parsed_short);
    EXPECT_EQ(parsed_short.message().name(), "system_status_request");
    EXPECT_EQ(parsed_short.message().get_unsigned("seq").value_or(0), 0x1234u);

    auto long_request = request(protocol, "bus_echo_test_request", 0x5678);
    ASSERT_EQ(long_request.bytes().size(), 123u);
    auto parsed_long = protocol.parse_request(long_request.bytes());
    ASSERT_TRUE(parsed_long);
    EXPECT_EQ(parsed_long.message().name(), "bus_echo_test_request");
    EXPECT_EQ(parsed_long.message().get_unsigned("seq").value_or(0), 0x5678u);
}

TEST(ProductProtocolTest, ParsesEveryGeneratedRequestDescriptor) {
    ProductProtocol protocol;
    size_t request_count = 0;
    for (const auto& descriptor :
         MB_DDF::HWTest::GeneratedProductProtocol::kMessages) {
        if (descriptor.role !=
            MB_DDF::HWTest::GeneratedProductProtocol::MessageRole::Request) {
            continue;
        }
        ++request_count;
        auto message = protocol.create_message(descriptor.name, false);
        ASSERT_TRUE(message) << descriptor.name;
        const auto parsed = protocol.parse_request(message.bytes());
        ASSERT_TRUE(parsed) << descriptor.name;
        EXPECT_EQ(parsed.message().name(), descriptor.name);
    }
    EXPECT_EQ(request_count, 17u);
}

TEST(ProductProtocolTest, EncodesImuStreamFeedbackLayoutAndCommandIds) {
    ProductProtocol protocol;
    auto start = protocol.create_message("imu_stream_start_request", false);
    auto feedback = protocol.create_message("imu_stream_feedback_response", false);
    auto stop = protocol.create_message("imu_stream_stop_request", false);
    ASSERT_TRUE(start);
    ASSERT_TRUE(feedback);
    ASSERT_TRUE(stop);

    EXPECT_EQ(start.get_unsigned("type_group").value_or(0), 0x09u);
    EXPECT_EQ(start.get_unsigned("sub_type").value_or(0), 0x10u);
    EXPECT_EQ(feedback.get_unsigned("type_group").value_or(0), 0x09u);
    EXPECT_EQ(feedback.get_unsigned("sub_type").value_or(0), 0x01u);
    EXPECT_EQ(stop.get_unsigned("type_group").value_or(0), 0x09u);
    EXPECT_EQ(stop.get_unsigned("sub_type").value_or(0), 0x11u);
    EXPECT_EQ(feedback.bytes().size(), 123u);

    EXPECT_EQ(feedback.data_offset("source_seq").value_or(99), 8u);
    EXPECT_EQ(feedback.data_offset("delta_angle_x").value_or(99), 10u);
    EXPECT_EQ(feedback.data_offset("acceleration_z").value_or(99), 54u);
    EXPECT_EQ(feedback.data_offset("temperature").value_or(99), 58u);
    EXPECT_EQ(feedback.data_offset("source_reserved").value_or(99), 65u);
    ASSERT_TRUE(feedback.set_unsigned("source_seq", 0x1234));
    ASSERT_TRUE(feedback.set_float("delta_angle_x", 1.25F));
    ASSERT_TRUE(feedback.set_signed("temperature", -123));
    EXPECT_EQ(feedback.get_unsigned("source_seq").value_or(0), 0x1234u);
    EXPECT_FLOAT_EQ(feedback.get_float("delta_angle_x").value_or(0.0F), 1.25F);
    EXPECT_EQ(feedback.get_signed("temperature").value_or(0), -123);
}

TEST(ProductProtocolTest, RemovedPowerAndAdCommandsAreUnknown) {
    ProductProtocol protocol;
    for (const std::string_view name : {
             "power_switch_request",
             "power_switch_response",
             "ad_read_request",
             "ad_read_response",
         }) {
        EXPECT_FALSE(protocol.create_message(name, false)) << name;
    }

    const auto known = request(protocol, "system_status_request", 0x4321);
    for (const std::array<uint8_t, 2> command : {
             std::array<uint8_t, 2>{0x04, 0x03},
             std::array<uint8_t, 2>{0x05, 0x02},
         }) {
        auto bytes = std::vector<uint8_t>(known.bytes().begin(),
                                          known.bytes().end());
        bytes[1] = command[0];
        bytes[2] = command[1];
        const auto parsed = protocol.parse_request(bytes);
        ASSERT_FALSE(parsed);
        EXPECT_EQ(parsed.error().code, ProductErrorCode::CmdUnknown);
        EXPECT_EQ(parsed.error().orig_type_group, command[0]);
        EXPECT_EQ(parsed.error().orig_sub_type, command[1]);
        EXPECT_EQ(parsed.error().orig_sequence, 0x4321u);
    }
}

TEST(ProductProtocolTest, EncodesHelmBoardDutyPercentAndDirectionLayout) {
    ProductProtocol protocol;
    auto request = protocol.create_message("helm_board_test_request", false);
    auto response = protocol.create_message("helm_board_test_response", false);
    ASSERT_TRUE(request);
    ASSERT_TRUE(response);

    const std::array<uint8_t, 4> duty_percent{0, 25, 50, 100};
    EXPECT_EQ(request.get_unsigned("pwm_command_reserved").value_or(1), 0u);
    for (size_t channel = 0; channel < duty_percent.size(); ++channel) {
        const std::string suffix = "[" + std::to_string(channel) + "]";
        ASSERT_TRUE(request.set_unsigned("pwm_duty_percent" + suffix,
                                         duty_percent[channel]));
        EXPECT_EQ(request.data_offset("pwm_duty_percent" + suffix)
                      .value_or(99),
                  6u + channel);
    }
    ASSERT_TRUE(request.set_unsigned("direction[0]", 1));
    ASSERT_TRUE(request.set_unsigned("direction[3]", 1));
    EXPECT_EQ(request.data_offset("pwm_command_reserved").value_or(99), 5u);
    EXPECT_EQ(request.data_offset("direction[3]").value_or(99), 5u);
    EXPECT_EQ(request.bytes()[5], 0x90u);
    EXPECT_EQ(request.bytes()[6], 0u);
    EXPECT_EQ(request.bytes()[7], 25u);
    EXPECT_EQ(request.bytes()[8], 50u);
    EXPECT_EQ(request.bytes()[9], 100u);

    ASSERT_TRUE(response.set_unsigned("pwm_duty_match[0]", 1));
    ASSERT_TRUE(response.set_unsigned("pwm_duty_match[2]", 1));
    ASSERT_TRUE(response.set_unsigned("direction_readback[1]", 1));
    ASSERT_TRUE(response.set_unsigned("direction_readback[2]", 1));
    EXPECT_EQ(response.data_offset("pwm_duty_match[0]").value_or(99), 8u);
    EXPECT_EQ(response.data_offset("direction_readback[3]").value_or(99), 8u);
    EXPECT_EQ(response.bytes()[8], 0x65u);
    EXPECT_EQ(request.get_unsigned("direction[3]").value_or(0), 1u);
    EXPECT_TRUE(response.set_signed("helm_AD_value[2]", -32768));
    EXPECT_EQ(response.get_signed("helm_AD_value[2]").value_or(0), -32768);
}

TEST(ProductProtocolTest, RejectsUnknownCommandAndNonzeroReservedBytes) {
    ProductProtocol protocol;
    auto message = request(protocol, "system_status_request", 0x4321);
    auto unknown_bytes = std::vector<uint8_t>(message.bytes().begin(),
                                              message.bytes().end());
    unknown_bytes[1] = 0xEE;
    unknown_bytes[2] = 0xDD;
    const auto unknown = protocol.parse_request(unknown_bytes);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ProductErrorCode::CmdUnknown);
    EXPECT_EQ(unknown.error().orig_type_group, 0xEEu);
    EXPECT_EQ(unknown.error().orig_sub_type, 0xDDu);
    EXPECT_EQ(unknown.error().orig_sequence, 0x4321u);

    auto reserved_bytes = std::vector<uint8_t>(message.bytes().begin(),
                                               message.bytes().end());
    const auto pad = message.data_offset("pad");
    ASSERT_TRUE(pad.has_value());
    reserved_bytes[*pad] = 1;
    const auto reserved = protocol.parse_request(reserved_bytes);
    ASSERT_FALSE(reserved);
    EXPECT_EQ(reserved.error().code, ProductErrorCode::ParamOutOfRange);
}

TEST(ProductProtocolTest, RejectsNanAndInfinityWhenSettingOrParsingF32) {
    ProductProtocol protocol;
    auto message = protocol.create_message("helm_start_request", false);
    ASSERT_TRUE(message);
    EXPECT_FALSE(message.set_float("freq", std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(message.set_float("freq", std::numeric_limits<float>::infinity()));

    for (const uint32_t invalid_bits : {0x7FC00000u, 0x7F800000u}) {
        auto bytes = std::vector<uint8_t>(message.bytes().begin(), message.bytes().end());
        const auto offset = message.data_offset("freq");
        ASSERT_TRUE(offset.has_value());
        for (size_t index = 0; index < sizeof(invalid_bits); ++index) {
            bytes[*offset + index] =
                static_cast<uint8_t>(invalid_bits >> (8 * index));
        }
        const auto parsed = protocol.parse_request(bytes);
        ASSERT_FALSE(parsed);
        EXPECT_EQ(parsed.error().code, ProductErrorCode::ParamOutOfRange);
    }
}

TEST(ProductProtocolTest, RejectsUnsupportedLengthAndVersionWithOriginalHeader) {
    ProductProtocol protocol;
    std::array<uint8_t, 12> malformed{};
    malformed[0] = 0x11;
    malformed[1] = 0x04;
    malformed[2] = 0x01;
    malformed[3] = 0xCD;
    malformed[4] = 0xAB;

    auto wrong_length = protocol.parse_request(malformed);
    ASSERT_FALSE(wrong_length);
    EXPECT_EQ(wrong_length.error().code, ProductErrorCode::LenMismatch);
    EXPECT_EQ(wrong_length.error().orig_sequence, 0xABCDu);

    auto valid = request(protocol, "di_read_request", 7);
    ASSERT_TRUE(valid.set_unsigned("version", 0x10));
    auto wrong_version = protocol.parse_request(valid.bytes());
    ASSERT_FALSE(wrong_version);
    EXPECT_EQ(wrong_version.error().code, ProductErrorCode::CmdUnknown);
    EXPECT_EQ(wrong_version.error().orig_type_group, 0x04u);
    EXPECT_EQ(wrong_version.error().orig_sub_type, 0x01u);
    EXPECT_EQ(wrong_version.error().orig_sequence, 7u);
}

TEST(ProductProtocolTest, ReportsLengthMismatchForKnownCommandInWrongFrameClass) {
    ProductProtocol protocol;
    auto valid = request(protocol, "system_status_request", 0x3456);
    std::vector<uint8_t> wrong_class(123, 0);
    std::copy(valid.bytes().begin(), valid.bytes().end(), wrong_class.begin());

    auto parsed = protocol.parse_request(wrong_class);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ProductErrorCode::LenMismatch);
    EXPECT_EQ(parsed.error().orig_sequence, 0x3456u);
}

TEST(ProductProtocolTest, EncodesLittleEndianScalarsAndBitFields) {
    ProductProtocol protocol;
    auto response = protocol.create_message("dh_control_response");
    ASSERT_TRUE(response);

    ASSERT_TRUE(response.set_unsigned("err_code", 0x1234));
    EXPECT_EQ(response.get_unsigned("err_code").value_or(0), 0x1234u);
    ASSERT_TRUE(response.set_signed("telemetry[0]", -123456));
    EXPECT_EQ(response.get_signed("telemetry[0]").value_or(0), -123456);
    ASSERT_TRUE(response.set_unsigned("dh_status.ch3", 2));
    EXPECT_EQ(response.get_unsigned("dh_status.ch3").value_or(0), 2u);
    EXPECT_EQ(response.get_unsigned("dh_status.ch2").value_or(0), 0u);

    const auto err_offset = response.data_offset("err_code");
    ASSERT_TRUE(err_offset.has_value());
    EXPECT_EQ(response.bytes()[*err_offset], 0x34u);
    EXPECT_EQ(response.bytes()[*err_offset + 1], 0x12u);
}

TEST(ProductProtocolTest, EncodesEngineeringValuesWithGeneratedFixedPointLsb) {
    ProductProtocol protocol;
    auto health = protocol.create_message("elec_health_status_response", false);
    auto dh = protocol.create_message("dh_control_response", false);
    ASSERT_TRUE(health);
    ASSERT_TRUE(dh);

    ASSERT_TRUE(health.set_scaled_signed("c_volt", 28.506));
    EXPECT_EQ(health.get_signed("c_volt").value_or(0), 2851);
    ASSERT_TRUE(health.set_scaled_signed("external_vol", 3.303));
    EXPECT_EQ(health.get_signed("external_vol").value_or(0), 330);
    ASSERT_TRUE(health.set_scaled_signed("js_5V", 5.047));
    EXPECT_EQ(health.get_signed("js_5V").value_or(0), 505);
    ASSERT_TRUE(health.set_scaled_signed("value_YX", 5.045));
    EXPECT_EQ(health.get_signed("value_YX").value_or(0), 2048);
    EXPECT_EQ(health.data_offset("value_YX").value_or(99), 27u);
    ASSERT_TRUE(dh.set_scaled_signed("telemetry[0]", 12.3454));
    EXPECT_EQ(dh.get_signed("telemetry[0]").value_or(0), 12345);
    ASSERT_TRUE(dh.set_scaled_signed("telemetry[1]", -0.0016));
    EXPECT_EQ(dh.get_signed("telemetry[1]").value_or(0), -2);
}

TEST(ProductProtocolTest, RejectsInvalidEngineeringValuesWithoutSaturation) {
    ProductProtocol protocol;
    auto health = protocol.create_message("elec_health_status_response", false);
    auto flash = protocol.create_message("spi_flash_test_response", false);
    ASSERT_TRUE(health);
    ASSERT_TRUE(flash);

    EXPECT_FALSE(health.set_scaled_signed(
        "c_volt", std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(health.set_scaled_signed(
        "c_volt", std::numeric_limits<double>::infinity()));
    EXPECT_FALSE(health.set_scaled_signed("c_volt", 400.0));
    EXPECT_FALSE(flash.set_scaled_signed("sjl_result", 1.0));
    EXPECT_FALSE(health.set_scaled_signed("missing", 1.0));
}

TEST(ProductProtocolTest, AppliesDefaultsAndZeroFillsReservedBytes) {
    ProductProtocol protocol;
    auto request_message = protocol.create_message("memperf_test_request", false);
    ASSERT_TRUE(request_message);
    EXPECT_EQ(request_message.get_unsigned("version").value_or(0), 0x11u);
    EXPECT_EQ(request_message.get_unsigned("type_group").value_or(0), 0x02u);
    EXPECT_EQ(request_message.get_unsigned("sub_type").value_or(0), 0x01u);
    EXPECT_EQ(request_message.get_unsigned("length").value_or(0), 65536u);
    EXPECT_EQ(request_message.get_unsigned("seed").value_or(0), 0x5A5A5A5Au);
    EXPECT_TRUE(request_message.reserved_bytes_are_zero());
}

TEST(ProductProtocolTest, UsesIndependentTransmitSequenceAndWraps) {
    ProductProtocol protocol(0xFFFF);
    auto first = protocol.create_message("system_status_response");
    auto second = protocol.create_message("di_read_response");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.get_unsigned("seq").value_or(0), 0xFFFFu);
    EXPECT_EQ(second.get_unsigned("seq").value_or(1), 0u);

    auto incoming = request(protocol, "system_status_request", 0x2222);
    auto parsed = protocol.parse_request(incoming.bytes());
    ASSERT_TRUE(parsed);
    auto third = protocol.create_message("system_status_response");
    EXPECT_EQ(third.get_unsigned("seq").value_or(0), 1u);
}

TEST(ProductProtocolTest, ExplicitlyAssignsSequenceOnlyWhenSenderAcceptsMessage) {
    ProductProtocol protocol(0x1234);
    auto first = protocol.create_message("system_status_response", false);
    auto second = protocol.create_message("di_read_response", false);

    ASSERT_TRUE(protocol.assign_transmit_sequence(first));
    ASSERT_TRUE(protocol.assign_transmit_sequence(second));
    EXPECT_EQ(first.get_unsigned("seq").value_or(0), 0x1234u);
    EXPECT_EQ(second.get_unsigned("seq").value_or(0), 0x1235u);
}

TEST(ProductProtocolTest, ErrorResponseEchoesRequestSequenceInSeqAndOrigSequence) {
    ProductProtocol protocol(9);
    MB_DDF::HWTest::ProductParseError error{};
    error.code = ProductErrorCode::CmdUnknown;
    error.orig_type_group = 0x99;
    error.orig_sub_type = 0x77;
    error.orig_sequence = 0x4567;
    error.detail = 0xAABBCCDD;

    auto response = protocol.create_error_response(error);
    ASSERT_TRUE(response);
    EXPECT_EQ(response.get_unsigned("seq").value_or(0), 0x4567u);
    EXPECT_EQ(response.get_unsigned("orig_seq").value_or(0), 0x4567u);
    EXPECT_EQ(response.get_unsigned("orig_tg").value_or(0), 0x99u);
    EXPECT_EQ(response.get_unsigned("orig_st").value_or(0), 0x77u);
    EXPECT_EQ(response.get_unsigned("detail").value_or(0), 0xAABBCCDDu);
}
