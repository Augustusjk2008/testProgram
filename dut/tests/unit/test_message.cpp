/**
 * @file test_message.cpp
 * @brief Message和CRC32单元测试
 */

#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include "MB_DDF/DDS/Message.h"

using namespace MB_DDF::DDS;

// ==============================
// CRC32算法验证
// ==============================
TEST(MessageCRC32, AlgorithmVerification) {
    // 验证文档中的标准CRC32测试向量
    // 数据: AA BB CC DD EE FF 00 11
    // 期望: 0x891A7844 (反向算法)
    EXPECT_EQ(Message::verify_crc32_algorithms(), 0);
}

TEST(MessageCRC32, KnownValue) {
    const uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    constexpr size_t test_size = sizeof(data);
    constexpr uint32_t expected_crc = 0x891A7844;

    uint32_t crc = MessageHeader::calculate_crc32_reflected(data, test_size);
    EXPECT_EQ(crc, expected_crc);
}

TEST(MessageCRC32, EmptyData) {
    uint32_t crc = MessageHeader::calculate_crc32_reflected(nullptr, 0);
    EXPECT_EQ(crc, 0);

    crc = MessageHeader::calculate_crc32_reflected(reinterpret_cast<const uint8_t*>(""), 0);
    EXPECT_EQ(crc, 0);
}

// ==============================
// Message构造与验证
// ==============================
TEST(MessageConstruct, BasicMessage) {
    const char* data = "Hello, DDS!";
    size_t data_size = strlen(data) + 1;

    Message msg(1, 100, const_cast<void*>(static_cast<const void*>(data)), data_size);

    EXPECT_EQ(msg.header.topic_id, 1);
    EXPECT_EQ(msg.header.sequence, 100);
    EXPECT_EQ(msg.header.data_size, data_size);
    EXPECT_TRUE(msg.header.is_valid());
}

TEST(MessageConstruct, ChecksumVerification) {
    const char* data = "Test message for checksum";
    size_t data_size = strlen(data) + 1;

    // 正确用法：分配足够内存容纳 Message + 数据
    size_t total_size = sizeof(Message) + data_size;
    auto* storage = new uint8_t[total_size];

    // placement new 构造 Message（不传入数据指针，避免构造函数计算错误校验和）
    auto* msg = new (storage) Message();
    msg->header.topic_id = 42;
    msg->header.sequence = 999;
    msg->header.data_size = data_size;
    msg->header.set_timestamp();

    // 复制数据到 Message 后面的正确位置
    memcpy(msg->get_data(), data, data_size);

    // 现在计算校验和（基于实际存储位置的数据）
    msg->header.set_checksum(msg->get_data(), data_size);

    // 验证消息有效性（包含校验和）
    EXPECT_TRUE(msg->is_valid(true));

    delete[] storage;
}

TEST(MessageConstruct, ZeroData) {
    Message msg(1, 1, nullptr, 0);

    EXPECT_EQ(msg.header.data_size, 0);
    EXPECT_TRUE(msg.is_valid(true));  // 空数据消息也是有效的
}

// ==============================
// 数据指针访问
// ==============================
TEST(MessageData, DataAccess) {
    char buffer[] = "Test data buffer";
    size_t data_size = strlen(buffer) + 1;

    // 在堆上分配消息+数据空间模拟共享内存布局
    size_t total_size = sizeof(Message) + data_size;
    auto* storage = new uint8_t[total_size];

    // placement new构造Message
    auto* msg = new (storage) Message(1, 1, nullptr, 0);
    msg->header.data_size = data_size;

    // 复制数据到消息后面
    memcpy(msg->get_data(), buffer, data_size);

    // 验证数据可读
    EXPECT_STREQ(static_cast<const char*>(msg->get_data()), buffer);

    delete[] storage;
}

// ==============================
// 时间戳测试
// ==============================
TEST(MessageTimestamp, TimestampSet) {
    Message msg;

    auto before = std::chrono::steady_clock::now();
    msg.header.set_timestamp();
    auto after = std::chrono::steady_clock::now();

    // 验证时间戳在合理范围内（纳秒级）
    auto msg_time = std::chrono::nanoseconds(msg.header.timestamp);
    auto msg_tp = std::chrono::steady_clock::time_point(msg_time);

    EXPECT_GE(msg_tp, before);
    EXPECT_LE(msg_tp, after);
}

