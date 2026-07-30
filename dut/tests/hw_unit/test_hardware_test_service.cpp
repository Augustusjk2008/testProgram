#include "MB_DDF_HW_Test/HardwareTestService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace MB_DDF;

namespace {

class RecordingEndpoint final : public HW::IByteEndpoint {
public:
    HW::Result<size_t> send(HW::BufferView data) override {
        if (before_send) {
            before_send(data);
        }
        ++send_attempts;
        attempted.emplace_back(data.data, data.data + data.size);
        if (always_busy || busy_sends_remaining > 0) {
            if (busy_sends_remaining > 0) {
                --busy_sends_remaining;
            }
            return HW::Status::error(HW::StatusCode::Busy, 0, "TX busy");
        }
        sent.emplace_back(data.data, data.data + data.size);
        return data.size;
    }

    HW::Result<size_t> receive(HW::MutableBufferView buffer, HW::Timeout) override {
        if (!receive_failures.empty()) {
            auto status = std::move(receive_failures.front());
            receive_failures.pop_front();
            return status;
        }
        if (received.empty()) {
            return size_t{0};
        }
        auto bytes = std::move(received.front());
        received.pop_front();
        if (bytes.size() > buffer.size) {
            return HW::Status::error(HW::StatusCode::BufferTooSmall, 0, "test buffer");
        }
        std::copy(bytes.begin(), bytes.end(), buffer.data);
        return bytes.size();
    }

    size_t mtu() const override { return 255; }

    std::deque<std::vector<uint8_t>> received;
    std::deque<HW::Status> receive_failures;
    std::vector<std::vector<uint8_t>> sent;
    std::vector<std::vector<uint8_t>> attempted;
    int busy_sends_remaining{0};
    bool always_busy{false};
    int send_attempts{0};
    std::function<void(HW::BufferView)> before_send;
};

class FakeProvider final : public HWTest::IHardwareTestProvider {
public:
    HWTest::ProductErrorCode handle(const HWTest::ProductMessage& request,
                                    HWTest::ProductMessage& response) override {
        requests.emplace_back(request.name());
        if (response.name() == "system_status_response") {
            response.set_float("cpu_usage", 12.5f);
        }
        return next_error;
    }

    bool helm_feedback_active() const override { return feedback_active; }

    HWTest::ProductErrorCode build_helm_feedback(HWTest::ProductMessage& response) override {
        ++feedback_calls;
        response.set_unsigned("sample_count", 1);
        response.set_unsigned("first_timestamp_us_low", 123);
        response.set_unsigned("first_timestamp_us_high", 0);
        response.set_unsigned("sample[0].serial_a", 7);
        response.set_unsigned("sample[0].serial_b", 8);
        response.set_float("sample[0].ins[0]", 12.5F);
        response.set_float("sample[0].fdb[0]", 11.5F);
        return next_error;
    }

    std::optional<HWTest::ProductErrorCode> poll_helm_feedback(
        HWTest::ProductMessage& response) override {
        if (!feedback_ready) {
            ++feedback_calls;
            return std::nullopt;
        }
        return build_helm_feedback(response);
    }

    bool imu_stream_active() const override { return imu_active; }

    std::optional<HWTest::ProductErrorCode> poll_imu_stream_feedback(
        HWTest::ProductMessage& response) override {
        ++imu_feedback_calls;
        if (!imu_feedback_ready) {
            return std::nullopt;
        }
        response.set_unsigned("source_seq", 0x3456u);
        response.set_float("delta_angle_x", 2.5F);
        return next_error;
    }

    HWTest::ProductErrorCode next_error{HWTest::ProductErrorCode::Ok};
    bool feedback_active{false};
    bool feedback_ready{true};
    bool imu_active{false};
    bool imu_feedback_ready{false};
    int feedback_calls{0};
    int imu_feedback_calls{0};
    std::vector<std::string> requests;
};

class ConcurrentCallDetectingProvider final : public HWTest::IHardwareTestProvider {
public:
    HWTest::ProductErrorCode handle(const HWTest::ProductMessage&,
                                    HWTest::ProductMessage&) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            handling.store(true, std::memory_order_relaxed);
            entered.notify_all();
        }
        std::unique_lock<std::mutex> lock(mutex);
        release.wait(lock, [&]() { return allow_handle_to_finish; });
        handling.store(false, std::memory_order_relaxed);
        return HWTest::ProductErrorCode::Ok;
    }

    bool helm_feedback_active() const override { return true; }

    HWTest::ProductErrorCode build_helm_feedback(HWTest::ProductMessage&) override {
        std::lock_guard<std::mutex> lock(mutex);
        feedback_started = true;
        feedback_overlapped_handle = handling.load(std::memory_order_relaxed);
        entered.notify_all();
        return HWTest::ProductErrorCode::Ok;
    }

    void wait_until_handle_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(entered.wait_for(lock, std::chrono::seconds(1),
                                     [&]() {
                                         return handling.load(std::memory_order_relaxed);
                                     }));
    }

    void allow_handle() {
        std::lock_guard<std::mutex> lock(mutex);
        allow_handle_to_finish = true;
        release.notify_all();
    }

    bool feedback_has_started() const {
        std::lock_guard<std::mutex> lock(mutex);
        return feedback_started;
    }

    bool feedback_overlapped() const {
        std::lock_guard<std::mutex> lock(mutex);
        return feedback_overlapped_handle;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable release;
    std::atomic_bool handling{false};
    bool allow_handle_to_finish{false};
    bool feedback_started{false};
    bool feedback_overlapped_handle{false};
};

