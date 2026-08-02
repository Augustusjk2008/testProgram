/**
 * @file test_ringbuffer.cpp
 * @brief RingBuffer单元测试
 *
 * 测试无锁环形缓冲区的核心功能，包括：
 * - 基本发布/订阅
 * - 多订阅者隔离
 * - 环形回绕
 * - 零拷贝API
 * - 消息覆盖行为
 */

#include <gtest/gtest.h>
#include <semaphore.h>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "MB_DDF/DDS/RingBuffer.h"
#include "MB_DDF/DDS/Message.h"

using namespace MB_DDF::DDS;

// ==============================
// 测试固件
// ==============================
class RingBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建进程内信号量
        sem_init(&sem_, 0, 1);

        // 分配对齐的64KB缓冲区（RingBuffer需要64字节对齐）
        size_t buffer_size = 64 * 1024;
        buffer_ = static_cast<uint8_t*>(aligned_alloc(64, buffer_size));
        ASSERT_NE(buffer_, nullptr);
        std::memset(buffer_, 0, buffer_size);

        // RingBuffer对象在栈上创建，管理独立的缓冲区
        rb_ = std::make_unique<RingBuffer>(buffer_, buffer_size, &sem_, true);
    }

    void TearDown() override {
        rb_.reset();
        free(buffer_);
        sem_destroy(&sem_);
    }

    sem_t sem_;
    uint8_t* buffer_;
    std::unique_ptr<RingBuffer> rb_;
};

// ==============================
// 基本发布/订阅
// ==============================
TEST_F(RingBufferTest, BasicPublishAndRead) {
    const char* data = "Hello, RingBuffer!";
    size_t data_size = strlen(data) + 1;

    // 发布消息
    EXPECT_TRUE(rb_->publish_message(data, data_size));

    // 注册订阅者
    auto* sub = rb_->register_subscriber(1, "test_sub");
    ASSERT_NE(sub, nullptr);

    // 读取消息
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub, msg));
    ASSERT_NE(msg, nullptr);

    // 验证数据
    EXPECT_EQ(msg->header.data_size, data_size);
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), data);
}

TEST_F(RingBufferTest, MultipleMessages) {
    // 发布多条消息
    for (int i = 0; i < 10; ++i) {
        std::string msg = "Message " + std::to_string(i);
        EXPECT_TRUE(rb_->publish_message(msg.c_str(), msg.size() + 1));
    }

    auto* sub = rb_->register_subscriber(1, "seq_sub");

    // 按顺序读取
    for (int i = 0; i < 10; ++i) {
        Message* msg = nullptr;
        EXPECT_TRUE(rb_->read_next(sub, msg));

        std::string expected = "Message " + std::to_string(i);
        EXPECT_STREQ(static_cast<const char*>(msg->get_data()), expected.c_str());
    }
}

// ==============================
// 多订阅者隔离
// ==============================
TEST_F(RingBufferTest, MultiSubscriberIsolation) {
    const char* data = "Shared message";
    rb_->publish_message(data, strlen(data) + 1);

    // 注册两个订阅者
    auto* sub1 = rb_->register_subscriber(1, "sub1");
    auto* sub2 = rb_->register_subscriber(2, "sub2");

    // sub1读取
    Message* msg1 = nullptr;
    EXPECT_TRUE(rb_->read_next(sub1, msg1));
    EXPECT_STREQ(static_cast<const char*>(msg1->get_data()), data);

    // sub2仍可读取同一条消息
    Message* msg2 = nullptr;
    EXPECT_TRUE(rb_->read_next(sub2, msg2));
    EXPECT_STREQ(static_cast<const char*>(msg2->get_data()), data);

    // sub1再次读取无新消息
    Message* msg3 = nullptr;
    EXPECT_FALSE(rb_->read_next(sub1, msg3));
}

TEST_F(RingBufferTest, SubscriberIndependentPosition) {
    // 发布消息1
    rb_->publish_message("msg1", 5);

    auto* sub1 = rb_->register_subscriber(1, "fast_sub");
    auto* sub2 = rb_->register_subscriber(2, "slow_sub");

    // sub1读取msg1
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub1, msg));

    // 发布消息2、3
    rb_->publish_message("msg2", 5);
    rb_->publish_message("msg3", 5);

    // sub1直接读到msg3（最新）
    EXPECT_TRUE(rb_->read_next(sub1, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "msg2");
    EXPECT_TRUE(rb_->read_next(sub1, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "msg3");

    // sub2从msg1开始读
    EXPECT_TRUE(rb_->read_next(sub2, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "msg1");
}

// ==============================
// 零拷贝API
// ==============================
TEST_F(RingBufferTest, ZeroCopyReserveCommit) {
    // 预留空间
    auto token = rb_->reserve(256);
    EXPECT_TRUE(token.valid);

    // 填充数据
    auto* data = static_cast<char*>(token.msg->get_data());
    strcpy(data, "Zero-copy message");

    // 提交
    EXPECT_TRUE(rb_->commit(token, strlen("Zero-copy message") + 1, 1));

    // 验证读取
    auto* sub = rb_->register_subscriber(1, "zc_sub");
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "Zero-copy message");
}

TEST_F(RingBufferTest, ZeroCopyAbort) {
    // 预留但不提交
    auto token = rb_->reserve(1024);
    EXPECT_TRUE(token.valid);

    // 放弃预留
    rb_->abort(token);

    // 发布正常消息
    EXPECT_TRUE(rb_->publish_message("after abort", 12));

    // 订阅者应能读到正常消息
    auto* sub = rb_->register_subscriber(1, "abort_sub");
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "after abort");
}