// ==============================
// 消息大小计算
// ==============================
TEST(MessageSize, TotalSize) {
    EXPECT_EQ(Message::total_size(0), sizeof(MessageHeader));
    EXPECT_EQ(Message::total_size(100), sizeof(MessageHeader) + 100);
    EXPECT_EQ(Message::total_size(1024), sizeof(MessageHeader) + 1024);
}

TEST(MessageSize, MsgSizeMethod) {
    Message msg;
    msg.header.data_size = 256;

    EXPECT_EQ(msg.msg_size(), sizeof(MessageHeader) + 256);
}

// ==============================
// 边界条件
// ==============================
TEST(MessageBoundary, LargeDataSize) {
    // 测试大数据（接近32位上限）
    Message msg(0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF, nullptr, 0xFFFFFFFF);

    EXPECT_EQ(msg.header.topic_id, 0xFFFFFFFF);
    EXPECT_EQ(msg.header.sequence, 0xFFFFFFFFFFFFFFFF);
    EXPECT_EQ(msg.header.data_size, 0xFFFFFFFF);
}

// ==============================
// Message::update() 测试
// ==============================
TEST(MessageUpdate, UpdateTimestamp) {
    char buffer[] = "Test data for update";
    size_t data_size = strlen(buffer) + 1;

    size_t total_size = sizeof(Message) + data_size;
    auto* storage = new uint8_t[total_size];

    auto* msg = new (storage) Message();
    msg->header.topic_id = 1;
    msg->header.sequence = 100;
    msg->header.data_size = data_size;

    // 复制数据
    memcpy(msg->get_data(), buffer, data_size);

    // 记录旧时间戳
    uint64_t old_timestamp = msg->header.timestamp;
    uint32_t old_checksum = msg->header.checksum;

    // 等待一小段时间确保时间戳变化
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 调用 update
    msg->update(true);

    // 验证时间戳已更新
    EXPECT_GT(msg->header.timestamp, old_timestamp);

    // 验证校验和已重新计算（不为0）
    EXPECT_NE(msg->header.checksum, 0);
    EXPECT_NE(msg->header.checksum, old_checksum);

    // 验证消息仍然有效
    EXPECT_TRUE(msg->is_valid(true));

    delete[] storage;
}

TEST(MessageUpdate, UpdateWithoutChecksum) {
    char buffer[] = "No checksum update";
    size_t data_size = strlen(buffer) + 1;

    size_t total_size = sizeof(Message) + data_size;
    auto* storage = new uint8_t[total_size];

    auto* msg = new (storage) Message();
    msg->header.data_size = data_size;
    memcpy(msg->get_data(), buffer, data_size);

    // 调用 update 但禁用校验和
    msg->update(false);

    // 校验和应该为0
    EXPECT_EQ(msg->header.checksum, 0);

    // 禁用校验和验证时消息有效
    EXPECT_TRUE(msg->is_valid(false));

    // 启用校验和验证时失败（因为校验和为0但数据非空）
    EXPECT_FALSE(msg->is_valid(true));

    delete[] storage;
}

TEST(MessageUpdate, UpdateEmptyData) {
    auto* storage = new uint8_t[sizeof(Message)];
    auto* msg = new (storage) Message();
    msg->header.data_size = 0;

    uint64_t old_timestamp = msg->header.timestamp;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    msg->update(true);

    // 时间戳应该更新
    EXPECT_GT(msg->header.timestamp, old_timestamp);

    // 空数据的校验和应该为0
    EXPECT_EQ(msg->header.checksum, 0);

    delete[] storage;
}

// ==============================
// CRC32Mode::NORMAL 正向算法测试
// ==============================
TEST(MessageCRC32, NormalModeBasic) {
    const char* data = "Hello, World!";
    size_t data_size = strlen(data);

    // 使用正向算法计算
    uint32_t crc_normal = MessageHeader::calculate_crc32_normal(data, data_size);

    // 验证结果不为0且不同于反向算法
    EXPECT_NE(crc_normal, 0);

    uint32_t crc_reflected = MessageHeader::calculate_crc32_reflected(data, data_size);
    EXPECT_NE(crc_normal, crc_reflected); // 两种算法结果应该不同
}

TEST(MessageCRC32, NormalModeConsistency) {
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};

    // 多次计算应该得到相同结果
    uint32_t crc1 = MessageHeader::calculate_crc32_normal(data, sizeof(data));
    uint32_t crc2 = MessageHeader::calculate_crc32_normal(data, sizeof(data));

    EXPECT_EQ(crc1, crc2);
}