class DhOrderingProvider final : public HWTest::IHardwareTestProvider {
public:
    HWTest::ProductErrorCode handle(const HWTest::ProductMessage&,
                                    HWTest::ProductMessage&) override {
        events.emplace_back("unexpected-handle");
        return HWTest::ProductErrorCode::TaskExecFailed;
    }

    HWTest::ProductErrorCode begin_dh(const HWTest::ProductMessage&) override {
        ++begin_calls;
        events.emplace_back("begin");
        return begin_error;
    }

    HWTest::ProductErrorCode handle_dh_control_report(const HWTest::ProductMessage&,
                                                      HWTest::ProductMessage&,
                                                      size_t report_index) override {
        events.emplace_back("report" + std::to_string(report_index));
        return report_index < report_errors.size()
                   ? report_errors[report_index]
                   : HWTest::ProductErrorCode::Ok;
    }

    bool helm_feedback_active() const override { return false; }
    HWTest::ProductErrorCode build_helm_feedback(HWTest::ProductMessage&) override {
        return HWTest::ProductErrorCode::Ok;
    }

    HWTest::ProductErrorCode begin_error{HWTest::ProductErrorCode::Ok};
    std::vector<HWTest::ProductErrorCode> report_errors;
    std::vector<std::string> events;
    int begin_calls{0};
};

class HelmStartOrderingProvider final : public HWTest::IHardwareTestProvider {
public:
    HWTest::ProductErrorCode handle(const HWTest::ProductMessage& request,
                                    HWTest::ProductMessage&) override {
        if (request.name() != "helm_start_request") {
            return HWTest::ProductErrorCode::CmdUnknown;
        }
        std::unique_lock<std::mutex> lock(mutex);
        active.store(true, std::memory_order_relaxed);
        handle_entered = true;
        condition.notify_all();
        condition.wait(lock, [&]() { return allow_handle_to_finish; });
        return HWTest::ProductErrorCode::Ok;
    }

    bool helm_feedback_active() const override {
        return active.load(std::memory_order_relaxed);
    }

    HWTest::ProductErrorCode build_helm_feedback(HWTest::ProductMessage&) override {
        feedback_started.store(true, std::memory_order_relaxed);
        return HWTest::ProductErrorCode::Ok;
    }

    void wait_until_handle_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(1),
                                       [&]() { return handle_entered; }));
    }

    void release_handle() {
        std::lock_guard<std::mutex> lock(mutex);
        allow_handle_to_finish = true;
        condition.notify_all();
    }

    std::atomic_bool feedback_started{false};

private:
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::atomic_bool active{false};
    bool handle_entered{false};
    bool allow_handle_to_finish{false};
};

class HelmStopOrderingProvider final : public HWTest::IHardwareTestProvider {
public:
    HWTest::ProductErrorCode handle(const HWTest::ProductMessage& request,
                                    HWTest::ProductMessage&) override {
        if (request.name() != "helm_stop_request") {
            return HWTest::ProductErrorCode::CmdUnknown;
        }
        active.store(false, std::memory_order_relaxed);
        return HWTest::ProductErrorCode::Ok;
    }

    bool helm_feedback_active() const override {
        return active.load(std::memory_order_relaxed);
    }

    HWTest::ProductErrorCode build_helm_feedback(
        HWTest::ProductMessage& response) override {
        response.set_unsigned("sample_count", 1);
        {
            std::lock_guard<std::mutex> lock(mutex);
            feedback_entered = true;
            condition.notify_all();
        }
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&]() { return allow_feedback_to_finish; });
        return HWTest::ProductErrorCode::Ok;
    }

    void wait_until_feedback_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(1),
                                       [&]() { return feedback_entered; }));
    }

    void release_feedback() {
        std::lock_guard<std::mutex> lock(mutex);
        allow_feedback_to_finish = true;
        condition.notify_all();
    }

private:
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::atomic_bool active{true};
    bool feedback_entered{false};
    bool allow_feedback_to_finish{false};
};

std::vector<uint8_t> make_request(std::string_view name, uint16_t sequence = 0) {
    HWTest::ProductProtocol builder;
    auto request = builder.create_message(name, false);
    EXPECT_TRUE(request.set_unsigned("seq", sequence));
    const auto bytes = request.bytes();
    return {bytes.begin(), bytes.end()};
}

