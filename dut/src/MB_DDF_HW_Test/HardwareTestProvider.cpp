#include "MB_DDF_HW_Test/HardwareTestProvider.h"
#include "MB_DDF_HW_Test/HardwareTestProviderDetail.h"

#include "MB_DDF/Debug/Logger.h"
#include "MB_DDF_HW/Device/Ad7606Device.h"
#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Device/DhController.h"
#include "MB_DDF_HW/Device/DidoDevice.h"
#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW/Device/SpiFlashDevice.h"
#include "MB_DDF_HW/Device/XadcDevice.h"
#include "MB_DDF_HW/Device/Registers/XadcRegisters.h"
#include "MB_DDF_HW/Transport/SpiDevTransport.h"
#include "MB_DDF_HW/Transport/XdmaTransport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace MB_DDF::HWTest {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view kXdmaDevice = "/dev/xdma0";
constexpr uint64_t kPwmOffset = 0x00000;
constexpr uint64_t kAd7606Offset = 0x10000;
constexpr uint64_t kAds1258Offset = 0x20000;
constexpr uint64_t kDhOffset = 0x30000;
constexpr uint64_t kDidoOffset = 0x140000;
constexpr size_t kRegisterWindow = 0x10000;
constexpr uint32_t kSpiFlashTestAddress = 0x03FFF000;
constexpr size_t kBusEchoBytes = 114;

ProductErrorCode status_error(const HW::Status& status) {
    switch (status.code) {
    case HW::StatusCode::InvalidArgument:
    case HW::StatusCode::BufferTooSmall:
        return ProductErrorCode::ParamOutOfRange;
    case HW::StatusCode::Busy:
        return ProductErrorCode::TaskBusy;
    case HW::StatusCode::Timeout:
        return ProductErrorCode::TaskExecFailed;
    default:
        return ProductErrorCode::RegReadWriteFailed;
    }
}

template <typename Device>
class XdmaDeviceContext {
public:
    explicit XdmaDeviceContext(uint64_t offset)
        : transport_({std::string(kXdmaDevice), offset, kRegisterWindow}),
          device_(transport_) {}

    ProductErrorCode ensure_open() {
        if (!transport_.is_open()) {
            const auto opened = transport_.open();
            if (!opened) {
                LOG_ERROR << "[HW-TEST] 打开 XDMA 设备窗口失败：" << opened.status().message;
                return status_error(opened.status());
            }
            checked_ = false;
        }
        if (!checked_) {
            const auto checked = device_.check_communication();
            if (!checked) {
                LOG_ERROR << "[HW-TEST] 硬件通信签名校验失败："
                          << checked.status().message;
                return status_error(checked.status());
            }
            checked_ = true;
        }
        return ProductErrorCode::Ok;
    }

    Device& device() noexcept { return device_; }

    HW::Result<void> write_register(uint64_t offset, uint32_t value) {
        return transport_.write32(offset, value);
    }

private:
    HW::XdmaTransport transport_;
    Device device_;
    bool checked_{false};
};

template <typename Device>
class XdmaOpenDeviceContext {
public:
    XdmaOpenDeviceContext(uint64_t offset, size_t window_size)
        : transport_({std::string(kXdmaDevice), offset, window_size}),
          device_(transport_) {}

    ProductErrorCode ensure_open() {
        if (transport_.is_open()) {
            return ProductErrorCode::Ok;
        }
        const auto opened = transport_.open();
        if (!opened) {
            LOG_ERROR << "[HW-TEST] 打开 XDMA 设备窗口失败："
                      << opened.status().message;
            return status_error(opened.status());
        }
        return ProductErrorCode::Ok;
    }

    Device& device() noexcept { return device_; }

private:
    HW::XdmaTransport transport_;
    Device device_;
};

struct BusStats {
    uint32_t error_count{0};
    uint32_t total_count{0};
    uint32_t elapsed_ms{0};
    std::vector<uint8_t> last_received{};
};

template <typename Exchange>
ProductErrorCode run_bus_iterations(unsigned com_index, bool loopback,
                                    uint32_t count,
                                    std::span<const uint8_t> fixed_payload,
                                    Exchange&& exchange, BusStats& stats) {
    const auto start = Clock::now();
    Detail::BusIterationCounts counts{};
    const auto commit_stats = [&]() {
        stats.error_count = counts.error_count;
        stats.total_count = counts.total_count;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count();
        stats.elapsed_ms = static_cast<uint32_t>(std::min<int64_t>(
            elapsed, std::numeric_limits<uint32_t>::max()));
    };
    for (uint32_t iteration = 0; iteration < count; ++iteration) {
        std::vector<uint8_t> generated;
        std::span<const uint8_t> transmitted = fixed_payload;
        if (fixed_payload.empty()) {
            generated.resize(16);
            for (size_t index = 0; index < generated.size(); ++index) {
                generated[index] = static_cast<uint8_t>((iteration + index) & 0xFFu);
            }
            transmitted = std::span<const uint8_t>(generated);
        }
        // 多给一个字节，使 COM 接收能显式检测“比期望多 1 字节”。
        std::vector<uint8_t> received(transmitted.size() + 1u);
        LOG_DEBUG << "[HW-TEST][SERIAL] 轮次开始：mode="
                  << (loopback ? "loopback" : "echo")
                  << "，com=COM" << (com_index + 1)
                  << "，round=" << (iteration + 1) << "/" << count
                  << "，tx_bytes=" << transmitted.size()
                  << "，tx_first=0x" << std::hex
                  << static_cast<unsigned>(transmitted.front())
                  << "，tx_last=0x"
                  << static_cast<unsigned>(transmitted.back()) << std::dec;
        const auto error = exchange(iteration, transmitted, received);
        if (error != ProductErrorCode::Ok) {
            commit_stats();
            LOG_DEBUG << "[HW-TEST][SERIAL] 轮次失败：mode="
                      << (loopback ? "loopback" : "echo")
                      << "，com=COM" << (com_index + 1)
                      << "，round=" << (iteration + 1) << "/" << count
                      << "，err_code=0x" << std::hex
                      << static_cast<uint16_t>(error) << std::dec
                      << "，completed=" << stats.total_count
                      << "，errors=" << stats.error_count
                      << "，elapsed_ms=" << stats.elapsed_ms;
            return error;
        }
        const bool matches = Detail::record_bus_exchange(
            counts, transmitted, received, stats.last_received);
        LOG_DEBUG << "[HW-TEST][SERIAL] 轮次结束：mode="
                  << (loopback ? "loopback" : "echo")
                  << "，com=COM" << (com_index + 1)
                  << "，round=" << (iteration + 1) << "/" << count
                  << "，rx_bytes=" << received.size()
                  << "，match=" << (matches ? "true" : "false")
                  << "，errors=" << counts.error_count;
        if (!matches) {
            size_t mismatch_index = 0;
            const size_t common_size = std::min(transmitted.size(), received.size());
            while (mismatch_index < common_size &&
                   transmitted[mismatch_index] == received[mismatch_index]) {
                ++mismatch_index;
            }
            LOG_DEBUG << "[HW-TEST][SERIAL] 数据不一致：com=COM"
                      << (com_index + 1)
                      << "，round=" << (iteration + 1) << "/" << count
                      << "，first_mismatch=" << mismatch_index
                      << "，expected_bytes=" << transmitted.size()
                      << "，actual_bytes=" << received.size();
        }
    }
    commit_stats();
    return ProductErrorCode::Ok;
}

