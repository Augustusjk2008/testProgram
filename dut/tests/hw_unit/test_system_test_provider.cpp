#include "MB_DDF_HW_Test/SystemTestProvider.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>

using MB_DDF::HWTest::ProductErrorCode;
using MB_DDF::HWTest::ProductProtocol;
using MB_DDF::HWTest::SystemTestProvider;
using MB_DDF::HWTest::ITimerLoadExecutor;

namespace {

class RecordingTimerLoad final : public ITimerLoadExecutor {
public:
    ProductErrorCode start() override {
        ++start_calls;
        if (active) {
            return ProductErrorCode::TaskBusy;
        }
        if (start_result == ProductErrorCode::Ok) {
            active = true;
        }
        return start_result;
    }

    ProductErrorCode stop() override {
        ++stop_calls;
        if (!active) {
            return ProductErrorCode::Ok;
        }
        active = false;
        return stop_result;
    }

    ProductErrorCode start_result{ProductErrorCode::Ok};
    ProductErrorCode stop_result{ProductErrorCode::Ok};
    size_t start_calls{0};
    size_t stop_calls{0};
    bool active{false};
};

class FixedK7Temperature final : public MB_DDF::HWTest::IK7TemperatureSource {
public:
    bool read_k7_temperature(float& celsius) override {
        celsius = 25.0F;
        return true;
    }
};

uint64_t timer_sample_count(const MB_DDF::HWTest::ProductMessage& response) {
    uint64_t total = 0;
    for (size_t index = 0; index < 8; ++index) {
        total += response.get_unsigned("buckets[" + std::to_string(index) + "]")
                     .value_or(0);
    }
    return total;
}

} // namespace

TEST(SystemTestProviderTest, UsesReferenceTimerJitterBucketBoundaries) {
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(0.0), 0u);
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(1.999), 0u);
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(2.0), 1u);
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(4.0), 2u);
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(8.0), 3u);
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(16.0), 4u);
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(32.0), 5u);
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(64.0), 6u);
    EXPECT_EQ(SystemTestProvider::timer_bucket_for_jitter(100.0), 7u);
}

TEST(SystemTestProviderTest, CpuTimesExcludeGuestFieldsAlreadyIncludedInUserTimes) {
    SystemTestProvider::CpuTimes times{};

    ASSERT_TRUE(SystemTestProvider::parse_cpu_times(
        "cpu  100 20 30 400 5 6 7 8 10 3", times));

    EXPECT_EQ(times.total, 576u);
    EXPECT_EQ(times.idle, 405u);
}

TEST(SystemTestProviderTest, CpuUsageUsesParsedTotalAndIdleDeltas) {
    const SystemTestProvider::CpuTimes before{576u, 405u};
    const SystemTestProvider::CpuTimes after{676u, 445u};
    float usage = 0.0F;

    ASSERT_TRUE(SystemTestProvider::calculate_cpu_usage(before, after, usage));
    EXPECT_FLOAT_EQ(usage, 60.0F);
}

TEST(SystemTestProviderTest, ExtractsLeafPciBdfOnlyFromResolvedXdmaFactPath) {
    const auto bdf = SystemTestProvider::pci_bdf_from_sysfs_path(
        "/sys/devices/platform/fe150000.pcie/pci0004:40/"
        "0004:40:00.0/0004:41:00.0");

    ASSERT_TRUE(bdf.has_value());
    EXPECT_EQ(*bdf, "0004:41:00.0");
    EXPECT_FALSE(SystemTestProvider::pci_bdf_from_sysfs_path(
                     "/sys/devices/platform/xdma0/device")
                     .has_value());
    EXPECT_FALSE(SystemTestProvider::pci_bdf_from_sysfs_path(
                     "/sys/devices/pci0000:00/0000:00:20.0")
                     .has_value());
    EXPECT_FALSE(SystemTestProvider::pci_bdf_from_sysfs_path(
                     "/sys/devices/pci0000:00/0000:00:1f.8")
                     .has_value());
}

TEST(SystemTestProviderTest, RejectsUnsupportedMemoryTypeBeforeAllocating) {
    ProductProtocol protocol;
    auto request = protocol.create_message("memperf_test_request", false);
    auto response = protocol.create_message("memperf_test_response", false);
    ASSERT_TRUE(request.set_unsigned("memperf_type", 7));

    SystemTestProvider provider;
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::ParamOutOfRange);
}

TEST(SystemTestProviderTest, ExecutesOneKiBMemoryPerformanceTypesThreeThroughSix) {
    ProductProtocol protocol;
    SystemTestProvider provider;

    for (const uint8_t type : std::array<uint8_t, 4>{3, 4, 5, 6}) {
        SCOPED_TRACE(static_cast<unsigned>(type));
        auto request = protocol.create_message("memperf_test_request", false);
        auto response = protocol.create_message("memperf_test_response", false);
        ASSERT_TRUE(request.set_unsigned("memperf_type", type));
        ASSERT_TRUE(request.set_unsigned("length", 1));
        ASSERT_TRUE(request.set_unsigned("seed", 0x5A5AA5A5u));

        ASSERT_EQ(provider.handle(request, response), ProductErrorCode::Ok);
        EXPECT_EQ(response.get_unsigned("memperf_type"), type);
        EXPECT_EQ(response.get_unsigned("error_count"), 0u);

        const auto write_bandwidth = response.get_float("write_bandwidth");
        const auto read_bandwidth = response.get_float("read_bandwidth");
        ASSERT_TRUE(write_bandwidth.has_value());
        ASSERT_TRUE(read_bandwidth.has_value());
        if (type == 3) {
            EXPECT_EQ(*write_bandwidth, 0.0F);
            EXPECT_GT(*read_bandwidth, 0.0F);
        } else if (type == 4 || type == 6) {
            EXPECT_GT(*write_bandwidth, 0.0F);
            EXPECT_EQ(*read_bandwidth, 0.0F);
        } else {
            EXPECT_GT(*write_bandwidth, 0.0F);
            EXPECT_GT(*read_bandwidth, 0.0F);
        }
    }
}

