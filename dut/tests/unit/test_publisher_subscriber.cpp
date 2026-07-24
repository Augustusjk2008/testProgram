/**
 * @file test_publisher_subscriber.cpp
 * @brief Publisher 和 Subscriber 单元测试
 *
 * 测试发布者和订阅者的核心功能，包括同步和异步模式。
 */

#include <gtest/gtest.h>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

#include "MB_DDF/DDS/Publisher.h"
#include "MB_DDF/DDS/Subscriber.h"
#include "MB_DDF/DDS/RingBuffer.h"
#include "MB_DDF/DDS/TopicRegistry.h"
#include "MB_DDF/DDS/SharedMemory.h"

using namespace MB_DDF::DDS;

// ==============================
// 测试固件
// ==============================
class PubSubTest : public ::testing::Test {
protected:
    void SetUp() override {
        shm_unlink("/test_pubsub_shm");
        sem_unlink("/test_pubsub_shm_sem");

        shm_manager_ = std::make_unique<SharedMemoryManager>("/test_pubsub_shm", 4 * 1024 * 1024);
        ASSERT_NE(shm_manager_->get_address(), nullptr);

        registry_ = std::make_unique<TopicRegistry>(
            shm_manager_->get_address(), shm_manager_->get_size(), shm_manager_.get());

        metadata_ = registry_->register_topic("rt://test/topic", 256 * 1024);
        ASSERT_NE(metadata_, nullptr);

        // 在 RingBuffer 管理的独立缓冲区上创建
        buffer_ = static_cast<uint8_t*>(aligned_alloc(64, 256 * 1024));
        ASSERT_NE(buffer_, nullptr);
        memset(buffer_, 0, 256 * 1024);

        sem_init(&sem_, 0, 1);
        ring_buffer_ = std::make_unique<RingBuffer>(buffer_, 256 * 1024, &sem_, true);
    }

    void TearDown() override {
        ring_buffer_.reset();
        free(buffer_);
        sem_destroy(&sem_);
        registry_.reset();
        shm_manager_.reset();
        shm_unlink("/test_pubsub_shm");
        sem_unlink("/test_pubsub_shm_sem");
    }

    std::unique_ptr<SharedMemoryManager> shm_manager_;
    std::unique_ptr<TopicRegistry> registry_;
    TopicMetadata* metadata_;
    uint8_t* buffer_;
    sem_t sem_;
    std::unique_ptr<RingBuffer> ring_buffer_;
};

// ==============================
// Publisher 基础测试
// ==============================
TEST_F(PubSubTest, PublisherBasicPublish) {
    Publisher pub(metadata_, ring_buffer_.get(), "test_pub");

    const char* data = "Hello, Publisher!";
    EXPECT_TRUE(pub.publish(data, strlen(data) + 1));

    // 验证 Publisher 信息
    EXPECT_EQ(pub.get_topic_id(), metadata_->topic_id);
    EXPECT_EQ(pub.get_topic_name(), "rt://test/topic");
    EXPECT_FALSE(pub.get_name().empty());
    EXPECT_NE(pub.get_id(), 0);
}

TEST_F(PubSubTest, PublisherWriteAlias) {
    Publisher pub(metadata_, ring_buffer_.get());

    int value = 42;
    EXPECT_TRUE(pub.write(&value, sizeof(value)));
}

TEST_F(PubSubTest, PublisherBeginMessageCommit) {
    Publisher pub(metadata_, ring_buffer_.get());

    auto msg = pub.begin_message(256);
    ASSERT_TRUE(msg.valid());

    void* data = msg.data();
    ASSERT_NE(data, nullptr);

    strcpy(static_cast<char*>(data), "Zero-copy message");
    EXPECT_TRUE(msg.commit(strlen("Zero-copy message") + 1));
}

TEST_F(PubSubTest, PublisherBeginMessageCancel) {
    Publisher pub(metadata_, ring_buffer_.get());

    {
        auto msg = pub.begin_message(256);
        ASSERT_TRUE(msg.valid());
        // 不 commit，析构时自动 cancel
    }

    // 可以重新发布
    EXPECT_TRUE(pub.publish("after cancel", 13));
}