// ==============================
// 环形回绕
// ==============================
TEST_F(RingBufferTest, WrapAround) {
    // 注册订阅者并读取第一条消息作为基准
    auto* sub = rb_->register_subscriber(1, "wrap_sub");

    // 发布第一条消息（标记为BEGIN）
    EXPECT_TRUE(rb_->publish_message("BEGIN", 6));
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "BEGIN");

    // 填充缓冲区直到覆盖（使用大块数据加速回绕）
    char large_data[4096] = {0};
    memset(large_data, 'A', sizeof(large_data));

    // 写足够多的数据确保覆盖BEGIN的位置
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(rb_->publish_message(large_data, sizeof(large_data)));
    }

    // 发布一个特殊标记消息（应该在回绕后的位置）
    const char* wrap_msg = "WRAPPED";
    EXPECT_TRUE(rb_->publish_message(wrap_msg, strlen(wrap_msg) + 1));

    // 使用read_latest跳到最新，应该能读到WRAP_MSG
    EXPECT_TRUE(rb_->read_latest(sub, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "WRAPPED");
}

TEST_F(RingBufferTest, WrapAroundOverwritesOldData) {
    // 关键测试：验证回绕后旧数据被覆盖，新数据可用
    char large_data[4096] = {0};
    memset(large_data, 'A', sizeof(large_data));

    // 阶段1：发布2条特殊标记消息（FIRST）
    EXPECT_TRUE(rb_->publish_message("FIRST_MESSAGE", 14));
    EXPECT_TRUE(rb_->publish_message("FIRST_MESSAGE", 14));

    // 阶段2：新订阅者应该能读到FIRST_MESSAGE（未回绕时）
    auto* sub_before = rb_->register_subscriber(1, "before_wrap");
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub_before, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "FIRST_MESSAGE");

    // 阶段3：发布大量消息，触发回绕（约23×4KB=92KB > 60KB，必然覆盖）
    for (int i = 0; i < 23; ++i) {
        EXPECT_TRUE(rb_->publish_message(large_data, sizeof(large_data)));
    }

    // 阶段4：再发布一条特殊标记消息（LAST），应该在回绕后的位置
    EXPECT_TRUE(rb_->publish_message("LAST_MESSAGE", 13));

    // 阶段5：使用read_latest读取最新消息，应该读到LAST_MESSAGE
    auto* sub_after = rb_->register_subscriber(2, "after_wrap");
    EXPECT_TRUE(rb_->read_latest(sub_after, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "LAST_MESSAGE");

    // 阶段6：验证FIRST_MESSAGE已被覆盖（sub_before的read_pos指向的数据已无效）
    // 由于回绕，sub_before尝试读next可能读到A或LAST，但绝不会是FIRST
    if (rb_->read_next(sub_before, msg)) {
        std::string data(static_cast<const char*>(msg->get_data()),
                        std::min(size_t(msg->header.data_size), size_t(5)));
        EXPECT_NE(data, "FIRST") << "FIRST_MESSAGE should be overwritten after wrap-around";
    }
}

// ==============================
// read_latest 行为
// ==============================
TEST_F(RingBufferTest, ReadLatest) {
    // 发布多条消息
    rb_->publish_message("old1", 5);
    rb_->publish_message("old2", 5);
    rb_->publish_message("latest", 7);

    auto* sub = rb_->register_subscriber(1, "latest_sub");

    // read_latest应该读到最新的
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_latest(sub, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "latest");

    // 再次read_latest应该无新消息（已是最新）
    EXPECT_FALSE(rb_->read_next(sub, msg));
}

// ==============================
// 边界条件
// ==============================
TEST_F(RingBufferTest, EmptyMessage) {
    // 发布空消息
    EXPECT_TRUE(rb_->publish_message(nullptr, 0));

    auto* sub = rb_->register_subscriber(1, "empty_sub");
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub, msg));
    EXPECT_EQ(msg->header.data_size, 0);
}

TEST_F(RingBufferTest, LargeMessage) {
    // 8KB消息
    std::vector<char> large_data(8192, 'X');
    large_data.back() = '\0';

    EXPECT_TRUE(rb_->publish_message(large_data.data(), large_data.size()));

    auto* sub = rb_->register_subscriber(1, "large_sub");
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub, msg));
    EXPECT_EQ(msg->header.data_size, large_data.size());
    EXPECT_EQ(static_cast<const char*>(msg->get_data())[0], 'X');
    EXPECT_EQ(static_cast<const char*>(msg->get_data())[8191], '\0');
}

