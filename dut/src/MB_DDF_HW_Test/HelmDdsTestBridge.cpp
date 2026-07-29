#include "MB_DDF_HW_Test/HelmDdsTestBridge.h"

#include "HelmControl/ProtocolModel/helm_command_contract.h"
#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/Debug/Logger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace MB_DDF::HWTest {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaximumQueuedFeedback = 4096;

class DdsHelmEndpoint final : public IHelmDdsEndpoint {
public:
    bool open(FeedbackCallback callback, std::string* error) override {
        auto& dds = DDS::DDSCore::instance();
        if (!dds.initialize()) {
            if (error != nullptr) *error = "DDSCore initialization failed";
            return false;
        }
        command_writer_ = dds.create_writer("local:://helm_command", false);
        feedback_reader_ = dds.create_reader(
            "local:://helm_feedback", false,
            [callback = std::move(callback)](
                const void* data, size_t size, uint64_t timestamp_ns) {
                if (callback) callback(data, size, timestamp_ns / 1000u);
            });
        if (!command_writer_ || !feedback_reader_) {
            if (error != nullptr) *error = "cannot create helm DDS endpoints";
            close();
            return false;
        }
        if (error != nullptr) error->clear();
        return true;
    }

    bool publish_command(std::span<const char> bytes,
                         std::string* error) override {
        if (!command_writer_) {
            if (error != nullptr) *error = "helm command writer is closed";
            return false;
        }
        const bool written = command_writer_->write(bytes.data(), bytes.size());
        if (!written) {
            if (error != nullptr) *error = "helm DDS command write was incomplete";
            return false;
        }
        if (error != nullptr) error->clear();
        return true;
    }

    void close() noexcept override {
        feedback_reader_.reset();
        command_writer_.reset();
    }

private:
    std::shared_ptr<DDS::Publisher> command_writer_;
    std::shared_ptr<DDS::Subscriber> feedback_reader_;
};

bool set_feedback_sample(ProductMessage& response,
                         size_t index,
                         const HelmFeedbackSample& sample,
                         uint64_t first_timestamp) {
    const std::string prefix = "sample[" + std::to_string(index) + "].";
    const uint64_t delta = sample.timestamp_us - first_timestamp;
    const auto& frame = sample.frame;
    bool ok = response.set_unsigned(prefix + "delta_us", delta) &&
              response.set_unsigned(prefix + "serial_b", frame.serial_b) &&
              response.set_unsigned(prefix + "version", frame.version) &&
              response.set_unsigned(prefix + "self_check", frame.bitGroup1.self_check) &&
              response.set_unsigned(prefix + "self_check_1", frame.bitGroup1.bit1) &&
              response.set_unsigned(prefix + "self_check_2", frame.bitGroup1.bit2) &&
              response.set_unsigned(prefix + "self_check_3", frame.bitGroup1.bit3) &&
              response.set_unsigned(prefix + "self_check_4", frame.bitGroup1.bit4) &&
              response.set_unsigned(prefix + "self_check_combined", frame.bitGroup1.bit) &&
              response.set_unsigned(prefix + "self_check_reserved", 0) &&
              response.set_unsigned(prefix + "timeout", frame.timeout) &&
              response.set_unsigned(prefix + "serial_a", frame.serial_a);
    for (size_t channel = 0; channel < 4; ++channel) {
        ok = response.set_float(
                 prefix + "fdb[" + std::to_string(channel) + "]",
                 frame.fdb[channel]) && ok;
        ok = response.set_float(
                 prefix + "ins[" + std::to_string(channel) + "]",
                 frame.ins[channel]) && ok;
    }
    return ok;
}

} // namespace

bool validate_helm_stream_parameters(
    const HelmStreamParameters& parameters) noexcept {
    return parameters.waveform <= 4u && parameters.enable_mask <= 0x0Fu &&
           std::isfinite(parameters.frequency_hz) &&
           std::isfinite(parameters.amplitude_deg) &&
           std::isfinite(parameters.offset_deg) &&
           std::isfinite(parameters.start_phase_radians) &&
           std::isfinite(parameters.maximum_frequency_hz) &&
           std::isfinite(parameters.sweep_duration_seconds) &&
           parameters.frequency_hz > 0.0 &&
           parameters.maximum_frequency_hz > 0.0 &&
           parameters.sweep_duration_seconds > 0.0;
}

