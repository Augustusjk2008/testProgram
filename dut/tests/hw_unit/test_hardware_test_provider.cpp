#include "MB_DDF_HW_Test/HardwareTestProvider.h"
#include "MB_DDF_HW_Test/HardwareTestProviderDetail.h"
#include "MB_DDF_HW_Test/ComEchoRunner.h"
#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/Ad7606Device.h"
#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW/Device/Registers/Ad7606Registers.h"
#include "MB_DDF_HW/Device/Registers/PwmRegisters.h"
#include "hw_unit/support/RecordingTransport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using MB_DDF::HWTest::HardwareTestProvider;
using MB_DDF::HWTest::ProductErrorCode;
using MB_DDF::HWTest::ProductProtocol;

namespace {

static_assert(MB_DDF::HWTest::Detail::kDhTelemetryAds1258Channels[0] == 1);
static_assert(MB_DDF::HWTest::Detail::kDhTelemetryAds1258Channels[1] == 4);
static_assert(MB_DDF::HWTest::Detail::kDhTelemetryAds1258Channels[22] == 25);

} // namespace

TEST(HardwareTestProviderTest, PopulatesDhTelemetryFromConfirmedAds1258Channels) {
    ProductProtocol protocol;
    auto response = protocol.create_message("dh_control_response", false);
    MB_DDF::HW::Ads1258Snapshot snapshot{};
    snapshot.raw[0] = 0x00780000u;
    snapshot.raw[1] = 0x00780000u;
    snapshot.raw[2] = 0x00780000u;
    snapshot.raw[3] = 0x00780000u;
    snapshot.raw[4] = 0x00F00000u;
    snapshot.raw[25] = 0x0057E400u;
    snapshot.raw[26] = 0x00780000u;

    ASSERT_EQ(MB_DDF::HWTest::Detail::populate_dh_telemetry(snapshot, response),
              ProductErrorCode::Ok);
    EXPECT_EQ(response.get_signed("telemetry[0]").value_or(0),
              std::llround(MB_DDF::HW::Ads1258Device::channel_voltage(
                               1, snapshot.raw[1]) /
                           0.001));
    EXPECT_EQ(response.get_signed("telemetry[1]").value_or(0),
              std::llround(MB_DDF::HW::Ads1258Device::channel_voltage(
                               4, snapshot.raw[4]) /
                           0.001));
    EXPECT_LT(response.get_signed("telemetry[1]").value_or(0), 0);
    EXPECT_EQ(response.get_signed("telemetry[22]").value_or(0),
              std::llround(MB_DDF::HW::Ads1258Device::channel_voltage(
                               25, snapshot.raw[25]) /
                           0.001));
    for (size_t channel = 2; channel < 22; ++channel) {
        EXPECT_EQ(response.get_signed("telemetry[" + std::to_string(channel) + "]")
                      .value_or(1),
                  0) << channel;
    }
}

TEST(HardwareTestProviderTest, DhTelemetryRejectsAnIncompatibleResponse) {
    ProductProtocol protocol;
    auto response = protocol.create_message("system_status_response", false);
    MB_DDF::HW::Ads1258Snapshot snapshot{};

    EXPECT_EQ(MB_DDF::HWTest::Detail::populate_dh_telemetry(snapshot, response),
              ProductErrorCode::TaskExecFailed);
}

TEST(HardwareTestProviderTest, HelmStartResponseCarriesVersionZeroZeroOne) {
    ProductProtocol protocol;
    auto response = protocol.create_message("helm_start_response", false);
    ASSERT_TRUE(response);
    EXPECT_EQ(response.get_unsigned("helm_version").value_or(0), 0x01u);
}

