#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/Gateway/DdsGatewayLocalBus.h"
#include "MB_DDF/DDS/Gateway/DomainGateway.h"

#include <csignal>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace MB_DDF::DDS;

namespace {

class FakeLocalBus : public GatewayLocalBus {
public:
    struct PublishedMessage {
        std::string topic_name;
        std::vector<uint8_t> payload;
        uint64_t sequence{0};
    };

    explicit FakeLocalBus(std::vector<LocalTopicInfo> initial_topics)
        : topics(std::move(initial_topics)) {}

    std::vector<LocalTopicInfo> list_topics() override {
        return topics;
    }

    bool subscribe_topic(const std::string& topic_name,
                         LocalMessageCallback callback,
                         uint64_t start_after_sequence) override {
        subscribe_attempts.emplace_back(topic_name, start_after_sequence);
        if (subscribe_failures_remaining > 0) {
            --subscribe_failures_remaining;
            return false;
        }
        callbacks[topic_name] = std::move(callback);
        return true;
    }

    uint64_t publish_topic(
        const std::string& topic_name,
        const void* data,
        size_t size,
        const LocalSequenceAssignedCallback& before_visible = {}) override {
        const uint64_t sequence = ++next_sequence[topic_name];
        PublishedMessage message;
        message.topic_name = topic_name;
        message.sequence = sequence;
        if (data != nullptr && size > 0) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            message.payload.assign(bytes, bytes + size);
        }
        published.push_back(std::move(message));

        if (before_visible) {
            before_visible(sequence);
        }

        if (emit_observer_during_publish) {
            auto it = callbacks.find(topic_name);
            if (it != callbacks.end()) {
                LocalMessageView view;
                view.topic_name = topic_name;
                view.sequence = sequence;
                view.timestamp = 1234;
                view.data = data;
                view.size = size;
                it->second(view);
            }
        }
        return sequence;
    }

    uint64_t emit_local(const std::string& topic_name,
                        const std::vector<uint8_t>& payload,
                        uint64_t sequence = 0) {
        if (sequence == 0) {
            sequence = ++next_sequence[topic_name];
        } else {
            next_sequence[topic_name] = std::max(next_sequence[topic_name], sequence);
        }

        auto it = callbacks.find(topic_name);
        if (it != callbacks.end()) {
            LocalMessageView view;
            view.topic_name = topic_name;
            view.sequence = sequence;
            view.timestamp = 1234;
            view.data = payload.empty() ? nullptr : payload.data();
            view.size = payload.size();
            it->second(view);
        }
        return sequence;
    }

    std::vector<LocalTopicInfo> topics;
    std::map<std::string, LocalMessageCallback> callbacks;
    std::map<std::string, uint64_t> next_sequence;
    std::vector<PublishedMessage> published;
    std::vector<std::pair<std::string, uint64_t>> subscribe_attempts;
    size_t subscribe_failures_remaining{0};
    bool emit_observer_during_publish{false};
};

class GatedDdsGatewayLocalBus : public DdsGatewayLocalBus {
public:
    std::vector<LocalTopicInfo> list_topics() override {
        const size_t call_index = list_call_count_.fetch_add(1, std::memory_order_acq_rel);
        if (call_index > 0) {
            std::unique_lock<std::mutex> lock(scan_gate_mutex_);
            scan_gate_cv_.wait(lock, [this] { return late_scans_allowed_; });
        }
        return DdsGatewayLocalBus::list_topics();
    }

    void allow_late_scans() {
        {
            std::lock_guard<std::mutex> lock(scan_gate_mutex_);
            late_scans_allowed_ = true;
        }
        scan_gate_cv_.notify_all();
    }

private:
    std::atomic<size_t> list_call_count_{0};
    std::mutex scan_gate_mutex_;
    std::condition_variable scan_gate_cv_;
    bool late_scans_allowed_{false};
};

class FakeEndpoint : public ExternalEndpoint {
public:
    bool send(const uint8_t* data, size_t size) override {
        if (data == nullptr && size > 0) {
            return false;
        }

        std::vector<uint8_t> frame;
        if (size > 0) {
            frame.assign(data, data + size);
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            sent.push_back(frame);
        }

        auto peer = peer_endpoint.lock();
        if (peer) {
            peer->push_rx(frame);
        }

        {
            std::unique_lock<std::mutex> lock(send_block_mutex);
            if (block_send) {
                send_entered = true;
                send_block_cv.notify_all();
                send_block_cv.wait(lock, [&] { return !block_send; });
            }
        }

        return send_ok;
    }

