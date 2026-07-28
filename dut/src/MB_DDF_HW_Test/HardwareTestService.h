#pragma once

#include "MB_DDF_HW/Endpoint/IByteEndpoint.h"
#include "MB_DDF_HW_Test/ProductProtocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace MB_DDF::HWTest {

class IHardwareTestProvider {
public:
    virtual ~IHardwareTestProvider() = default;
    virtual ProductErrorCode handle(const ProductMessage& request,
                                    ProductMessage& response) = 0;
    virtual ProductErrorCode begin_dh(const ProductMessage& request) {
        (void)request;
        return ProductErrorCode::Ok;
    }
    virtual ProductErrorCode handle_dh_control_report(const ProductMessage& request,
                                                      ProductMessage& response,
                                                      size_t report_index) {
        (void)report_index;
        return handle(request, response);
    }
    virtual bool helm_feedback_active() const = 0;
    virtual ProductErrorCode build_helm_feedback(ProductMessage& response) = 0;
    /// nullopt 表示当前没有可上送的舵控 DDS 反馈样本。
    virtual std::optional<ProductErrorCode> poll_helm_feedback(
        ProductMessage& response) {
        if (!helm_feedback_active()) return std::nullopt;
        return build_helm_feedback(response);
    }
    virtual bool imu_stream_active() const { return false; }
    /// nullopt 表示当前没有可上送帧；有值时 response 必须发送，错误码写入状态字段。
    virtual std::optional<ProductErrorCode> poll_imu_stream_feedback(
        ProductMessage& response) {
        (void)response;
        return std::nullopt;
    }
};

/// 组合 COM3、产品协议服务和真实板级 Provider 的 HW_TEST 编译期入口。
int run_hardware_test_service();

class HardwareTestService {
public:
    using Sleeper = std::function<void(std::chrono::microseconds)>;
    using StopPredicate = std::function<bool()>;

    HardwareTestService(HW::IByteEndpoint& endpoint, IHardwareTestProvider& provider,
                        uint16_t initial_transmit_sequence = 0,
                        Sleeper sleeper = {});

    bool process_once(HW::Timeout timeout);
    bool emit_helm_feedback_once();
    bool emit_imu_stream_feedback_once();
    int run(const StopPredicate& stop_requested);

private:
    static std::string_view response_name(std::string_view request_name);
    static void set_execution_status(ProductMessage& response, ProductErrorCode error);
    bool send_message(ProductMessage& message,
                      std::optional<uint16_t> explicit_sequence = std::nullopt);
    bool process_dh_request(const ProductMessage& request);

    HW::IByteEndpoint& endpoint_;
    IHardwareTestProvider& provider_;
    ProductProtocol protocol_;
    Sleeper sleeper_;
    std::mutex send_mutex_;
    // 硬件 Provider 调用串行化；舵 DDS 反馈不使用此锁，避免与板级测试绑定。
    std::mutex provider_mutex_;
    // 只保证同一 HELM 流的 START/反馈/STOP 线序，不管理舵控进程，也不锁板级请求。
    std::mutex helm_stream_order_mutex_;
};

} // namespace MB_DDF::HWTest