TEST(HardwareTestProviderTest, HelmBoardTestWritesDutyPercentAndReadsBackAd7606) {
    namespace AdRegisters = MB_DDF::HW::Registers::Ad7606;
    namespace PwmRegisters = MB_DDF::HW::Registers::Pwm;
    MB_DDF::HW::Test::RecordingTransport pwm_transport;
    MB_DDF::HW::Test::RecordingTransport ad_transport;
    ASSERT_TRUE(pwm_transport.open());
    ASSERT_TRUE(ad_transport.open());
    pwm_transport.preset(PwmRegisters::Peak, 1001u);
    pwm_transport.queue_reads(PwmRegisters::Enable, {0u, 0x0Fu});
    pwm_transport.queue_reads(PwmRegisters::UpdateEnable, {1u});
    for (unsigned channel = 0; channel < 4; ++channel) {
        pwm_transport.queue_reads(PwmRegisters::direction(channel),
                                  {(0x06u & (1u << channel)) != 0u ? 1u : 0u});
    }
    ad_transport.queue_reads(AdRegisters::AcquisitionEnable, {1u});
    ad_transport.queue_reads(AdRegisters::FilterEnable, {1u});
    const std::array<int16_t, 4> ad_values{0, 32767, -32768, 16384};
    for (unsigned channel = 0; channel < ad_values.size(); ++channel) {
        ad_transport.preset(
            AdRegisters::channel(channel),
            static_cast<uint16_t>(ad_values[channel]));
    }

    MB_DDF::HW::PwmDevice pwm(pwm_transport);
    MB_DDF::HW::Ad7606Device ad7606(ad_transport);
    ProductProtocol protocol;
    auto request = protocol.create_message("helm_board_test_request", false);
    auto response = protocol.create_message("helm_board_test_response", false);
    const std::array<uint8_t, 4> requested_percent{0, 1, 50, 100};
    const std::array<uint32_t, 4> expected_duty{0, 10, 501, 1001};
    ASSERT_EQ(request.get_unsigned("pwm_command_reserved").value_or(1), 0u);
    for (unsigned channel = 0; channel < requested_percent.size(); ++channel) {
        const std::string suffix = "[" + std::to_string(channel) + "]";
        ASSERT_TRUE(request.set_unsigned("pwm_duty_percent" + suffix,
                                         requested_percent[channel]));
    }
    ASSERT_TRUE(request.set_unsigned("direction[1]", 1));
    ASSERT_TRUE(request.set_unsigned("direction[2]", 1));

    EXPECT_EQ(MB_DDF::HWTest::Detail::run_helm_board_test(
                  request, pwm, ad7606, response),
              ProductErrorCode::Ok);
    for (unsigned channel = 0; channel < 4; ++channel) {
        const uint64_t expected_direction = (0x06u >> channel) & 1u;
        EXPECT_EQ(response.get_unsigned(
                      "pwm_duty_match[" + std::to_string(channel) + "]")
                      .value_or(2),
                  1u);
        EXPECT_EQ(response.get_unsigned(
                      "direction_readback[" + std::to_string(channel) + "]")
                      .value_or(2),
                  expected_direction);
        EXPECT_EQ(response.get_unsigned(
                      "pwm_duty[" + std::to_string(channel) + "]")
                      .value_or(2),
                  expected_duty[channel]);
        EXPECT_EQ(response.get_signed(
                      "helm_AD_value[" + std::to_string(channel) + "]")
                      .value_or(1),
                  ad_values[channel]);
    }
    EXPECT_EQ(response.get_unsigned("pwm_peak").value_or(0), 1001u);
    EXPECT_EQ(response.get_unsigned("pwm_enable_mask").value_or(0), 0x0Fu);
    EXPECT_EQ(response.get_unsigned("pwm_update_enabled").value_or(0), 1u);
    EXPECT_EQ(response.get_unsigned("ad_acquisition_enabled").value_or(0), 1u);
    EXPECT_EQ(response.get_unsigned("ad_filter_enabled").value_or(0), 1u);

    const auto& accesses = pwm_transport.accesses();
    const auto first_direction = std::find_if(
        accesses.begin(), accesses.end(), [](const auto& access) {
            return access.write && access.offset == PwmRegisters::direction(0);
        });
    const auto first_duty = std::find_if(
        accesses.begin(), accesses.end(), [](const auto& access) {
            return access.write && access.offset == PwmRegisters::duty(0);
        });
    ASSERT_NE(first_direction, accesses.end());
    ASSERT_NE(first_duty, accesses.end());
    EXPECT_LT(first_direction, first_duty);

    const auto has_pwm_write = [&](uint64_t offset, uint32_t value) {
        return std::any_of(accesses.begin(), accesses.end(), [&](const auto& access) {
            return access.write && access.offset == offset && access.value == value;
        });
    };
    EXPECT_TRUE(has_pwm_write(PwmRegisters::UpdateEnable, 0xFFFFu));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::DutyMode, 0xFFFFu));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::direction(0), 0xFFFFu));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::direction(1), 0xAAAAu));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::direction(2), 0xAAAAu));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::direction(3), 0xFFFFu));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::duty(0), 0u));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::duty(1), 10u));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::duty(2), 501u));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::duty(3), 1001u));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::Enable, 0xAAAAu));
    EXPECT_TRUE(has_pwm_write(PwmRegisters::UpdateEnable, 0xAAAAu));

    const auto& ad_accesses = ad_transport.accesses();
    const auto has_ad_write = [&](uint64_t offset, uint32_t value) {
        return std::any_of(
            ad_accesses.begin(), ad_accesses.end(), [&](const auto& access) {
                return access.write && access.offset == offset &&
                       access.value == value;
            });
    };
    EXPECT_TRUE(has_ad_write(AdRegisters::AcquisitionEnable, 0xAAAAu));
    EXPECT_TRUE(has_ad_write(AdRegisters::FilterEnable, 0xAAAAu));
}