    int32_t receive(uint8_t* data, size_t capacity) override {
        if (data == nullptr && capacity > 0) {
            return -1;
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (rx.empty()) {
            return 0;
        }

        auto frame = std::move(rx.front());
        rx.pop_front();
        const size_t copy_size = std::min(capacity, frame.size());
        if (copy_size > 0) {
            std::memcpy(data, frame.data(), copy_size);
        }
        return static_cast<int32_t>(copy_size);
    }

    int32_t receive(uint8_t* data, size_t capacity, uint32_t timeout_us) override {
        const int32_t immediate = receive(data, capacity);
        if (immediate != 0 || timeout_us == 0) {
            return immediate;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(
            std::min<uint32_t>(timeout_us, 1000)));
        return receive(data, capacity);
    }

    size_t mtu() const override {
        return mtu_value;
    }

    void push_rx(const std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock(mutex);
        rx.push_back(frame);
    }

    size_t sent_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return sent.size();
    }

    std::vector<uint8_t> sent_frame(size_t index) const {
        std::lock_guard<std::mutex> lock(mutex);
        return sent.at(index);
    }

    void enable_send_block() {
        std::lock_guard<std::mutex> lock(send_block_mutex);
        block_send = true;
        send_entered = false;
    }

    bool wait_for_send_entry(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(send_block_mutex);
        return send_block_cv.wait_for(lock, timeout, [&] { return send_entered; });
    }

    void release_send() {
        std::lock_guard<std::mutex> lock(send_block_mutex);
        block_send = false;
        send_block_cv.notify_all();
    }

    bool send_ok{true};
    size_t mtu_value{4096};
    std::weak_ptr<FakeEndpoint> peer_endpoint;

private:
    mutable std::mutex mutex;
    std::mutex send_block_mutex;
    std::condition_variable send_block_cv;
    bool block_send{false};
    bool send_entered{false};
    std::deque<std::vector<uint8_t>> rx;
    std::vector<std::vector<uint8_t>> sent;
};

std::pair<std::shared_ptr<FakeEndpoint>, std::shared_ptr<FakeEndpoint>> make_link() {
    auto first = std::make_shared<FakeEndpoint>();
    auto second = std::make_shared<FakeEndpoint>();
    first->peer_endpoint = second;
    second->peer_endpoint = first;
    return {first, second};
}

GatewayEnvelope make_remote_envelope(uint32_t origin_domain,
                                     uint32_t sender_domain,
                                     uint64_t gateway_id,
                                     uint64_t message_id,
                                     uint8_t ttl,
                                     const std::string& topic_name,
                                     const std::vector<uint8_t>& payload) {
    GatewayEnvelope envelope;
    envelope.header.origin_domain_id = origin_domain;
    envelope.header.sender_domain_id = sender_domain;
    envelope.header.origin_gateway_id = gateway_id;
    envelope.header.message_id = message_id;
    envelope.header.ttl = ttl;
    envelope.topic_name = topic_name;
    envelope.payload = payload;
    return envelope;
}

std::vector<LocalTopicInfo> one_topic() {
    return {{1, "rt://sensor/temp", 1024}};
}

DomainGatewayConfig config(uint32_t domain_id, uint64_t gateway_id) {
    DomainGatewayConfig cfg;
    cfg.domain_id = domain_id;
    cfg.gateway_id = gateway_id;
    cfg.topic_scan_period_ms = 1000;
    cfg.default_ttl = 4;
    return cfg;
}

bool wait_for_sent_count(const std::shared_ptr<FakeEndpoint>& endpoint,
                         size_t expected,
                         std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (endpoint->sent_count() >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return endpoint->sent_count() >= expected;
}

} // namespace