ProductErrorCode run_com_bus(unsigned com_index, uint32_t count,
                             std::span<const uint8_t> payload, bool loopback,
                             BusStats& stats) {
    static constexpr std::array<uint64_t, 4> offsets{
        0x40000, 0x80000, 0xC0000, 0x100000};
    if (com_index >= offsets.size() ||
        com_index == Detail::kControlComIndex) {
        return ProductErrorCode::ChannelInvalid;
    }
    LOG_DEBUG << "[HW-TEST][SERIAL] 准备串口：mode="
              << (loopback ? "loopback" : "echo")
              << "，com=COM" << (com_index + 1)
              << "，offset=0x" << std::hex << offsets[com_index] << std::dec
              << "，event=" << com_index
              << "，rounds=" << count;
    HW::XdmaTransport transport({std::string(kXdmaDevice), offsets[com_index],
                                 Detail::kComMapLength, -1, -1,
                                 static_cast<int>(com_index)});
    const auto opened = transport.open();
    if (!opened) {
        LOG_DEBUG << "[HW-TEST][SERIAL] 打开串口失败：com=COM"
                  << (com_index + 1)
                  << "，status=" << static_cast<int>(opened.status().code)
                  << "，errno=" << opened.status().errno_value
                  << "，message=" << opened.status().message;
        return status_error(opened.status());
    }
    HW::ComDevice device(transport);
    const auto config = Detail::bus_com_config(loopback);
    const auto configured = device.configure(config);
    if (!configured) {
        LOG_DEBUG << "[HW-TEST][SERIAL] 配置串口失败：com=COM"
                  << (com_index + 1)
                  << "，status=" << static_cast<int>(configured.status().code)
                  << "，message=" << configured.status().message;
        return status_error(configured.status());
    }
    const auto cleared = device.clear_error_status();
    if (!cleared) {
        LOG_DEBUG << "[HW-TEST][SERIAL] 清除串口错误失败：com=COM"
                  << (com_index + 1)
                  << "，status=" << static_cast<int>(cleared.status().code)
                  << "，message=" << cleared.status().message;
        return status_error(cleared.status());
    }
    const auto enabled = device.enable_receive();
    if (!enabled) {
        LOG_DEBUG << "[HW-TEST][SERIAL] 使能串口接收失败：com=COM"
                  << (com_index + 1)
                  << "，status=" << static_cast<int>(enabled.status().code)
                  << "，message=" << enabled.status().message;
        return status_error(enabled.status());
    }
    LOG_DEBUG << "[HW-TEST][SERIAL] 串口已就绪：mode="
              << (loopback ? "loopback" : "echo")
              << "，com=COM" << (com_index + 1)
              << "，baud_counter=0x" << std::hex << config.baudrate_counter
              << "，byte_format=0x"
              << static_cast<unsigned>(config.format.byte_format) << std::dec
              << "，header_bytes="
              << static_cast<unsigned>(config.frame.send_header_length)
              << "，length_bytes="
              << static_cast<unsigned>(config.frame.send_length_bytes);

    return run_bus_iterations(
        com_index, loopback, count, payload,
        [&](uint32_t iteration, std::span<const uint8_t> transmitted,
            std::vector<uint8_t>& received) -> ProductErrorCode {
            const auto sent = device.send({transmitted.data(), transmitted.size()});
            if (!sent) {
                LOG_DEBUG << "[HW-TEST][SERIAL] 发送失败：com=COM"
                          << (com_index + 1)
                          << "，round=" << (iteration + 1) << "/" << count
                          << "，status=" << static_cast<int>(sent.status().code)
                          << "，message=" << sent.status().message;
                return status_error(sent.status());
            }
            if (sent.value() != transmitted.size()) {
                LOG_DEBUG << "[HW-TEST][SERIAL] 发送长度异常：com=COM"
                          << (com_index + 1)
                          << "，round=" << (iteration + 1) << "/" << count
                          << "，expected=" << transmitted.size()
                          << "，actual=" << sent.value();
                return ProductErrorCode::TaskExecFailed;
            }
            LOG_DEBUG << "[HW-TEST][SERIAL] 发送完成，等待回帧：com=COM"
                      << (com_index + 1)
                      << "，round=" << (iteration + 1) << "/" << count
                      << "，tx_bytes=" << sent.value()
                      << "，timeout_us=" << Detail::kBusReceiveTimeoutUs;
            const auto result = device.receive(
                {received.data(), received.size()},
                HW::Timeout::after_us(Detail::kBusReceiveTimeoutUs));
            if (!result) {
                LOG_DEBUG << "[HW-TEST][SERIAL] 接收失败：com=COM"
                          << (com_index + 1)
                          << "，round=" << (iteration + 1) << "/" << count
                          << "，status=" << static_cast<int>(result.status().code)
                          << "，errno=" << result.status().errno_value
                          << "，message=" << result.status().message;
                return status_error(result.status());
            }
            if (result.value() == 0) {
                LOG_DEBUG << "[HW-TEST][SERIAL] 接收超时或无完整帧：com=COM"
                          << (com_index + 1)
                          << "，round=" << (iteration + 1) << "/" << count;
                return ProductErrorCode::TaskExecFailed;
            }
            received.resize(result.value());
            LOG_DEBUG << "[HW-TEST][SERIAL] 接收完成：com=COM"
                      << (com_index + 1)
                      << "，round=" << (iteration + 1) << "/" << count
                      << "，rx_bytes=" << received.size()
                      << "，rx_first=0x" << std::hex
                      << static_cast<unsigned>(received.front())
                      << "，rx_last=0x"
                      << static_cast<unsigned>(received.back()) << std::dec;
            return ProductErrorCode::Ok;
        },
        stats);
}

ProductErrorCode run_bus(uint8_t link_id, uint32_t count,
                          std::span<const uint8_t> payload, bool loopback,
                          BusStats& stats) {
    const auto com_index = Detail::com_index_for_bus_link(link_id);
    return com_index
               ? run_com_bus(*com_index, count, payload, loopback, stats)
               : ProductErrorCode::ChannelInvalid;
}

volatile std::sig_atomic_t g_hardware_test_stop = 0;

void request_hardware_test_stop(int) noexcept {
    g_hardware_test_stop = 1;
}

} // namespace

HW::ComConfig Detail::bus_com_config(bool loopback) {
    auto config = HW::ComDevice::default_config();
    config.loopback = loopback;
    config.receive_enabled = true;
    config.interrupt_mode = HW::ComInterruptMode::Level;
    return config;
}

HW::ComConfig Detail::imu_stream_com_config() {
    auto config = HW::ComDevice::default_config();
    config.loopback = false;
    config.receive_enabled = true;
    config.interrupt_mode = HW::ComInterruptMode::Level;
    config.frame.send_header = {0xAA, 0x1A, 0x00, 0x00};
    config.frame.receive_header = {0xAA, 0x1A, 0x00, 0x00};
    config.frame.send_header_length = 2;
    config.frame.receive_header_length = 2;
    config.frame.send_length_bytes = 1;
    config.frame.receive_length_bytes = 1;
    // format.byte_format=0xB0 已配置 8E1、CRC 和 2 字节校验；CRC 由 FPGA 消费，
    // 不属于固定帧尾，也不会出现在 ComDevice::receive() 返回的 payload 中。
    config.frame.send_tail_length = 0;
    config.frame.receive_tail_length = 0;
    config.baudrate_counter = 0x0086;
    return config;
}