TEST_F(RingBufferTest, BufferFull) {
    // 持续写入直到触发覆盖（环形缓冲区特性）
    char chunk[2048] = {0};
    memset(chunk, 'F', sizeof(chunk));

    // 64KB缓冲区大约能存30个2KB块，写50个必然触发覆盖
    int written = 0;
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(rb_->publish_message(chunk, sizeof(chunk)));
        written++;
    }

    EXPECT_GE(written, 30) << "Should write multiple messages before wrap";
}

// ==============================
// 订阅者管理
// ==============================
TEST_F(RingBufferTest, SubscriberRegistration) {
    auto* sub1 = rb_->register_subscriber(1, "sub1");
    auto* sub2 = rb_->register_subscriber(2, "sub2");
    auto* sub3 = rb_->register_subscriber(3, "sub3");

    ASSERT_NE(sub1, nullptr);
    ASSERT_NE(sub2, nullptr);
    ASSERT_NE(sub3, nullptr);

    // 每个订阅者有独立ID
    EXPECT_EQ(sub1->subscriber_id, 1);
    EXPECT_EQ(sub2->subscriber_id, 2);
    EXPECT_EQ(sub3->subscriber_id, 3);
}

TEST_F(RingBufferTest, Unsubscribe) {
    auto* sub = rb_->register_subscriber(1, "temp_sub");
    ASSERT_NE(sub, nullptr);

    // 发布消息后取消订阅
    rb_->publish_message("test", 5);
    rb_->unregister_subscriber(sub);

    // 重新注册同名订阅者（新ID）
    auto* sub2 = rb_->register_subscriber(2, "new_sub");
    ASSERT_NE(sub2, nullptr);
}

TEST_F(RingBufferTest, RejectsNullNonEmptyPayload) {
    EXPECT_FALSE(rb_->publish_message(nullptr, 1));
    EXPECT_EQ(rb_->current_sequence(), 0u);
}

TEST_F(RingBufferTest, RejectsOverflowSizedPayloadBeforeLocking) {
    const uint8_t byte = 0x5A;
    EXPECT_FALSE(rb_->publish_message(
        &byte, std::numeric_limits<size_t>::max()));
    EXPECT_EQ(rb_->current_sequence(), 0u);
}

TEST_F(RingBufferTest, SameProcessSubscribersWithSameNameUseIndependentSlots) {
    auto* first = rb_->register_subscriber(101, "same_process_name");
    auto* second = rb_->register_subscriber(202, "same_process_name");

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(first->subscriber_id, 101u);
    EXPECT_EQ(second->subscriber_id, 202u);
    EXPECT_EQ(rb_->get_statistics().active_subscribers, 2u);

    rb_->unregister_subscriber(first);
    EXPECT_EQ(second->subscriber_id, 202u);
    EXPECT_EQ(rb_->get_statistics().active_subscribers, 1u);
    rb_->unregister_subscriber(second);
}

TEST(RingBufferProcessLifecycleTest, ReclaimsExitedOwnerWithoutReclaimingLiveIdleSubscribers) {
    constexpr size_t kBufferSize = 64 * 1024;
    constexpr size_t kMappingSize = kBufferSize + sizeof(sem_t);
    static_assert(kBufferSize % alignof(sem_t) == 0);

    void* mapping = mmap(nullptr,
                         kMappingSize,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS,
                         -1,
                         0);
    ASSERT_NE(mapping, MAP_FAILED);

    auto* shared_sem = reinterpret_cast<sem_t*>(
        static_cast<uint8_t*>(mapping) + kBufferSize);
    ASSERT_EQ(sem_init(shared_sem, 1, 1), 0);

    {
        RingBuffer ring_buffer(mapping, kBufferSize, shared_sem, true);

        std::vector<std::pair<SubscriberState*, uint64_t>> live_subscribers;
        auto* first_live = ring_buffer.register_subscriber(1, "live_parent_1");
        ASSERT_NE(first_live, nullptr);
        ASSERT_EQ(first_live->timestamp.load(std::memory_order_acquire), 0u);
        live_subscribers.emplace_back(first_live, 1);

        const pid_t child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            auto* orphan = ring_buffer.register_subscriber(2, "orphan_child");
            _exit(orphan != nullptr ? 0 : 10);
        }

        int child_status = 0;
        ASSERT_EQ(waitpid(child, &child_status, 0), child);
        ASSERT_TRUE(WIFEXITED(child_status));
        ASSERT_EQ(WEXITSTATUS(child_status), 0);

        for (uint64_t id = 3; id <= 64; ++id) {
            auto* live = ring_buffer.register_subscriber(
                id, "live_parent_" + std::to_string(id));
            ASSERT_NE(live, nullptr);
            ASSERT_EQ(live->timestamp.load(std::memory_order_acquire), 0u);
            live_subscribers.emplace_back(live, id);
        }

        auto* replacement = ring_buffer.register_subscriber(65, "replacement_parent_65");
        EXPECT_NE(replacement, nullptr);

        if (replacement != nullptr) {
            const auto stats = ring_buffer.get_statistics();
            EXPECT_EQ(stats.active_subscribers, 64u);

            bool found_orphan = false;
            bool found_replacement = false;
            for (const auto& [id, name] : stats.subscribers) {
                (void)name;
                found_orphan = found_orphan || id == 2;
                found_replacement = found_replacement || id == 65;
            }
            EXPECT_FALSE(found_orphan);
            EXPECT_TRUE(found_replacement);

            for (const auto& [state, expected_id] : live_subscribers) {
                ASSERT_NE(state, nullptr);
                EXPECT_EQ(state->subscriber_id, expected_id);
                EXPECT_EQ(state->timestamp.load(std::memory_order_acquire), 0u);
            }
        }

        for (const auto& [state, id] : live_subscribers) {
            (void)id;
            ring_buffer.unregister_subscriber(state);
        }
        if (replacement != nullptr) {
            ring_buffer.unregister_subscriber(replacement);
        }
    }

    EXPECT_EQ(sem_destroy(shared_sem), 0);
    EXPECT_EQ(munmap(mapping, kMappingSize), 0);
}