TEST_F(PubSubTest, PublisherPublishFill) {
    Publisher pub(metadata_, ring_buffer_.get());

    bool called = false;
    auto result = pub.publish_fill(256, [&called](void* buf, size_t cap) -> size_t {
        called = true;
        EXPECT_NE(buf, nullptr);
        EXPECT_GE(cap, 256);
        strcpy(static_cast<char*>(buf), "Filled message");
        return strlen("Filled message") + 1;
    });

    EXPECT_TRUE(result);
    EXPECT_TRUE(called);
}

TEST_F(PubSubTest, PublisherPublishFillZeroReturns) {
    Publisher pub(metadata_, ring_buffer_.get());

    // fill 返回 0 表示取消
    auto result = pub.publish_fill(256, [](void*, size_t) -> size_t {
        return 0; // 取消
    });

    EXPECT_FALSE(result);
}

// ==============================
// Subscriber 基础测试
// ==============================
TEST_F(PubSubTest, SubscriberSyncRead) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "test_sub");

    // 先订阅（不设置回调，使用同步模式）
    EXPECT_TRUE(sub.subscribe(nullptr));
    EXPECT_TRUE(sub.is_subscribed());

    // 发布消息
    const char* data = "Sync read test";
    pub.publish(data, strlen(data) + 1);

    // 同步读取
    char buffer[256] = {0};
    size_t received = sub.read(buffer, sizeof(buffer), true); // latest
    EXPECT_GT(received, 0);
    EXPECT_STREQ(buffer, data);

    sub.unsubscribe();
    EXPECT_FALSE(sub.is_subscribed());
}

TEST_F(PubSubTest, SubscriberSyncReadNext) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get());

    sub.subscribe(nullptr);

    pub.publish("msg1", 5);
    pub.publish("msg2", 5);

    char buffer[256] = {0};

    // read_next 读取第一条
    size_t n1 = sub.read(buffer, sizeof(buffer), false);
    EXPECT_GT(n1, 0);

    // 第二次读取第二条
    size_t n2 = sub.read(buffer, sizeof(buffer), false);
    EXPECT_GT(n2, 0);

    sub.unsubscribe();
}

TEST_F(PubSubTest, SubscriberAsyncCallback) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "async_sub");

    std::atomic<int> received_count{0};
    std::atomic<uint64_t> last_timestamp{0};

    auto callback = [&received_count, &last_timestamp](const void* data, size_t size, uint64_t ts) {
        received_count++;
        last_timestamp.store(ts);
        EXPECT_NE(data, nullptr);
        EXPECT_GT(size, 0);
    };

    EXPECT_TRUE(sub.subscribe(callback));

    // 发布多条消息
    for (int i = 0; i < 5; ++i) {
        std::string msg = "message " + std::to_string(i);
        pub.publish(msg.c_str(), msg.size() + 1);
    }

    // 等待回调处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_GE(received_count.load(), 1); // 至少收到一条

    sub.unsubscribe();
}

TEST_F(PubSubTest, SubscriberDoubleSubscribeFails) {
    Subscriber sub(metadata_, ring_buffer_.get());

    EXPECT_TRUE(sub.subscribe(nullptr));
    EXPECT_FALSE(sub.subscribe(nullptr)); // 重复订阅应该失败
}

TEST_F(PubSubTest, SubscriberReadWithoutSubscribe) {
    Subscriber sub(metadata_, ring_buffer_.get());

    char buffer[256] = {0};
    size_t n = sub.read(buffer, sizeof(buffer), true);
    EXPECT_EQ(n, 0); // 未订阅时读取返回0
}

// ==============================
// Pub/Sub 集成测试
// ==============================
TEST_F(PubSubTest, PubSubMultipleSubscribers) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub1(metadata_, ring_buffer_.get(), "sub1");
    Subscriber sub2(metadata_, ring_buffer_.get(), "sub2");

    sub1.subscribe(nullptr);
    sub2.subscribe(nullptr);

    const char* data = "Broadcast message";
    pub.publish(data, strlen(data) + 1);

    char buf1[256] = {0};
    char buf2[256] = {0};

    size_t n1 = sub1.read(buf1, sizeof(buf1), true);
    size_t n2 = sub2.read(buf2, sizeof(buf2), true);

    EXPECT_GT(n1, 0);
    EXPECT_GT(n2, 0);
    EXPECT_STREQ(buf1, data);
    EXPECT_STREQ(buf2, data);

    sub1.unsubscribe();
    sub2.unsubscribe();
}