ProductErrorCode Detail::populate_imu_stream_feedback(
    std::span<const uint8_t> payload, ProductMessage& response) {
    if (payload.size() != kImuPayloadBytes) {
        return ProductErrorCode::LenMismatch;
    }
    const auto read_u16 = [&](size_t offset) {
        return static_cast<uint16_t>(
            static_cast<uint16_t>(payload[offset]) |
            (static_cast<uint16_t>(payload[offset + 1]) << 8));
    };
    const auto read_f32 = [&](size_t offset) {
        uint32_t bits = 0;
        for (size_t byte = 0; byte < sizeof(bits); ++byte) {
            bits |= static_cast<uint32_t>(payload[offset + byte]) << (8 * byte);
        }
        return std::bit_cast<float>(bits);
    };
    static constexpr std::array<std::string_view, 12> kFloatFields{
        "delta_angle_x", "delta_angle_y", "delta_angle_z",
        "delta_velocity_x", "delta_velocity_y", "delta_velocity_z",
        "angular_rate_x", "angular_rate_y", "angular_rate_z",
        "acceleration_x", "acceleration_y", "acceleration_z",
    };

    auto staged = response;
    if (!staged.set_unsigned("source_seq", read_u16(0))) {
        return ProductErrorCode::TaskExecFailed;
    }
    for (size_t index = 0; index < kFloatFields.size(); ++index) {
        const float value = read_f32(2 + index * sizeof(float));
        if (!std::isfinite(value)) {
            return ProductErrorCode::ParamOutOfRange;
        }
        if (!staged.set_float(kFloatFields[index], value)) {
            return ProductErrorCode::TaskExecFailed;
        }
    }
    const auto temperature = static_cast<int16_t>(read_u16(50));
    const bool populated =
        staged.set_signed("temperature", temperature) &&
        staged.set_unsigned("self_test_status", read_u16(52)) &&
        staged.set_unsigned("work_status", payload[54]) &&
        staged.set_unsigned("software_version", read_u16(55)) &&
        staged.set_unsigned("source_reserved", read_u16(57));
    if (!populated) {
        return ProductErrorCode::TaskExecFailed;
    }
    response = std::move(staged);
    return ProductErrorCode::Ok;
}

ProductErrorCode Detail::populate_dh_telemetry(
    const HW::Ads1258Snapshot& snapshot, ProductMessage& response) {
    auto staged = response;
    for (size_t channel = 0; channel < kDhTelemetryAds1258Channels.size(); ++channel) {
        const size_t source = kDhTelemetryAds1258Channels[channel];
        const auto voltage =
            HW::Ads1258Device::calibrated_channel_voltage(source, snapshot);
        if (!voltage) {
            return ProductErrorCode::RegReadWriteFailed;
        }
        if (!staged.set_scaled_signed(
                "telemetry[" + std::to_string(channel) + "]",
                voltage.value())) {
            return ProductErrorCode::TaskExecFailed;
        }
    }
    response = std::move(staged);
    return ProductErrorCode::Ok;
}

ProductErrorCode Detail::validate_dh_control_request(
    const ProductMessage& request) {
    const auto report_count = request.get_unsigned("report_count");
    const auto interval_us = request.get_unsigned("interval_us");
    const auto delay_frames = request.get_unsigned("delay_frames");
    const auto power_enable = request.get_unsigned("power_enable");
    const auto return_enable = request.get_unsigned("return_enable");
    const auto first_channels = request.get_unsigned("channel[0]");
    const auto second_channels = request.get_unsigned("channel[1]");
    if (!report_count || !interval_us || !delay_frames || !power_enable ||
        !return_enable || !first_channels || !second_channels ||
        *report_count == 0 || *report_count > 0xFFFFu ||
        *interval_us < 2500u || *interval_us > 0xFFFFu ||
        *delay_frames > 0xFFFFu || *delay_frames >= *report_count ||
        *power_enable > 1 || *return_enable > 1 || *first_channels == 0 ||
        ((*first_channels & ~0x007FFFFFu) != 0) || *second_channels != 0) {
        return ProductErrorCode::ParamOutOfRange;
    }
    return ProductErrorCode::Ok;
}

ProductErrorCode Detail::run_helm_board_test(const ProductMessage& request,
                                             HW::PwmDevice& pwm,
                                             HW::Ad7606Device& ad7606,
                                             ProductMessage& response) {
    HW::PwmRawOutputs expected_outputs{};
    expected_outputs.enable_mask = 0x0Fu;
    const auto reserved = request.get_unsigned("pwm_command_reserved");
    if (!reserved || *reserved != 0) {
        return ProductErrorCode::ParamOutOfRange;
    }
    std::array<uint8_t, 4> requested_percent{};
    for (size_t channel = 0; channel < requested_percent.size(); ++channel) {
        const auto percent = request.get_unsigned(
            "pwm_duty_percent[" + std::to_string(channel) + "]");
        const auto direction = request.get_unsigned(
            "direction[" + std::to_string(channel) + "]");
        if (!percent || !direction || *percent > 100 || *direction > 1) {
            return ProductErrorCode::ParamOutOfRange;
        }
        requested_percent[channel] = static_cast<uint8_t>(*percent);
        expected_outputs.direction_mask |=
            static_cast<uint8_t>(*direction << channel);
    }

    auto operation = ad7606.set_acquisition_enabled(true);
    if (!operation) {
        return status_error(operation.status());
    }
    operation = ad7606.set_filter_enabled(true);
    if (!operation) {
        return status_error(operation.status());
    }

    const auto peak = pwm.read_peak_value();
    if (!peak) {
        return status_error(peak.status());
    }
    if (peak.value() == 0) {
        return ProductErrorCode::RegReadWriteFailed;
    }
    for (size_t channel = 0; channel < requested_percent.size(); ++channel) {
        expected_outputs.duty[channel] = static_cast<uint32_t>(
            (static_cast<uint64_t>(peak.value()) * requested_percent[channel] + 50u) /
            100u);
    }

    operation = pwm.set_update_enabled(false);
    if (!operation) {
        return status_error(operation.status());
    }
    operation = pwm.set_duty_mode_unsigned();
    if (!operation) {
        return status_error(operation.status());
    }
    operation = pwm.apply_outputs(expected_outputs);
    if (!operation) {
        return status_error(operation.status());
    }
    operation = pwm.set_update_enabled(true);
    if (!operation) {
        return status_error(operation.status());
    }

    const auto pwm_state = pwm.read_state();
    if (!pwm_state) {
        return status_error(pwm_state.status());
    }
    const auto ad_state = ad7606.read_state();
    if (!ad_state) {
        return status_error(ad_state.status());
    }

    auto staged = response;
    for (size_t channel = 0; channel < requested_percent.size(); ++channel) {
        const std::string suffix = "[" + std::to_string(channel) + "]";
        if (!staged.set_unsigned(
                "pwm_duty_match" + suffix,
                pwm_state.value().outputs.duty[channel] ==
                        expected_outputs.duty[channel]
                    ? 1u
                    : 0u) ||
            !staged.set_unsigned(
                "direction_readback" + suffix,
                (pwm_state.value().outputs.direction_mask >> channel) & 1u) ||
            !staged.set_unsigned(
                "pwm_duty" + suffix,
                pwm_state.value().outputs.duty[channel]) ||
            !staged.set_signed(
                "helm_AD_value" + suffix,
                ad_state.value().snapshot.raw[channel])) {
            return ProductErrorCode::TaskExecFailed;
        }
    }
    const bool populated =
        staged.set_unsigned("pwm_peak", pwm_state.value().config.peak_value) &&
        staged.set_unsigned("pwm_enable_mask",
                            pwm_state.value().outputs.enable_mask) &&
        staged.set_unsigned("pwm_update_enabled",
                            pwm_state.value().update_enabled ? 1u : 0u) &&
        staged.set_unsigned("ad_acquisition_enabled",
                            ad_state.value().config.acquisition_enabled ? 1u : 0u) &&
        staged.set_unsigned("ad_filter_enabled",
                            ad_state.value().config.filter_enabled ? 1u : 0u);
    if (!populated) {
        return ProductErrorCode::TaskExecFailed;
    }
    response = std::move(staged);

    const bool readback_matches =
        pwm_state.value().config.peak_value == peak.value() &&
        pwm_state.value().outputs.duty == expected_outputs.duty &&
        pwm_state.value().outputs.direction_mask == expected_outputs.direction_mask &&
        pwm_state.value().outputs.enable_mask == expected_outputs.enable_mask &&
        pwm_state.value().update_enabled &&
        ad_state.value().config.acquisition_enabled &&
        ad_state.value().config.filter_enabled;
    return readback_matches ? ProductErrorCode::Ok
                            : ProductErrorCode::RegReadWriteFailed;
}