HWTest::ProductMessage decode_sent(std::string_view name, const std::vector<uint8_t>& bytes) {
    HWTest::ProductProtocol builder;
    auto expected = builder.create_message(name, false);
    EXPECT_EQ(expected.bytes().size(), bytes.size());
    return HWTest::ProductMessage::from_descriptor(name, bytes);
}

} // namespace

TEST(HardwareTestServiceTest, RoutesOrdinaryRequestAndEchoesRequestSequence) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    endpoint.received.push_back(make_request("system_status_request", 0xBEEF));
    HWTest::HardwareTestService service(endpoint, provider, 3);

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    ASSERT_EQ(provider.requests, std::vector<std::string>{"system_status_request"});
    ASSERT_EQ(endpoint.sent.size(), 1u);
    EXPECT_EQ(endpoint.sent[0][1], 0x01u);
    EXPECT_EQ(endpoint.sent[0][2], 0x01u);
    auto response = decode_sent("system_status_response", endpoint.sent[0]);
    EXPECT_EQ(response.get_unsigned("seq").value_or(0), 0xBEEFu);
    EXPECT_EQ(response.get_unsigned("status").value_or(1), 0u);
    EXPECT_EQ(response.get_unsigned("err_code").value_or(1), 0u);
    EXPECT_FLOAT_EQ(response.get_float("cpu_usage").value_or(0), 12.5f);
}

TEST(HardwareTestServiceTest, EncodesProviderFailureInOrdinaryResponse) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    provider.next_error = HWTest::ProductErrorCode::TaskExecFailed;
    endpoint.received.push_back(make_request("di_read_request", 1));
    HWTest::HardwareTestService service(endpoint, provider);

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    ASSERT_EQ(endpoint.sent.size(), 1u);
    auto response = decode_sent("di_read_response", endpoint.sent[0]);
    EXPECT_EQ(response.get_unsigned("status").value_or(0), 1u);
    EXPECT_EQ(response.get_unsigned("err_code").value_or(0),
              static_cast<uint16_t>(HWTest::ProductErrorCode::TaskExecFailed));
}

TEST(HardwareTestServiceTest, SendsFrameErrorWithoutCallingProvider) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    auto malformed = make_request("di_read_request", 0x1234);
    malformed[0] = 0x10;
    endpoint.received.push_back(std::move(malformed));
    HWTest::HardwareTestService service(endpoint, provider, 8);

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_TRUE(provider.requests.empty());
    ASSERT_EQ(endpoint.sent.size(), 1u);
    EXPECT_EQ(endpoint.sent[0][1], 0xFFu);
    EXPECT_EQ(endpoint.sent[0][2], 0x00u);
    auto response = decode_sent("error_response", endpoint.sent[0]);
    EXPECT_EQ(response.get_unsigned("seq").value_or(0), 0x1234u);
    EXPECT_EQ(response.get_unsigned("orig_seq").value_or(0), 0x1234u);
}

TEST(HardwareTestServiceTest, SendsRequestedNumberOfDhReportsWithRequestSequences) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("seq", 0x9000));
    ASSERT_TRUE(request.set_unsigned("report_count", 3));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 0));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    HWTest::HardwareTestService service(endpoint, provider, 0xFFFE,
                                        [](std::chrono::microseconds) {});

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    ASSERT_EQ(endpoint.sent.size(), 3u);
    EXPECT_EQ(provider.requests.size(), 3u);
    EXPECT_EQ(endpoint.sent[0][1], 0x06u);
    EXPECT_EQ(endpoint.sent[0][2], 0x02u);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[0])
                  .get_unsigned("seq").value_or(0), 0x9000u);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[1])
                  .get_unsigned("seq").value_or(0), 0x9001u);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[2])
                  .get_unsigned("seq").value_or(1), 0x9002u);
}

TEST(HardwareTestServiceTest, SendsReadOnlyBaselinesBeforeBeginningDh) {
    RecordingEndpoint endpoint;
    DhOrderingProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("seq", 0x9000));
    ASSERT_TRUE(request.set_unsigned("report_count", 4));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 2));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    size_t send_index = 0;
    endpoint.before_send = [&](HW::BufferView) {
        provider.events.emplace_back("send" + std::to_string(send_index++));
    };
    HWTest::HardwareTestService service(endpoint, provider, 0,
                                        [](std::chrono::microseconds) {});

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(provider.begin_calls, 1);
    EXPECT_EQ(provider.events,
              (std::vector<std::string>{"report0", "send0", "report1", "send1",
                                        "begin", "report2", "send2", "report3", "send3"}));
    ASSERT_EQ(endpoint.sent.size(), 4u);
    for (size_t index = 0; index < endpoint.sent.size(); ++index) {
        const auto response = decode_sent("dh_control_response", endpoint.sent[index]);
        EXPECT_EQ(response.get_unsigned("seq").value_or(0),
                  0x9000u + index);
        EXPECT_EQ(response.get_unsigned("status").value_or(1), 0u);
    }
}