TEST(HardwareTestProviderTest, HelmBoardTestRejectsPwmReadbackMismatch) {
    namespace AdRegisters = MB_DDF::HW::Registers::Ad7606;
    namespace PwmRegisters = MB_DDF::HW::Registers::Pwm;
    MB_DDF::HW::Test::RecordingTransport pwm_transport;
    MB_DDF::HW::Test::RecordingTransport ad_transport;
    ASSERT_TRUE(pwm_transport.open());
    ASSERT_TRUE(ad_transport.open());
    pwm_transport.preset(PwmRegisters::Peak, 1001u);
    pwm_transport.queue_reads(PwmRegisters::Enable, {0u, 0x0Fu});
    pwm_transport.queue_reads(PwmRegisters::UpdateEnable, {1u});
    pwm_transport.queue_reads(PwmRegisters::duty(0), {500u});
    for (unsigned channel = 0; channel < 4; ++channel) {
        pwm_transport.queue_reads(PwmRegisters::direction(channel), {0u});
    }
    ad_transport.queue_reads(AdRegisters::AcquisitionEnable, {1u});
    ad_transport.queue_reads(AdRegisters::FilterEnable, {1u});

    MB_DDF::HW::PwmDevice pwm(pwm_transport);
    MB_DDF::HW::Ad7606Device ad7606(ad_transport);
    ProductProtocol protocol;
    auto request = protocol.create_message("helm_board_test_request", false);
    auto response = protocol.create_message("helm_board_test_response", false);
    ASSERT_TRUE(request.set_unsigned("pwm_duty_percent[0]", 50));

    EXPECT_EQ(MB_DDF::HWTest::Detail::run_helm_board_test(
                  request, pwm, ad7606, response),
              ProductErrorCode::RegReadWriteFailed);
    EXPECT_EQ(response.get_unsigned("pwm_enable_mask").value_or(0), 0x0Fu);
    EXPECT_EQ(response.get_unsigned("pwm_duty[0]").value_or(0), 500u);
    EXPECT_EQ(response.get_unsigned("pwm_duty_match[0]").value_or(1), 0u);
    for (unsigned channel = 1; channel < 4; ++channel) {
        EXPECT_EQ(response.get_unsigned(
                      "pwm_duty_match[" + std::to_string(channel) + "]")
                      .value_or(0),
                  1u);
    }
}

TEST(HardwareTestProviderTest, HelmBoardTestRejectsInvalidDutyCommandsBeforeIo) {
    MB_DDF::HW::Test::RecordingTransport pwm_transport;
    MB_DDF::HW::Test::RecordingTransport ad_transport;
    MB_DDF::HW::PwmDevice pwm(pwm_transport);
    MB_DDF::HW::Ad7606Device ad7606(ad_transport);
    ProductProtocol protocol;

    auto out_of_range = protocol.create_message("helm_board_test_request", false);
    auto response = protocol.create_message("helm_board_test_response", false);
    ASSERT_TRUE(out_of_range.set_unsigned("pwm_duty_percent[2]", 101));
    EXPECT_EQ(MB_DDF::HWTest::Detail::run_helm_board_test(
                  out_of_range, pwm, ad7606, response),
              ProductErrorCode::ParamOutOfRange);

    auto reserved = protocol.create_message("helm_board_test_request", false);
    ASSERT_TRUE(reserved.set_unsigned("pwm_command_reserved", 1));
    EXPECT_EQ(MB_DDF::HWTest::Detail::run_helm_board_test(
                  reserved, pwm, ad7606, response),
              ProductErrorCode::ParamOutOfRange);
    EXPECT_TRUE(pwm_transport.accesses().empty());
    EXPECT_TRUE(ad_transport.accesses().empty());
}