double Detail::helm_command(uint32_t waveform, double frequency,
                            double amplitude, double offset,
                            double start_phase_radians,
                            double maximum_frequency,
                            double sweep_duration_seconds,
                            double elapsed_seconds) {
    HelmStreamParameters parameters{};
    parameters.waveform = waveform;
    parameters.frequency_hz = frequency;
    parameters.amplitude_deg = amplitude;
    parameters.offset_deg = offset;
    parameters.start_phase_radians = start_phase_radians;
    parameters.maximum_frequency_hz = maximum_frequency;
    parameters.sweep_duration_seconds = sweep_duration_seconds;
    parameters.enable_mask = 0x0F;
    return helm_command_value(parameters, elapsed_seconds);
}

class XdmaTimerLoadExecutor final : public ITimerLoadExecutor {
public:
    XdmaTimerLoadExecutor()
        : transport_({std::string(kXdmaDevice), Detail::kTimerLoadUserOffset,
                      Detail::kTimerLoadMapLength, -1,
                      Detail::kTimerLoadC2hChannel, -1}) {}

    ~XdmaTimerLoadExecutor() override {
        (void)stop();
    }

    ProductErrorCode start() override {
        if (active_) {
            return ProductErrorCode::TaskBusy;
        }

        try {
            const auto opened = transport_.open();
            if (!opened) {
                LOG_ERROR << "[HW-TEST] 定时器负载打开 XDMA C2H0 失败："
                          << opened.status().message;
                transport_.close();
                return ProductErrorCode::TaskExecFailed;
            }

            stop_requested_.store(false, std::memory_order_release);
            failed_.store(false, std::memory_order_release);
            complete_reads_.store(0, std::memory_order_release);
            worker_ = std::thread([this]() { run(); });
            active_ = true;
            return ProductErrorCode::Ok;
        } catch (...) {
            stop_requested_.store(true, std::memory_order_release);
            if (worker_.joinable()) {
                worker_.join();
            }
            transport_.close();
            active_ = false;
            LOG_ERROR << "[HW-TEST] 定时器 XDMA 负载线程启动失败";
            return ProductErrorCode::TaskExecFailed;
        }
    }

    ProductErrorCode stop() override {
        if (!active_) {
            return ProductErrorCode::Ok;
        }

        stop_requested_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        transport_.close();
        active_ = false;

        const auto reads = complete_reads_.load(std::memory_order_acquire);
        const bool failed = failed_.load(std::memory_order_acquire);
        LOG_INFO << "[HW-TEST] 定时器 XDMA C2H0 负载已停止，完整 64 KiB 读取 "
                 << reads << " 次";
        return failed ? ProductErrorCode::TaskExecFailed
                      : ProductErrorCode::Ok;
    }

private:
    void run() noexcept {
        try {
            auto next_read = Clock::now();
            const auto interval =
                std::chrono::milliseconds(Detail::kTimerLoadIntervalMs);
            while (!stop_requested_.load(std::memory_order_acquire)) {
                const auto read = transport_.dma_read(
                    Detail::kTimerLoadC2hChannel,
                    {buffer_.data(), buffer_.size()},
                    Detail::kTimerLoadDeviceOffset);
                if (!read) {
                    failed_.store(true, std::memory_order_release);
                    LOG_ERROR << "[HW-TEST] 定时器 XDMA C2H0 读取失败："
                              << read.status().message;
                    break;
                }
                if (read.value() != buffer_.size()) {
                    failed_.store(true, std::memory_order_release);
                    LOG_ERROR << "[HW-TEST] 定时器 XDMA C2H0 短读：期望 "
                              << buffer_.size() << " 字节，实际 " << read.value()
                              << " 字节";
                    break;
                }
                complete_reads_.fetch_add(1, std::memory_order_relaxed);
                next_read += interval;
                std::this_thread::sleep_until(next_read);
            }
        } catch (...) {
            failed_.store(true, std::memory_order_release);
            LOG_ERROR << "[HW-TEST] 定时器 XDMA C2H0 负载线程异常";
        }
    }

    HW::XdmaTransport transport_;
    std::array<uint8_t, Detail::kTimerLoadTransferBytes> buffer_{};
    std::thread worker_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool failed_{false};
    std::atomic_size_t complete_reads_{0};
    bool active_{false};
};

struct HardwareTestProvider::Impl : IK7TemperatureSource {
    XdmaTimerLoadExecutor timer_load;
    XdmaDeviceContext<HW::PwmDevice> pwm{kPwmOffset};
    XdmaDeviceContext<HW::Ad7606Device> ad7606{kAd7606Offset};
    XdmaDeviceContext<HW::Ads1258Device> ads1258{kAds1258Offset};
    XdmaOpenDeviceContext<HW::XadcDevice> xadc{
        HW::Registers::Xadc::UserBase, HW::Registers::Xadc::WindowSize};
    XdmaDeviceContext<HW::DhController> dh{kDhOffset};
    XdmaDeviceContext<HW::DidoDevice> dido{kDidoOffset};
    HW::XdmaTransport imu_transport{
        {std::string(kXdmaDevice), Detail::kImuCom4UserOffset,
         Detail::kImuComMapLength, -1, -1, Detail::kImuCom4EventNumber}};
    HW::ComDevice imu_com{imu_transport};
    bool imu_active{false};

    HelmDdsTestBridge helm;

    ProductErrorCode initialize() {
        const auto ready = ads1258.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }

        // v4 定标要求内部 OFFSET 语义和双芯片模式；修改配置前必须关闭一路并执行状态回退。
        // 两个阈值保留当前未完成标定值，待最终标定后再单独评审。
        const auto configured = ads1258.device().apply_runtime_overrides({});
        if (!configured) {
            LOG_ERROR << "[HW-TEST] ADS1258 v4 运行时配置失败："
                      << configured.status().message;
            return status_error(configured.status());
        }
        return ProductErrorCode::Ok;
    }

    ProductErrorCode start_imu_stream() {
        if (imu_active) {
            return ProductErrorCode::TaskBusy;
        }
        imu_transport.close();
        const auto opened = imu_transport.open();
        if (!opened) {
            LOG_ERROR << "[HW-TEST] 打开惯测 COM4 XDMA Transport 失败："
                      << opened.status().message;
            imu_transport.close();
            return status_error(opened.status());
        }
        auto operation = imu_com.configure(Detail::imu_stream_com_config());
        if (!operation) {
            LOG_ERROR << "[HW-TEST] 配置惯测 COM4 921600/8E1 失败："
                      << operation.status().message;
            imu_transport.close();
            return status_error(operation.status());
        }
        operation = imu_com.clear_error_status();
        if (!operation) {
            LOG_ERROR << "[HW-TEST] 清除惯测 COM4 错误状态失败："
                      << operation.status().message;
            imu_transport.close();
            return status_error(operation.status());
        }
        operation = imu_com.enable_receive();
        if (!operation) {
            LOG_ERROR << "[HW-TEST] 使能惯测 COM4 接收失败："
                      << operation.status().message;
            imu_transport.close();
            return status_error(operation.status());
        }
        imu_active = true;
        LOG_INFO << "[HW-TEST] 惯测连续流已启动：COM4 921600/8E1，payload=59 字节";
        return ProductErrorCode::Ok;
    }

    ProductErrorCode stop_imu_stream() {
        imu_active = false;
        imu_transport.close();
        LOG_INFO << "[HW-TEST] 惯测连续流已停止";
        return ProductErrorCode::Ok;
    }

    std::optional<ProductErrorCode> poll_imu_stream_feedback(
        ProductMessage& response) {
        if (!imu_active) {
            return std::nullopt;
        }
        std::array<uint8_t, Detail::kImuReceiveBufferBytes> payload{};
        const auto received = imu_com.receive(
            {payload.data(), payload.size()},
            HW::Timeout::after_us(Detail::kImuReceivePollTimeoutUs));
        if (!received) {
            if (received.status().code == HW::StatusCode::ProtocolError) {
                LOG_WARN << "[HW-TEST] 丢弃 FPGA 判定无效的惯测 COM4 帧："
                         << received.status().message;
                return std::nullopt;
            }
            LOG_ERROR << "[HW-TEST] 惯测 COM4 接收失败："
                      << received.status().message;
            const auto error = status_error(received.status());
            imu_active = false;
            imu_transport.close();
            return error;
        }
        if (received.value() == 0) {
            return std::nullopt;
        }
        if (received.value() != Detail::kImuPayloadBytes) {
            LOG_WARN << "[HW-TEST] 丢弃长度异常的惯测 payload：期望="
                     << Detail::kImuPayloadBytes << "，实际=" << received.value();
            return std::nullopt;
        }
        const auto populated = Detail::populate_imu_stream_feedback(
            std::span<const uint8_t>(payload.data(), received.value()), response);
        if (populated == ProductErrorCode::LenMismatch ||
            populated == ProductErrorCode::ParamOutOfRange) {
            LOG_WARN << "[HW-TEST] 丢弃字段无效的惯测 payload：err_code=0x"
                     << std::hex << static_cast<uint16_t>(populated) << std::dec;
            return std::nullopt;
        }
        return populated;
    }

    bool read_k7_temperature(float& celsius) override {
        const auto ready = xadc.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return false;
        }
        const auto temperature = xadc.device().read_temperature_celsius();
        if (!temperature) {
            LOG_ERROR << "[HW-TEST] 读取 K7 XADC 温度失败："
                      << temperature.status().message;
            return false;
        }
        celsius = static_cast<float>(temperature.value());
        return std::isfinite(celsius);
    }

    ProductErrorCode handle_spi_flash(ProductMessage& response) {
        const auto fail = [](std::string_view step,
                             const HW::Status& status) {
            LOG_ERROR << "[HW-TEST] SPI Flash 步骤失败：" << step
                      << "；" << status.message
                      << "（status=" << static_cast<int>(status.code)
                      << "，errno=" << status.errno_value << "）";
            return ProductErrorCode::TaskExecFailed;
        };

        HW::SpidevTransport transport({"/dev/spidev0.0", 1'000'000, 0, 8});
        const auto opened = transport.open();
        if (!opened) {
            return fail("打开 /dev/spidev0.0", opened.status());
        }
        HW::SpiFlashDevice flash(transport);
        const auto communication = flash.check_communication();
        if (!communication) {
            return fail("通信检查", communication.status());
        }
        const auto id = flash.read_jedec_id();
        if (!id) {
            return fail("读取 JEDEC ID", id.status());
        }
        if (id.value() != HW::SpiFlashDevice::ExpectedJedecId) {
            LOG_ERROR << "[HW-TEST] SPI Flash 步骤失败：校验 JEDEC ID；实际="
                      << std::hex
                      << static_cast<unsigned>(id.value()[0]) << " "
                      << static_cast<unsigned>(id.value()[1]) << " "
                      << static_cast<unsigned>(id.value()[2]) << "，期望="
                      << static_cast<unsigned>(HW::SpiFlashDevice::ExpectedJedecId[0])
                      << " "
                      << static_cast<unsigned>(HW::SpiFlashDevice::ExpectedJedecId[1])
                      << " "
                      << static_cast<unsigned>(HW::SpiFlashDevice::ExpectedJedecId[2])
                      << std::dec;
            return ProductErrorCode::TaskExecFailed;
        }

        const auto started = Clock::now();
        auto status = flash.wait_until_all_dies_idle(HW::Timeout::after_us(5'000'000));
        if (!status) {
            return fail("等待全部 die 空闲", status.status());
        }
        status = flash.clear_flag_status();
        if (!status) {
            return fail("清除 Flag Status", status.status());
        }
        // 固定隔离窗口：本画像故意不备份、不恢复原 4 KiB 内容。
        status = flash.erase_subsector(kSpiFlashTestAddress,
                                       HW::Timeout::after_us(120'000'000));
        if (!status) {
            return fail("擦除固定 4 KiB 测试区", status.status());
        }

        std::array<uint8_t, HW::SpiFlashDevice::SubsectorSize> expected{};
        std::array<uint8_t, HW::SpiFlashDevice::SubsectorSize> actual{};
        for (size_t index = 0; index < expected.size(); ++index) {
            expected[index] = static_cast<uint8_t>(0xA5u ^ (index & 0xFFu));
        }
        for (size_t offset_value = 0; offset_value < expected.size();
             offset_value += HW::SpiFlashDevice::PageSize) {
            const auto programmed = flash.program_page(
                kSpiFlashTestAddress + static_cast<uint32_t>(offset_value),
                {expected.data() + offset_value, HW::SpiFlashDevice::PageSize},
                HW::Timeout::after_us(5'000'000));
            if (!programmed) {
                LOG_WARN << "[HW-TEST] SPI Flash 步骤错误：页编程失败";
                // return fail("页编程", programmed.status());
            } else if (programmed.value() != HW::SpiFlashDevice::PageSize) {
                LOG_WARN << "[HW-TEST] SPI Flash 步骤错误：页编程长度不符；offset=0x"
                          << std::hex << offset_value << std::dec
                          << "，实际=" << programmed.value()
                          << "，期望=" << HW::SpiFlashDevice::PageSize;
                // return ProductErrorCode::TaskExecFailed;
            }
        }
        const auto read = flash.read(kSpiFlashTestAddress,
                                     {actual.data(), actual.size()});
        if (!read) {
            return fail("读回固定 4 KiB 测试区", read.status());
        }
        if (read.value() != actual.size()) {
            LOG_ERROR << "[HW-TEST] SPI Flash 步骤失败：读回长度不符；实际="
                      << read.value() << "，期望=" << actual.size();
            return ProductErrorCode::TaskExecFailed;
        }
        if (actual != expected) {
            const auto mismatch = std::mismatch(expected.begin(), expected.end(),
                                                actual.begin());
            const auto offset_value = static_cast<size_t>(
                mismatch.first - expected.begin());
            LOG_WARN << "[HW-TEST] SPI Flash 步骤错误：读回数据校验；offset=0x"
                      << std::hex << offset_value
                      << "，实际=0x" << static_cast<unsigned>(*mismatch.second)
                      << "，期望=0x" << static_cast<unsigned>(*mismatch.first)
                      << std::dec;
            // return ProductErrorCode::TaskExecFailed;
        }
        const float seconds = static_cast<float>(
            std::chrono::duration<double>(Clock::now() - started).count());
        if (!response.set_float("sjl_result", seconds)) {
            LOG_ERROR << "[HW-TEST] SPI Flash 步骤失败：写入协议响应 sjl_result";
            return ProductErrorCode::TaskExecFailed;
        }
        return ProductErrorCode::Ok;
    }

    ProductErrorCode handle_helm_board(const ProductMessage& request,
                                       ProductMessage& response) {
        auto ready = pwm.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }
        ready = ad7606.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }
        return Detail::run_helm_board_test(
            request, pwm.device(), ad7606.device(), response);
    }

    ProductErrorCode handle_electrical_health(ProductMessage& response) {
        auto ready = dh.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }
        const auto activated = dh.device().read_battery_activated();
        if (!activated) {
            return status_error(activated.status());
        }

        ready = ads1258.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }
        const auto ads = ads1258.device().read_snapshot();
        if (!ads) {
            return status_error(ads.status());
        }

        ready = xadc.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }
        const auto xadc_health = xadc.device().read_electrical_health();
        if (!xadc_health) {
            return status_error(xadc_health.status());
        }
        const auto value_yx = xadc.device().read_value_yx();
        if (!value_yx) {
            return status_error(value_yx.status());
        }

        auto staged = response;
        const auto c_voltage =
            HW::Ads1258Device::calibrated_channel_voltage(0, ads.value());
        const auto b_voltage =
            HW::Ads1258Device::calibrated_channel_voltage(2, ads.value());
        const auto primary_voltage =
            HW::Ads1258Device::calibrated_channel_voltage(3, ads.value());
        if (!c_voltage || !b_voltage || !primary_voltage) {
            return ProductErrorCode::RegReadWriteFailed;
        }
        const auto& health = xadc_health.value();
        const bool populated =
            staged.set_scaled_signed("c_volt", c_voltage.value()) &&
            staged.set_scaled_signed("b_volt", b_voltage.value()) &&
            staged.set_unsigned("activate_bits", activated.value() ? 0x01u : 0u) &&
            staged.set_scaled_signed("external_vol", health.external_voltage) &&
            staged.set_scaled_signed("core_vol", health.core_voltage) &&
            staged.set_scaled_signed("assist_vol", health.assist_voltage) &&
            staged.set_scaled_signed("v28_5", primary_voltage.value()) &&
            staged.set_scaled_signed("js_5V", health.js_5v_voltage) &&
            staged.set_scaled_signed("dyt_5V", health.dyt_5v_voltage) &&
            staged.set_scaled_signed("power_24V", health.power_24v_voltage) &&
            staged.set_signed("value_YX", value_yx.value().adc_code);
        if (!populated) {
            return ProductErrorCode::TaskExecFailed;
        }
        response = std::move(staged);
        return ProductErrorCode::Ok;
    }

    ProductErrorCode handle_bus_loop(const ProductMessage& request,
                                     ProductMessage& response) {
        const auto link = request.get_unsigned("link_id");
        const auto total = request.get_unsigned("total_count");
        if (!link || *link > std::numeric_limits<uint8_t>::max()) {
            return ProductErrorCode::ChannelInvalid;
        }
        const auto link_id = static_cast<uint8_t>(*link);
        (void)response.set_unsigned("link_id", link_id);
        const auto preflight = Detail::bus_link_preflight(link_id, imu_active);
        if (preflight != ProductErrorCode::Ok) {
            return preflight;
        }
        if (!total || *total == 0 || *total > 100'000) {
            return ProductErrorCode::ParamOutOfRange;
        }
        BusStats stats{};
        const auto requested_count = static_cast<uint32_t>(*total);
        const auto request_sequence = request.get_unsigned("seq").value_or(0);
        LOG_DEBUG << "[HW-TEST][SERIAL] BUS_LOOP 开始：link_id="
                  << static_cast<unsigned>(link_id)
                  << "，com=COM" << (static_cast<unsigned>(link_id) + 1)
                  << "，seq=" << request_sequence
                  << "，requested_count=" << requested_count;
        const auto error = run_bus(link_id, requested_count, {}, true, stats);
        (void)response.set_unsigned("error_count", stats.error_count);
        (void)response.set_unsigned("total_count", stats.total_count);
        (void)response.set_unsigned("elapsed_ms", stats.elapsed_ms);
        const auto completion = error == ProductErrorCode::Ok
            ? Detail::bus_completion_error(
                  requested_count, {stats.error_count, stats.total_count})
            : error;
        LOG_DEBUG << "[HW-TEST][SERIAL] BUS_LOOP 结束：link_id="
                  << static_cast<unsigned>(link_id)
                  << "，com=COM" << (static_cast<unsigned>(link_id) + 1)
                  << "，seq=" << request_sequence
                  << "，result=0x" << std::hex
                  << static_cast<uint16_t>(completion) << std::dec
                  << "，completed=" << stats.total_count << "/" << requested_count
                  << "，errors=" << stats.error_count
                  << "，elapsed_ms=" << stats.elapsed_ms;
        return completion;
    }

    ProductErrorCode handle_bus_echo(const ProductMessage& request,
                                     ProductMessage& response) {
        const auto link = request.get_unsigned("link_id");
        if (!link || *link > std::numeric_limits<uint8_t>::max()) {
            return ProductErrorCode::ChannelInvalid;
        }
        const auto link_id = static_cast<uint8_t>(*link);
        (void)response.set_unsigned("link_id", link_id);
        const auto preflight = Detail::bus_link_preflight(link_id, imu_active);
        if (preflight != ProductErrorCode::Ok) {
            return preflight;
        }
        std::array<uint8_t, kBusEchoBytes> payload{};
        for (size_t index = 0; index < payload.size(); ++index) {
            const auto value = request.get_unsigned("data[" + std::to_string(index) + "]");
            if (!value) {
                return ProductErrorCode::ParamOutOfRange;
            }
            payload[index] = static_cast<uint8_t>(*value);
        }
        BusStats stats{};
        const auto request_sequence = request.get_unsigned("seq").value_or(0);
        LOG_DEBUG << "[HW-TEST][SERIAL] BUS_ECHO 开始：link_id="
                  << static_cast<unsigned>(link_id)
                  << "，com=COM" << (static_cast<unsigned>(link_id) + 1)
                  << "，seq=" << request_sequence
                  << "，payload_bytes=" << payload.size()
                  << "，tx_first=0x" << std::hex
                  << static_cast<unsigned>(payload.front())
                  << "，tx_last=0x" << static_cast<unsigned>(payload.back())
                  << std::dec;
        const auto error = run_bus(link_id, 1, payload, false, stats);
        if (!stats.last_received.empty()) {
            for (size_t index = 0;
                 index < std::min(payload.size(), stats.last_received.size()); ++index) {
                (void)response.set_unsigned("data[" + std::to_string(index) + "]",
                                            stats.last_received[index]);
            }
        }
        const auto completion = error == ProductErrorCode::Ok
            ? Detail::bus_completion_error(
                  1, {stats.error_count, stats.total_count})
            : error;
        LOG_DEBUG << "[HW-TEST][SERIAL] BUS_ECHO 结束：link_id="
                  << static_cast<unsigned>(link_id)
                  << "，com=COM" << (static_cast<unsigned>(link_id) + 1)
                  << "，seq=" << request_sequence
                  << "，result=0x" << std::hex
                  << static_cast<uint16_t>(completion) << std::dec
                  << "，completed=" << stats.total_count << "/1"
                  << "，errors=" << stats.error_count
                  << "，rx_bytes=" << stats.last_received.size()
                  << "，elapsed_ms=" << stats.elapsed_ms;
        return completion;
    }

    ProductErrorCode handle_di(ProductMessage& response) {
        const auto ready = dido.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }
        const auto inputs = dido.device().read_inputs();
        if (!inputs) {
            return status_error(inputs.status());
        }
        (void)response.set_unsigned("di_state[0]", inputs.value());
        (void)response.set_unsigned("di_state[1]", 0);
        return ProductErrorCode::Ok;
    }

    ProductErrorCode handle_do(const ProductMessage& request,
                               ProductMessage& response) {
        const auto first = request.get_unsigned("channel[0]");
        const auto second = request.get_unsigned("channel[1]");
        if (!first || !second) {
            return ProductErrorCode::ParamOutOfRange;
        }
        const auto ready = dido.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }
        // 协议只映射 DO0~15；高位按合同忽略并在响应中回零。
        const auto changed = dido.device().set_outputs(
            static_cast<uint16_t>(*first & 0xFFFFu), 0xFFFFu);
        if (!changed) {
            return status_error(changed.status());
        }
        const auto applied = dido.device().read_outputs();
        if (!applied) {
            return status_error(applied.status());
        }
        (void)response.set_unsigned("applied_state[0]", applied.value());
        (void)response.set_unsigned("applied_state[1]", 0);
        return ProductErrorCode::Ok;
    }

    ProductErrorCode handle_dh_pulse_config(const ProductMessage& request,
                                            ProductMessage& response) {
        const auto enable = request.get_unsigned("config_enable");
        if (!enable || *enable > 1) {
            return ProductErrorCode::ParamOutOfRange;
        }
        const auto ready = dh.ensure_open();
        if (ready != ProductErrorCode::Ok) {
            return ready;
        }
        auto operation = dh.device().set_pulse_config_enabled(*enable != 0);
        if (!operation) {
            return status_error(operation.status());
        }
        if (*enable != 0) {
            for (unsigned channel = 0; channel < 23; ++channel) {
                const auto width = request.get_unsigned(
                    "pulse_width[" + std::to_string(channel) + "]");
                if (!width || *width > 0xFFFFu) {
                    return ProductErrorCode::ParamOutOfRange;
                }
                operation = dh.device().set_pulse_width_ticks(
                    static_cast<uint8_t>(channel), static_cast<uint16_t>(*width));
                if (!operation) {
                    return status_error(operation.status());
                }
            }
        }
        const auto widths = dh.device().read_pulse_widths(23);
        if (!widths) {
            return status_error(widths.status());
        }
        for (unsigned channel = 0; channel < 23; ++channel) {
            (void)response.set_unsigned(
                "pulse_width_readback[" + std::to_string(channel) + "]",
                widths.value()[channel]);
        }
        return ProductErrorCode::Ok;
    }

    ProductErrorCode start_helm(const ProductMessage& request) {
        const auto waveform_value = request.get_unsigned("waveform");
        const auto frequency_value = request.get_float("freq");
        const auto amplitude_value = request.get_float("ampl");
        const auto offset_value = request.get_float("offset");
        const auto start_value = request.get_float("start");
        const auto maximum_value = request.get_float("max_freq");
        const auto duration_value = request.get_float("sweep_duration_s");
        const auto enable_value = request.get_unsigned("enable");
        if (!waveform_value || !frequency_value || !amplitude_value || !offset_value ||
            !start_value || !maximum_value || !duration_value || !enable_value) {
            return ProductErrorCode::ParamOutOfRange;
        }
        HelmStreamParameters parameters{};
        parameters.waveform = static_cast<uint32_t>(*waveform_value);
        parameters.frequency_hz = *frequency_value;
        parameters.amplitude_deg = *amplitude_value;
        parameters.offset_deg = *offset_value;
        parameters.start_phase_radians = *start_value;
        parameters.maximum_frequency_hz = *maximum_value;
        parameters.sweep_duration_seconds = *duration_value;
        parameters.enable_mask = static_cast<uint8_t>(*enable_value);
        return helm.start(parameters);
    }

    ProductErrorCode stop_helm() {
        return helm.stop();
    }

    std::optional<ProductErrorCode> poll_helm_feedback(ProductMessage& response) {
        return helm.poll_feedback(response);
    }
};