TEST(HardwareTestServiceTest, RejectsInvalidDhRequestBeforeAnyProviderCall) {
    struct InvalidCase {
        const char* name;
        std::function<void(HWTest::ProductMessage&)> apply;
    };
    const std::vector<InvalidCase> cases{
        {"zero report count", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("report_count", 0));
         }},
        {"interval below minimum", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("interval_us", 2499));
         }},
        {"delay equals report count", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("delay_frames", 3));
         }},
        {"delay exceeds report count", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("delay_frames", 4));
         }},
        {"empty channel mask", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("channel[0]", 0));
         }},
        {"channel zero high bit", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("channel[0]", uint64_t{1} << 23));
         }},
        {"channel one used", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("channel[1]", 1));
         }},
        {"invalid power", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("power_enable", 2));
         }},
        {"invalid return", [](HWTest::ProductMessage& request) {
             ASSERT_TRUE(request.set_unsigned("return_enable", 2));
         }},
    };

    for (const auto& test_case : cases) {
        RecordingEndpoint endpoint;
        DhOrderingProvider provider;
        HWTest::ProductProtocol builder;
        auto request = builder.create_message("dh_control_request", false);
        ASSERT_TRUE(request.set_unsigned("power_enable", 1)) << test_case.name;
        ASSERT_TRUE(request.set_unsigned("return_enable", 1)) << test_case.name;
        ASSERT_TRUE(request.set_unsigned("channel[0]", 1)) << test_case.name;
        ASSERT_TRUE(request.set_unsigned("channel[1]", 0)) << test_case.name;
        ASSERT_TRUE(request.set_unsigned("report_count", 3)) << test_case.name;
        ASSERT_TRUE(request.set_unsigned("delay_frames", 1)) << test_case.name;
        ASSERT_TRUE(request.set_unsigned("interval_us", 2500)) << test_case.name;
        test_case.apply(request);
        endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
        HWTest::HardwareTestService service(endpoint, provider, 0,
                                            [](std::chrono::microseconds) {});

        ASSERT_TRUE(service.process_once(HW::Timeout::poll())) << test_case.name;
        EXPECT_TRUE(provider.events.empty()) << test_case.name;
        EXPECT_EQ(provider.begin_calls, 0) << test_case.name;
        ASSERT_EQ(endpoint.sent.size(), 1u) << test_case.name;
        const auto response = decode_sent("dh_control_response", endpoint.sent.front());
        EXPECT_EQ(response.get_unsigned("status").value_or(0), 1u) << test_case.name;
        EXPECT_EQ(response.get_unsigned("err_code").value_or(0),
                  static_cast<uint16_t>(HWTest::ProductErrorCode::ParamOutOfRange))
            << test_case.name;
    }
}

TEST(HardwareTestServiceTest, StopsWithoutBeginningDhWhenBaselineCaptureFails) {
    RecordingEndpoint endpoint;
    DhOrderingProvider provider;
    provider.report_errors = {HWTest::ProductErrorCode::TaskExecFailed};
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("report_count", 3));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 1));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    HWTest::HardwareTestService service(endpoint, provider, 0,
                                        [](std::chrono::microseconds) {});

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(provider.events, (std::vector<std::string>{"report0"}));
    EXPECT_EQ(provider.begin_calls, 0);
    ASSERT_EQ(endpoint.sent.size(), 1u);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent.front())
                  .get_unsigned("err_code").value_or(0),
              static_cast<uint16_t>(HWTest::ProductErrorCode::TaskExecFailed));
}

TEST(HardwareTestServiceTest, StopsWithoutBeginningDhWhenBaselineSendFails) {
    RecordingEndpoint endpoint;
    endpoint.always_busy = true;
    DhOrderingProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 1));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    HWTest::HardwareTestService service(endpoint, provider, 0,
                                        [](std::chrono::microseconds) {});

    EXPECT_FALSE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(provider.events, (std::vector<std::string>{"report0"}));
    EXPECT_EQ(provider.begin_calls, 0);
    EXPECT_EQ(endpoint.send_attempts, 1001);
}

TEST(HardwareTestServiceTest, ContinuesReportingAfterCaptureFailureFollowingDhBegin) {
    RecordingEndpoint endpoint;
    DhOrderingProvider provider;
    provider.report_errors = {HWTest::ProductErrorCode::Ok,
                              HWTest::ProductErrorCode::TaskExecFailed,
                              HWTest::ProductErrorCode::Ok};
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("report_count", 3));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 1));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    size_t send_index = 0;
    endpoint.before_send = [&](HW::BufferView) {
        provider.events.emplace_back("send" + std::to_string(send_index++));
    };
    HWTest::HardwareTestService service(endpoint, provider, 0,
                                        [](std::chrono::microseconds) {});

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(provider.begin_calls, 1);
    EXPECT_EQ(provider.events,
              (std::vector<std::string>{"report0", "send0", "begin", "report1", "send1",
                                        "report2", "send2"}));
    ASSERT_EQ(endpoint.sent.size(), 3u);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[0])
                  .get_unsigned("status").value_or(1),
              0u);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[1])
                  .get_unsigned("err_code").value_or(0),
              static_cast<uint16_t>(HWTest::ProductErrorCode::TaskExecFailed));
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[2])
                  .get_unsigned("status").value_or(1),
              0u);
}