TEST(HardwareTestProviderTest, SpiResultRemainsFloatDescriptor) {
    ProductProtocol protocol;
    auto response = protocol.create_message("spi_flash_test_response", false);
    ASSERT_TRUE(response);
    EXPECT_TRUE(response.set_float("sjl_result", 1.25f));
    EXPECT_FLOAT_EQ(response.get_float("sjl_result").value_or(0), 1.25f);
}

TEST(HardwareTestProviderTest, BusStatisticsCountOnlyCompletedIterations) {
    MB_DDF::HWTest::Detail::BusIterationCounts counts{};
    MB_DDF::HWTest::Detail::record_bus_iteration(counts, true);
    MB_DDF::HWTest::Detail::record_bus_iteration(counts, false);

    EXPECT_EQ(counts.total_count, 2u);
    EXPECT_EQ(counts.error_count, 1u);
    const std::array<uint8_t, 3> expected{1, 2, 3};
    const std::array<uint8_t, 4> too_long{1, 2, 3, 4};
    EXPECT_FALSE(MB_DDF::HWTest::Detail::bus_payload_matches(expected, too_long));
}

TEST(HardwareTestProviderTest, MapsOnlyBusComLinksAndReservesCom3) {
    EXPECT_EQ(MB_DDF::HWTest::Detail::com_index_for_bus_link(0).value_or(99),
              0u);
    EXPECT_EQ(MB_DDF::HWTest::Detail::com_index_for_bus_link(1).value_or(99),
              1u);
    EXPECT_EQ(MB_DDF::HWTest::Detail::com_index_for_bus_link(2).value_or(99),
              2u);
    EXPECT_EQ(MB_DDF::HWTest::Detail::com_index_for_bus_link(3).value_or(99),
              3u);
    for (const uint8_t link_id : {uint8_t{4}, uint8_t{5}, uint8_t{6}}) {
        EXPECT_FALSE(MB_DDF::HWTest::Detail::com_index_for_bus_link(link_id));
    }
    EXPECT_EQ(MB_DDF::HWTest::Detail::kControlBusLinkId, 2u);
}

TEST(HardwareTestProviderTest, PreflightRejectsUnsupportedLinksAndImuOwnedCom4) {
    using MB_DDF::HWTest::Detail::bus_link_preflight;

    EXPECT_EQ(bus_link_preflight(0, false), ProductErrorCode::Ok);
    EXPECT_EQ(bus_link_preflight(1, false), ProductErrorCode::Ok);
    EXPECT_EQ(bus_link_preflight(3, false), ProductErrorCode::Ok);
    EXPECT_EQ(bus_link_preflight(2, false), ProductErrorCode::ChannelInvalid);
    EXPECT_EQ(bus_link_preflight(4, false), ProductErrorCode::ChannelInvalid);
    EXPECT_EQ(bus_link_preflight(5, false), ProductErrorCode::ChannelInvalid);
    EXPECT_EQ(bus_link_preflight(6, false), ProductErrorCode::ChannelInvalid);
    EXPECT_EQ(bus_link_preflight(3, true), ProductErrorCode::TaskBusy);
}

TEST(HardwareTestProviderTest, BusConfigurationSelectsInternalLoopbackOnlyForLoop) {
    const auto loop_config = MB_DDF::HWTest::Detail::bus_com_config(true);
    const auto echo_config = MB_DDF::HWTest::Detail::bus_com_config(false);

    EXPECT_TRUE(loop_config.loopback);
    EXPECT_FALSE(echo_config.loopback);
    EXPECT_TRUE(loop_config.receive_enabled);
    EXPECT_TRUE(echo_config.receive_enabled);
    EXPECT_EQ(loop_config.interrupt_mode, MB_DDF::HW::ComInterruptMode::Level);
    EXPECT_EQ(echo_config.interrupt_mode, MB_DDF::HW::ComInterruptMode::Level);
    EXPECT_EQ(MB_DDF::HWTest::Detail::kBusReceiveTimeoutUs, 5'000'000u);
}