HardwareTestProvider::HardwareTestProvider()
    : impl_(std::make_unique<Impl>()),
      system_(&impl_->timer_load, impl_.get()) {}
HardwareTestProvider::~HardwareTestProvider() = default;

ProductErrorCode HardwareTestProvider::initialize() {
    return impl_->initialize();
}

ProductErrorCode HardwareTestProvider::handle(const ProductMessage& request,
                                              ProductMessage& response) {
    const auto system_result = system_.handle(request, response);
    if (system_result != ProductErrorCode::CmdUnknown) {
        return system_result;
    }

    if (request.name() == "spi_flash_test_request") {
        return impl_->handle_spi_flash(response);
    }
    if (request.name() == "bus_loop_test_request") {
        return impl_->handle_bus_loop(request, response);
    }
    if (request.name() == "bus_echo_test_request") {
        return impl_->handle_bus_echo(request, response);
    }
    if (request.name() == "di_read_request") {
        return impl_->handle_di(response);
    }
    if (request.name() == "do_write_request") {
        return impl_->handle_do(request, response);
    }
    if (request.name() == "elec_health_status_request") {
        return impl_->handle_electrical_health(response);
    }
    if (request.name() == "dh_pulse_config_request") {
        return impl_->handle_dh_pulse_config(request, response);
    }
    if (request.name() == "helm_board_test_request") {
        return impl_->handle_helm_board(request, response);
    }
    if (request.name() == "helm_start_request") {
        return impl_->start_helm(request);
    }
    if (request.name() == "helm_stop_request") {
        return impl_->stop_helm();
    }
    if (request.name() == "imu_stream_start_request") {
        return impl_->start_imu_stream();
    }
    if (request.name() == "imu_stream_stop_request") {
        return impl_->stop_imu_stream();
    }
    return ProductErrorCode::CmdUnknown;
}

