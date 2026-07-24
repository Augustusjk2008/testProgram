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
        response.set_signed("zl[0][0]", 123);
        return next_error;
    }

    HWTest::ProductErrorCode next_error{HWTest::ProductErrorCode::Ok};
    bool feedback_active{false};
    int feedback_calls{0};
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
        events.emplace_back("begin");
        return HWTest::ProductErrorCode::Ok;
    }

    HWTest::ProductErrorCode handle_dh_control_report(const HWTest::ProductMessage&,
                                                      HWTest::ProductMessage&,
                                                      size_t report_index) override {
        events.emplace_back("report" + std::to_string(report_index));
        return HWTest::ProductErrorCode::Ok;
    }

    bool helm_feedback_active() const override { return false; }
    HWTest::ProductErrorCode build_helm_feedback(HWTest::ProductMessage&) override {
        return HWTest::ProductErrorCode::Ok;
    }

    std::vector<std::string> events;
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
    ASSERT_TRUE(request.set_unsigned("seq", 0x9000));
    ASSERT_TRUE(request.set_unsigned("report_count", 3));
    ASSERT_TRUE(request.set_unsigned("delay_us", 0));
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

TEST(HardwareTestServiceTest, RejectsDhIntervalBelowMinimumBeforeBeginningDh) {
    RecordingEndpoint endpoint;
    DhOrderingProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 1));
    ASSERT_TRUE(request.set_unsigned("return_enable", 1));
    ASSERT_TRUE(request.set_unsigned("seq", 0x2345));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("delay_us", 0));
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
    EXPECT_EQ(feedback.get_signed("zl[0][0]").value_or(0), 123);
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

TEST(HardwareTestServiceTest, SendsEachDhReportWithoutAddingTransmitTimeToCadence) {
    RecordingEndpoint endpoint;
    DhOrderingProvider provider;
    HWTest::ProductProtocol builder;
    auto request = builder.create_message("dh_control_request", false);
    ASSERT_TRUE(request.set_unsigned("power_enable", 0));
    ASSERT_TRUE(request.set_unsigned("return_enable", 0));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("delay_us", 0));
    ASSERT_TRUE(request.set_unsigned("interval_us", 10000));
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
    EXPECT_LT(cadence_waits.front(), std::chrono::microseconds(9000));
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
    ASSERT_TRUE(request.set_unsigned("seq", 0xFFFFu));
    ASSERT_TRUE(request.set_unsigned("report_count", 2));
    ASSERT_TRUE(request.set_unsigned("delay_us", 0));
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

TEST(HardwareTestServiceTest, SerializesProviderCallsAcrossReceiveAndFeedbackThreads) {
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
    EXPECT_FALSE(provider.feedback_has_started());

    provider.allow_handle();
    EXPECT_TRUE(processing.get());
    EXPECT_TRUE(feedback.get());
    EXPECT_TRUE(provider.feedback_has_started());
    EXPECT_FALSE(provider.feedback_overlapped());
}