TEST(DomainGatewayTest, ScanTopicsSubscribesNormalTopicsOnly) {
    auto bus = std::make_shared<FakeLocalBus>(std::vector<LocalTopicInfo>{
        {1, "rt://sensor/temp", 1024},
        {2, "gateway://status", 1024},
        {3, "", 1024},
    });
    DomainGateway gateway(config(1, 101), bus);

    gateway.scan_topics_once();

    EXPECT_NE(bus->callbacks.find("rt://sensor/temp"), bus->callbacks.end());
    EXPECT_EQ(bus->callbacks.find("gateway://status"), bus->callbacks.end());
    EXPECT_EQ(bus->callbacks.find(""), bus->callbacks.end());
}

TEST(DomainGatewayTest, InitialTopicRetryPreservesStartupSequenceBoundary) {
    constexpr uint64_t kStartupBoundary = 42;
    auto bus = std::make_shared<FakeLocalBus>(std::vector<LocalTopicInfo>{
        {1, "rt://sensor/retry", 1024, kStartupBoundary},
    });
    bus->subscribe_failures_remaining = 1;
    DomainGateway gateway(config(1, 101), bus);

    ASSERT_TRUE(gateway.start());
    gateway.stop();
    gateway.scan_topics_once();

    ASSERT_EQ(bus->subscribe_attempts.size(), 2u);
    EXPECT_EQ(bus->subscribe_attempts[0].second, kStartupBoundary);
    EXPECT_EQ(bus->subscribe_attempts[1].second, kStartupBoundary);
}

TEST(DomainGatewayTest, RestartSkipsHistoryForTopicCreatedWhileStopped) {
    constexpr uint64_t kRestartBoundary = 73;
    auto bus = std::make_shared<FakeLocalBus>(std::vector<LocalTopicInfo>{});
    DomainGateway gateway(config(1, 101), bus);

    ASSERT_TRUE(gateway.start());
    gateway.stop();

    bus->topics.push_back(
        {1, "rt://sensor/created-while-stopped", 1024, kRestartBoundary});

    ASSERT_TRUE(gateway.start());
    gateway.stop();

    ASSERT_EQ(bus->subscribe_attempts.size(), 1u);
    EXPECT_EQ(bus->subscribe_attempts[0].second, kRestartBoundary);
}

TEST(DomainGatewayTest, LocalTopicMessageSendsToAllEnabledEndpoints) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto endpoint_a = std::make_shared<FakeEndpoint>();
    auto endpoint_b = std::make_shared<FakeEndpoint>();
    DomainGateway gateway(config(2, 202), bus);
    ASSERT_TRUE(gateway.add_endpoint({"a", endpoint_a, true}));
    ASSERT_TRUE(gateway.add_endpoint({"b", endpoint_b, true}));
    gateway.scan_topics_once();

    const std::vector<uint8_t> payload{'T', '1'};
    bus->emit_local("rt://sensor/temp", payload, 10);

    ASSERT_EQ(endpoint_a->sent_count(), 1u);
    ASSERT_EQ(endpoint_b->sent_count(), 1u);

    GatewayEnvelope envelope;
    const auto frame = endpoint_a->sent_frame(0);
    ASSERT_TRUE(deserialize_gateway_envelope(frame.data(), frame.size(), envelope));
    EXPECT_EQ(envelope.header.origin_domain_id, 2u);
    EXPECT_EQ(envelope.header.sender_domain_id, 2u);
    EXPECT_EQ(envelope.header.origin_gateway_id, 202u);
    EXPECT_EQ(envelope.header.message_id, 1u);
    EXPECT_EQ(envelope.header.ttl, 4u);
    EXPECT_EQ(envelope.topic_name, "rt://sensor/temp");
    EXPECT_EQ(envelope.payload, payload);
}