TEST(HardwareTestServiceTest, StopsAfterBeginFailureFollowingSuccessfulBaselines) {
    RecordingEndpoint endpoint;
    DhOrderingProvider provider;
    provider.begin_error = HWTest::ProductErrorCode::TaskExecFailed;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("seq", 0x3456));
    ASSERT_TRUE(request.set_unsigned("report_count", 3));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 1));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    size_t send_index = 0;
    endpoint.before_send = [&](HW::BufferView) {
        provider.events.emplace_back("send" + std::to_string(send_index++));
    };
    HWTest::HardwareTestService service(endpoint, provider, 0,
                                        [](std::chrono::microseconds) {});

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(provider.begin_calls, 1);
    EXPECT_EQ(provider.events,
              (std::vector<std::string>{"report0", "send0", "begin", "send1"}));
    ASSERT_EQ(endpoint.sent.size(), 2u);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[0])
                  .get_unsigned("seq").value_or(0),
              0x3456u);
    const auto failure = decode_sent("dh_control_response", endpoint.sent[1]);
    EXPECT_EQ(failure.get_unsigned("seq").value_or(0), 0x3457u);
    EXPECT_EQ(failure.get_unsigned("err_code").value_or(0),
              static_cast<uint16_t>(HWTest::ProductErrorCode::TaskExecFailed));
}

TEST(HardwareTestServiceTest, StopsAfterSendFailureFollowingDhBegin) {
    RecordingEndpoint endpoint;
    endpoint.always_busy = true;
    DhOrderingProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 0));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    HWTest::HardwareTestService service(endpoint, provider, 0,
                                        [](std::chrono::microseconds) {});

    EXPECT_FALSE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(provider.events, (std::vector<std::string>{"begin", "report0"}));
    EXPECT_EQ(provider.begin_calls, 1);
    EXPECT_EQ(endpoint.send_attempts, 1001);
}

TEST(HardwareTestServiceTest, RejectsDhIntervalBelowMinimumBeforeBeginningDh) {
    RecordingEndpoint endpoint;
    DhOrderingProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("seq", 0x2345));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 0));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2499));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    HWTest::HardwareTestService service(endpoint, provider, 0,
                                        [](std::chrono::microseconds) {});

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_TRUE(provider.events.empty());
    ASSERT_EQ(endpoint.sent.size(), 1u);
    const auto response = decode_sent("dh_control_response", endpoint.sent[0]);
    EXPECT_EQ(response.get_unsigned("seq").value_or(0), 0x2345u);
    EXPECT_EQ(response.get_unsigned("status").value_or(0), 1u);
    EXPECT_EQ(response.get_unsigned("err_code").value_or(0),
              static_cast<uint16_t>(HWTest::ProductErrorCode::ParamOutOfRange));
}

TEST(HardwareTestServiceTest, EmitsHelmFeedbackThroughSameSender) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    provider.feedback_active = true;
    HWTest::HardwareTestService service(endpoint, provider, 44);

    ASSERT_TRUE(service.emit_helm_feedback_once());
    ASSERT_EQ(provider.feedback_calls, 1);
    ASSERT_EQ(endpoint.sent.size(), 1u);
    auto feedback = decode_sent("helm_feedback_response", endpoint.sent[0]);
    EXPECT_EQ(feedback.get_unsigned("seq").value_or(0), 44u);
    EXPECT_EQ(feedback.get_unsigned("sample_count").value_or(0), 1u);
    EXPECT_EQ(feedback.get_unsigned("sample[0].serial_a").value_or(0), 7u);
    EXPECT_FLOAT_EQ(feedback.get_float("sample[0].fdb[0]").value_or(0.0F),
                    11.5F);
}

TEST(HardwareTestServiceTest, ActiveHelmWithoutFeedbackDoesNotEmitEmptyFrame) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    provider.feedback_active = true;
    provider.feedback_ready = false;
    HWTest::HardwareTestService service(endpoint, provider, 44);

    ASSERT_TRUE(service.emit_helm_feedback_once());

    EXPECT_EQ(provider.feedback_calls, 1);
    EXPECT_TRUE(endpoint.sent.empty());
}

TEST(HardwareTestServiceTest, InactiveFeedbackDoesNotAffectOrdinaryEchoSequence) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    HWTest::HardwareTestService service(endpoint, provider, 77);

    ASSERT_TRUE(service.emit_helm_feedback_once());
    endpoint.received.push_back(make_request("system_status_request"));
    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));

    ASSERT_EQ(endpoint.sent.size(), 1u);
    EXPECT_EQ(decode_sent("system_status_response", endpoint.sent[0])
                  .get_unsigned("seq").value_or(0),
              0u);
}