double helm_command_value(const HelmStreamParameters& parameters,
                          double elapsed_seconds) noexcept {
    if (!validate_helm_stream_parameters(parameters) ||
        !std::isfinite(elapsed_seconds)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double time = std::max(0.0, elapsed_seconds);
    const double fixed_phase = 2.0 * kPi * parameters.frequency_hz * time +
                               parameters.start_phase_radians;
    switch (parameters.waveform) {
    case 0:
        return parameters.offset_deg +
               parameters.amplitude_deg * std::sin(fixed_phase);
    case 1:
        return parameters.offset_deg + parameters.amplitude_deg *
               (std::sin(fixed_phase) >= 0.0 ? 1.0 : -1.0);
    case 2: {
        double normalized = std::fmod(fixed_phase / (2.0 * kPi), 1.0);
        if (normalized < 0.0) normalized += 1.0;
        const double triangle = normalized < 0.25 ? normalized * 4.0
                                : normalized < 0.75 ? 2.0 - normalized * 4.0
                                                    : normalized * 4.0 - 4.0;
        return parameters.offset_deg + parameters.amplitude_deg * triangle;
    }
    case 3:
        return parameters.offset_deg;
    case 4: {
        if (time > parameters.sweep_duration_seconds) return 0.0;
        double sweep_phase = 0.0;
        const double gap = parameters.maximum_frequency_hz -
                           parameters.frequency_hz;
        if (std::abs(gap) < 1.0e-12) {
            sweep_phase = 2.0 * kPi * parameters.frequency_hz * time;
        } else {
            const double scaled_end = parameters.maximum_frequency_hz *
                                      parameters.sweep_duration_seconds;
            const double denominator = scaled_end - time * gap;
            if (denominator <= 0.0) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            sweep_phase = 2.0 * kPi * parameters.frequency_hz *
                          parameters.maximum_frequency_hz *
                          parameters.sweep_duration_seconds / gap *
                          std::log(scaled_end / denominator);
        }
        return parameters.offset_deg + parameters.amplitude_deg *
               std::sin(sweep_phase + parameters.start_phase_radians);
    }
    default:
        return std::numeric_limits<double>::quiet_NaN();
    }
}

std::array<float, 4> helm_channel_commands(
    const HelmStreamParameters& parameters,
    double elapsed_seconds) noexcept {
    std::array<float, 4> result{};
    const double command = helm_command_value(parameters, elapsed_seconds);
    if (!std::isfinite(command)) {
        result.fill(std::numeric_limits<float>::quiet_NaN());
        return result;
    }
    for (size_t channel = 0; channel < result.size(); ++channel) {
        result[channel] = (parameters.enable_mask & (1u << channel)) != 0u
            ? static_cast<float>(command) : 0.0F;
    }
    return result;
}

ProductErrorCode populate_helm_feedback_batch(
    std::span<const HelmFeedbackSample> samples,
    ProductMessage& response) {
    if (!response || samples.empty() ||
        samples.size() > kHelmFeedbackBatchMaximum) {
        return ProductErrorCode::ParamOutOfRange;
    }
    const uint64_t first_timestamp = samples.front().timestamp_us;
    if (!response.set_unsigned("sample_count", samples.size()) ||
        !response.set_unsigned("first_timestamp_us_low",
                               first_timestamp & 0xFFFFFFFFu) ||
        !response.set_unsigned("first_timestamp_us_high",
                               first_timestamp >> 32)) {
        return ProductErrorCode::TaskExecFailed;
    }
    for (size_t index = 0; index < samples.size(); ++index) {
        if (samples[index].timestamp_us < first_timestamp ||
            samples[index].timestamp_us - first_timestamp > 0xFFFFu) {
            return ProductErrorCode::ParamOutOfRange;
        }
        if (!set_feedback_sample(response, index, samples[index], first_timestamp)) {
            return ProductErrorCode::TaskExecFailed;
        }
    }
    return ProductErrorCode::Ok;
}

HelmDdsTestBridge::HelmDdsTestBridge(
    std::unique_ptr<IHelmDdsEndpoint> endpoint)
    : endpoint_(endpoint ? std::move(endpoint)
                         : std::make_unique<DdsHelmEndpoint>()) {}

HelmDdsTestBridge::~HelmDdsTestBridge() {
    (void)stop();
}

ProductErrorCode HelmDdsTestBridge::start(
    const HelmStreamParameters& parameters) {
    if (!validate_helm_stream_parameters(parameters)) {
        return ProductErrorCode::ParamOutOfRange;
    }
    if (active_.load(std::memory_order_acquire)) {
        return ProductErrorCode::TaskBusy;
    }
    if (command_thread_.joinable()) command_thread_.join();
    {
        std::lock_guard<std::mutex> lock(feedback_mutex_);
        feedback_.clear();
    }
    parameters_ = parameters;
    publish_failed_.store(false, std::memory_order_release);
    next_serial_.store(0, std::memory_order_release);
    std::string error;
    if (!endpoint_->open(
            [this](const void* data, size_t size, uint64_t timestamp_us) {
                receive_feedback(data, size, timestamp_us);
            },
            &error)) {
        LOG_ERROR << "[HW-TEST] 打开舵控 DDS 端点失败：" << error;
        endpoint_->close();
        return ProductErrorCode::HelmDdsFailed;
    }
    endpoint_open_ = true;
    if (!publish_neutral_command(&error)) {
        LOG_ERROR << "[HW-TEST] 发布舵控 DDS 解锁首帧失败：" << error;
        endpoint_->close();
        endpoint_open_ = false;
        return ProductErrorCode::HelmDdsFailed;
    }
    started_ = std::chrono::steady_clock::now();
    active_.store(true, std::memory_order_release);
    command_thread_ = std::thread([this] { command_loop(); });
    return ProductErrorCode::Ok;
}

ProductErrorCode HelmDdsTestBridge::stop() {
    active_.store(false, std::memory_order_release);
    if (command_thread_.joinable()) command_thread_.join();
    ProductErrorCode result = ProductErrorCode::Ok;
    if (endpoint_ && endpoint_open_) {
        std::string error;
        if (!publish_neutral_command(&error)) {
            LOG_ERROR << "[HW-TEST] 发布舵控 DDS 回零尾帧失败：" << error;
            result = ProductErrorCode::HelmDdsFailed;
        }
        endpoint_->close();
        endpoint_open_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(feedback_mutex_);
        feedback_.clear();
    }
    return result;
}

bool HelmDdsTestBridge::active() const noexcept {
    return active_.load(std::memory_order_acquire);
}

std::optional<ProductErrorCode> HelmDdsTestBridge::poll_feedback(
    ProductMessage& response) {
    if (publish_failed_.exchange(false, std::memory_order_acq_rel)) {
        return ProductErrorCode::HelmDdsFailed;
    }
    if (!active()) return std::nullopt;
    std::array<HelmFeedbackSample, kHelmFeedbackBatchMaximum> batch{};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(feedback_mutex_);
        if (feedback_.empty()) return std::nullopt;
        const uint64_t first_timestamp = feedback_.front().timestamp_us;
        while (!feedback_.empty() && count < batch.size()) {
            const uint64_t timestamp = feedback_.front().timestamp_us;
            if (timestamp < first_timestamp ||
                timestamp - first_timestamp > 0xFFFFu) {
                break;
            }
            batch[count++] = std::move(feedback_.front());
            feedback_.pop_front();
        }
    }
    if (count == 0) return ProductErrorCode::TaskExecFailed;
    return populate_helm_feedback_batch(
        std::span<const HelmFeedbackSample>(batch.data(), count), response);
}