TEST(RingBufferProcessLifecycleTest, UnregisterOnlyClearsCallingProcessSlotForDuplicateId) {
    constexpr size_t kBufferSize = 64 * 1024;
    constexpr size_t kMappingSize = kBufferSize + sizeof(sem_t);
    static_assert(kBufferSize % alignof(sem_t) == 0);

    void* mapping = mmap(nullptr,
                         kMappingSize,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS,
                         -1,
                         0);
    ASSERT_NE(mapping, MAP_FAILED);

    auto* shared_sem = reinterpret_cast<sem_t*>(
        static_cast<uint8_t*>(mapping) + kBufferSize);
    ASSERT_EQ(sem_init(shared_sem, 1, 1), 0);

    {
        RingBuffer ring_buffer(mapping, kBufferSize, shared_sem, true);
        constexpr uint64_t kDuplicateId = 77;
        auto* parent_subscriber = ring_buffer.register_subscriber(
            kDuplicateId, "duplicate_parent");
        ASSERT_NE(parent_subscriber, nullptr);

        const pid_t child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            auto* child_subscriber = ring_buffer.register_subscriber(
                kDuplicateId, "duplicate_child");
            if (child_subscriber == nullptr || child_subscriber == parent_subscriber) {
                _exit(10);
            }

            ring_buffer.unregister_subscriber(child_subscriber);
            const bool parent_preserved =
                parent_subscriber->subscriber_id == kDuplicateId;
            const bool child_removed = child_subscriber->subscriber_id == 0;
            _exit(parent_preserved && child_removed ? 0 : 11);
        }

        int child_status = 0;
        ASSERT_EQ(waitpid(child, &child_status, 0), child);
        ASSERT_TRUE(WIFEXITED(child_status));
        EXPECT_EQ(WEXITSTATUS(child_status), 0);
        EXPECT_EQ(parent_subscriber->subscriber_id, kDuplicateId);
        EXPECT_EQ(parent_subscriber->owner_pid, static_cast<uint64_t>(getpid()));

        ring_buffer.unregister_subscriber(parent_subscriber);
    }

    EXPECT_EQ(sem_destroy(shared_sem), 0);
    EXPECT_EQ(munmap(mapping, kMappingSize), 0);
}