TEST_F(PubSubTest, PubSubLargeMessage) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get());

    sub.subscribe(nullptr);

    // 8KB 消息
    std::vector<char> large_msg(8192, 'X');
    large_msg.back() = '\0';

    EXPECT_TRUE(pub.publish(large_msg.data(), large_msg.size()));

    std::vector<char> buffer(8192, 0);
    size_t received = sub.read(buffer.data(), buffer.size(), true);

    EXPECT_EQ(received, large_msg.size());
    EXPECT_EQ(buffer[0], 'X');
    EXPECT_EQ(buffer[8190], 'X');

    sub.unsubscribe();
}

TEST_F(PubSubTest, PubSubStressTest) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get());

    std::atomic<int> received{0};
    sub.subscribe([&received](const void*, size_t, uint64_t) {
        received++;
    });

    const int num_messages = 100;
    for (int i = 0; i < num_messages; ++i) {
        std::string msg = "msg_" + std::to_string(i);
        pub.publish(msg.c_str(), msg.size() + 1);
    }

    // 等待处理
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_GE(received.load(), num_messages / 2); // 至少收到一半（环形缓冲区可能覆盖）

    sub.unsubscribe();
}

// ==============================
// 边界条件测试
// ==============================
TEST_F(PubSubTest, PublisherNullBuffer) {
    // Publisher 构造时传入空的 RingBuffer（实际上构造函数需要非空）
    // 这个测试验证当 RingBuffer 为 nullptr 时的行为
    Publisher pub(metadata_, nullptr);
    EXPECT_FALSE(pub.publish("test", 5));
}

TEST_F(PubSubTest, SubscriberUnsubscribeWithoutSubscribe) {
    Subscriber sub(metadata_, ring_buffer_.get());
    // 取消未订阅的订阅者应该安全
    sub.unsubscribe();
    EXPECT_FALSE(sub.is_subscribed());
}

TEST_F(PubSubTest, PublisherPublishEmptyData) {
    Publisher pub(metadata_, ring_buffer_.get());

    // 发布空数据应该成功（大小为0的消息）
    EXPECT_TRUE(pub.publish("", 1)); // 至少包含终止符
}

// ==============================
// Subscriber 超时读取测试
// ==============================
TEST_F(PubSubTest, SubscriberDdsReadTimeout) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "timeout_sub");

    sub.subscribe(nullptr);

    char buffer[256] = {0};
    auto start = std::chrono::steady_clock::now();
    int32_t timeout_result = sub.read(buffer, sizeof(buffer), static_cast<uint32_t>(100000));
    auto end = std::chrono::steady_clock::now();
    EXPECT_EQ(timeout_result, 0);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), 50);

    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pub.publish("timeout-ok", 11);
    });
    int32_t read_result = sub.read(buffer, sizeof(buffer), static_cast<uint32_t>(500000));
    producer.join();
    EXPECT_EQ(read_result, 11);
    EXPECT_STREQ(buffer, "timeout-ok");

    sub.unsubscribe();
}

TEST_F(PubSubTest, SubscriberPollReturnsImmediatelyWhenEmpty) {
    Subscriber sub(metadata_, ring_buffer_.get(), "poll_empty_sub");
    ASSERT_TRUE(sub.subscribe(nullptr));

    char buffer[256] = {0};
    auto start = std::chrono::steady_clock::now();
    const size_t n = sub.poll(buffer, sizeof(buffer));
    auto end = std::chrono::steady_clock::now();

    EXPECT_EQ(n, 0u);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(),
              20000);
    EXPECT_EQ(ring_buffer_->get_statistics().waiting_subscribers, 0u);

    sub.unsubscribe();
}