TEST(HardwareTestServiceTest, RoutesImuStartAndStopWithEchoedRequestSequences) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    endpoint.received.push_back(make_request("imu_stream_start_request", 0x1234));
    endpoint.received.push_back(make_request("imu_stream_stop_request", 0x5678));
    HWTest::HardwareTestService service(endpoint, provider, 90);

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));

    ASSERT_EQ(provider.requests,
              (std::vector<std::string>{"imu_stream_start_request",
                                        "imu_stream_stop_request"}));
    ASSERT_EQ(endpoint.sent.size(), 2u);
    EXPECT_EQ(decode_sent("imu_stream_start_response", endpoint.sent[0])
                  .get_unsigned("seq").value_or(0), 0x1234u);
    EXPECT_EQ(decode_sent("imu_stream_stop_response", endpoint.sent[1])
                  .get_unsigned("seq").value_or(0), 0x5678u);
}

TEST(HardwareTestServiceTest, EmitsImuFeedbackWithoutConsumingSequenceWhileIdle) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    provider.imu_active = true;
    HWTest::HardwareTestService service(endpoint, provider, 44);

    ASSERT_TRUE(service.emit_imu_stream_feedback_once());
    EXPECT_TRUE(endpoint.sent.empty());
    provider.imu_feedback_ready = true;
    ASSERT_TRUE(service.emit_imu_stream_feedback_once());

    ASSERT_EQ(provider.imu_feedback_calls, 2);
    ASSERT_EQ(endpoint.sent.size(), 1u);
    const auto feedback = decode_sent("imu_stream_feedback_response", endpoint.sent[0]);
    EXPECT_EQ(feedback.get_unsigned("seq").value_or(0), 44u);
    EXPECT_EQ(feedback.get_unsigned("source_seq").value_or(0), 0x3456u);
    EXPECT_FLOAT_EQ(feedback.get_float("delta_angle_x").value_or(0.0F), 2.5F);
    EXPECT_EQ(feedback.get_unsigned("status").value_or(1), 0u);
}

TEST(HardwareTestServiceTest, SendsOneTerminalImuErrorFeedbackFromProvider) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    provider.imu_active = true;
    provider.imu_feedback_ready = true;
    provider.next_error = HWTest::ProductErrorCode::RegReadWriteFailed;
    HWTest::HardwareTestService service(endpoint, provider, 9);

    ASSERT_TRUE(service.emit_imu_stream_feedback_once());

    ASSERT_EQ(endpoint.sent.size(), 1u);
    const auto feedback = decode_sent("imu_stream_feedback_response", endpoint.sent[0]);
    EXPECT_EQ(feedback.get_unsigned("status").value_or(0), 1u);
    EXPECT_EQ(feedback.get_unsigned("err_code").value_or(0),
              static_cast<uint16_t>(HWTest::ProductErrorCode::RegReadWriteFailed));
}

TEST(HardwareTestServiceTest, SendsHelmStartAckBeforeFeedbackWithIncreasingSequence) {
    RecordingEndpoint endpoint;
    HelmStartOrderingProvider provider;
    endpoint.received.push_back(make_request("helm_start_request"));
    HWTest::HardwareTestService service(endpoint, provider, 30);
    bool feedback_started_before_ack = false;
    endpoint.before_send = [&](HW::BufferView data) {
        if (data.size > 2 && data.data[1] == 0x07 && data.data[2] == 0x10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            feedback_started_before_ack =
                provider.feedback_started.load(std::memory_order_relaxed);
        }
    };

    auto start = std::async(std::launch::async, [&]() {
        return service.process_once(HW::Timeout::poll());
    });
    provider.wait_until_handle_entered();
    auto feedback = std::async(std::launch::async, [&]() {
        return service.emit_helm_feedback_once();
    });
    provider.release_handle();

    ASSERT_TRUE(start.get());
    ASSERT_TRUE(feedback.get());
    EXPECT_FALSE(feedback_started_before_ack);
    ASSERT_EQ(endpoint.sent.size(), 2u);
    EXPECT_EQ(decode_sent("helm_start_response", endpoint.sent[0])
                  .get_unsigned("seq").value_or(0),
              0u);
    EXPECT_EQ(decode_sent("helm_feedback_response", endpoint.sent[1])
                  .get_unsigned("seq").value_or(0),
              30u);
}

TEST(HardwareTestServiceTest, SendsPendingHelmFeedbackBeforeStopAck) {
    RecordingEndpoint endpoint;
    HelmStopOrderingProvider provider;
    endpoint.received.push_back(make_request("helm_stop_request", 0x1234));
    HWTest::HardwareTestService service(endpoint, provider, 40);

    auto feedback = std::async(std::launch::async, [&]() {
        return service.emit_helm_feedback_once();
    });
    provider.wait_until_feedback_entered();
    auto stop = std::async(std::launch::async, [&]() {
        return service.process_once(HW::Timeout::poll());
    });

    EXPECT_EQ(stop.wait_for(std::chrono::milliseconds(20)),
              std::future_status::timeout);
    provider.release_feedback();

    ASSERT_TRUE(feedback.get());
    ASSERT_TRUE(stop.get());
    ASSERT_EQ(endpoint.sent.size(), 2u);
    EXPECT_EQ(decode_sent("helm_feedback_response", endpoint.sent[0])
                  .get_unsigned("seq").value_or(0),
              40u);
    EXPECT_EQ(decode_sent("helm_stop_response", endpoint.sent[1])
                  .get_unsigned("seq").value_or(0),
              0x1234u);
}

