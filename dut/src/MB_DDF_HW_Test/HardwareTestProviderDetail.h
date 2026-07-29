#pragma once

#include "MB_DDF_HW/Device/Ad7606Device.h"
#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW_Test/ProductProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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

inline bool record_bus_exchange(BusIterationCounts& counts,
                                std::span<const uint8_t> expected,
                                std::span<const uint8_t> actual,
                                std::vector<uint8_t>& last_received) {
    const bool matches = bus_payload_matches(expected, actual);
    record_bus_iteration(counts, matches);
    last_received.assign(actual.begin(), actual.end());
    return matches;
}

inline constexpr unsigned kControlComIndex = 2;
inline constexpr unsigned kImuComIndex = 3;
inline constexpr uint8_t kControlBusLinkId = 2;
inline constexpr uint32_t kBusReceiveTimeoutUs = 5'000'000;
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
    switch (link_id) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return kControlComIndex;
    case 3:
        return kImuComIndex;
    default:
        return std::nullopt;
    }
}

constexpr ProductErrorCode bus_link_preflight(uint8_t link_id,
                                              bool imu_stream_active) noexcept {
    if (link_id == kControlBusLinkId) {
        return ProductErrorCode::ChannelInvalid;
    }
    const auto com_index = com_index_for_bus_link(link_id);
    if (!com_index) {
        return ProductErrorCode::ChannelInvalid;
    }
    return *com_index == kImuComIndex && imu_stream_active
               ? ProductErrorCode::TaskBusy
               : ProductErrorCode::Ok;
}

constexpr ProductErrorCode bus_completion_error(
    uint32_t requested_count, const BusIterationCounts& counts) noexcept {
    return counts.total_count == requested_count && counts.error_count == 0
               ? ProductErrorCode::Ok
               : ProductErrorCode::TaskExecFailed;
}

ProductErrorCode populate_dh_telemetry(const HW::Ads1258Snapshot& snapshot,
                                       ProductMessage& response);

HW::ComConfig bus_com_config(bool loopback);
HW::ComConfig imu_stream_com_config();

ProductErrorCode populate_imu_stream_feedback(
    std::span<const uint8_t> payload, ProductMessage& response);

ProductErrorCode run_helm_board_test(const ProductMessage& request,
                                     HW::PwmDevice& pwm,
                                     HW::Ad7606Device& ad7606,
                                     ProductMessage& response);

double helm_command(uint32_t waveform, double frequency, double amplitude,
                    double offset, double start_phase_radians,
                    double maximum_frequency, double sweep_duration_seconds,
                    double elapsed_seconds);

} // namespace MB_DDF::HWTest::Detail