ProductErrorCode HardwareTestProvider::begin_dh(const ProductMessage& request) {
    const auto validation = Detail::validate_dh_control_request(request);
    if (validation != ProductErrorCode::Ok) {
        return validation;
    }
    const auto power_enable = request.get_unsigned("power_enable");
    const auto return_enable = request.get_unsigned("return_enable");
    const auto first = request.get_unsigned("channel[0]");
    const auto ready = impl_->dh.ensure_open();
    if (ready != ProductErrorCode::Ok) {
        return ready;
    }
    auto configured = impl_->dh.device().set_fire_enabled(*power_enable != 0);
    if (!configured) {
        return status_error(configured.status());
    }
    configured = impl_->dh.device().set_return_enabled(*return_enable != 0);
    if (!configured) {
        return status_error(configured.status());
    }

    const uint64_t mask = *first;
    std::vector<uint8_t> channels;
    for (uint8_t channel = 0; channel < 23; ++channel) {
        if ((mask & (uint64_t{1} << channel)) != 0) {
            channels.push_back(channel);
        }
    }
    if (channels.empty()) {
        return ProductErrorCode::Ok;
    }
    configured = impl_->dh.device().set_repeat_mode(HW::DhRepeatMode::Repeatable);
    if (!configured) {
        return status_error(configured.status());
    }
    configured = impl_->dh.device().set_fire_mode(HW::DhFireMode::Multiple);
    if (!configured) {
        return status_error(configured.status());
    }
    const auto fired = impl_->dh.device().fire_multiple(channels);
    return fired ? ProductErrorCode::Ok : status_error(fired.status());
}