TEST(DomainGatewayTest, RemoteMessagePublishesAndForwardsExceptIngress) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto ingress = std::make_shared<FakeEndpoint>();
    auto egress = std::make_shared<FakeEndpoint>();
    DomainGateway gateway(config(2, 202), bus);
    ASSERT_TRUE(gateway.add_endpoint({"ingress", ingress, true}));
    ASSERT_TRUE(gateway.add_endpoint({"egress", egress, true}));

    const std::vector<uint8_t> payload{'R', 'X'};
    const auto remote = make_remote_envelope(1, 1, 101, 77, 3, "rt://sensor/temp", payload);
    const auto frame = serialize_gateway_envelope(remote);
    ingress->push_rx(frame);

    gateway.poll_once(0);

    ASSERT_EQ(bus->published.size(), 1u);
    EXPECT_EQ(bus->published[0].topic_name, "rt://sensor/temp");
    EXPECT_EQ(bus->published[0].payload, payload);
    EXPECT_EQ(ingress->sent_count(), 0u);
    ASSERT_EQ(egress->sent_count(), 1u);

    GatewayEnvelope forwarded;
    const auto forwarded_frame = egress->sent_frame(0);
    ASSERT_TRUE(deserialize_gateway_envelope(forwarded_frame.data(), forwarded_frame.size(), forwarded));
    EXPECT_EQ(forwarded.header.origin_domain_id, 1u);
    EXPECT_EQ(forwarded.header.sender_domain_id, 2u);
    EXPECT_EQ(forwarded.header.origin_gateway_id, 101u);
    EXPECT_EQ(forwarded.header.message_id, 77u);
    EXPECT_EQ(forwarded.header.ttl, 2u);
}

TEST(DomainGatewayTest, RemoteInjectedSequenceIsSuppressed) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto ingress = std::make_shared<FakeEndpoint>();
    auto egress = std::make_shared<FakeEndpoint>();
    DomainGateway gateway(config(2, 202), bus);
    ASSERT_TRUE(gateway.add_endpoint({"ingress", ingress, true}));
    ASSERT_TRUE(gateway.add_endpoint({"egress", egress, true}));
    gateway.scan_topics_once();

    const std::vector<uint8_t> payload{'R'};
    const auto remote = make_remote_envelope(1, 1, 101, 88, 2, "rt://sensor/temp", payload);
    ingress->push_rx(serialize_gateway_envelope(remote));
    gateway.poll_once(0);

    ASSERT_EQ(bus->published.size(), 1u);
    ASSERT_EQ(egress->sent_count(), 1u);

    bus->emit_local("rt://sensor/temp", payload, bus->published[0].sequence);
    EXPECT_EQ(egress->sent_count(), 1u);
    EXPECT_EQ(ingress->sent_count(), 0u);
}

TEST(DomainGatewayTest, DuplicateRemoteMessageIsDropped) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto ingress = std::make_shared<FakeEndpoint>();
    auto egress = std::make_shared<FakeEndpoint>();
    DomainGateway gateway(config(2, 202), bus);
    ASSERT_TRUE(gateway.add_endpoint({"ingress", ingress, true}));
    ASSERT_TRUE(gateway.add_endpoint({"egress", egress, true}));

    const std::vector<uint8_t> payload{'D'};
    const auto remote = make_remote_envelope(1, 1, 101, 99, 3, "rt://sensor/temp", payload);
    const auto frame = serialize_gateway_envelope(remote);
    ingress->push_rx(frame);
    ingress->push_rx(frame);

    gateway.poll_once(0);
    gateway.poll_once(0);

    EXPECT_EQ(bus->published.size(), 1u);
    EXPECT_EQ(egress->sent_count(), 1u);
}

TEST(DomainGatewayTest, FakeTopologyOneTwoThreeFloodsThroughMiddle) {
    auto bus1 = std::make_shared<FakeLocalBus>(one_topic());
    auto bus2 = std::make_shared<FakeLocalBus>(one_topic());
    auto bus3 = std::make_shared<FakeLocalBus>(one_topic());

    auto [ep12, ep21] = make_link();
    auto [ep23, ep32] = make_link();

    DomainGateway gateway1(config(1, 101), bus1);
    DomainGateway gateway2(config(2, 202), bus2);
    DomainGateway gateway3(config(3, 303), bus3);

    ASSERT_TRUE(gateway1.add_endpoint({"to2", ep12, true}));
    ASSERT_TRUE(gateway2.add_endpoint({"to1", ep21, true}));
    ASSERT_TRUE(gateway2.add_endpoint({"to3", ep23, true}));
    ASSERT_TRUE(gateway3.add_endpoint({"to2", ep32, true}));

    gateway1.scan_topics_once();
    gateway2.scan_topics_once();
    gateway3.scan_topics_once();

    const std::vector<uint8_t> from_one{'1'};
    bus1->emit_local("rt://sensor/temp", from_one, 1);
    gateway2.poll_once(0);
    gateway3.poll_once(0);

    ASSERT_EQ(bus3->published.size(), 1u);
    EXPECT_EQ(bus3->published[0].payload, from_one);
    EXPECT_EQ(bus2->published.size(), 1u);

    const std::vector<uint8_t> from_three{'3'};
    bus3->emit_local("rt://sensor/temp", from_three, 2);
    gateway2.poll_once(1);
    gateway1.poll_once(0);

    ASSERT_EQ(bus1->published.size(), 1u);
    EXPECT_EQ(bus1->published[0].payload, from_three);
}