TEST(HardwareTestProviderTest, BusCompletionRequiresEveryRequestedExchangeToMatch) {
    MB_DDF::HWTest::Detail::BusIterationCounts complete{};
    for (size_t index = 0; index < 3; ++index) {
        MB_DDF::HWTest::Detail::record_bus_iteration(complete, true);
    }
    EXPECT_EQ(MB_DDF::HWTest::Detail::bus_completion_error(3, complete),
              ProductErrorCode::Ok);

    MB_DDF::HWTest::Detail::BusIterationCounts mismatched{};
    MB_DDF::HWTest::Detail::record_bus_iteration(mismatched, true);
    MB_DDF::HWTest::Detail::record_bus_iteration(mismatched, false);
    EXPECT_EQ(MB_DDF::HWTest::Detail::bus_completion_error(2, mismatched),
              ProductErrorCode::TaskExecFailed);

    MB_DDF::HWTest::Detail::BusIterationCounts incomplete{};
    MB_DDF::HWTest::Detail::record_bus_iteration(incomplete, true);
    EXPECT_EQ(MB_DDF::HWTest::Detail::bus_completion_error(2, incomplete),
              ProductErrorCode::TaskExecFailed);
}

TEST(HardwareTestProviderTest, ShortEchoIsRetainedAndFailsStrictCompletion) {
    const std::array<uint8_t, 3> sent{0x4D, 0x42, 0x31};
    const std::array<uint8_t, 2> received{0x4D, 0x42};
    MB_DDF::HWTest::Detail::BusIterationCounts counts{};
    std::vector<uint8_t> last_received;

    EXPECT_FALSE(MB_DDF::HWTest::Detail::record_bus_exchange(
        counts, sent, received, last_received));
    EXPECT_EQ(last_received,
              (std::vector<uint8_t>{received.begin(), received.end()}));
    EXPECT_EQ(MB_DDF::HWTest::Detail::bus_completion_error(1, counts),
              ProductErrorCode::TaskExecFailed);

    const std::array<uint8_t, 3> wrong_contents{0x4D, 0x42, 0x30};
    MB_DDF::HWTest::Detail::BusIterationCounts wrong_content_counts{};
    std::vector<uint8_t> wrong_content_received;
    EXPECT_FALSE(MB_DDF::HWTest::Detail::record_bus_exchange(
        wrong_content_counts, sent, wrong_contents, wrong_content_received));
    EXPECT_EQ(wrong_content_received,
              (std::vector<uint8_t>{wrong_contents.begin(), wrong_contents.end()}));
    EXPECT_EQ(MB_DDF::HWTest::Detail::bus_completion_error(
                  1, wrong_content_counts),
              ProductErrorCode::TaskExecFailed);
}

TEST(HardwareTestProviderTest, HelmCommandUsesStartAsRadianPhase) {
    constexpr double pi = 3.14159265358979323846;
    EXPECT_NEAR(MB_DDF::HWTest::Detail::helm_command(
                    0, 1.0, 10.0, 2.0, pi / 2.0, 0.0, 25.0, 0.0),
                12.0, 1.0e-9);
}

TEST(HardwareTestProviderTest, HelmSweepUsesReferenceTwentyFiveSecondPhaseIntegral) {
    constexpr double pi = 3.14159265358979323846;
    constexpr double duration = 25.0;
    constexpr double start_frequency = 1.0;
    constexpr double end_frequency = 2.0;
    constexpr double time = 12.5;
    const double expected_phase =
        2.0 * pi * start_frequency * end_frequency * duration /
        (end_frequency - start_frequency) *
        std::log((end_frequency * duration) /
                 (end_frequency * duration -
                  time * (end_frequency - start_frequency)));
    EXPECT_NEAR(MB_DDF::HWTest::Detail::helm_command(
                    4, start_frequency, 1.0, 0.0, 0.0,
                    end_frequency, duration, time),
                std::sin(expected_phase), 1.0e-9);
    EXPECT_NEAR(MB_DDF::HWTest::Detail::helm_command(
                    4, 2.0, 1.0, 0.0, 0.0, 2.0, duration, 0.125),
                1.0, 1.0e-9);
    EXPECT_EQ(MB_DDF::HWTest::Detail::helm_command(
                  4, 1.0, 1.0, 3.0, 0.0, 2.0, duration, duration + 0.001),
              0.0);
}

TEST(HardwareTestProviderTest, RejectsCom3BusLoopWithoutOpeningHardware) {
    ProductProtocol protocol;
    auto request = protocol.create_message("bus_loop_test_request", false);
    auto response = protocol.create_message("bus_loop_test_response", false);
    ASSERT_TRUE(request.set_unsigned("link_id", 2));
    ASSERT_TRUE(request.set_unsigned("total_count", 1));

    HardwareTestProvider provider;
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::ChannelInvalid);
    EXPECT_EQ(response.get_unsigned("link_id").value_or(0), 2u);
}

