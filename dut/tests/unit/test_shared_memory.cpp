/**
 * @file test_shared_memory.cpp
 * @brief SharedMemoryManager单元测试
 *
 * 注意：这些测试在目标板上运行，使用真实的POSIX共享内存
 * 设计上，共享内存不允许销毁（unlink），因为其他进程可能还在使用
 */

#include <gtest/gtest.h>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "MB_DDF/DDS/SharedMemory.h"

using namespace MB_DDF::DDS;

// ==============================
// 基本创建
// ==============================
TEST(SharedMemoryTest, CreateAndGetAddress) {
    const char* shm_name = "/test_shm_001";
    const size_t shm_size = 64 * 1024;

    // 清理可能存在的旧共享内存
    shm_unlink(shm_name);

    SharedMemoryManager shm(shm_name, shm_size);
    EXPECT_NE(shm.get_address(), nullptr);
    EXPECT_EQ(shm.get_size(), shm_size);

    // 注意：析构时不会unlink共享内存，这是设计上的（其他进程可能还在使用）
    // 这里手动清理测试用的共享内存
    shm_unlink(shm_name);
}

TEST(SharedMemoryTest, ZeroInitializesZeroLengthExistingSegment) {
    const char* shm_name = "/test_shm_009";
    const char* sem_name = "/test_shm_009_sem";
    const size_t shm_size = 8 * 1024;

    shm_unlink(shm_name);
    sem_unlink(sem_name);

    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);
    ASSERT_NE(fd, -1);
    close(fd);

    SharedMemoryManager shm(shm_name, shm_size);
    const uint8_t* base = static_cast<const uint8_t*>(shm.get_address());
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(shm.get_size(), shm_size);

    bool all_zero = true;
    size_t first_nonzero = 0;
    for (size_t i = 0; i < shm_size; ++i) {
        if (base[i] != 0) {
            all_zero = false;
            first_nonzero = i;
            break;
        }
    }
    EXPECT_TRUE(all_zero) << "first_nonzero_byte=" << first_nonzero;

    shm_unlink(shm_name);
    sem_unlink(sem_name);
}

TEST(SharedMemoryTest, OpenExisting) {
    const char* shm_name = "/test_shm_002";
    const size_t shm_size = 32 * 1024;

    shm_unlink(shm_name);

    // 第一个进程创建
    void* addr1 = nullptr;
    {
        SharedMemoryManager creator(shm_name, shm_size);
        addr1 = creator.get_address();
        ASSERT_NE(addr1, nullptr);

        // 写入数据
        strcpy(static_cast<char*>(addr1), "Hello from creator");
    }

    // 第二个进程打开（同一个构造函数会打开已存在的）
    {
        SharedMemoryManager opener(shm_name, shm_size);
        EXPECT_EQ(opener.get_size(), shm_size);

        // 读取数据
        void* addr2 = opener.get_address();
        EXPECT_STREQ(static_cast<char*>(addr2), "Hello from creator");
    }

    shm_unlink(shm_name);
}

// ==============================
// 数据读写
// ==============================
TEST(SharedMemoryTest, WriteAndRead) {
    const char* shm_name = "/test_shm_003";
    const size_t shm_size = 16 * 1024;

    shm_unlink(shm_name);

    SharedMemoryManager shm(shm_name, shm_size);
    void* base = shm.get_address();
    ASSERT_NE(base, nullptr);

    // 写入测试数据
    const char* test_data = "Shared memory test data";
    memcpy(base, test_data, strlen(test_data) + 1);

    // 读取验证
    EXPECT_STREQ(static_cast<char*>(base), test_data);

    shm_unlink(shm_name);
}