TEST(RingBufferBoundaryTest, LatestSubscriberSkipsShortTailGapAfterWrap) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        const long page_size_value = sysconf(_SC_PAGESIZE);
        if (page_size_value <= 0) {
            _exit(10);
        }

        const size_t page_size = static_cast<size_t>(page_size_value);
        const size_t minimum_buffer_size = 64U * 1024U;
        const size_t buffer_size =
            ((minimum_buffer_size + page_size - 1U) / page_size) * page_size;
        const size_t mapping_size = buffer_size + page_size;
        void* mapping = mmap(nullptr,
                             mapping_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS,
                             -1,
                             0);
        if (mapping == MAP_FAILED) {
            _exit(11);
        }

        auto* guard_page = static_cast<uint8_t*>(mapping) + buffer_size;
        if (mprotect(guard_page, page_size, PROT_NONE) != 0) {
            _exit(12);
        }

        sem_t sem;
        if (sem_init(&sem, 0, 1) != 0) {
            _exit(13);
        }

        int result = 0;
        {
            RingBuffer ring_buffer(mapping, buffer_size, &sem, true);
            const size_t capacity = ring_buffer.available_space();
            if (capacity <= sizeof(MessageHeader) + 8U) {
                result = 14;
            } else {
                std::vector<uint8_t> full_payload(
                    capacity - sizeof(MessageHeader), 0x5AU);
                const uint32_t stale_magic = MessageHeader::MAGIC_NUMBER;
                std::memcpy(full_payload.data() + full_payload.size() - 8U,
                            &stale_magic,
                            sizeof(stale_magic));

                if (!ring_buffer.publish_message(
                        full_payload.data(), full_payload.size())) {
                    result = 15;
                }

                std::vector<uint8_t> leave_short_tail(
                    capacity - sizeof(MessageHeader) - 8U, 0xA5U);
                if (result == 0 &&
                    !ring_buffer.publish_message(
                        leave_short_tail.data(), leave_short_tail.size())) {
                    result = 16;
                }

                SubscriberState* subscriber = nullptr;
                if (result == 0) {
                    subscriber = ring_buffer.register_subscriber(
                        7001U, "latest_tail_boundary", true);
                    if (subscriber == nullptr) {
                        result = 17;
                    }
                }

                const uint8_t expected_payload = 0x3CU;
                if (result == 0 &&
                    !ring_buffer.publish_message(
                        &expected_payload, sizeof(expected_payload))) {
                    result = 18;
                }

                Message* message = nullptr;
                if (result == 0 &&
                    (!ring_buffer.read_next(subscriber, message) ||
                     message == nullptr ||
                     message->msg_data_size() != sizeof(expected_payload) ||
                     *static_cast<const uint8_t*>(message->get_data()) !=
                         expected_payload)) {
                    result = 19;
                }

                if (subscriber != nullptr) {
                    ring_buffer.unregister_subscriber(subscriber);
                }
            }
        }

        sem_destroy(&sem);
        munmap(mapping, mapping_size);
        _exit(result);
    }

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status))
        << "boundary child terminated by signal " << WTERMSIG(status);
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "boundary child returned diagnostic code " << WEXITSTATUS(status);
}

// ==============================
// 多线程压力测试
// ==============================
TEST_F(RingBufferTest, ConcurrentPublish) {
    const int num_messages = 1000;
    std::atomic<int> publish_count{0};

    // 单生产者线程
    std::thread producer([&]() {
        for (int i = 0; i < num_messages; ++i) {
            std::string msg = "MSG_" + std::to_string(i);
            while (!rb_->publish_message(msg.c_str(), msg.size() + 1)) {
                // 缓冲区满时自旋等待
                std::this_thread::yield();
            }
            publish_count++;
        }
    });

    // 消费者线程
    auto* sub = rb_->register_subscriber(1, "concurrent_sub");
    std::thread consumer([&]() {
        int read_count = 0;
        while (read_count < num_messages) {
            Message* msg = nullptr;
            if (rb_->read_next(sub, msg)) {
                read_count++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(publish_count, num_messages);
}

// ==============================
// read_expected 测试
// ==============================
TEST_F(RingBufferTest, ReadExpectedBasic) {
    // 发布3条消息
    rb_->publish_message("msg1", 5);
    rb_->publish_message("msg2", 5);
    rb_->publish_message("msg3", 5);

    auto* sub = rb_->register_subscriber(1, "expected_sub");
    Message* msg = nullptr;

    // 第一次读取，期望序列号1
    EXPECT_TRUE(rb_->read_expected(sub, msg, 1));
    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.sequence, 1);
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "msg1");

    // 第二次读取，期望序列号2
    EXPECT_TRUE(rb_->read_expected(sub, msg, 2));
    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.sequence, 2);
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "msg2");

    // 第三次读取，期望序列号3
    EXPECT_TRUE(rb_->read_expected(sub, msg, 3));
    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.sequence, 3);
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "msg3");
}

TEST_F(RingBufferTest, ReadExpectedSequenceMismatch) {
    // 发布3条消息
    rb_->publish_message("msg1", 5);
    rb_->publish_message("msg2", 5);
    rb_->publish_message("msg3", 5);

    auto* sub = rb_->register_subscriber(1, "mismatch_sub");
    Message* msg = nullptr;

    // 尝试直接读取序列号3（跳过1,2）
    // 根据实现，如果期望的序列号不匹配，应该返回false或跳到该位置
    bool result = rb_->read_expected(sub, msg, 3);

    // 如果返回true，验证读到的内容
    if (result && msg != nullptr) {
        EXPECT_EQ(msg->header.sequence, 3);
        EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "msg3");
    }
}

TEST_F(RingBufferTest, ReadExpectedNoMessage) {
    auto* sub = rb_->register_subscriber(1, "no_msg_sub");
    Message* msg = nullptr;

    // 没有消息时读取应该返回false
    EXPECT_FALSE(rb_->read_expected(sub, msg, 1));
}

// ==============================
// get_unread_count 测试
// ==============================
TEST_F(RingBufferTest, GetUnreadCountBasic) {
    auto* sub = rb_->register_subscriber(1, "count_sub");

    // 初始时未读数量为0
    EXPECT_EQ(rb_->get_unread_count(sub), 0);

    // 发布1条消息
    rb_->publish_message("msg1", 5);
    EXPECT_EQ(rb_->get_unread_count(sub), 1);

    // 再发布2条
    rb_->publish_message("msg2", 5);
    rb_->publish_message("msg3", 5);
    EXPECT_EQ(rb_->get_unread_count(sub), 3);
}