TEST(HardwareTestServiceTest, BusyFlowControlRetriesSameAssignedFrameThenSucceeds) {
    RecordingEndpoint endpoint;
    endpoint.busy_sends_remaining = 3;
    FakeProvider provider;
    endpoint.received.push_back(make_request("system_status_request"));
    std::vector<std::chrono::microseconds> sleeps;
    HWTest::HardwareTestService service(
        endpoint, provider, 9,
        [&](std::chrono::microseconds duration) { sleeps.push_back(duration); });

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    ASSERT_EQ(endpoint.sent.size(), 1u);
    ASSERT_EQ(endpoint.attempted.size(), 4u);
    EXPECT_TRUE(std::all_of(endpoint.attempted.begin(), endpoint.attempted.end(),
                            [&](const auto& bytes) {
                                return bytes == endpoint.attempted.front();
                            }));
    EXPECT_EQ(decode_sent("system_status_response", endpoint.sent[0])
                  .get_unsigned("seq").value_or(0),
              0u);
    EXPECT_EQ(sleeps, (std::vector<std::chrono::microseconds>(
                          3, std::chrono::microseconds(100))));
}

TEST(HardwareTestServiceTest, PermanentBusyFailsWithinBoundWithoutReassigningSequence) {
    RecordingEndpoint endpoint;
    endpoint.always_busy = true;
    FakeProvider provider;
    endpoint.received.push_back(make_request("system_status_request"));
    HWTest::HardwareTestService service(endpoint, provider, 20,
                                        [](std::chrono::microseconds) {});

    EXPECT_FALSE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(endpoint.send_attempts, 1001);
    ASSERT_FALSE(endpoint.attempted.empty());
    EXPECT_TRUE(std::all_of(endpoint.attempted.begin(), endpoint.attempted.end(),
                            [&](const auto& bytes) {
                                return bytes == endpoint.attempted.front();
                            }));
    EXPECT_EQ(decode_sent("system_status_response", endpoint.attempted.front())
                  .get_unsigned("seq").value_or(0),
              0u);
}

TEST(HardwareTestServiceTest, BeginsDhBeforeFirstReportWhenDelayFramesIsZeroAndKeepsCadence) {
    RecordingEndpoint endpoint;
    DhOrderingProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 0));
    ASSERT_TRUE(request.set_unsigned("return_enable", 0));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 0));
    ASSERT_TRUE(request.set_unsigned("interval_us", 50000));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    std::vector<std::chrono::microseconds> cadence_waits;
    HWTest::HardwareTestService service(
        endpoint, provider, 0,
        [&](std::chrono::microseconds duration) {
            cadence_waits.push_back(duration);
            provider.events.emplace_back("wait");
            std::this_thread::sleep_for(duration);
        });
    size_t send_index = 0;
    endpoint.before_send = [&](HW::BufferView) {
        provider.events.emplace_back("send" + std::to_string(send_index));
        if (send_index == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ++send_index;
    };

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(provider.events,
              (std::vector<std::string>{"begin", "report0", "send0", "wait",
                                        "report1", "send1"}));
    ASSERT_EQ(cadence_waits.size(), 1u);
    EXPECT_GT(cadence_waits.front(), std::chrono::microseconds(0));
    EXPECT_LT(cadence_waits.front(), std::chrono::microseconds(49000));
}

TEST(HardwareTestServiceTest, PulseConfigUsesDedicatedResponseDescriptor) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    endpoint.received.push_back(make_request("dh_pulse_config_request", 0x2468));
    HWTest::HardwareTestService service(endpoint, provider, 9);

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    ASSERT_EQ(endpoint.sent.size(), 1u);
    EXPECT_EQ(endpoint.sent[0][1], 0x06u);
    EXPECT_EQ(endpoint.sent[0][2], 0x01u);
    const auto response = decode_sent("dh_pulse_config_response", endpoint.sent[0]);
    EXPECT_EQ(response.get_unsigned("seq").value_or(0), 0x2468u);
}

TEST(HardwareTestServiceTest, HelmBoardTestUsesOrdinaryResponseAndEchoesSequence) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    endpoint.received.push_back(make_request("helm_board_test_request", 0x1357));
    HWTest::HardwareTestService service(endpoint, provider, 9);

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    ASSERT_EQ(provider.requests,
              std::vector<std::string>{"helm_board_test_request"});
    ASSERT_EQ(endpoint.sent.size(), 1u);
    const auto response = decode_sent("helm_board_test_response", endpoint.sent[0]);
    EXPECT_EQ(response.get_unsigned("seq").value_or(0), 0x1357u);
    EXPECT_EQ(response.get_unsigned("status").value_or(1), 0u);
    EXPECT_EQ(response.get_unsigned("err_code").value_or(1), 0u);
}