TEST(HardwareTestProviderTest, RejectsBusLoopCountsOutsideOneToOneHundredThousandBeforeIo) {
    ProductProtocol protocol;
    HardwareTestProvider provider;
    for (const uint32_t total_count : {uint32_t{0}, uint32_t{100'001}}) {
        auto request = protocol.create_message("bus_loop_test_request", false);
        auto response = protocol.create_message("bus_loop_test_response", false);
        ASSERT_TRUE(request.set_unsigned("link_id", 0));
        ASSERT_TRUE(request.set_unsigned("total_count", total_count));

        EXPECT_EQ(provider.handle(request, response),
                  ProductErrorCode::ParamOutOfRange)
            << "total_count=" << total_count;
    }
}

TEST(HardwareTestProviderTest, UnsupportedLinksAreRejectedBeforeLoopOrEchoHardwareAccess) {
    ProductProtocol protocol;
    HardwareTestProvider provider;
    for (const uint8_t link_id : {uint8_t{2}, uint8_t{4}, uint8_t{5}, uint8_t{6},
                                  uint8_t{7}}) {
        auto loop_request = protocol.create_message("bus_loop_test_request", false);
        auto loop_response = protocol.create_message("bus_loop_test_response", false);
        auto echo_request = protocol.create_message("bus_echo_test_request", false);
        auto echo_response = protocol.create_message("bus_echo_test_response", false);
        ASSERT_TRUE(loop_request.set_unsigned("link_id", link_id));
        ASSERT_TRUE(loop_request.set_unsigned("total_count", 1));
        ASSERT_TRUE(echo_request.set_unsigned("link_id", link_id));

        EXPECT_EQ(provider.handle(loop_request, loop_response),
                  ProductErrorCode::ChannelInvalid)
            << "link=" << static_cast<unsigned>(link_id);
        EXPECT_EQ(provider.handle(echo_request, echo_response),
                  ProductErrorCode::ChannelInvalid)
            << "link=" << static_cast<unsigned>(link_id);
    }
}

TEST(HardwareTestProviderTest, ElectricalHealthIncludesValueYxAndActivationBit) {
    ProductProtocol protocol;
    auto response = protocol.create_message("elec_health_status_response", false);

    EXPECT_TRUE(response.set_signed("c_volt", 1));
    EXPECT_TRUE(response.set_signed("b_volt", 2));
    EXPECT_TRUE(response.set_unsigned("activate_bits", 1));
    EXPECT_TRUE(response.set_signed("value_YX", 0x0800));
    EXPECT_EQ(response.get_unsigned("activate_bits").value_or(0), 1u);
    EXPECT_EQ(response.get_signed("value_YX").value_or(0), 0x0800);
    EXPECT_FALSE(response.set_signed("c_threshold", 3));
    EXPECT_FALSE(response.set_signed("b_threshold", 4));
}

TEST(HardwareTestProviderTest, DhControlRejectsChannelsOutsideTwentyThreeWithoutHardwareAccess) {
    ProductProtocol protocol;
    HardwareTestProvider provider;
    auto request = protocol.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", (uint64_t{1} << 23)));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    EXPECT_EQ(provider.begin_dh(request), ProductErrorCode::ParamOutOfRange);

    ASSERT_TRUE(request.set_unsigned("channel[0]", 0));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    EXPECT_EQ(provider.begin_dh(request), ProductErrorCode::ParamOutOfRange);

    ASSERT_TRUE(request.set_unsigned("channel[1]", 1));
    EXPECT_EQ(provider.begin_dh(request), ProductErrorCode::ParamOutOfRange);

    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("power_enable", 2));
    EXPECT_EQ(provider.begin_dh(request), ProductErrorCode::ParamOutOfRange);

    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 2));
    EXPECT_EQ(provider.begin_dh(request), ProductErrorCode::ParamOutOfRange);
}

TEST(HardwareTestProviderTest, DirectDhControlRequiresServiceBurstOrchestration) {
    ProductProtocol protocol;
    HardwareTestProvider provider;
    auto request = protocol.create_message("dh_control_request", false);
    auto response = protocol.create_message("dh_control_response", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 0));
    ASSERT_TRUE(request.set_unsigned("return_enable", 0));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 1));

    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::CmdUnknown);
}