TEST(DomainGatewayTest, StopPreventsMonitoredTopicsFromSending) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto endpoint = std::make_shared<FakeEndpoint>();
    auto cfg = config(1, 101);
    cfg.topic_scan_period_ms = 1;
    DomainGateway gateway(cfg, bus);
    ASSERT_TRUE(gateway.add_endpoint({"egress", endpoint, true}));
    ASSERT_TRUE(gateway.start());

    bus->emit_local("rt://sensor/temp", {'A'}, 1);
    ASSERT_EQ(endpoint->sent_count(), 1u);

    gateway.stop();
    bus->emit_local("rt://sensor/temp", {'B'}, 2);
    EXPECT_EQ(endpoint->sent_count(), 1u);

    ASSERT_TRUE(gateway.start());
    bus->emit_local("rt://sensor/temp", {'C'}, 3);
    EXPECT_EQ(endpoint->sent_count(), 2u);
    gateway.stop();
}

TEST(DomainGatewayTest, DestructorWaitsForInFlightLocalCallback) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto endpoint = std::make_shared<FakeEndpoint>();
    endpoint->enable_send_block();

    auto gateway = std::make_unique<DomainGateway>(config(1, 101), bus);
    ASSERT_TRUE(gateway->add_endpoint({"egress", endpoint, true}));
    gateway->scan_topics_once();

    std::thread callback_thread([&] {
        bus->emit_local("rt://sensor/temp", {'A'}, 1);
    });
    if (!endpoint->wait_for_send_entry(std::chrono::milliseconds(1000))) {
        endpoint->release_send();
        callback_thread.join();
        FAIL() << "local callback did not enter endpoint send";
    }

    std::atomic<bool> destroyed{false};
    std::thread destroy_thread([&] {
        gateway.reset();
        destroyed.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(destroyed.load(std::memory_order_acquire));

    endpoint->release_send();
    callback_thread.join();
    destroy_thread.join();
    EXPECT_TRUE(destroyed.load(std::memory_order_acquire));
}

TEST(DomainGatewayTest, InlineObserverCannotBeatRemoteSequenceSuppression) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto ingress = std::make_shared<FakeEndpoint>();
    auto egress = std::make_shared<FakeEndpoint>();
    DomainGateway gateway(config(2, 202), bus);
    ASSERT_TRUE(gateway.add_endpoint({"ingress", ingress, true}));
    ASSERT_TRUE(gateway.add_endpoint({"egress", egress, true}));
    gateway.scan_topics_once();
    bus->emit_observer_during_publish = true;

    const auto remote = make_remote_envelope(
        1, 1, 101, 501, 1, "rt://sensor/temp", {'R'});
    ingress->push_rx(serialize_gateway_envelope(remote));
    gateway.poll_once(0);

    EXPECT_EQ(ingress->sent_count(), 0u);
    EXPECT_EQ(egress->sent_count(), 0u);
}