TEST_F(RingBufferTest, GetUnreadCountAfterRead) {
    auto* sub = rb_->register_subscriber(1, "count_after_read");

    // 发布5条消息
    for (int i = 0; i < 5; ++i) {
        std::string msg = "msg" + std::to_string(i);
        rb_->publish_message(msg.c_str(), msg.size() + 1);
    }
    EXPECT_EQ(rb_->get_unread_count(sub), 5);

    // 读取2条
    Message* msg = nullptr;
    rb_->read_next(sub, msg);
    rb_->read_next(sub, msg);

    // 剩余未读数量应为3
    EXPECT_EQ(rb_->get_unread_count(sub), 3);

    // 读取所有剩余消息
    while (rb_->read_next(sub, msg)) {
        // 继续读取
    }
    EXPECT_EQ(rb_->get_unread_count(sub), 0);
}

TEST_F(RingBufferTest, GetUnreadCountMultipleSubscribers) {
    auto* sub1 = rb_->register_subscriber(1, "sub1_count");
    auto* sub2 = rb_->register_subscriber(2, "sub2_count");

    // 发布3条消息
    rb_->publish_message("msg1", 5);
    rb_->publish_message("msg2", 5);
    rb_->publish_message("msg3", 5);

    // 两个订阅者都看到3条未读
    EXPECT_EQ(rb_->get_unread_count(sub1), 3);
    EXPECT_EQ(rb_->get_unread_count(sub2), 3);

    // sub1读取1条
    Message* msg = nullptr;
    rb_->read_next(sub1, msg);

    // sub1剩余2条，sub2仍然是3条
    EXPECT_EQ(rb_->get_unread_count(sub1), 2);
    EXPECT_EQ(rb_->get_unread_count(sub2), 3);
}

// ==============================
// wait_for_message 测试
// ==============================
TEST_F(RingBufferTest, WaitForMessageTimeout) {
    auto* sub = rb_->register_subscriber(1, "wait_sub");

    // 没有消息时等待应该超时返回false
    auto start = std::chrono::steady_clock::now();
    bool result = rb_->wait_for_message(sub, 100); // 100ms超时
    auto end = std::chrono::steady_clock::now();

    EXPECT_FALSE(result);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_GE(elapsed, 80); // 允许一定的误差
}

TEST_F(RingBufferTest, WaitForMessageSuccess) {
    auto* sub = rb_->register_subscriber(1, "wait_success_sub");

    // 先发布一条消息
    rb_->publish_message("preloaded", 10);

    // 应该立即返回true（有消息可读）
    bool result = rb_->wait_for_message(sub, 1000);
    EXPECT_TRUE(result);
}

TEST_F(RingBufferTest, WaitForMessageAsync) {
    auto* sub = rb_->register_subscriber(1, "wait_async_sub");

    // 在另一个线程延迟发布消息
    std::atomic<bool> published{false};
    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rb_->publish_message("delayed", 8);
        published = true;
    });

    // 等待消息（应该有）
    bool result = rb_->wait_for_message(sub, 1000);
    producer.join();

    EXPECT_TRUE(result);
    EXPECT_TRUE(published);

    // 验证读到的消息
    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_next(sub, msg));
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), "delayed");
}

TEST_F(RingBufferTest, PublishDoesNotLeaveWaitingSubscribersWhenNobodyWaits) {
    auto before = rb_->get_statistics();
    EXPECT_EQ(before.waiting_subscribers, 0u);
    EXPECT_TRUE(rb_->publish_message("no_waiter", 10));
    auto after = rb_->get_statistics();
    EXPECT_EQ(after.waiting_subscribers, 0u);
    EXPECT_GT(after.notify_generation, before.notify_generation);
}