TEST_F(PubSubTest, SubscriberPollReadsPublishedMessage) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "poll_read_sub");
    ASSERT_TRUE(sub.subscribe(nullptr));

    const char payload[] = "poll-ok";
    ASSERT_TRUE(pub.publish(payload, sizeof(payload)));

    char buffer[256] = {0};
    const size_t n = sub.poll(buffer, sizeof(buffer));

    EXPECT_EQ(n, sizeof(payload));
    EXPECT_STREQ(buffer, payload);

    sub.unsubscribe();
}

TEST_F(PubSubTest, SubscriberReadBlockingStillWaits) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "blocking_wait_sub");
    ASSERT_TRUE(sub.subscribe(nullptr));

    const char payload[] = "blocking-ok";
    char buffer[256] = {0};
    std::atomic<bool> publish_ok{false};
    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        publish_ok.store(pub.publish(payload, sizeof(payload)), std::memory_order_release);
    });

    auto start = std::chrono::steady_clock::now();
    const int32_t n = sub.read_blocking(buffer, sizeof(buffer), 500000, true);
    auto end = std::chrono::steady_clock::now();
    producer.join();

    EXPECT_TRUE(publish_ok.load(std::memory_order_acquire));
    EXPECT_EQ(n, static_cast<int32_t>(sizeof(payload)));
    EXPECT_STREQ(buffer, payload);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(),
              25);

    sub.unsubscribe();
}

TEST_F(PubSubTest, SubscriberStrategyPollingDoesNotIncreaseWaiterCount) {
    Subscriber sub(metadata_, ring_buffer_.get(), "strategy_poll_sub");
    ASSERT_TRUE(sub.subscribe(nullptr));

    EXPECT_EQ(ring_buffer_->get_statistics().waiting_subscribers, 0u);
    char buffer[256] = {0};
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(sub.read(buffer, sizeof(buffer), ReadStrategy::Polling, 0, true), 0);
    }
    EXPECT_EQ(ring_buffer_->get_statistics().waiting_subscribers, 0u);

    sub.unsubscribe();
}

TEST_F(PubSubTest, SubscriberStrategyBlockingUsesWaitPath) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "strategy_blocking_sub");
    ASSERT_TRUE(sub.subscribe(nullptr));

    const char payload[] = "wait-path";
    char buffer[256] = {0};
    std::atomic<int32_t> read_result{0};

    std::thread consumer([&]() {
        const int32_t n = sub.read(
            buffer,
            sizeof(buffer),
            ReadStrategy::Blocking,
            1000000,
            true);
        read_result.store(n, std::memory_order_release);
    });

    bool observed_waiter = false;
    for (int i = 0; i < 100; ++i) {
        if (ring_buffer_->get_statistics().waiting_subscribers > 0) {
            observed_waiter = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(observed_waiter);
    EXPECT_TRUE(pub.publish(payload, sizeof(payload)));
    consumer.join();

    EXPECT_EQ(read_result.load(std::memory_order_acquire),
              static_cast<int32_t>(sizeof(payload)));
    EXPECT_STREQ(buffer, payload);
    EXPECT_EQ(ring_buffer_->get_statistics().waiting_subscribers, 0u);

    sub.unsubscribe();
}

// 测试使用 RingBuffer wait_for_message 实现超时等待
TEST_F(PubSubTest, SubscriberWaitForMessageTimeout) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "wait_sub");

    sub.subscribe(nullptr);

    // 获取订阅者状态
    auto* state = ring_buffer_->register_subscriber(999, "wait_test_sub");
    ASSERT_NE(state, nullptr);

    // 等待消息，应该超时
    auto start = std::chrono::steady_clock::now();
    bool has_msg = ring_buffer_->wait_for_message(state, 100); // 100ms
    auto end = std::chrono::steady_clock::now();

    EXPECT_FALSE(has_msg);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_GE(elapsed_ms, 50);  // 至少等待了50ms
}

