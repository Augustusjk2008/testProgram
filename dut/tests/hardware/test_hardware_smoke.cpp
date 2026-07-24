#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/SharedMemory.h"

using namespace MB_DDF::DDS;

namespace {

void cleanup_dds_shared_state() {
    DDSCore::instance().shutdown();
    shm_unlink("/MB_DDF_V2_SHM");
    sem_unlink("/MB_DDF_V2_SHM_sem");
}

} // namespace

class HardwareSmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup_dds_shared_state();
    }

    void TearDown() override {
        cleanup_dds_shared_state();
    }
};

TEST_F(HardwareSmokeTest, TargetHasRequiredPosixRuntimeFacilities) {
    EXPECT_EQ(access("/proc/self/comm", R_OK), 0);

    struct statvfs shm_stats {};
    ASSERT_EQ(statvfs("/dev/shm", &shm_stats), 0) << "target must mount /dev/shm for POSIX shared memory";

    SharedMemoryManager shm("/mb_ddf_hw_smoke_shm", 1024 * 1024);
    ASSERT_NE(shm.get_address(), nullptr);
    ASSERT_NE(shm.get_semaphore(), nullptr);

    shm_unlink("/mb_ddf_hw_smoke_shm");
    sem_unlink("/mb_ddf_hw_smoke_shm_sem");
}

TEST_F(HardwareSmokeTest, DDSCoreInitializesAndRestartsOnTarget) {
    auto& dds = DDSCore::instance();

    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));
    dds.shutdown();

    ASSERT_TRUE(dds.initialize(16 * 1024 * 1024));
}

TEST_F(HardwareSmokeTest, SameProcessPubSubRoundTripOnTarget) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(32 * 1024 * 1024));

    auto pub = dds.create_publisher("rt://hardware/smoke");
    auto sub = dds.create_subscriber("rt://hardware/smoke");

    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    const char payload[] = "hardware-smoke";
    EXPECT_GT(dds.data_write(pub, payload, sizeof(payload)), 0u);

    char buffer[128] = {};
    const size_t read = dds.data_read(sub, buffer, sizeof(buffer));

    ASSERT_EQ(read, sizeof(payload));
    EXPECT_STREQ(buffer, payload);
}
