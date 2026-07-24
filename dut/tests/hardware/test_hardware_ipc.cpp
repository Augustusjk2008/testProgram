#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "MB_DDF/DDS/DDSCore.h"

using namespace MB_DDF::DDS;

namespace {

void cleanup_dds_shared_state() {
    DDSCore::instance().shutdown();
    shm_unlink("/MB_DDF_V2_SHM");
    sem_unlink("/MB_DDF_V2_SHM_sem");
}

bool wait_for_child(pid_t pid, int* status, int timeout_seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t result = waitpid(pid, status, WNOHANG);
        if (result == pid) {
            return true;
        }
        if (result == -1) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return false;
}

} // namespace

class HardwareIpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup_dds_shared_state();
    }

    void TearDown() override {
        cleanup_dds_shared_state();
    }
};

TEST_F(HardwareIpcTest, ChildProcessReadsMessagePublishedByParent) {
    auto& parent_dds = DDSCore::instance();
    ASSERT_TRUE(parent_dds.initialize(32 * 1024 * 1024));

    const std::string topic = "rt://hardware/ipc/parent_to_child";
    auto parent_pub = parent_dds.create_publisher(topic);
    ASSERT_NE(parent_pub, nullptr);

    pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        auto& child_dds = DDSCore::instance();
        auto child_sub = child_dds.create_subscriber(topic);
        if (!child_sub) {
            _exit(10);
        }

        char buffer[128] = {};
        for (int i = 0; i < 100; ++i) {
            size_t read = child_dds.data_read(child_sub, buffer, sizeof(buffer));
            if (read > 0 && std::strcmp(buffer, "parent-to-child") == 0) {
                child_dds.shutdown();
                _exit(0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        child_dds.shutdown();
        _exit(11);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const char payload[] = "parent-to-child";
    ASSERT_GT(parent_dds.data_write(parent_pub, payload, sizeof(payload)), 0u);

    int status = 0;
    ASSERT_TRUE(wait_for_child(child, &status, 5)) << "child process timed out";
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(HardwareIpcTest, ParentProcessReadsMessagePublishedByChild) {
    auto& parent_dds = DDSCore::instance();
    ASSERT_TRUE(parent_dds.initialize(32 * 1024 * 1024));

    const std::string topic = "rt://hardware/ipc/child_to_parent";
    auto parent_sub = parent_dds.create_subscriber(topic);
    ASSERT_NE(parent_sub, nullptr);

    pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        auto& child_dds = DDSCore::instance();
        auto child_pub = child_dds.create_publisher(topic);
        if (!child_pub) {
            _exit(20);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const char payload[] = "child-to-parent";
        if (child_dds.data_write(child_pub, payload, sizeof(payload)) == 0) {
            child_dds.shutdown();
            _exit(21);
        }

        child_dds.shutdown();
        _exit(0);
    }

    char buffer[128] = {};
    bool received = false;
    for (int i = 0; i < 100; ++i) {
        size_t read = parent_dds.data_read(parent_sub, buffer, sizeof(buffer));
        if (read > 0 && std::strcmp(buffer, "child-to-parent") == 0) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    int status = 0;
    ASSERT_TRUE(wait_for_child(child, &status, 5)) << "child process timed out";
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_TRUE(received);
}