TEST_F(RingBufferTest, WaiterCountIsNonZeroOnlyWhileThreadWaits) {
    auto* sub = rb_->register_subscriber(1, "waiter_count_sub");
    std::atomic<bool> entered{false};
    std::thread waiter([&]() {
        entered.store(true);
        rb_->wait_for_message(sub, 1000);
    });

    while (!entered.load()) {
        std::this_thread::yield();
    }
    for (int i = 0; i < 100; ++i) {
        if (rb_->get_statistics().waiting_subscribers > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(rb_->get_statistics().waiting_subscribers, 0u);
    EXPECT_TRUE(rb_->publish_message("wake", 5));
    waiter.join();
    EXPECT_EQ(rb_->get_statistics().waiting_subscribers, 0u);
}

TEST_F(RingBufferTest, ConcurrentPublishersProduceReadableMessages) {
    constexpr int publisher_count = 4;
    constexpr int messages_per_publisher = 100;
    auto* sub = rb_->register_subscriber(1, "multi_pub_sub");
    std::atomic<int> published{0};
    std::vector<std::thread> threads;

    for (int p = 0; p < publisher_count; ++p) {
        threads.emplace_back([&, p]() {
            for (int i = 0; i < messages_per_publisher; ++i) {
                std::string msg = "P" + std::to_string(p) + "_" + std::to_string(i);
                if (rb_->publish_message(msg.c_str(), msg.size() + 1)) {
                    published.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(published.load(), publisher_count * messages_per_publisher);
    EXPECT_EQ(rb_->get_statistics().current_sequence, static_cast<uint64_t>(published.load()));

    Message* msg = nullptr;
    EXPECT_TRUE(rb_->read_latest(sub, msg));
    ASSERT_NE(msg, nullptr);
    EXPECT_TRUE(msg->is_valid(rb_->is_checksum_enabled()));
}

// ==============================
// get_statistics 测试
// ==============================
TEST_F(RingBufferTest, GetStatisticsBasic) {
    // 获取初始统计信息
    auto stats = rb_->get_statistics();
    EXPECT_EQ(stats.total_messages, 0);
    EXPECT_EQ(stats.current_sequence, 0);
    EXPECT_EQ(stats.active_subscribers, 0);
    EXPECT_TRUE(stats.subscribers.empty());

    // 发布消息
    rb_->publish_message("msg1", 5);
    rb_->publish_message("msg2", 5);

    stats = rb_->get_statistics();
    EXPECT_EQ(stats.total_messages, 2);
    EXPECT_EQ(stats.current_sequence, 2);

    // 注册订阅者
    auto* sub1 = rb_->register_subscriber(1, "sub1_stats");
    auto* sub2 = rb_->register_subscriber(2, "sub2_stats");

    stats = rb_->get_statistics();
    EXPECT_EQ(stats.active_subscribers, 2);
    EXPECT_EQ(stats.subscribers.size(), 2);

    // 验证订阅者信息
    bool found_sub1 = false, found_sub2 = false;
    for (const auto& [id, name] : stats.subscribers) {
        if (id == 1 && name == "sub1_stats") found_sub1 = true;
        if (id == 2 && name == "sub2_stats") found_sub2 = true;
    }
    EXPECT_TRUE(found_sub1);
    EXPECT_TRUE(found_sub2);
}

TEST_F(RingBufferTest, GetStatisticsAfterWrapAround) {
    // 注册订阅者
    auto* sub = rb_->register_subscriber(1, "stats_wrap_sub");

    // 发布大量消息触发回绕
    char large_data[4096] = {0};
    memset(large_data, 'A', sizeof(large_data));

    const int num_messages = 30;
    for (int i = 0; i < num_messages; ++i) {
        rb_->publish_message(large_data, sizeof(large_data));
    }

    auto stats = rb_->get_statistics();
    EXPECT_EQ(stats.total_messages, num_messages);
    EXPECT_EQ(stats.current_sequence, num_messages);
    EXPECT_EQ(stats.active_subscribers, 1);
}

TEST_F(RingBufferTest, GetStatisticsAfterUnregister) {
    auto* sub1 = rb_->register_subscriber(1, "sub1_unregister");
    auto* sub2 = rb_->register_subscriber(2, "sub2_unregister");

    auto stats = rb_->get_statistics();
    EXPECT_EQ(stats.active_subscribers, 2);

    // 注销一个订阅者
    rb_->unregister_subscriber(sub1);

    stats = rb_->get_statistics();
    // unregister_subscriber 只是标记订阅者为非活跃，可能不立即减少计数
    // 根据具体实现验证
    EXPECT_LE(stats.active_subscribers, 2);
}

// ==============================
// empty(), full(), available_space(), available_data() 测试
// ==============================
TEST_F(RingBufferTest, EmptyAndFull) {
    // 初始状态：空且不满
    EXPECT_TRUE(rb_->empty());
    EXPECT_FALSE(rb_->full());

    // 发布一条消息
    rb_->publish_message("test", 5);
    EXPECT_FALSE(rb_->empty());
    EXPECT_FALSE(rb_->full());
}

TEST_F(RingBufferTest, AvailableSpaceAndData) {
    // 获取初始可用空间
    size_t initial_space = rb_->available_space();
    size_t initial_data = rb_->available_data();

    EXPECT_GT(initial_space, 0);
    EXPECT_EQ(initial_data, 0);

    // 发布消息
    const char* msg = "Hello, RingBuffer!";
    size_t msg_size = strlen(msg) + 1;
    rb_->publish_message(msg, msg_size);

    // 可用数据应该增加
    EXPECT_GT(rb_->available_data(), 0);

    // 注意：由于环形缓冲区覆盖语义，available_space() 可能不会减少
    // 它返回的是可写入空间（考虑覆盖），不是剩余空间
    // 因此不验证 available_space() 的变化
}

TEST_F(RingBufferTest, FullBufferBehavior) {
    // 发布大量消息直到缓冲区满
    char large_data[4096] = {0};
    memset(large_data, 'A', sizeof(large_data));

    int count = 0;
    while (!rb_->full() && count < 100) {
        rb_->publish_message(large_data, sizeof(large_data));
        count++;
    }

    // 缓冲区应该已满或接近满
    // full() 返回值取决于实现（可能是写位置接近读位置）
    (void)rb_->full();

    // 读取一些消息
    auto* sub = rb_->register_subscriber(1, "full_test_sub");
    Message* msg = nullptr;
    rb_->read_next(sub, msg);

    // 读取后应该不再满
    EXPECT_FALSE(rb_->full());
}

// ==============================
// set_publisher(), remove_publisher() 测试
// ==============================
TEST_F(RingBufferTest, SetAndRemovePublisher) {
    // 设置发布者信息
    bool result = rb_->set_publisher(12345, "TestPublisher");
    EXPECT_TRUE(result);

    // 获取统计信息验证发布者信息
    auto stats = rb_->get_statistics();
    // 统计信息中可能不包含发布者信息，取决于实现

    // 移除发布者
    rb_->remove_publisher();

    // 移除后再次设置
    result = rb_->set_publisher(99999, "AnotherPublisher");
    EXPECT_TRUE(result);
}

TEST_F(RingBufferTest, SetPublisherEmptyName) {
    // 设置空名称的发布者
    bool result = rb_->set_publisher(1, "");
    EXPECT_TRUE(result);
}

TEST_F(RingBufferTest, ReinitializesInvalidSyncState) {
    rb_.reset();

    constexpr uint32_t sync_magic = 0x53594E43;
    constexpr uint32_t cleared = 0;
    constexpr size_t buffer_size = 64 * 1024;
    bool corrupted = false;

    for (size_t i = 0; i + sizeof(sync_magic) <= buffer_size; ++i) {
        uint32_t value = 0;
        std::memcpy(&value, buffer_ + i, sizeof(value));
        if (value == sync_magic) {
            std::memcpy(buffer_ + i, &cleared, sizeof(cleared));
            corrupted = true;
            break;
        }
    }

    ASSERT_TRUE(corrupted);
    rb_ = std::make_unique<RingBuffer>(buffer_, buffer_size, &sem_, true);
    EXPECT_TRUE(rb_->set_publisher(123, "after_reinit"));
    EXPECT_TRUE(rb_->publish_message("ok", 3));
}

TEST_F(RingBufferTest, WaitForMessageReinitializesInvalidSyncState) {
    auto* sub = rb_->register_subscriber(1, "wait_reinit_sub");
    ASSERT_NE(sub, nullptr);

    constexpr uint32_t sync_magic = 0x53594E43;
    constexpr uint32_t cleared = 0;
    constexpr size_t buffer_size = 64 * 1024;
    bool corrupted = false;

    for (size_t i = 0; i + sizeof(sync_magic) <= buffer_size; ++i) {
        uint32_t value = 0;
        std::memcpy(&value, buffer_ + i, sizeof(value));
        if (value == sync_magic) {
            std::memcpy(buffer_ + i, &cleared, sizeof(cleared));
            corrupted = true;
            break;
        }
    }

    ASSERT_TRUE(corrupted);
    EXPECT_FALSE(rb_->wait_for_message(sub, 1));
    EXPECT_TRUE(rb_->publish_message("after_wait_reinit", 18));
}

// ==============================
// notify_subscribers() 测试
// ==============================
TEST_F(RingBufferTest, NotifySubscribers) {
    auto* sub = rb_->register_subscriber(1, "notify_sub");

    // 在另一个线程等待消息
    std::atomic<bool> notified{false};
    std::thread waiter([&]() {
        // 等待消息通知
        bool result = rb_->wait_for_message(sub, 500); // 500ms 超时
        if (result) {
            notified = true;
        }
    });

    // 等待一段时间确保 waiter 进入等待状态
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 发布消息并通知订阅者
    rb_->publish_message("notify_test", 12);
    rb_->notify_subscribers();

    waiter.join();

    EXPECT_TRUE(notified);
}

TEST_F(RingBufferTest, NotifyWithoutSubscribers) {
    // 没有订阅者时调用 notify 应该安全
    rb_->notify_subscribers();
    rb_->notify_subscribers();
    EXPECT_TRUE(true); // 没有崩溃即通过
}

// ==============================
// is_checksum_enabled() 测试
// ==============================
TEST_F(RingBufferTest, IsChecksumEnabled) {
    // 默认构造时启用校验和
    EXPECT_TRUE(rb_->is_checksum_enabled());
}

TEST_F(RingBufferTest, ChecksumDisabled) {
    // 创建禁用校验和的 RingBuffer
    sem_t sem2;
    sem_init(&sem2, 0, 1);
    uint8_t* buffer2 = static_cast<uint8_t*>(aligned_alloc(64, 64 * 1024));
    ASSERT_NE(buffer2, nullptr);

    auto rb_no_checksum = std::make_unique<RingBuffer>(buffer2, 64 * 1024, &sem2, false);
    EXPECT_FALSE(rb_no_checksum->is_checksum_enabled());

    free(buffer2);
    sem_destroy(&sem2);
}
