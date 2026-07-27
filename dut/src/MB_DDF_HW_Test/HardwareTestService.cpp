#include "MB_DDF_HW_Test/HardwareTestService.h"

#include "MB_DDF/Debug/Logger.h"

#include <array>
#include <chrono>
#include <string_view>
#include <thread>
#include <utility>

namespace MB_DDF::HWTest {
namespace {

void default_sleep(std::chrono::microseconds duration) {
    if (duration.count() > 0) {
        std::this_thread::sleep_for(duration);
    }
}

std::string bytes_to_hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

} // namespace

HardwareTestService::HardwareTestService(HW::IByteEndpoint& endpoint,
                                         IHardwareTestProvider& provider,
                                         uint16_t initial_transmit_sequence,
                                         Sleeper sleeper)
    : endpoint_(endpoint), provider_(provider), protocol_(initial_transmit_sequence),
      sleeper_(sleeper ? std::move(sleeper) : Sleeper(default_sleep)) {}

std::string_view HardwareTestService::response_name(std::string_view request_name) {
    struct Mapping {
        std::string_view request;
        std::string_view response;
    };
    static constexpr Mapping mappings[] = {
        {"system_status_request", "system_status_response"},
        {"memperf_test_request", "memperf_test_response"},
        {"spi_flash_test_request", "spi_flash_test_response"},
        {"bus_loop_test_request", "bus_loop_test_response"},
        {"bus_echo_test_request", "bus_echo_test_response"},
        {"di_read_request", "di_read_response"},
        {"do_write_request", "do_write_response"},
        {"elec_health_status_request", "elec_health_status_response"},
        {"dh_control_request", "dh_control_response"},
        {"dh_pulse_config_request", "dh_pulse_config_response"},
        {"helm_board_test_request", "helm_board_test_response"},
        {"helm_start_request", "helm_start_response"},
        {"helm_stop_request", "helm_stop_response"},
        {"timer_jitter_start_request", "timer_jitter_start_response"},
        {"timer_jitter_stop_request", "timer_jitter_stop_response"},
    };
    for (const auto& mapping : mappings) {
        if (mapping.request == request_name) {
            return mapping.response;
        }
    }
    return {};
}

void HardwareTestService::set_execution_status(ProductMessage& response,
                                                ProductErrorCode error) {
    (void)response.set_unsigned("status", error == ProductErrorCode::Ok ? 0u : 1u);
    (void)response.set_unsigned("err_code", static_cast<uint16_t>(error));
}

bool HardwareTestService::send_message(ProductMessage& message,
                                        std::optional<uint16_t> explicit_sequence) {
    if (!message) {
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mutex_);
    const bool sequence_assigned = explicit_sequence
                                       ? message.set_unsigned("seq", *explicit_sequence)
                                       : protocol_.assign_transmit_sequence(message);
    if (!sequence_assigned) {
        LOG_ERROR << "[HW-TEST] 无法为产品协议响应分配发送序号";
        return false;
    }
    const auto bytes = message.bytes();
    constexpr size_t kMaximumBusyRetries = 1000;
    for (size_t retry = 0;; ++retry) {
        const auto sent = endpoint_.send({bytes.data(), bytes.size()});
        if (sent) {
            if (sent.value() != bytes.size()) {
                LOG_ERROR << "[HW-TEST] 产品协议数据段发送不完整：期望="
                          << bytes.size() << "，实际=" << sent.value();
                return false;
            }
            return true;
        }
        if (sent.status().code != HW::StatusCode::Busy ||
            retry >= kMaximumBusyRetries) {
            LOG_ERROR << "[HW-TEST] 发送产品协议数据段失败："
                      << sent.status().message;
            return false;
        }
        // TX Busy 表示该帧尚未被硬件接受；这里只等待 ready 后重交同一帧，
        // 不重新分配序号，也不是协议层超时自动重发。
        sleeper_(std::chrono::microseconds(100));
    }
}

bool HardwareTestService::process_dh_request(const ProductMessage& request) {
    const auto report_count_value = request.get_unsigned("report_count");
    const auto delay_value = request.get_unsigned("delay_us");
    const auto interval_value = request.get_unsigned("interval_us");
    if (!report_count_value || !delay_value || !interval_value || *report_count_value == 0 ||
        *report_count_value > 0xFFFFu || *interval_value < 2500u) {
        auto response = protocol_.create_message("dh_control_response", false);
        set_execution_status(response, ProductErrorCode::ParamOutOfRange);
        return send_message(response, static_cast<uint16_t>(
                                        request.get_unsigned("seq").value_or(0)));
    }

    {
        std::lock_guard<std::mutex> lock(provider_mutex_);
        const auto error = provider_.begin_dh(request);
        if (error != ProductErrorCode::Ok) {
            auto response = protocol_.create_message("dh_control_response", false);
            set_execution_status(response, error);
            return send_message(response, static_cast<uint16_t>(
                                            request.get_unsigned("seq").value_or(0)));
        }
    }

    const auto request_sequence = static_cast<uint16_t>(
        request.get_unsigned("seq").value_or(0));
    const auto interval = std::chrono::microseconds(*interval_value);
    auto next_sample = std::chrono::steady_clock::now() +
                       std::chrono::microseconds(*delay_value);
    for (uint64_t index = 0; index < *report_count_value; ++index) {
        const auto now = std::chrono::steady_clock::now();
        if (next_sample > now) {
            sleeper_(std::chrono::ceil<std::chrono::microseconds>(next_sample - now));
        }

        auto response = protocol_.create_message("dh_control_response", false);
        ProductErrorCode error = ProductErrorCode::TaskExecFailed;
        std::chrono::steady_clock::time_point sample_started;
        {
            std::lock_guard<std::mutex> lock(provider_mutex_);
            sample_started = std::chrono::steady_clock::now();
            error = provider_.handle_dh_control_report(
                request, response, static_cast<size_t>(index));
            set_execution_status(response, error);
            if (!send_message(response, static_cast<uint16_t>(
                                           request_sequence +
                                           static_cast<uint16_t>(index)))) {
                return false;
            }
        }
        // 从本帧实际采样起点计算下一截止时刻，发送耗时只占用周期余量。
        next_sample = sample_started + interval;
    }
    return true;
}

bool HardwareTestService::process_once(HW::Timeout timeout) {
    std::array<uint8_t, 255> buffer{};
    const auto received = endpoint_.receive({buffer.data(), buffer.size()}, timeout);
    if (!received) {
        if (received.status().code == HW::StatusCode::ProtocolError) {
            LOG_WARN << "[HW-TEST] 丢弃物理层校验失败的数据帧："
                     << received.status().message;
            return true;
        }
        LOG_ERROR << "[HW-TEST] 接收产品协议数据段失败：" << received.status().message;
        return false;
    }
    if (received.value() == 0) {
        return true;
    }

    const auto parsed = protocol_.parse_request(
        std::span<const uint8_t>(buffer.data(), received.value()));
    if (!parsed) {
        LOG_WARN << "[HW-TEST] 产品协议请求解析失败：err_code=0x"
                 << std::hex << static_cast<uint16_t>(parsed.error().code)
                 << "，detail=0x" << parsed.error().detail
                 << "，payload=" << bytes_to_hex(std::span<const uint8_t>(
                        buffer.data(), received.value()));
        auto response = protocol_.create_error_response(parsed.error(), false);
        return send_message(response, parsed.error().orig_sequence);
    }
    const auto& request = parsed.message();
    if (request.name() == "dh_control_request") {
        return process_dh_request(request);
    }

    const auto response_descriptor_name = response_name(request.name());
    if (response_descriptor_name.empty()) {
        ProductParseError error{};
        error.code = ProductErrorCode::CmdUnknown;
        error.orig_type_group = static_cast<uint8_t>(request.get_unsigned("type_group").value_or(0));
        error.orig_sub_type = static_cast<uint8_t>(request.get_unsigned("sub_type").value_or(0));
        error.orig_sequence = static_cast<uint16_t>(request.get_unsigned("seq").value_or(0));
        auto response = protocol_.create_error_response(error, false);
        return send_message(response, error.orig_sequence);
    }

    auto response = protocol_.create_message(response_descriptor_name, false);
    std::lock_guard<std::mutex> lock(provider_mutex_);
    const auto error = provider_.handle(request, response);
    set_execution_status(response, error);
    return send_message(response, static_cast<uint16_t>(
                                  request.get_unsigned("seq").value_or(0)));
}

bool HardwareTestService::emit_helm_feedback_once() {
    std::lock_guard<std::mutex> lock(provider_mutex_);
    if (!provider_.helm_feedback_active()) {
        return true;
    }
    auto response = protocol_.create_message("helm_feedback_response", false);
    const auto error = provider_.build_helm_feedback(response);
    set_execution_status(response, error);
    return send_message(response);
}

int HardwareTestService::run(const StopPredicate& stop_requested) {
    if (!stop_requested) {
        return 1;
    }
    std::atomic_bool feedback_failed{false};
    std::atomic_bool shutdown_requested{false};
    std::thread feedback_worker([&]() {
        while (!stop_requested() &&
               !shutdown_requested.load(std::memory_order_relaxed)) {
            if (!emit_helm_feedback_once()) {
                feedback_failed.store(true, std::memory_order_relaxed);
                shutdown_requested.store(true, std::memory_order_relaxed);
                return;
            }
            sleeper_(std::chrono::milliseconds(1));
        }
    });

    int result = 0;
    while (!stop_requested() &&
           !shutdown_requested.load(std::memory_order_relaxed) &&
           !feedback_failed.load(std::memory_order_relaxed)) {
        if (!process_once(HW::Timeout::after_us(100000))) {
            result = 1;
            break;
        }
    }
    shutdown_requested.store(true, std::memory_order_relaxed);
    feedback_worker.join();
    return feedback_failed.load(std::memory_order_relaxed) ? 1 : result;
}

} // namespace MB_DDF::HWTest