TEST(DomainGatewayTest, ConsumedSuppressionCannotEvictReusedSequencePrematurely) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto ingress = std::make_shared<FakeEndpoint>();
    DomainGateway gateway(config(2, 202), bus);
    ASSERT_TRUE(gateway.add_endpoint({"ingress", ingress, true}));
    gateway.scan_topics_once();

    const std::string topic_name = "rt://sensor/temp";
    auto first = make_remote_envelope(1, 1, 101, 1, 1, topic_name, {'R'});
    ingress->push_rx(serialize_gateway_envelope(first));
    gateway.poll_once(0);
    ASSERT_EQ(bus->published.back().sequence, 1u);
    bus->emit_local(topic_name, {'R'}, 1);

    // 填满窗口但不触发观察回调，随后复用已消费过的sequence=1。
    // 旧实现的顺序deque仍残留第一次的key，会错误淘汰刚加入的新key。
    for (uint64_t message_id = 2; message_id <= 4097; ++message_id) {
        const auto remote = make_remote_envelope(
            1, 1, 101, message_id, 1, topic_name, {'R'});
        ingress->push_rx(serialize_gateway_envelope(remote));
        gateway.poll_once(0);
    }

    bus->next_sequence[topic_name] = 0;
    const auto reused = make_remote_envelope(1, 1, 101, 5000, 1, topic_name, {'N'});
    ingress->push_rx(serialize_gateway_envelope(reused));
    gateway.poll_once(0);
    ASSERT_EQ(bus->published.back().sequence, 1u);

    bus->emit_local(topic_name, {'N'}, 1);
    EXPECT_EQ(ingress->sent_count(), 0u);
}

TEST(DomainGatewayTest, RemoteInternalTopicIsDroppedWithoutPublishingOrForwarding) {
    auto bus = std::make_shared<FakeLocalBus>(one_topic());
    auto ingress = std::make_shared<FakeEndpoint>();
    auto egress = std::make_shared<FakeEndpoint>();
    DomainGateway gateway(config(2, 202), bus);
    ASSERT_TRUE(gateway.add_endpoint({"ingress", ingress, true}));
    ASSERT_TRUE(gateway.add_endpoint({"egress", egress, true}));

    const auto remote = make_remote_envelope(
        1, 1, 101, 601, 3, "gateway://status", {'X'});
    ingress->push_rx(serialize_gateway_envelope(remote));
    gateway.poll_once(0);

    EXPECT_TRUE(bus->published.empty());
    EXPECT_EQ(ingress->sent_count(), 0u);
    EXPECT_EQ(egress->sent_count(), 0u);
}

class DdsGatewayLocalBusTest : public ::testing::Test {
protected:
    void SetUp() override {
        DDSCore::instance().shutdown();
        shm_unlink("/MB_DDF_V2_SHM");
        sem_unlink("/MB_DDF_V2_SHM_sem");
    }

    void TearDown() override {
        DDSCore::instance().shutdown();
        shm_unlink("/MB_DDF_V2_SHM");
        sem_unlink("/MB_DDF_V2_SHM_sem");
    }
};

TEST_F(DdsGatewayLocalBusTest, InitialTopicHistoryIsSkippedButFreshMessageIsForwarded) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    constexpr char topic[] = "rt://gateway/preexisting-topic";
    const char historical_payload[] = "historical";
    const char fresh_payload[] = "fresh";
    auto publisher = dds.create_publisher(topic, true);
    ASSERT_NE(publisher, nullptr);
    ASSERT_TRUE(publisher->publish(historical_payload, sizeof(historical_payload)));

    auto bus = std::make_shared<DdsGatewayLocalBus>();
    auto endpoint = std::make_shared<FakeEndpoint>();
    auto cfg = config(1, 101);
    cfg.topic_scan_period_ms = 10;

    {
        DomainGateway gateway(cfg, bus);
        ASSERT_TRUE(gateway.add_endpoint({"egress", endpoint, true}));
        ASSERT_TRUE(gateway.start());

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ASSERT_EQ(endpoint->sent_count(), 0u)
            << "gateway replayed a message that existed before start";

        ASSERT_TRUE(publisher->publish(fresh_payload, sizeof(fresh_payload)));
        ASSERT_TRUE(wait_for_sent_count(endpoint, 1, std::chrono::seconds(1)));
        gateway.stop();

        ASSERT_EQ(endpoint->sent_count(), 1u);
        GatewayEnvelope envelope;
        const auto frame = endpoint->sent_frame(0);
        ASSERT_TRUE(deserialize_gateway_envelope(frame.data(), frame.size(), envelope));
        EXPECT_EQ(envelope.topic_name, topic);
        const std::vector<uint8_t> expected_payload(
            reinterpret_cast<const uint8_t*>(fresh_payload),
            reinterpret_cast<const uint8_t*>(fresh_payload) + sizeof(fresh_payload));
        EXPECT_EQ(envelope.payload, expected_payload);
    }

    bus.reset();
    publisher.reset();
}

