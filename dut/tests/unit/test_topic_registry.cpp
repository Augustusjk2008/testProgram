/**
 * @file test_topic_registry.cpp
 * @brief TopicRegistry单元测试
 *
 * 注意：TopicRegistry应该构造在栈/堆上，而不是placement new在共享内存中。
 * 它的构造函数会清零整个共享内存区域，如果自身在其中会导致段错误。
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>

#include "MB_DDF/DDS/TopicRegistry.h"
#include "MB_DDF/DDS/SharedMemory.h"

using namespace MB_DDF::DDS;

// ==============================
// 测试固件
// ==============================
class TopicRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 清理可能存在的旧共享内存和信号量
        shm_unlink("/test_topic_registry");
        sem_unlink("/test_topic_registry_sem");

        // 创建共享内存管理器
        shm_manager_ = std::make_unique<SharedMemoryManager>("/test_topic_registry", 4 * 1024 * 1024); // 4MB
        ASSERT_NE(shm_manager_->get_address(), nullptr);

        // TopicRegistry构造在栈上（正确用法）
        void* shm_addr = shm_manager_->get_address();
        size_t shm_size = shm_manager_->get_size();
        registry_ = std::make_unique<TopicRegistry>(shm_addr, shm_size, shm_manager_.get());
    }

    void TearDown() override {
        // 先销毁registry，再销毁共享内存
        registry_.reset();
        shm_manager_.reset();
        shm_unlink("/test_topic_registry");
        sem_unlink("/test_topic_registry_sem");
    }

    std::unique_ptr<SharedMemoryManager> shm_manager_;
    std::unique_ptr<TopicRegistry> registry_;
};

// ==============================
// Topic注册与查找
// ==============================
TEST_F(TopicRegistryTest, RegisterTopic) {
    auto* info = registry_->register_topic("rt://test/topic1", 64 * 1024);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->ring_buffer_size, 64 * 1024);
    EXPECT_GT(info->topic_id, 0);
}

TEST_F(TopicRegistryTest, GetExistingTopicMetadata) {
    auto* info1 = registry_->register_topic("rt://test/topic2", 32 * 1024);
    ASSERT_NE(info1, nullptr);
    uint32_t topic_id = info1->topic_id;

    // 再次查找同一个topic
    auto* info2 = registry_->get_topic_metadata("rt://test/topic2");
    ASSERT_NE(info2, nullptr);
    EXPECT_EQ(info2->topic_id, topic_id);
}

TEST_F(TopicRegistryTest, GetNonExistingTopicMetadata) {
    auto* info = registry_->get_topic_metadata("rt://non/existing/topic");
    EXPECT_EQ(info, nullptr);
}

TEST_F(TopicRegistryTest, RegisterDuplicateTopicReturnsSame) {
    auto* info1 = registry_->register_topic("rt://duplicate/topic", 16 * 1024);
    ASSERT_NE(info1, nullptr);

    auto* info2 = registry_->register_topic("rt://duplicate/topic", 16 * 1024);
    ASSERT_NE(info2, nullptr);

    // 应该返回同一个topic
    EXPECT_EQ(info1->topic_id, info2->topic_id);
}

// ==============================
// 多个Topic管理
// ==============================
TEST_F(TopicRegistryTest, RegisterMultipleTopics) {
    std::vector<std::string> names = {
        "rt://topic/a", "rt://topic/b", "rt://topic/c",
        "rt://sensor/imu", "rt://sensor/gps", "rt://sensor/camera"
    };

    std::vector<uint32_t> ids;
    for (const auto& name : names) {
        auto* info = registry_->register_topic(name, 16 * 1024);
        ASSERT_NE(info, nullptr) << "Failed to register: " << name;
        ids.push_back(info->topic_id);
    }

    // 验证ID唯一性
    std::sort(ids.begin(), ids.end());
    auto last = std::unique(ids.begin(), ids.end());
    EXPECT_EQ(last, ids.end()) << "Topic IDs should be unique";
}

// ==============================
// 通过ID获取
// ==============================
TEST_F(TopicRegistryTest, GetTopicMetadataById) {
    auto* info = registry_->register_topic("rt://byid/test", 64 * 1024);
    ASSERT_NE(info, nullptr);
    uint32_t topic_id = info->topic_id;

    // 通过ID查找
    auto* found = registry_->get_topic_metadata(topic_id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->topic_id, topic_id);
    EXPECT_STREQ(found->topic_name, "rt://byid/test");
}

TEST_F(TopicRegistryTest, GetTopicMetadataByInvalidId) {
    auto* found = registry_->get_topic_metadata(99999);
    EXPECT_EQ(found, nullptr);
}

// ==============================
// 获取所有Topic
// ==============================
TEST_F(TopicRegistryTest, GetAllTopics) {
    // 初始为空
    auto all = registry_->get_all_topics();
    EXPECT_EQ(all.size(), 0);

    // 注册几个topic
    registry_->register_topic("rt://all/topic1", 16 * 1024);
    registry_->register_topic("rt://all/topic2", 32 * 1024);
    registry_->register_topic("rt://all/topic3", 64 * 1024);

    all = registry_->get_all_topics();
    EXPECT_EQ(all.size(), 3);
}

// ==============================
// Topic名称格式验证
// ==============================
TEST_F(TopicRegistryTest, ValidTopicNameFormat) {
    // 测试标准命名格式
    EXPECT_TRUE(registry_->is_valid_topic_name("rt://navigation/pose"));
    EXPECT_TRUE(registry_->is_valid_topic_name("shm://camera/frame"));
    EXPECT_TRUE(registry_->is_valid_topic_name("domain://path/to/topic"));
}

TEST_F(TopicRegistryTest, InvalidTopicNameFormat) {
    // 无效格式
    EXPECT_FALSE(registry_->is_valid_topic_name(""));           // 空
    EXPECT_FALSE(registry_->is_valid_topic_name("topic"));       // 无://
    EXPECT_FALSE(registry_->is_valid_topic_name("://topic"));    // 空domain
    EXPECT_FALSE(registry_->is_valid_topic_name("domain://"));   // 空path
    EXPECT_FALSE(registry_->is_valid_topic_name("test/topic1")); // 缺少://
}

// ==============================
// 内存边界
// ==============================
TEST_F(TopicRegistryTest, LargeBufferSize) {
    // 创建大buffer的topic
    auto* info = registry_->register_topic("rt://large/topic", 512 * 1024);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->ring_buffer_size, 512 * 1024);
}

TEST_F(TopicRegistryTest, SmallBufferSize) {
    // 最小buffer大小
    auto* info = registry_->register_topic("rt://small/topic", 4 * 1024);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->ring_buffer_size, 4 * 1024);
}

// ==============================
// Topic元数据
// ==============================
TEST_F(TopicRegistryTest, TopicMetadata) {
    auto* info = registry_->register_topic("rt://meta/test", 64 * 1024);
    ASSERT_NE(info, nullptr);

    // 验证基本字段
    EXPECT_GT(info->topic_id, 0);
    EXPECT_EQ(info->ring_buffer_size, 64 * 1024);
    EXPECT_GT(info->ring_buffer_offset, 0);  // 偏移量应该大于0（在头部之后）
    EXPECT_STREQ(info->topic_name, "rt://meta/test");
}

// ==============================
// Topic名称解析注册
// ==============================
TEST_F(TopicRegistryTest, RegisterWithProtocolPrefix) {
    auto* info1 = registry_->register_topic("rt://navigation/pose", 32 * 1024);
    ASSERT_NE(info1, nullptr);

    auto* info2 = registry_->register_topic("shm://camera/frame", 128 * 1024);
    ASSERT_NE(info2, nullptr);

    // 查找
    auto* found = registry_->get_topic_metadata("rt://navigation/pose");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->topic_id, info1->topic_id);
}

// ==============================
// 共享内存边界耗尽测试
// ==============================
TEST_F(TopicRegistryTest, MemoryExhaustion) {
    // 使用一个很小的共享内存（1MB）
    shm_unlink("/test_shm_exhaust");
    sem_unlink("/test_shm_exhaust_sem");

    auto small_shm = std::make_unique<SharedMemoryManager>("/test_shm_exhaust", 1024 * 1024);  // 1MB
    ASSERT_NE(small_shm->get_address(), nullptr);

    auto small_registry = std::make_unique<TopicRegistry>(
        small_shm->get_address(), small_shm->get_size(), small_shm.get());

    int registered_count = 0;
    // 尝试注册大量 topic 直到内存耗尽
    for (int i = 0; i < 1000; ++i) {
        std::string topic_name = "rt://exhaust/topic" + std::to_string(i);
        auto* info = small_registry->register_topic(topic_name, 64 * 1024);  // 每个 64KB

        if (info == nullptr) {
            // 内存耗尽，注册失败
            break;
        }
        registered_count++;
    }

    // 应该成功注册了一些，但不到 1000 个（因为内存有限）
    EXPECT_GT(registered_count, 0);
    EXPECT_LT(registered_count, 1000);

    // 验证已注册的 topic 可以正常查找
    for (int i = 0; i < registered_count; ++i) {
        std::string topic_name = "rt://exhaust/topic" + std::to_string(i);
        auto* found = small_registry->get_topic_metadata(topic_name);
        EXPECT_NE(found, nullptr) << "Topic " << i << " should exist";
    }

    // 清理
    small_registry.reset();
    small_shm.reset();
    shm_unlink("/test_shm_exhaust");
    sem_unlink("/test_shm_exhaust_sem");
}

TEST_F(TopicRegistryTest, TotalBufferSizeTracking) {
    // 注册一系列 topic，验证总缓冲区大小跟踪
    size_t total_size = 0;
    std::vector<size_t> sizes = {16*1024, 32*1024, 64*1024, 128*1024};

    for (size_t i = 0; i < sizes.size(); ++i) {
        std::string topic_name = "rt://size/topic" + std::to_string(i);
        auto* info = registry_->register_topic(topic_name, sizes[i]);
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->ring_buffer_size, sizes[i]);
        total_size += sizes[i];
    }

    // 获取所有 topic 验证总大小
    auto all_topics = registry_->get_all_topics();
    EXPECT_EQ(all_topics.size(), sizes.size());

    size_t actual_total = 0;
    for (const auto* topic : all_topics) {
        actual_total += topic->ring_buffer_size;
    }
    EXPECT_EQ(actual_total, total_size);
}
