/**
 * @file test_semaphore_guard.cpp
 * @brief SemaphoreGuard单元测试
 */

#include <gtest/gtest.h>
#include <semaphore.h>
#include <thread>
#include <atomic>

#include "MB_DDF/DDS/SemaphoreGuard.h"

using namespace MB_DDF::DDS;

// ==============================
// 基本RAII功能
// ==============================
TEST(SemaphoreGuardTest, BasicAcquireRelease) {
    sem_t sem;
    ASSERT_EQ(sem_init(&sem, 0, 1), 0);  // 初始值为1

    EXPECT_EQ(sem_trywait(&sem), 0);  // 获取信号量
    sem_post(&sem);  // 释放

    {
        SemaphoreGuard guard(&sem);
        EXPECT_TRUE(guard.acquired());
        // 此时信号量值为0
        EXPECT_EQ(sem_trywait(&sem), -1);  // 应该获取失败
    }  // guard析构，自动释放

    // 释放后应该可以获取
    EXPECT_EQ(sem_trywait(&sem), 0);
    sem_post(&sem);

    sem_destroy(&sem);
}

TEST(SemaphoreGuardTest, MultipleGuardsSequential) {
    sem_t sem;
    ASSERT_EQ(sem_init(&sem, 0, 1), 0);

    for (int i = 0; i < 5; ++i) {
        SemaphoreGuard guard(&sem);
        EXPECT_TRUE(guard.acquired());
        // guard析构时自动释放
    }

    sem_destroy(&sem);
}

// ==============================
// 空信号量处理
// ==============================
TEST(SemaphoreGuardTest, NullSemaphore) {
    SemaphoreGuard guard(nullptr);
    EXPECT_FALSE(guard.acquired());
    // 应该安全析构，不崩溃
}

// ==============================
// 移动语义
// ==============================
TEST(SemaphoreGuardTest, MoveConstructor) {
    sem_t sem;
    ASSERT_EQ(sem_init(&sem, 0, 1), 0);

    {
        SemaphoreGuard guard1(&sem);
        EXPECT_TRUE(guard1.acquired());

        SemaphoreGuard guard2(std::move(guard1));
        EXPECT_TRUE(guard2.acquired());
        EXPECT_FALSE(guard1.acquired());  // 原guard不再持有

        // guard2析构时释放
    }

    // 应该可以重新获取
    EXPECT_EQ(sem_trywait(&sem), 0);
    sem_post(&sem);

    sem_destroy(&sem);
}

TEST(SemaphoreGuardTest, MoveAssignment) {
    sem_t sem1, sem2;
    ASSERT_EQ(sem_init(&sem1, 0, 1), 0);
    ASSERT_EQ(sem_init(&sem2, 0, 1), 0);

    {
        SemaphoreGuard guard1(&sem1);
        SemaphoreGuard guard2(&sem2);
        EXPECT_TRUE(guard1.acquired());
        EXPECT_TRUE(guard2.acquired());

        guard2 = std::move(guard1);
        EXPECT_TRUE(guard2.acquired());
        EXPECT_FALSE(guard1.acquired());
        // guard2析构时释放sem1
    }

    // sem1应该被释放
    EXPECT_EQ(sem_trywait(&sem1), 0);
    sem_post(&sem1);

    // sem2也应该被释放（guard2析构前持有）
    EXPECT_EQ(sem_trywait(&sem2), 0);
    sem_post(&sem2);

    sem_destroy(&sem1);
    sem_destroy(&sem2);
}

// ==============================
// 提前释放
// ==============================
TEST(SemaphoreGuardTest, EarlyRelease) {
    sem_t sem;
    ASSERT_EQ(sem_init(&sem, 0, 1), 0);

    {
        SemaphoreGuard guard(&sem);
        EXPECT_TRUE(guard.acquired());

        guard.release();  // 提前释放
        EXPECT_FALSE(guard.acquired());

        // 此时应该可以获取信号量
        EXPECT_EQ(sem_trywait(&sem), 0);
        sem_post(&sem);
    }  // 再次析构，应该安全

    sem_destroy(&sem);
}

TEST(SemaphoreGuardTest, DoubleRelease) {
    sem_t sem;
    ASSERT_EQ(sem_init(&sem, 0, 1), 0);

    {
        SemaphoreGuard guard(&sem);
        guard.release();
        guard.release();  // 再次释放应该安全
        EXPECT_FALSE(guard.acquired());
    }

    sem_destroy(&sem);
}

// ==============================
// 多线程竞争
// ==============================
TEST(SemaphoreGuardTest, MultiThreadAcquire) {
    sem_t sem;
    ASSERT_EQ(sem_init(&sem, 0, 1), 0);  // 二元信号量

    std::atomic<int> acquired_count{0};
    std::atomic<int> failed_count{0};

    auto worker = [&sem, &acquired_count, &failed_count]() {
        SemaphoreGuard guard(&sem);
        if (guard.acquired()) {
            acquired_count++;
            // 模拟一些工作
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            failed_count++;
        }
    };

    std::thread t1(worker);
    std::thread t2(worker);
    std::thread t3(worker);

    t1.join();
    t2.join();
    t3.join();

    // 只有1个能获取成功，另外2个会阻塞直到成功
    EXPECT_EQ(acquired_count.load(), 3);
    EXPECT_EQ(failed_count.load(), 0);

    sem_destroy(&sem);
}

// ==============================
// 信号量值保护
// ==============================
TEST(SemaphoreGuardTest, SemaphoreValueProtection) {
    sem_t sem;
    ASSERT_EQ(sem_init(&sem, 0, 2), 0);  // 初始值为2

    {
        SemaphoreGuard guard1(&sem);
        EXPECT_TRUE(guard1.acquired());

        {
            SemaphoreGuard guard2(&sem);
            EXPECT_TRUE(guard2.acquired());

            // 两个guard都持有，信号量值为0
            EXPECT_EQ(sem_trywait(&sem), -1);
        }  // guard2释放，信号量值为1

        // 应该还能获取一次
        EXPECT_EQ(sem_trywait(&sem), 0);
        sem_post(&sem);
    }  // guard1释放，信号量值为1

    EXPECT_EQ(sem_trywait(&sem), 0);
    sem_post(&sem);

    sem_destroy(&sem);
}

// ==============================
// 异常安全（模拟异常场景）
// ==============================
TEST(SemaphoreGuardTest, ExceptionSafety) {
    sem_t sem;
    ASSERT_EQ(sem_init(&sem, 0, 1), 0);

    try {
        SemaphoreGuard guard(&sem);
        EXPECT_TRUE(guard.acquired());
        throw std::runtime_error("test exception");
    } catch (const std::runtime_error&) {
        // 异常抛出后，guard应该已经析构并释放信号量
    }

    // 信号量应该已被释放
    EXPECT_EQ(sem_trywait(&sem), 0);
    sem_post(&sem);

    sem_destroy(&sem);
}

