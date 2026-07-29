#pragma once

#include "HelmControl/ProtocolModel/helm_fdb_frame_protocol.h"
#include "HelmControl/ProtocolModel/helm_ins_frame_protocol.h"
#include "MB_DDF_HW_Test/ProductProtocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace MB_DDF::HWTest {

inline constexpr std::chrono::microseconds kHelmCommandPeriod{1000};
inline constexpr size_t kHelmFeedbackBatchMaximum = 5;

struct HelmStreamParameters {
    uint32_t waveform{0};
    double frequency_hz{1.0};
    double amplitude_deg{1.8};
    double offset_deg{0.0};
    double start_phase_radians{0.0};
    double maximum_frequency_hz{80.0};
    double sweep_duration_seconds{25.0};
    uint8_t enable_mask{0x0F};
};

struct HelmFeedbackSample {
    uint64_t timestamp_us{0};
    ProtocolModel::Helm_fdb_frame frame{};
};

bool validate_helm_stream_parameters(const HelmStreamParameters& parameters) noexcept;

double helm_command_value(const HelmStreamParameters& parameters,
                          double elapsed_seconds) noexcept;

std::array<float, 4> helm_channel_commands(
    const HelmStreamParameters& parameters,
    double elapsed_seconds) noexcept;

ProductErrorCode populate_helm_feedback_batch(
    std::span<const HelmFeedbackSample> samples,
    ProductMessage& response);

class IHelmDdsEndpoint {
public:
    using FeedbackCallback = std::function<void(
        const void* data, size_t size, uint64_t timestamp_us)>;

    virtual ~IHelmDdsEndpoint() = default;
    virtual bool open(FeedbackCallback callback, std::string* error) = 0;
    virtual bool publish_command(std::span<const char> bytes,
                                 std::string* error) = 0;
    virtual void close() noexcept = 0;
};

class HelmDdsTestBridge final {
public:
    explicit HelmDdsTestBridge(std::unique_ptr<IHelmDdsEndpoint> endpoint = {});
    ~HelmDdsTestBridge();

    HelmDdsTestBridge(const HelmDdsTestBridge&) = delete;
    HelmDdsTestBridge& operator=(const HelmDdsTestBridge&) = delete;

    ProductErrorCode start(const HelmStreamParameters& parameters);
    ProductErrorCode stop();
    bool active() const noexcept;
    std::optional<ProductErrorCode> poll_feedback(ProductMessage& response);

private:
    bool publish_command_frame(const std::array<float, 4>& commands,
                               std::string* error);
    bool publish_neutral_command(std::string* error);
    void command_loop();
    void receive_feedback(const void* data, size_t size, uint64_t timestamp_us);

    std::unique_ptr<IHelmDdsEndpoint> endpoint_;
    HelmStreamParameters parameters_{};
    std::thread command_thread_;
    std::atomic_bool active_{false};
    std::atomic_bool publish_failed_{false};
    std::atomic_uint16_t next_serial_{0};
    bool endpoint_open_{false};
    std::chrono::steady_clock::time_point started_{};
    mutable std::mutex feedback_mutex_;
    std::deque<HelmFeedbackSample> feedback_;
};

} // namespace MB_DDF::HWTest