TEST(SystemTestProviderTest, TimerModeZeroReturnsCompleteStatistics) {
    ProductProtocol protocol;
    auto request = protocol.create_message("timer_jitter_start_request", false);
    auto response = protocol.create_message("timer_jitter_start_response", false);
    ASSERT_TRUE(request.set_unsigned("mode", 0));

    SystemTestProvider provider;
    ASSERT_EQ(provider.handle(request, response), ProductErrorCode::Ok);
    EXPECT_EQ(timer_sample_count(response), 250u);
    EXPECT_TRUE(response.get_float("avg_jitter").has_value());
    EXPECT_TRUE(response.get_float("max_jitter").has_value());
}

TEST(SystemTestProviderTest, TimerModeOneRunsInjectedLoadAndReturnsStatistics) {
    ProductProtocol protocol;
    auto request = protocol.create_message("timer_jitter_start_request", false);
    auto response = protocol.create_message("timer_jitter_start_response", false);
    ASSERT_TRUE(request.set_unsigned("mode", 1));
    RecordingTimerLoad load;

    SystemTestProvider provider(&load);
    ASSERT_EQ(provider.handle(request, response), ProductErrorCode::Ok);
    EXPECT_EQ(load.start_calls, 1u);
    EXPECT_EQ(load.stop_calls, 1u);
    EXPECT_FALSE(load.active);
    EXPECT_EQ(timer_sample_count(response), 250u);
}

TEST(SystemTestProviderTest, TimerModeOneRequiresLoadExecutor) {
    ProductProtocol protocol;
    auto request = protocol.create_message("timer_jitter_start_request", false);
    auto response = protocol.create_message("timer_jitter_start_response", false);
    ASSERT_TRUE(request.set_unsigned("mode", 1));

    SystemTestProvider provider;
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::TaskExecFailed);
}

TEST(SystemTestProviderTest, TimerModeOnePropagatesBusyWithoutStoppingExistingLoad) {
    ProductProtocol protocol;
    auto request = protocol.create_message("timer_jitter_start_request", false);
    auto response = protocol.create_message("timer_jitter_start_response", false);
    ASSERT_TRUE(request.set_unsigned("mode", 1));
    RecordingTimerLoad load;
    load.active = true;

    SystemTestProvider provider(&load);
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::TaskBusy);
    EXPECT_EQ(load.start_calls, 1u);
    EXPECT_EQ(load.stop_calls, 0u);
    EXPECT_TRUE(load.active);
}

TEST(SystemTestProviderTest, TimerModeOnePropagatesLoadFailureAfterCleanup) {
    ProductProtocol protocol;
    auto request = protocol.create_message("timer_jitter_start_request", false);
    auto response = protocol.create_message("timer_jitter_start_response", false);
    ASSERT_TRUE(request.set_unsigned("mode", 1));
    RecordingTimerLoad load;
    load.stop_result = ProductErrorCode::TaskExecFailed;

    SystemTestProvider provider(&load);
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::TaskExecFailed);
    EXPECT_EQ(load.start_calls, 1u);
    EXPECT_EQ(load.stop_calls, 1u);
    EXPECT_FALSE(load.active);
}

TEST(SystemTestProviderTest, TimerStopIsIdempotentCleanupAck) {
    ProductProtocol protocol;
    auto request = protocol.create_message("timer_jitter_stop_request", false);
    auto response = protocol.create_message("timer_jitter_stop_response", false);
    RecordingTimerLoad load;
    ASSERT_EQ(load.start(), ProductErrorCode::Ok);

    SystemTestProvider provider(&load);
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::Ok);
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::Ok);
    EXPECT_EQ(load.stop_calls, 2u);
    EXPECT_FALSE(load.active);
}

TEST(SystemTestProviderTest, RejectsUnsupportedTimerMode) {
    ProductProtocol protocol;
    auto request = protocol.create_message("timer_jitter_start_request", false);
    auto response = protocol.create_message("timer_jitter_start_response", false);
    ASSERT_TRUE(request.set_unsigned("mode", 2));

    SystemTestProvider provider;
    EXPECT_EQ(provider.handle(request, response), ProductErrorCode::ParamOutOfRange);
}

TEST(SystemTestProviderTest, ReadsTargetScmiCpuAndXdmaPcieSources) {
    ASSERT_TRUE(std::filesystem::exists(
        "/sys/kernel/debug/clk/scmi_clk_cpul/clk_rate"));
    ASSERT_TRUE(std::filesystem::exists(
        "/sys/kernel/debug/clk/scmi_clk_cpub01/clk_rate"));
    ASSERT_TRUE(std::filesystem::exists(
        "/sys/class/xdma/xdma0_user/device"));

    ProductProtocol protocol;
    auto request = protocol.create_message("system_status_request", false);
    auto response = protocol.create_message("system_status_response", false);
    FixedK7Temperature k7_temperature;
    SystemTestProvider provider(nullptr, &k7_temperature);

    ASSERT_EQ(provider.handle(request, response), ProductErrorCode::Ok);
    EXPECT_GT(response.get_float("cpu_freq_little").value_or(0.0F), 0.0F);
    EXPECT_GT(response.get_float("cpu_freq_big").value_or(0.0F), 0.0F);
    EXPECT_GT(response.get_float("pcie_speed").value_or(0.0F), 0.0F);
    EXPECT_GT(response.get_float("pcie_width").value_or(0.0F), 0.0F);
}