TEST_F(DdsGatewayLocalBusTest, LateDiscoveredTopicForwardsMessagePublishedAfterGatewayStart) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    auto bus = std::make_shared<GatedDdsGatewayLocalBus>();
    auto endpoint = std::make_shared<FakeEndpoint>();
    auto cfg = config(1, 101);
    cfg.topic_scan_period_ms = 1;

    {
        DomainGateway gateway(cfg, bus);
        ASSERT_TRUE(gateway.add_endpoint({"egress", endpoint, true}));
        ASSERT_TRUE(gateway.start());

        constexpr char topic[] = "rt://gateway/late-topic";
        const char payload[] = "published-before-discovery";
        auto publisher = dds.create_publisher(topic, true);
        const bool published = publisher != nullptr &&
                               publisher->publish(payload, sizeof(payload));

        // start() 的首次扫描已经完成；放行后台扫描前先完成发布，保证 observer
        // 注册边界稳定落在首条消息之后。随后显式扫描，不依赖周期 sleep 的时序。
        bus->allow_late_scans();
        ASSERT_NE(publisher, nullptr);
        ASSERT_TRUE(published);
        gateway.scan_topics_once();

        const bool forwarded = wait_for_sent_count(endpoint, 1, std::chrono::seconds(1));
        gateway.stop();

        ASSERT_TRUE(forwarded)
            << "message published after gateway start but before topic discovery was skipped";
        ASSERT_EQ(endpoint->sent_count(), 1u);
        GatewayEnvelope envelope;
        const auto frame = endpoint->sent_frame(0);
        ASSERT_TRUE(deserialize_gateway_envelope(frame.data(), frame.size(), envelope));
        EXPECT_EQ(envelope.topic_name, topic);
        const std::vector<uint8_t> expected_payload(
            reinterpret_cast<const uint8_t*>(payload),
            reinterpret_cast<const uint8_t*>(payload) + sizeof(payload));
        EXPECT_EQ(envelope.payload, expected_payload);
    }

    bus.reset();
}

TEST_F(DdsGatewayLocalBusTest, BeforeVisibleRejectsSameTopicObserverReentryWithoutBlocking) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        auto& dds = DDSCore::instance();
        if (!dds.initialize(16 * 1024 * 1024)) {
            _exit(10);
        }

        int result = 0;
        {
            constexpr char topic[] = "rt://gateway/before-visible-reentry";
            const char payload[] = "reentry";
            DdsGatewayLocalBus bus;
            std::shared_ptr<Subscriber> reentrant_observer;
            bool callback_called = false;

            signal(SIGALRM, SIG_DFL);
            alarm(2);
            const uint64_t sequence = bus.publish_topic(
                topic,
                payload,
                sizeof(payload),
                [&](uint64_t) {
                    callback_called = true;
                    reentrant_observer = dds.create_observer(
                        topic, [](const LocalMessageView&) {});
                });
            alarm(0);

            if (!callback_called) {
                result = 11;
            } else if (sequence == 0) {
                result = 12;
            } else if (reentrant_observer != nullptr) {
                result = 13;
            }

            reentrant_observer.reset();
            auto observer_after_callback = dds.create_observer(
                topic, [](const LocalMessageView&) {});
            if (result == 0 && observer_after_callback == nullptr) {
                result = 14;
            }
            observer_after_callback.reset();
        }

        dds.shutdown();
        _exit(result);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);

    ASSERT_EQ(waited, child);
    if (!WIFEXITED(status)) {
        ASSERT_TRUE(WIFEXITED(status))
            << "isolated reentry child terminated by signal " << WTERMSIG(status);
        return;
    }
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "isolated reentry child returned diagnostic code " << WEXITSTATUS(status);
}