TEST(HardwareTestProviderTest, DhBeginRepeatsServiceBusinessValidationBeforeHardwareAccess) {
    ProductProtocol protocol;
    HardwareTestProvider provider;
    auto request = protocol.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 0));
    ASSERT_TRUE(request.set_unsigned("return_enable", 0));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("report_count", 0));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 0));
    EXPECT_EQ(provider.begin_dh(request), ProductErrorCode::ParamOutOfRange);

    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2499));
    EXPECT_EQ(provider.begin_dh(request), ProductErrorCode::ParamOutOfRange);

    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 2));
    EXPECT_EQ(provider.begin_dh(request), ProductErrorCode::ParamOutOfRange);
}

TEST(HardwareTestProviderTest, DhPulseConfigRejectsInvalidEnableBeforeHardwareAccess) {
    ProductProtocol protocol;
    HardwareTestProvider provider;
    auto request = protocol.create_message("dh_pulse_config_request", false);
    auto response = protocol.create_message("dh_pulse_config_response", false);
    ASSERT_TRUE(request.set_unsigned("config_enable", 2));
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::ParamOutOfRange);
}

TEST(HardwareTestProviderTest, HelmParametersDoNotImposeAngleLimits) {
    MB_DDF::HWTest::HelmStreamParameters parameters{};
    parameters.waveform = 0;
    parameters.frequency_hz = 1.0;
    parameters.amplitude_deg = 250.0;
    parameters.offset_deg = -100.0;
    parameters.start_phase_radians = 0.0;
    parameters.maximum_frequency_hz = 80.0;
    parameters.sweep_duration_seconds = 25.0;
    parameters.enable_mask = 0x0F;

    EXPECT_TRUE(MB_DDF::HWTest::validate_helm_stream_parameters(parameters));
}

TEST(HardwareTestProviderTest, Com3ImageUsesFixedWindowEventAnd614400EightEOneDefaults) {
    static_assert(MB_DDF::HWTest::kCom3UserOffset == 0xC0000u);
    static_assert(MB_DDF::HWTest::kComRegisterWindowSize == 0x40000u);
    static_assert(MB_DDF::HWTest::kCom3EventNumber == 2);
    const auto config = MB_DDF::HW::ComDevice::default_config();
    EXPECT_EQ(config.format.byte_format, 0xB0u);
    EXPECT_EQ(config.format.receive_control, 0x21u);
    EXPECT_EQ(config.baudrate_counter, 0x00CAu);
}

TEST(HardwareTestProviderTest, ImuStreamUsesCom4AndConfirmed921600FrameConfiguration) {
    static_assert(MB_DDF::HWTest::Detail::kImuCom4UserOffset == 0x100000u);
    static_assert(MB_DDF::HWTest::Detail::kImuComMapLength == 0x40000u);
    static_assert(MB_DDF::HWTest::Detail::kImuCom4EventNumber == 3);
    static_assert(MB_DDF::HWTest::Detail::kImuPayloadBytes == 59u);
    static_assert(MB_DDF::HWTest::Detail::kImuReceiveBufferBytes == 255u);
    static_assert(MB_DDF::HWTest::Detail::kImuReceiveBufferBytes >
                  MB_DDF::HWTest::Detail::kImuPayloadBytes);
    const auto config = MB_DDF::HWTest::Detail::imu_stream_com_config();
    EXPECT_EQ(config.format.byte_format, 0xB0u);
    EXPECT_EQ(config.frame.receive_header[0], 0xAAu);
    EXPECT_EQ(config.frame.receive_header[1], 0x1Au);
    EXPECT_EQ(config.frame.receive_header_length, 2u);
    EXPECT_EQ(config.frame.receive_length_bytes, 1u);
    EXPECT_EQ(config.frame.receive_tail_length, 0u);
    EXPECT_EQ(config.baudrate_counter, 0x0086u);
    EXPECT_EQ(config.interrupt_mode, MB_DDF::HW::ComInterruptMode::Level);
}