TEST(HardwareTestServiceTest, DhSequenceWrapsFromFFFFToZero) {
    RecordingEndpoint endpoint;
    FakeProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 0));
    ASSERT_TRUE(request.set_unsigned("return_enable", 0));
    ASSERT_TRUE(request.set_unsigned("channel[0]", 1));
    ASSERT_TRUE(request.set_unsigned("channel[1]", 0));
    ASSERT_TRUE(request.set_unsigned("seq", 0xFFFFu));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("delay_frames", 0));
    ASSERT_TRUE(request.set_unsigned("interval_us", 2500));
    endpoint.received.emplace_back(request.bytes().begin(), request.bytes().end());
    HWTest::HardwareTestService service(endpoint, provider, 3,
                                        [](std::chrono::microseconds) {});

    ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
    ASSERT_EQ(endpoint.sent.size(), 2u);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[0])
                  .get_unsigned("seq").value_or(0), 0xFFFFu);
    EXPECT_EQ(decode_sent("dh_control_response", endpoint.sent[1])
                  .get_unsigned("seq").value_or(1), 0u);
}

TEST(HardwareTestServiceTest, AcceptsOversizedPayloadAndReturnsLengthError) {
    for (const size_t length : {size_t{124}, size_t{255}}) {
        RecordingEndpoint endpoint;
        FakeProvider provider;
        std::vector<uint8_t> payload(length, 0);
        payload[0] = 0x11;
        payload[1] = 0x04;
        payload[2] = 0x01;
        payload[3] = 0x34;
        payload[4] = 0x12;
        endpoint.received.push_back(std::move(payload));
        HWTest::HardwareTestService service(endpoint, provider);

        ASSERT_TRUE(service.process_once(HW::Timeout::poll()));
        ASSERT_EQ(endpoint.sent.size(), 1u);
        const auto error = decode_sent("error_response", endpoint.sent[0]);
        EXPECT_EQ(error.get_unsigned("err_code").value_or(0),
                  static_cast<uint16_t>(HWTest::ProductErrorCode::LenMismatch));
        EXPECT_EQ(error.get_unsigned("orig_seq").value_or(0), 0x1234u);
    }
}

TEST(HardwareTestServiceTest, DropsProtocolErrorAndProcessesFollowingRequest) {
    RecordingEndpoint endpoint;
    endpoint.receive_failures.push_back(
        HW::Status::error(HW::StatusCode::ProtocolError, 0, "bad physical frame"));
    endpoint.received.push_back(make_request("system_status_request"));
    FakeProvider provider;
    HWTest::HardwareTestService service(endpoint, provider);

    EXPECT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_TRUE(endpoint.sent.empty());
    EXPECT_TRUE(service.process_once(HW::Timeout::poll()));
    EXPECT_EQ(endpoint.sent.size(), 1u);
}

TEST(HardwareTestServiceTest, ReceiveFailureStopsWorkerWithoutExternalSignal) {
    RecordingEndpoint endpoint;
    endpoint.receive_failures.push_back(
        HW::Status::error(HW::StatusCode::IoError, 0, "link lost"));
    FakeProvider provider;
    std::atomic_bool stop{false};
    HWTest::HardwareTestService service(endpoint, provider);

    auto running = std::async(std::launch::async, [&]() {
        return service.run([&]() {
            return stop.load(std::memory_order_relaxed);
        });
    });
    const auto ready = running.wait_for(std::chrono::milliseconds(200));
    if (ready != std::future_status::ready) {
        stop.store(true, std::memory_order_relaxed);
        ASSERT_EQ(running.wait_for(std::chrono::seconds(1)),
                  std::future_status::ready);
    }
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_EQ(running.get(), 1);
}

TEST(HardwareTestServiceTest, HelmFeedbackDoesNotSerializeBoardRequestHandling) {
    RecordingEndpoint endpoint;
    ConcurrentCallDetectingProvider provider;
    endpoint.received.push_back(make_request("system_status_request"));
    HWTest::HardwareTestService service(endpoint, provider);

    auto processing = std::async(std::launch::async, [&]() {
        return service.process_once(HW::Timeout::poll());
    });
    provider.wait_until_handle_entered();

    auto feedback = std::async(std::launch::async, [&]() {
        return service.emit_helm_feedback_once();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(provider.feedback_has_started());
    EXPECT_TRUE(provider.feedback_overlapped());

    provider.allow_handle();
    EXPECT_TRUE(processing.get());
    EXPECT_TRUE(feedback.get());
    EXPECT_TRUE(provider.feedback_has_started());
    EXPECT_TRUE(provider.feedback_overlapped());
}