TEST(MessageCRC32, NormalModeEmptyData) {
    // 空数据应该返回0
    uint32_t crc = MessageHeader::calculate_crc32_normal(nullptr, 0);
    EXPECT_EQ(crc, 0);

    crc = MessageHeader::calculate_crc32_normal(reinterpret_cast<const uint8_t*>(""), 0);
    EXPECT_EQ(crc, 0);
}

TEST(MessageCRC32, CalculateChecksumWithMode) {
    const char* data = "Test data for checksum mode";
    size_t data_size = strlen(data);

    // 使用通用接口测试两种模式
    uint32_t crc_reflected = MessageHeader::calculate_checksum(data, data_size, CRC32Mode::REFLECTED);
    uint32_t crc_normal = MessageHeader::calculate_checksum(data, data_size, CRC32Mode::NORMAL);

    // 验证两种模式结果不同
    EXPECT_NE(crc_reflected, crc_normal);

    // 验证与直接调用一致
    EXPECT_EQ(crc_reflected, MessageHeader::calculate_crc32_reflected(data, data_size));
    EXPECT_EQ(crc_normal, MessageHeader::calculate_crc32_normal(data, data_size));
}

// ==============================
// 校验和验证失败场景测试
// ==============================
TEST(MessageChecksum, VerificationFailure) {
    char buffer[] = "Original data";
    size_t data_size = strlen(buffer) + 1;

    size_t total_size = sizeof(Message) + data_size;
    auto* storage = new uint8_t[total_size];

    auto* msg = new (storage) Message();
    msg->header.data_size = data_size;
    memcpy(msg->get_data(), buffer, data_size);

    // 计算并设置正确的校验和
    msg->header.set_checksum(msg->get_data(), data_size);
    EXPECT_TRUE(msg->is_valid(true));

    // 修改数据（模拟数据损坏）
    char* data_ptr = static_cast<char*>(msg->get_data());
    data_ptr[0] = 'X'; // 修改第一个字符

    // 验证校验和失败
    EXPECT_FALSE(msg->is_valid(true));

    delete[] storage;
}

TEST(MessageChecksum, VerificationFailureMultipleBytes) {
    uint8_t data[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    size_t data_size = sizeof(data);

    size_t total_size = sizeof(Message) + data_size;
    auto* storage = new uint8_t[total_size];

    auto* msg = new (storage) Message();
    msg->header.data_size = data_size;
    memcpy(msg->get_data(), data, data_size);

    // 设置校验和
    msg->header.set_checksum(msg->get_data(), data_size);
    EXPECT_TRUE(msg->is_valid(true));

    // 修改多个字节
    uint8_t* data_ptr = static_cast<uint8_t*>(msg->get_data());
    data_ptr[3] = 0xFF;
    data_ptr[7] = 0xAA;

    // 验证失败
    EXPECT_FALSE(msg->is_valid(true));

    delete[] storage;
}

TEST(MessageChecksum, WrongChecksumValue) {
    char buffer[] = "Test with wrong checksum";
    size_t data_size = strlen(buffer) + 1;

    size_t total_size = sizeof(Message) + data_size;
    auto* storage = new uint8_t[total_size];

    auto* msg = new (storage) Message();
    msg->header.data_size = data_size;
    memcpy(msg->get_data(), buffer, data_size);

    // 设置一个错误的校验和
    msg->header.checksum = 0xDEADBEEF;

    // 验证应该失败
    EXPECT_FALSE(msg->is_valid(true));

    // 禁用校验和验证时应该通过
    EXPECT_TRUE(msg->is_valid(false));

    delete[] storage;
}

TEST(MessageChecksum, ZeroChecksumWithData) {
    char buffer[] = "Data with zero checksum";
    size_t data_size = strlen(buffer) + 1;

    size_t total_size = sizeof(Message) + data_size;
    auto* storage = new uint8_t[total_size];

    auto* msg = new (storage) Message();
    msg->header.data_size = data_size;
    memcpy(msg->get_data(), buffer, data_size);

    // 校验和为0（未设置）
    msg->header.checksum = 0;

    // 启用校验和验证时应该失败（数据非空但校验和为0）
    EXPECT_FALSE(msg->is_valid(true));

    delete[] storage;
}