TEST(HardwareTestProviderTest, DecodesConfirmedImuPayloadIntoFeedbackFields) {
    ProductProtocol protocol;
    auto response = protocol.create_message("imu_stream_feedback_response", false);
    ASSERT_TRUE(response);
    std::array<uint8_t, MB_DDF::HWTest::Detail::kImuPayloadBytes> payload{};
    const auto put_u16 = [&](size_t offset, uint16_t value) {
        payload[offset] = static_cast<uint8_t>(value);
        payload[offset + 1] = static_cast<uint8_t>(value >> 8);
    };
    const auto put_f32 = [&](size_t offset, float value) {
        const uint32_t bits = std::bit_cast<uint32_t>(value);
        for (size_t byte = 0; byte < sizeof(bits); ++byte) {
            payload[offset + byte] = static_cast<uint8_t>(bits >> (8 * byte));
        }
    };
    put_u16(0, 0x1234u);
    for (size_t index = 0; index < 12; ++index) {
        put_f32(2 + index * sizeof(float), static_cast<float>(index) + 0.25F);
    }
    put_u16(50, static_cast<uint16_t>(static_cast<int16_t>(-123)));
    put_u16(52, 0x4567u);
    payload[54] = 0x89u;
    put_u16(55, 0xABCDu);
    put_u16(57, 0xEF01u);

    EXPECT_EQ(MB_DDF::HWTest::Detail::populate_imu_stream_feedback(payload, response),
              ProductErrorCode::Ok);
    EXPECT_EQ(response.get_unsigned("source_seq").value_or(0), 0x1234u);
    static constexpr std::array<const char*, 12> kFloatFields{
        "delta_angle_x", "delta_angle_y", "delta_angle_z",
        "delta_velocity_x", "delta_velocity_y", "delta_velocity_z",
        "angular_rate_x", "angular_rate_y", "angular_rate_z",
        "acceleration_x", "acceleration_y", "acceleration_z",
    };
    for (size_t index = 0; index < kFloatFields.size(); ++index) {
        EXPECT_FLOAT_EQ(response.get_float(kFloatFields[index]).value_or(0.0F),
                        static_cast<float>(index) + 0.25F)
            << index;
    }
    EXPECT_EQ(response.get_signed("temperature").value_or(0), -123);
    EXPECT_EQ(response.get_unsigned("self_test_status").value_or(0), 0x4567u);
    EXPECT_EQ(response.get_unsigned("work_status").value_or(0), 0x89u);
    EXPECT_EQ(response.get_unsigned("software_version").value_or(0), 0xABCDu);
    EXPECT_EQ(response.get_unsigned("source_reserved").value_or(0), 0xEF01u);
}

TEST(HardwareTestProviderTest, RejectsWrongLengthOrNonFiniteImuPayload) {
    ProductProtocol protocol;
    auto response = protocol.create_message("imu_stream_feedback_response", false);
    std::array<uint8_t, MB_DDF::HWTest::Detail::kImuPayloadBytes> payload{};

    EXPECT_EQ(MB_DDF::HWTest::Detail::populate_imu_stream_feedback(
                  std::span<const uint8_t>(payload.data(), payload.size() - 1), response),
              ProductErrorCode::LenMismatch);
    std::array<uint8_t, MB_DDF::HWTest::Detail::kImuPayloadBytes + 1> oversized{};
    EXPECT_EQ(MB_DDF::HWTest::Detail::populate_imu_stream_feedback(oversized, response),
              ProductErrorCode::LenMismatch);

    const std::array<float, 3> non_finite{
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (size_t field = 0; field < 12; ++field) {
        for (const float value : non_finite) {
            payload.fill(0);
            const uint32_t bits = std::bit_cast<uint32_t>(value);
            for (size_t byte = 0; byte < sizeof(bits); ++byte) {
                payload[2 + field * sizeof(float) + byte] =
                    static_cast<uint8_t>(bits >> (8 * byte));
            }
            EXPECT_EQ(MB_DDF::HWTest::Detail::populate_imu_stream_feedback(
                          payload, response),
                      ProductErrorCode::ParamOutOfRange)
                << field;
        }
    }
}

TEST(HardwareTestProviderTest, TimerLoadUsesReadOnlyC2hZeroConfiguration) {
    static_assert(MB_DDF::HWTest::Detail::kTimerLoadUserOffset == 0u);
    static_assert(MB_DDF::HWTest::Detail::kTimerLoadMapLength == 0x1000u);
    static_assert(MB_DDF::HWTest::Detail::kTimerLoadC2hChannel == 0);
    static_assert(MB_DDF::HWTest::Detail::kTimerLoadDeviceOffset == 0u);
    static_assert(MB_DDF::HWTest::Detail::kTimerLoadTransferBytes == 64u * 1024u);
    static_assert(MB_DDF::HWTest::Detail::kTimerLoadIntervalMs == 12u);
}