TEST_F(DdsGatewayLocalBusTest, BeforeVisibleRejectsNestedPublishWithoutBlocking) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        auto& dds = DDSCore::instance();
        if (!dds.initialize(16 * 1024 * 1024)) {
            _exit(20);
        }

        constexpr char topic[] = "rt://gateway/before-visible-nested-publish";
        const char outer_payload[] = "outer";
        const char nested_payload[] = "nested";
        auto publisher = dds.create_publisher(topic, true);
        if (publisher == nullptr) {
            dds.shutdown();
            _exit(21);
        }

        bool callback_called = false;
        bool nested_published = true;
        signal(SIGALRM, SIG_DFL);
        alarm(2);
        const uint64_t sequence = publisher->publish_and_get_sequence(
            outer_payload,
            sizeof(outer_payload),
            [&](uint64_t) {
                callback_called = true;
                nested_published = publisher->publish(
                    nested_payload, sizeof(nested_payload));
            });
        alarm(0);

        int result = 0;
        if (!callback_called) {
            result = 22;
        } else if (sequence == 0) {
            result = 23;
        } else if (nested_published) {
            result = 24;
        }

        publisher.reset();
        dds.shutdown();
        _exit(result);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);

    ASSERT_EQ(waited, child);
    if (!WIFEXITED(status)) {
        ASSERT_TRUE(WIFEXITED(status))
            << "isolated nested publish child terminated by signal " << WTERMSIG(status);
        return;
    }
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "isolated nested publish child returned diagnostic code " << WEXITSTATUS(status);
}

TEST_F(DdsGatewayLocalBusTest, ListsTopicsAndPublishesWithSequence) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));
    ASSERT_NE(dds.create_publisher("rt://gateway/localbus"), nullptr);

    DdsGatewayLocalBus bus;
    const auto topics = bus.list_topics();
    const auto found = std::find_if(topics.begin(), topics.end(), [](const LocalTopicInfo& topic) {
        return topic.topic_name == "rt://gateway/localbus";
    });
    ASSERT_NE(found, topics.end());
    EXPECT_NE(found->topic_id, 0u);

    const char payload[] = "bus-publish";
    EXPECT_GT(bus.publish_topic("rt://gateway/localbus", payload, sizeof(payload)), 0u);
}

TEST_F(DdsGatewayLocalBusTest, SubscribeTopicReceivesLocalMessageView) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    DdsGatewayLocalBus bus;
    std::atomic<bool> received{false};
    uint64_t observed_sequence = 0;
    std::string observed_payload;

    ASSERT_TRUE(bus.subscribe_topic("rt://gateway/observe",
        [&](const LocalMessageView& message) {
            observed_sequence = message.sequence;
            observed_payload.assign(static_cast<const char*>(message.data), message.size - 1);
            received.store(true, std::memory_order_release);
        }, 0));

    const char payload[] = "observed";
    const uint64_t sequence = bus.publish_topic("rt://gateway/observe", payload, sizeof(payload));
    ASSERT_GT(sequence, 0u);

    for (int i = 0; i < 50 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(received.load(std::memory_order_acquire));
    EXPECT_EQ(observed_sequence, sequence);
    EXPECT_EQ(observed_payload, "observed");
}

TEST_F(DdsGatewayLocalBusTest, SequenceHookRunsBeforeObserverCanSeeMessage) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    DdsGatewayLocalBus bus;
    std::atomic<bool> hook_called{false};
    std::atomic<bool> observer_saw_hook{false};
    std::atomic<bool> received{false};
    std::atomic<uint64_t> hook_sequence{0};
    std::atomic<uint64_t> observed_sequence{0};

    ASSERT_TRUE(bus.subscribe_topic("rt://gateway/sequence-hook",
        [&](const LocalMessageView& message) {
            observed_sequence.store(message.sequence, std::memory_order_release);
            observer_saw_hook.store(hook_called.load(std::memory_order_acquire),
                                    std::memory_order_release);
            received.store(true, std::memory_order_release);
        }, 0));

    const char payload[] = "hooked";
    const uint64_t sequence = bus.publish_topic(
        "rt://gateway/sequence-hook", payload, sizeof(payload),
        [&](uint64_t assigned_sequence) {
            hook_sequence.store(assigned_sequence, std::memory_order_release);
            hook_called.store(true, std::memory_order_release);
        });
    ASSERT_GT(sequence, 0u);

    for (int i = 0; i < 50 && !received.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(received.load(std::memory_order_acquire));
    EXPECT_TRUE(observer_saw_hook.load(std::memory_order_acquire));
    EXPECT_EQ(hook_sequence.load(std::memory_order_acquire), sequence);
    EXPECT_EQ(observed_sequence.load(std::memory_order_acquire), sequence);
}