TEST_F(PubSubTest, SubscriberWaitForMessageSuccess) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "wait_success_sub");

    sub.subscribe(nullptr);

    // 先发布一条消息
    pub.publish("preloaded", 10);

    // 获取订阅者状态
    auto* state = ring_buffer_->register_subscriber(1000, "wait_success_test");
    ASSERT_NE(state, nullptr);

    // 等待应该立即返回（有消息）
    bool has_msg = ring_buffer_->wait_for_message(state, 1000); // 1s 超时
    EXPECT_TRUE(has_msg);
}

// ==============================
// Subscriber CPU 绑定测试
// ==============================
TEST_F(PubSubTest, SubscriberBindToCpu) {
    Subscriber sub(metadata_, ring_buffer_.get(), "cpu_bind_sub");

    // 绑定到 CPU 0（假设至少有一个 CPU）
    bool result = sub.bind_to_cpu(0);

    // 注意：在单核系统上或没有权限时可能失败，不强制断言成功
    // 但方法不应该崩溃
    (void)result;

    // 订阅后仍然可以绑定（取决于实现）
    sub.subscribe(nullptr);

    // 清理
    sub.unsubscribe();
}

TEST_F(PubSubTest, SubscriberBindToInvalidCpu) {
    Subscriber sub(metadata_, ring_buffer_.get(), "invalid_cpu_sub");

    // 绑定到不存在的 CPU 应该失败
    bool result = sub.bind_to_cpu(9999);
    EXPECT_FALSE(result);

    sub.subscribe(nullptr);
    sub.unsubscribe();
}

// ==============================
// 消息丢失检测测试
// ==============================
TEST_F(PubSubTest, MessageLossDetection) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber sub(metadata_, ring_buffer_.get(), "loss_detect_sub");

    std::atomic<int> received_count{0};
    std::atomic<uint64_t> last_sequence{0};
    std::atomic<int> loss_count{0};

    // 使用回调检测消息丢失
    sub.subscribe([&](const void* data, size_t size, uint64_t timestamp) {
        // 通过序列号检测丢失
        // 注意：这个测试假设我们可以通过某种方式获取序列号
        // 实际实现可能需要访问 RingBuffer 的 SubscriberState
        received_count++;
        (void)data;
        (void)size;
        (void)timestamp;
    });

    // 发布大量消息，可能导致缓冲区覆盖（消息丢失）
    const int num_messages = 500;
    for (int i = 0; i < num_messages; ++i) {
        std::string msg = "msg_" + std::to_string(i);
        pub.publish(msg.c_str(), msg.size() + 1);
    }

    // 等待处理
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 由于缓冲区有限，收到的消息数量应该少于发布的数量
    // 这就是消息丢失
    int received = received_count.load();
    EXPECT_LE(received, num_messages);

    // 如果确实发生了丢失，记录一下（不是错误，是预期行为）
    if (received < num_messages) {
        loss_count = num_messages - received;
        std::cout << "Detected message loss: " << loss_count.load()
                  << " messages lost (" << received << "/" << num_messages << " received)" << std::endl;
    }

    sub.unsubscribe();
}

TEST_F(PubSubTest, SlowSubscriberMessageLoss) {
    Publisher pub(metadata_, ring_buffer_.get());
    Subscriber slow_sub(metadata_, ring_buffer_.get(), "slow_sub");
    Subscriber fast_sub(metadata_, ring_buffer_.get(), "fast_sub");

    std::atomic<int> slow_received{0};
    std::atomic<int> fast_received{0};

    // 慢订阅者：处理每条消息有延迟
    slow_sub.subscribe([&](const void*, size_t, uint64_t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        slow_received++;
    });

    // 快订阅者：立即处理
    fast_sub.subscribe([&](const void*, size_t, uint64_t) {
        fast_received++;
    });

    // 快速发布消息
    const int num_messages = 50;
    for (int i = 0; i < num_messages; ++i) {
        pub.publish("x", 2);
    }

    // 等待
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 快订阅者应该收到更多消息
    EXPECT_GE(fast_received.load(), slow_received.load());

    // 慢订阅者可能丢失消息（因为处理不过来）
    if (slow_received.load() < num_messages) {
        std::cout << "Slow subscriber lost messages: "
                  << slow_received.load() << "/" << num_messages << std::endl;
    }

    slow_sub.unsubscribe();
    fast_sub.unsubscribe();
}

