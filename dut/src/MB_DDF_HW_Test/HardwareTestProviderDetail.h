#pragma once

#include "MB_DDF_HW/Device/Ad7606Device.h"
#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW_Test/ProductProtocol.h"
#include "MB_DDF_HW/Transport/ISpiTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace MB_DDF::HWTest::Detail {

struct BusIterationCounts {
    uint32_t error_count{0};
    uint32_t total_count{0};
};

constexpr void record_bus_iteration(BusIterationCounts& counts,
                                    bool payload_matches) noexcept {
    ++counts.total_count;
    if (!payload_matches) {
        ++counts.error_count;
    }
}

constexpr bool bus_payload_matches(std::span<const uint8_t> expected,
                                   std::span<const uint8_t> actual) noexcept {
    if (expected.size() != actual.size()) {
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (expected[index] != actual[index]) {
            return false;
        }
    }
    return true;
}

inline constexpr uint16_t kUdpSelfLoopPort = 3003;
inline constexpr unsigned kControlComIndex = 2;
inline constexpr uint8_t kControlBusLinkId = 4;
inline constexpr uint64_t kTimerLoadUserOffset = 0;
inline constexpr size_t kTimerLoadMapLength = 0x1000;
inline constexpr int kTimerLoadC2hChannel = 0;
inline constexpr uint64_t kTimerLoadDeviceOffset = 0;
inline constexpr size_t kTimerLoadTransferBytes = 64u * 1024u;
inline constexpr uint32_t kTimerLoadIntervalMs = 12;
inline constexpr uint64_t kImuCom4UserOffset = 0x100000;
inline constexpr size_t kImuComMapLength = 0x40000;
inline constexpr int kImuCom4EventNumber = 3;
inline constexpr size_t kImuPayloadBytes = 59;
inline constexpr size_t kImuReceiveBufferBytes = 255;
inline constexpr uint32_t kImuReceivePollTimeoutUs = 5000;
inline constexpr std::array<size_t, 23> kDhTelemetryAds1258Channels{
    1,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
};

constexpr std::optional<unsigned> com_index_for_bus_link(
    uint8_t link_id) noexcept {
    if (link_id < 2 || link_id > 5) {
        return std::nullopt;
    }
    return static_cast<unsigned>(link_id - 2);
}

constexpr std::string_view udp_self_loop_address(uint8_t link_id) noexcept {
    return link_id == 0 ? std::string_view{"192.168.1.29"}
           : link_id == 1 ? std::string_view{"192.168.7.29"}
                          : std::string_view{};
}

ProductErrorCode populate_dh_telemetry(const HW::Ads1258Snapshot& snapshot,
                                       ProductMessage& response);

HW::ComConfig imu_stream_com_config();

ProductErrorCode populate_imu_stream_feedback(
    std::span<const uint8_t> payload, ProductMessage& response);

ProductErrorCode run_helm_board_test(const ProductMessage& request,
                                     HW::PwmDevice& pwm,
                                     HW::Ad7606Device& ad7606,
                                     ProductMessage& response);

constexpr bool spi_echo_payload_allowed() noexcept {
    return false;
}

ProductErrorCode run_safe_spi_loop(HW::ISpiTransport& transport,
                                   uint32_t count,
                                   BusIterationCounts& counts);

double helm_command(uint32_t waveform, double frequency, double amplitude,
                    double offset, double start_phase_radians,
                    double maximum_frequency, double elapsed_seconds);

} // namespace MB_DDF::HWTest::Detail
