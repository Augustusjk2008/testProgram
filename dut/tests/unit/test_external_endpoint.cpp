#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/ExternalEndpoint.h"
#include "MB_DDF/DDS/ExternalPort.h"
#include "MB_DDF/DDS/Publisher.h"
#include "MB_DDF/DDS/Subscriber.h"

using namespace MB_DDF::DDS;

namespace {

class FakeExternalEndpoint : public ExternalEndpoint {
public:
    bool send(const uint8_t* data, size_t size) override {
        if (data == nullptr && size > 0) {
            return false;
        }
        last_tx.clear();
        if (size > 0) {
            last_tx.assign(data, data + size);
        }
        return send_ok;
    }

    int32_t receive(uint8_t* data, size_t capacity) override {
        ++receive_calls;
        if (data == nullptr && capacity > 0) {
            return -1;
        }

        const size_t copy_size = std::min(capacity, next_rx.size());
        if (copy_size > 0) {
            std::memcpy(data, next_rx.data(), copy_size);
            next_rx.erase(next_rx.begin(), next_rx.begin() + static_cast<std::ptrdiff_t>(copy_size));
        }
        return static_cast<int32_t>(copy_size);
    }

    int32_t receive(uint8_t* data, size_t capacity, uint32_t timeout_us) override {
        last_timeout_us = timeout_us;
        return receive(data, capacity);
    }

    size_t mtu() const override {
        return mtu_value;
    }

    bool send_ok{true};
    size_t mtu_value{256};
    uint32_t last_timeout_us{0};
    std::atomic<uint32_t> receive_calls{0};
    std::vector<uint8_t> last_tx{};
    std::vector<uint8_t> next_rx{};
};

} // namespace

TEST(ExternalEndpointTest, PublisherWriteDelegatesToExternalSend) {
    auto external = std::make_shared<FakeExternalEndpoint>();
    Publisher publisher(nullptr, nullptr, "external_pub", external);

    const char payload[] = "external-payload";
    ASSERT_TRUE(publisher.write(payload, sizeof(payload)));

    ASSERT_EQ(external->last_tx.size(), sizeof(payload));
    EXPECT_EQ(std::memcmp(external->last_tx.data(), payload, sizeof(payload)), 0);
}

TEST(ExternalEndpointTest, PublisherExternalModeDoesNotExposeRingBufferWriteSlot) {
    auto external = std::make_shared<FakeExternalEndpoint>();
    Publisher publisher(nullptr, nullptr, "external_pub", external);

    auto message = publisher.begin_message(128);
    EXPECT_FALSE(message.valid());

    const bool filled = publisher.publish_fill(128, [](void*, size_t) -> size_t {
        return 1;
    });
    EXPECT_FALSE(filled);
}

TEST(ExternalEndpointTest, SubscriberReadDelegatesToExternalReceive) {
    auto external = std::make_shared<FakeExternalEndpoint>();
    external->next_rx = {'A', 'B', 'C'};

    Subscriber subscriber(nullptr, nullptr, "external_sub", external);
    ASSERT_TRUE(subscriber.subscribe(nullptr));

    std::array<char, 8> buffer{};
    const size_t n = subscriber.read(buffer.data(), buffer.size());

    EXPECT_EQ(n, 3u);
    EXPECT_EQ(std::string(buffer.data(), 3), "ABC");
    EXPECT_EQ(external->receive_calls.load(), 1u);

    subscriber.unsubscribe();
}

TEST(ExternalEndpointTest, SubscriberTimedReadDelegatesTimeout) {
    auto external = std::make_shared<FakeExternalEndpoint>();
    external->next_rx = {'D', 'E'};

    Subscriber subscriber(nullptr, nullptr, "external_sub", external);
    ASSERT_TRUE(subscriber.subscribe(nullptr));

    std::array<char, 8> buffer{};
    const int32_t n = subscriber.read(buffer.data(), buffer.size(), static_cast<uint32_t>(250));

    EXPECT_EQ(n, 2);
    EXPECT_EQ(external->last_timeout_us, 250u);
    EXPECT_EQ(std::string(buffer.data(), 2), "DE");

    subscriber.unsubscribe();
}

TEST(ExternalEndpointTest, DDSCoreCreatesExternalPublisherAndSubscriber) {
    auto external = std::make_shared<FakeExternalEndpoint>();

    auto& dds = DDSCore::instance();
    auto publisher = dds.create_publisher("external://tx", external);
    auto subscriber = dds.create_subscriber("external://rx", external);

    ASSERT_NE(publisher, nullptr);
    ASSERT_NE(subscriber, nullptr);

    const char payload[] = "core-external";
    EXPECT_TRUE(publisher->write(payload, sizeof(payload)));
    EXPECT_EQ(external->last_tx.size(), sizeof(payload));

    external->next_rx = {'O', 'K'};
    std::array<char, 8> buffer{};
    EXPECT_EQ(subscriber->read(buffer.data(), buffer.size()), 2u);
    EXPECT_EQ(std::string(buffer.data(), 2), "OK");
}

TEST(ExternalPortTest, WriteAndReadUseExternalPublisherAndSubscriber) {
    auto external = std::make_shared<FakeExternalEndpoint>();
    ExternalPort port(external);

    const char payload[] = "port-write";
    ASSERT_TRUE(port.write(payload, sizeof(payload)));
    ASSERT_EQ(external->last_tx.size(), sizeof(payload));

    external->next_rx = {'R', 'X'};
    std::array<char, 8> buffer{};
    EXPECT_EQ(port.read(buffer.data(), buffer.size()), 2u);
    EXPECT_EQ(std::string(buffer.data(), 2), "RX");
}

TEST(ExternalEndpointTest, SubscriberCallbackReceivesExternalData) {
    auto external = std::make_shared<FakeExternalEndpoint>();
    external->next_rx = {'C', 'B'};

    Subscriber subscriber(nullptr, nullptr, "external_callback_sub", external);

    std::atomic<int> callback_count{0};
    ASSERT_TRUE(subscriber.subscribe([&](const void* data, size_t size, uint64_t timestamp) {
        EXPECT_EQ(timestamp, 0u);
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(size, 2u);
        callback_count.fetch_add(1, std::memory_order_relaxed);
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    subscriber.unsubscribe();

    EXPECT_GE(callback_count.load(), 1);
}