bool HelmDdsTestBridge::publish_command_frame(
    const std::array<float, 4>& commands,
    std::string* error) {
    ProtocolModel::Helm_ins_frame frame{};
    frame.serial_a = next_serial_.fetch_add(1, std::memory_order_relaxed);
    frame.Q = 0.0F;
    frame.temp_imu = 30.0;
    frame.temp_ground = 30.0;
    frame.helm_unlock = ProtocolModel::kHelmUnlockRequested;
    std::copy(commands.begin(), commands.end(), frame.ins);
    const auto bytes = ProtocolModel::Helm_ins_frameProtocol::packFrame(frame);
    return endpoint_->publish_command(
        std::span<const char>(bytes.data(), bytes.size()), error);
}

bool HelmDdsTestBridge::publish_neutral_command(std::string* error) {
    return publish_command_frame(std::array<float, 4>{}, error);
}

void HelmDdsTestBridge::command_loop() {
    auto next = started_;
    while (active_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - started_).count();
        const auto commands = helm_channel_commands(parameters_, elapsed);
        if (!std::all_of(commands.begin(), commands.end(),
                         [](float value) { return std::isfinite(value); })) {
            publish_failed_.store(true, std::memory_order_release);
            break;
        }
        std::string error;
        if (!publish_command_frame(commands, &error)) {
            LOG_ERROR << "[HW-TEST] 发布舵控 DDS 指令失败：" << error;
            publish_failed_.store(true, std::memory_order_release);
            break;
        }
        next += kHelmCommandPeriod;
        std::this_thread::sleep_until(next);
    }
}

void HelmDdsTestBridge::receive_feedback(
    const void* data, size_t size, uint64_t timestamp_us) {
    if (!active_.load(std::memory_order_acquire)) return;
    HelmFeedbackSample sample{};
    sample.timestamp_us = timestamp_us;
    if (!ProtocolModel::Helm_fdb_frameProtocol::unpackFrame(
            static_cast<const char*>(data), size, sample.frame)) {
        LOG_WARN << "[HW-TEST] 丢弃长度错误的舵控 DDS 反馈：" << size;
        return;
    }
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    if (feedback_.size() >= kMaximumQueuedFeedback) feedback_.pop_front();
    feedback_.push_back(std::move(sample));
}

} // namespace MB_DDF::HWTest