TEST(SharedMemoryTest, LargeDataTransfer) {
    const char* shm_name = "/test_shm_004";
    const size_t shm_size = 1024 * 1024; // 1MB

    shm_unlink(shm_name);

    SharedMemoryManager shm(shm_name, shm_size);
    uint8_t* base = static_cast<uint8_t*>(shm.get_address());
    ASSERT_NE(base, nullptr);

    // 写入大数据
    for (size_t i = 0; i < shm_size; ++i) {
        base[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // 验证
    for (size_t i = 0; i < shm_size; ++i) {
        EXPECT_EQ(base[i], static_cast<uint8_t>(i & 0xFF));
    }

    shm_unlink(shm_name);
}

// ==============================
// 信号量获取
// ==============================
TEST(SharedMemoryTest, GetSemaphore) {
    const char* shm_name = "/test_shm_005";
    const size_t shm_size = 64 * 1024;

    shm_unlink(shm_name);

    SharedMemoryManager shm(shm_name, shm_size);
    sem_t* sem = shm.get_semaphore();
    EXPECT_NE(sem, nullptr);

    shm_unlink(shm_name);
}

TEST(SharedMemoryTest, OpeningExistingDoesNotResetOwnedSemaphore) {
    const char* shm_name = "/test_shm_owned_semaphore";
    const char* sem_name = "/test_shm_owned_semaphore_sem";
    const size_t shm_size = 64 * 1024;

    shm_unlink(shm_name);
    sem_unlink(sem_name);

    {
        SharedMemoryManager owner(shm_name, shm_size);
        ASSERT_NE(owner.get_address(), nullptr);
        ASSERT_NE(owner.get_semaphore(), nullptr);
        ASSERT_NE(owner.get_semaphore(), SEM_FAILED);
        ASSERT_EQ(sem_wait(owner.get_semaphore()), 0);

        SharedMemoryManager observer(shm_name, shm_size);
        ASSERT_NE(observer.get_address(), nullptr);
        ASSERT_NE(observer.get_semaphore(), nullptr);
        ASSERT_NE(observer.get_semaphore(), SEM_FAILED);

        int value = -1;
        ASSERT_EQ(sem_getvalue(owner.get_semaphore(), &value), 0);
        EXPECT_EQ(value, 0);
        EXPECT_EQ(sem_post(owner.get_semaphore()), 0);
    }

    shm_unlink(shm_name);
    sem_unlink(sem_name);
}

// ==============================
// 多次创建
// ==============================
TEST(SharedMemoryTest, RecreateAfterUnlink) {
    const char* shm_name = "/test_shm_006";

    shm_unlink(shm_name);

    // 第一次创建
    {
        SharedMemoryManager shm1(shm_name, 64 * 1024);
        EXPECT_NE(shm1.get_address(), nullptr);
    }

    shm_unlink(shm_name);

    // 重新创建
    {
        SharedMemoryManager shm2(shm_name, 64 * 1024);
        EXPECT_NE(shm2.get_address(), nullptr);
    }

    shm_unlink(shm_name);
}

// ==============================
// 对齐要求
// ==============================
TEST(SharedMemoryTest, Alignment) {
    const char* shm_name = "/test_shm_007";

    shm_unlink(shm_name);

    SharedMemoryManager shm(shm_name, 64 * 1024);
    void* base = shm.get_address();
    ASSERT_NE(base, nullptr);

    // 验证至少4字节对齐（通常页面对齐）
    EXPECT_EQ(reinterpret_cast<uintptr_t>(base) % 4, 0);

    shm_unlink(shm_name);
}

// ==============================
// 跨进程数据共享（单进程模拟）
// ==============================
TEST(SharedMemoryTest, CrossProcessSharing) {
    const char* shm_name = "/test_shm_008";
    const size_t shm_size = 64 * 1024;

    shm_unlink(shm_name);

    // 模拟进程1：创建并写入
    {
        SharedMemoryManager shm(shm_name, shm_size);
        void* base = shm.get_address();
        ASSERT_NE(base, nullptr);

        // 写入结构化数据
        struct TestData {
            int counter;
            char message[32];
        };

        TestData* data = static_cast<TestData*>(base);
        data->counter = 42;
        strcpy(data->message, "Hello from process 1");
    }

    // 模拟进程2：打开并读取
    {
        SharedMemoryManager shm(shm_name, shm_size);
        void* base = shm.get_address();
        ASSERT_NE(base, nullptr);

        struct TestData {
            int counter;
            char message[32];
        };

        TestData* data = static_cast<TestData*>(base);
        EXPECT_EQ(data->counter, 42);
        EXPECT_STREQ(data->message, "Hello from process 1");
    }

    shm_unlink(shm_name);
}

