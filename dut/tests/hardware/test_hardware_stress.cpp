#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>

#include "MB_DDF/DDS/DDSCore.h"

using namespace MB_DDF::DDS;

namespace {

void cleanup_dds_shared_state() {
    DDSCore::instance().shutdown();
    shm_unlink("/MB_DDF_V2_SHM");
    sem_unlink("/MB_DDF_V2_SHM_sem");
}

} // namespace

class HardwareStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup_dds_shared_state();
    }

    void TearDown() override {
        cleanup_dds_shared_state();
    }
};

TEST_F(HardwareStressTest, CallbackSubscriberReceivesBurstOnTarget) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(64 * 1024 * 1024));

    std::atomic<int> received{0};
    auto sub = dds.create_subscriber(
        "rt://hardware/stress/callback",
        true,
        [&received](const void* data, size_t size, uint64_t) {
            if (data != nullptr && size > 0) {
                received.fetch_add(1);
            }
        });

    auto pub = dds.create_publisher("rt://hardware/stress/callback");
    ASSERT_NE(sub, nullptr);
    ASSERT_NE(pub, nullptr);

    constexpr int message_count = 1000;
    for (int i = 0; i < message_count; ++i) {
        std::string payload = "burst-" + std::to_string(i);
        ASSERT_GT(dds.data_write(pub, payload.c_str(), payload.size() + 1), 0u);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (received.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(received.load(), 1);
}

TEST_F(HardwareStressTest, ZeroCopyMessagesRemainReadableOnTarget) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(64 * 1024 * 1024));

    auto pub = dds.create_publisher("rt://hardware/stress/zerocopy");
    auto sub = dds.create_subscriber("rt://hardware/stress/zerocopy");

    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    for (int i = 0; i < 128; ++i) {
        auto msg = pub->begin_message(256);
        ASSERT_TRUE(msg.valid());

        std::string payload = "zero-copy-" + std::to_string(i);
        std::memcpy(msg.data(), payload.c_str(), payload.size() + 1);
        ASSERT_TRUE(msg.commit(payload.size() + 1));
    }

    char buffer[256] = {};
    const size_t read = dds.data_read(sub, buffer, sizeof(buffer));
    ASSERT_GT(read, 0u);
    EXPECT_NE(std::string(buffer).find("zero-copy-"), std::string::npos);
}

TEST_F(HardwareStressTest, RepeatedInitializeShutdownDoesNotLeaveSharedStateUnusable) {
    for (int i = 0; i < 10; ++i) {
        cleanup_dds_shared_state();

        auto& dds = DDSCore::instance();
        ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));

        auto pub = dds.create_publisher("rt://hardware/restart/" + std::to_string(i));
        auto sub = dds.create_subscriber("rt://hardware/restart/" + std::to_string(i));
        ASSERT_NE(pub, nullptr);
        ASSERT_NE(sub, nullptr);

        const char payload[] = "restart";
        ASSERT_GT(dds.data_write(pub, payload, sizeof(payload)), 0u);

        char buffer[64] = {};
        ASSERT_EQ(dds.data_read(sub, buffer, sizeof(buffer)), sizeof(payload));
        EXPECT_STREQ(buffer, payload);

        dds.shutdown();
    }
}