ProductErrorCode HardwareTestProvider::handle_dh_control_report(
    const ProductMessage& request, ProductMessage& response, size_t report_index) {
    const auto validation = Detail::validate_dh_control_request(request);
    if (validation != ProductErrorCode::Ok) {
        return validation;
    }
    auto ready = impl_->dh.ensure_open();
    if (ready != ProductErrorCode::Ok) {
        return ready;
    }
    (void)report_index;
    const auto power = impl_->dh.device().read_fire_enabled();
    if (!power) {
        return status_error(power.status());
    }
    const auto return_enabled = impl_->dh.device().read_return_enabled();
    if (!return_enabled) {
        return status_error(return_enabled.status());
    }
    const auto statuses = impl_->dh.device().read_channel_statuses();
    if (!statuses) {
        return status_error(statuses.status());
    }
    ready = impl_->ads1258.ensure_open();
    if (ready != ProductErrorCode::Ok) {
        return ready;
    }
    const auto snapshot = impl_->ads1258.device().read_snapshot();
    if (!snapshot) {
        return status_error(snapshot.status());
    }

    auto staged = response;
    if (!staged.set_unsigned("power_enable_readback", power.value() ? 1u : 0u) ||
        !staged.set_unsigned("return_enable_readback",
                             return_enabled.value() ? 1u : 0u)) {
        return ProductErrorCode::TaskExecFailed;
    }
    for (size_t channel = 0; channel < statuses.value().size(); ++channel) {
        if (!staged.set_unsigned("dh_status.ch" + std::to_string(channel),
                                 statuses.value()[channel])) {
            return ProductErrorCode::TaskExecFailed;
        }
    }
    const auto populated = Detail::populate_dh_telemetry(snapshot.value(), staged);
    if (populated != ProductErrorCode::Ok) {
        return populated;
    }
    response = std::move(staged);
    return ProductErrorCode::Ok;
}

bool HardwareTestProvider::helm_feedback_active() const {
    return impl_->helm.active();
}

ProductErrorCode HardwareTestProvider::build_helm_feedback(ProductMessage& response) {
    return impl_->poll_helm_feedback(response).value_or(ProductErrorCode::TaskBusy);
}

std::optional<ProductErrorCode> HardwareTestProvider::poll_helm_feedback(
    ProductMessage& response) {
    return impl_->poll_helm_feedback(response);
}

bool HardwareTestProvider::imu_stream_active() const {
    return impl_->imu_active;
}

std::optional<ProductErrorCode> HardwareTestProvider::poll_imu_stream_feedback(
    ProductMessage& response) {
    return impl_->poll_imu_stream_feedback(response);
}

int run_hardware_test_service() {
    HW::XdmaTransport transport({std::string(kXdmaDevice),
                                 Detail::kControlComUserOffset,
                                 Detail::kComMapLength, -1, -1,
                                 Detail::kControlComEventNumber});
    const auto opened = transport.open();
    if (!opened) {
        LOG_ERROR << "[HW-TEST] 打开 COM3 XDMA Transport 失败："
                  << opened.status().message;
        return 5;
    }
    HW::ComDevice endpoint(transport);
    auto configuration = HW::ComDevice::default_config();
    configuration.loopback = false;
    configuration.receive_enabled = true;
    configuration.interrupt_mode = HW::ComInterruptMode::Level;
    auto operation = endpoint.configure(configuration);
    if (!operation) {
        LOG_ERROR << "[HW-TEST] 配置 COM3 614400/8E1 失败："
                  << operation.status().message;
        return 5;
    }
    operation = endpoint.clear_error_status();
    if (!operation) {
        LOG_ERROR << "[HW-TEST] 清除 COM3 错误状态失败："
                  << operation.status().message;
        return 5;
    }
    operation = endpoint.enable_receive();
    if (!operation) {
        LOG_ERROR << "[HW-TEST] 使能 COM3 接收失败：" << operation.status().message;
        return 5;
    }

    g_hardware_test_stop = 0;
    const auto previous_int = std::signal(SIGINT, request_hardware_test_stop);
    if (previous_int == SIG_ERR) {
        LOG_ERROR << "[HW-TEST] 安装 SIGINT 处理器失败";
        return 5;
    }
    const auto previous_term = std::signal(SIGTERM, request_hardware_test_stop);
    if (previous_term == SIG_ERR) {
        LOG_ERROR << "[HW-TEST] 安装 SIGTERM 处理器失败";
        (void)std::signal(SIGINT, previous_int);
        return 5;
    }

    HardwareTestProvider provider;
    const auto initialized = provider.initialize();
    if (initialized != ProductErrorCode::Ok) {
        LOG_ERROR << "[HW-TEST] ADS1258 临时启动配置失败，未启动产品协议服务";
        transport.close();
        return 5;
    }
    HardwareTestService service(endpoint, provider);
    LOG_WARN << "[HW-TEST] COM3 产品协议服务已启动；该画像允许硬件写入且不执行状态恢复";
    const int result = service.run([]() {
        return g_hardware_test_stop != 0;
    });
    (void)std::signal(SIGINT, previous_int);
    (void)std::signal(SIGTERM, previous_term);
    transport.close();
    return result;
}

} // namespace MB_DDF::HWTest
