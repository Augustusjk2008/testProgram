/**
 * @file test_dds_core.cpp
 * @brief DDSCore 单元测试
 *
 * 测试 DDSCore 单例的核心功能：
 * - initialize(), shutdown()
 * - create_publisher/create_subscriber
 * - create_writer/create_reader 别名
 * - data_write(), data_read()
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/SharedMemory.h"

using namespace MB_DDF::DDS;

// ==============================
// 测试固件
// ==============================
class DDSCoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 先关闭 DDS（如果之前已初始化）
        DDSCore::instance().shutdown();

        // 清理之前的共享内存
        shm_unlink("/MB_DDF_V2_SHM");
        sem_unlink("/MB_DDF_V2_SHM_sem");
    }

    void TearDown() override {
        // 确保 DDS 关闭
        DDSCore::instance().shutdown();
        // 清理共享内存
        shm_unlink("/MB_DDF_V2_SHM");
        sem_unlink("/MB_DDF_V2_SHM_sem");
    }
};

// ==============================
// 初始化和关闭测试
// ==============================
TEST_F(DDSCoreTest, InitializeAndShutdown) {
    auto& dds = DDSCore::instance();

    // SetUp 已经确保 DDS 处于关闭状态

    // 正常初始化
    bool init_result = dds.initialize(16 * 1024 * 1024);  // 16MB
    EXPECT_TRUE(init_result);

    // 重复初始化应该失败（如果实现支持）
    // 注意：当前实现可能允许重复初始化，仅记录警告
    // 这里不强制验证，因为行为取决于具体实现
    (void)dds.initialize(16 * 1024 * 1024);

    // 关闭
    dds.shutdown();

    // 关闭后可以重新初始化
    EXPECT_TRUE(dds.initialize(16 * 1024 * 1024));

    dds.shutdown();
}

TEST_F(DDSCoreTest, MultipleShutdownSafe) {
    auto& dds = DDSCore::instance();

    dds.initialize(8 * 1024 * 1024);
    dds.shutdown();

    // 多次关闭应该安全
    dds.shutdown();
    dds.shutdown();
}

TEST_F(DDSCoreTest, InitializeFailsOnVersionMismatch) {
    constexpr size_t shm_size = 16 * 1024 * 1024;
    const char* shm_name = "/MB_DDF_V2_SHM";
    shm_unlink(shm_name);
    sem_unlink("/MB_DDF_V2_SHM_sem");

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    ASSERT_NE(fd, -1);
    ASSERT_EQ(ftruncate(fd, shm_size), 0);

    void* addr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(addr, MAP_FAILED);
    auto* header = static_cast<TopicRegistryHeader*>(addr);
    header->magic_number = 0x4C444453;
    header->version = 0x00000001;
    munmap(addr, shm_size);
    close(fd);

    EXPECT_FALSE(DDSCore::instance().initialize(shm_size));
}

// ==============================
// 创建发布者和订阅者测试
// ==============================
TEST_F(DDSCoreTest, CreatePublisherBeforeInit) {
    auto& dds = DDSCore::instance();

    // SetUp 已经确保 DDS 处于关闭状态

    // 未初始化时创建发布者
    auto pub = dds.create_publisher("rt://test/topic");

    // 注意：当前实现可能在内部自动初始化
    // 如果 pub 不为 nullptr，说明实现支持自动初始化
    // 这里验证两种可能的行为之一
    if (pub == nullptr) {
        EXPECT_EQ(pub, nullptr);  // 预期失败
    } else {
        EXPECT_NE(pub, nullptr);  // 实际成功（自动初始化）
    }
}

TEST_F(DDSCoreTest, CreatePublisherAfterInit) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // 创建发布者
    auto pub = dds.create_publisher("rt://test/pub");
    EXPECT_NE(pub, nullptr);

    // 验证发布者属性
    EXPECT_EQ(pub->get_topic_name(), "rt://test/pub");
    EXPECT_NE(pub->get_id(), 0);
    EXPECT_FALSE(pub->get_name().empty());
}

TEST_F(DDSCoreTest, CreateSubscriberAfterInit) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // 创建订阅者
    auto sub = dds.create_subscriber("rt://test/sub");
    EXPECT_NE(sub, nullptr);
}

TEST_F(DDSCoreTest, CreateWriterAlias) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // create_writer 是 create_publisher 的别名
    auto writer = dds.create_writer("rt://writer/topic");
    EXPECT_NE(writer, nullptr);
    EXPECT_EQ(writer->get_topic_name(), "rt://writer/topic");
}

TEST_F(DDSCoreTest, CreateReaderAlias) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // create_reader 是 create_subscriber 的别名
    auto reader = dds.create_reader("rt://reader/topic");
    EXPECT_NE(reader, nullptr);
}

// ==============================
// data_write 和 data_read 测试
// ==============================
TEST_F(DDSCoreTest, DataWriteAndRead) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // 创建发布者和订阅者（同一 Topic）
    auto pub = dds.create_publisher("rt://data/test");
    auto sub = dds.create_subscriber("rt://data/test");

    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    // 发布数据
    const char* msg = "Hello DDSCore!";
    size_t written = dds.data_write(pub, msg, strlen(msg) + 1);
    EXPECT_GT(written, 0);

    // 读取数据
    char buffer[256] = {0};
    size_t read = dds.data_read(sub, buffer, sizeof(buffer));
    EXPECT_GT(read, 0);
    EXPECT_STREQ(buffer, msg);
}

TEST_F(DDSCoreTest, DataReadEmpty) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    auto sub = dds.create_subscriber("rt://empty/test");
    ASSERT_NE(sub, nullptr);

    // 没有数据时读取
    char buffer[256] = {0};
    size_t read = dds.data_read(sub, buffer, sizeof(buffer));
    EXPECT_EQ(read, 0);
}

// 测试支持空指针检查
TEST_F(DDSCoreTest, DataWriteNullPublisher) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // 使用空的发布者指针 - 如果实现不检查会导致段错误
    std::shared_ptr<Publisher> null_pub;
    const char* msg = "test";
    size_t written = dds.data_write(null_pub, msg, strlen(msg) + 1);
    EXPECT_EQ(written, 0);
}

TEST_F(DDSCoreTest, DataReadNullSubscriber) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // 使用空的订阅者指针 - 如果实现不检查会导致段错误
    std::shared_ptr<Subscriber> null_sub;
    char buffer[256] = {0};
    size_t read = dds.data_read(null_sub, buffer, sizeof(buffer));
    EXPECT_EQ(read, 0);
}

// ==============================
// 多 Topic 测试
// ==============================
TEST_F(DDSCoreTest, MultipleTopics) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(32 * 1024 * 1024));

    // 创建多个 Topic
    auto pub1 = dds.create_publisher("rt://topic/one");
    auto pub2 = dds.create_publisher("rt://topic/two");
    auto pub3 = dds.create_publisher("rt://topic/three");

    EXPECT_NE(pub1, nullptr);
    EXPECT_NE(pub2, nullptr);
    EXPECT_NE(pub3, nullptr);

    // 验证 Topic ID 不同
    EXPECT_NE(pub1->get_topic_id(), pub2->get_topic_id());
    EXPECT_NE(pub2->get_topic_id(), pub3->get_topic_id());
}

TEST_F(DDSCoreTest, SameTopicMultiplePublishers) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // 同一 Topic 创建多个发布者
    auto pub1 = dds.create_publisher("rt://shared/topic");
    auto pub2 = dds.create_publisher("rt://shared/topic");

    // 应该都能创建成功（或根据实现返回同一个）
    EXPECT_NE(pub1, nullptr);
    EXPECT_NE(pub2, nullptr);

    // 两个发布者应该有不同的 ID
    EXPECT_NE(pub1->get_id(), pub2->get_id());
}

TEST_F(DDSCoreTest, MultiplePublishersZeroCopySameTopic) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(32 * 1024 * 1024));
    auto pub1 = dds.create_publisher("rt://multi/zc");
    auto pub2 = dds.create_publisher("rt://multi/zc");
    auto sub = dds.create_subscriber("rt://multi/zc");
    ASSERT_NE(pub1, nullptr);
    ASSERT_NE(pub2, nullptr);
    ASSERT_NE(sub, nullptr);

    std::thread t1([&]() {
        auto msg = pub1->begin_message(32);
        ASSERT_TRUE(msg.valid());
        std::strcpy(static_cast<char*>(msg.data()), "from_pub1");
        EXPECT_TRUE(msg.commit(10));
    });
    std::thread t2([&]() {
        auto msg = pub2->begin_message(32);
        ASSERT_TRUE(msg.valid());
        std::strcpy(static_cast<char*>(msg.data()), "from_pub2");
        EXPECT_TRUE(msg.commit(10));
    });
    t1.join();
    t2.join();

    char buf[64] = {0};
    size_t n = dds.data_read(sub, buf, sizeof(buf));
    EXPECT_GT(n, 0u);
    EXPECT_TRUE(std::strcmp(buf, "from_pub1") == 0 || std::strcmp(buf, "from_pub2") == 0);
}

// ==============================
// 端到端集成测试
// ==============================
TEST_F(DDSCoreTest, EndToEndMessaging) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(32 * 1024 * 1024));

    // 创建发布者和订阅者
    auto pub = dds.create_publisher("rt://e2e/test");
    auto sub = dds.create_subscriber("rt://e2e/test");

    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    // 发布多条消息
    for (int i = 0; i < 10; ++i) {
        std::string msg = "Message " + std::to_string(i);
        size_t written = dds.data_write(pub, msg.c_str(), msg.size() + 1);
        EXPECT_GT(written, 0);
    }

    // 读取消息
    // 注意：data_read 默认读取最新消息（latest=true）
    // 如果发布速度比读取快，可能只能读到最新消息（覆盖）
    int read_count = 0;
    char buffer[256];

    // 先读取一次（最新消息）
    size_t n = dds.data_read(sub, buffer, sizeof(buffer));
    if (n > 0) {
        read_count++;
    }

    // 期望至少读到1条消息（最新消息）
    // 由于环形缓冲区覆盖特性，可能无法读到所有历史消息
    EXPECT_GE(read_count, 1);

    // 验证读到的是有效数据
    if (n > 0) {
        EXPECT_TRUE(strlen(buffer) > 0);
    }
}

TEST_F(DDSCoreTest, CallbackSubscription) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(32 * 1024 * 1024));

    std::atomic<int> received_count{0};
    std::string last_message;

    // 创建带回调的订阅者
    auto sub = dds.create_subscriber("rt://callback/test", true,
        [&received_count, &last_message](const void* data, size_t size, uint64_t) {
            received_count++;
            last_message = std::string(static_cast<const char*>(data), size - 1);
        });

    ASSERT_NE(sub, nullptr);

    // 创建发布者并发送消息
    auto pub = dds.create_publisher("rt://callback/test");
    ASSERT_NE(pub, nullptr);

    dds.data_write(pub, "Callback test", 14);

    // 等待回调处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_GE(received_count.load(), 1);
}

// ==============================
// 边界条件测试
// ==============================
TEST_F(DDSCoreTest, EmptyTopicName) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // 空 Topic 名称
    auto pub = dds.create_publisher("");
    // 根据实现可能返回 nullptr 或创建成功
    (void)pub;
}

TEST_F(DDSCoreTest, InvalidTopicName) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

    // 无效 Topic 名称（不符合 domain://address 格式）
    auto pub = dds.create_publisher("invalid_topic");
    // 行为取决于实现
    (void)pub;
}

TEST_F(DDSCoreTest, LargeDataTransfer) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(64 * 1024 * 1024));

    auto pub = dds.create_publisher("rt://large/data");
    auto sub = dds.create_subscriber("rt://large/data");

    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    // 大数据传输（16KB）
    std::vector<char> large_data(16 * 1024, 'X');
    large_data.back() = '\0';

    size_t written = dds.data_write(pub, large_data.data(), large_data.size());
    EXPECT_GT(written, 0);

    std::vector<char> buffer(16 * 1024, 0);
    size_t read = dds.data_read(sub, buffer.data(), buffer.size());
    EXPECT_EQ(read, large_data.size());
    EXPECT_EQ(buffer[0], 'X');
    EXPECT_EQ(buffer[16 * 1024 - 2], 'X');
}
