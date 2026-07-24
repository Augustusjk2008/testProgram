#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
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

uint64_t steady_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

double elapsed_seconds(std::chrono::steady_clock::time_point start,
                       std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double>(end - start).count();
}

void record_metric(const std::string& name, double value) {
    testing::Test::RecordProperty(name, std::to_string(value));
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

struct PayloadCase {
    size_t payload_size;
    int message_count;
};

struct LatencyPayload {
    uint64_t sequence;
    uint64_t sent_ns;
    uint8_t padding[240];
};

struct Latency64Payload {
    uint64_t sequence;
    uint64_t sent_ns;
    uint8_t padding[48];
};
static_assert(sizeof(Latency64Payload) == 64, "Latency64Payload must stay 64 bytes");

bool set_current_thread_realtime_affinity(int cpu_id, int priority, std::string& error) {
    const long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count <= 0 || cpu_id < 0 || cpu_id >= cpu_count) {
        error = "invalid cpu_id=" + std::to_string(cpu_id) +
                ", online_cpus=" + std::to_string(cpu_count);
        return false;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    const int affinity_result = pthread_setaffinity_np(
        pthread_self(),
        sizeof(cpu_set_t),
        &cpuset);
    if (affinity_result != 0) {
        error = "pthread_setaffinity_np(cpu=" + std::to_string(cpu_id) +
                ") failed: " + std::strerror(affinity_result);
        return false;
    }

    sched_param param {};
    param.sched_priority = priority;
    const int sched_result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (sched_result != 0) {
        error = "pthread_setschedparam(SCHED_FIFO, priority=" +
                std::to_string(priority) + ") failed: " + std::strerror(sched_result);
        return false;
    }

    int applied_policy = 0;
    sched_param applied_param {};
    const int get_result = pthread_getschedparam(pthread_self(), &applied_policy, &applied_param);
    if (get_result != 0) {
        error = "pthread_getschedparam failed: " + std::string(std::strerror(get_result));
        return false;
    }
    if (applied_policy != SCHED_FIFO || applied_param.sched_priority != priority) {
        error = "unexpected scheduler policy/priority: policy=" +
                std::to_string(applied_policy) + ", priority=" +
                std::to_string(applied_param.sched_priority);
        return false;
    }

    return true;
}

int read_int_env_or_default(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return default_value;
    }

    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return default_value;
    }

    return static_cast<int>(parsed);
}

void cpu_relax() {
#if defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

} // namespace

class HardwarePerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup_dds_shared_state();
    }

    void TearDown() override {
        cleanup_dds_shared_state();
    }
};

TEST_F(HardwarePerformanceTest, SameProcessRoundTripThroughputOnTarget) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(128 * 1024 * 1024));

    const std::vector<PayloadCase> cases = {
        {64, 5000},
        {1024, 3000},
        {16 * 1024, 512},
    };

    for (const auto& test_case : cases) {
        const std::string suffix = std::to_string(test_case.payload_size) + "B";
        const std::string topic = "rt://hardware/perf/roundtrip/" + suffix;
        auto pub = dds.create_publisher(topic);
        auto sub = dds.create_subscriber(topic);
        ASSERT_NE(pub, nullptr);
        ASSERT_NE(sub, nullptr);

        std::vector<uint8_t> payload(test_case.payload_size, 0x5A);
        std::vector<uint8_t> buffer(test_case.payload_size, 0);

        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < test_case.message_count; ++i) {
            payload[0] = static_cast<uint8_t>(i & 0xFF);
            ASSERT_GT(dds.data_write(pub, payload.data(), payload.size()), 0u);

            const size_t read = sub->read(buffer.data(), buffer.size(), false);
            ASSERT_EQ(read, payload.size());
            ASSERT_EQ(buffer[0], payload[0]);
        }
        const auto end = std::chrono::steady_clock::now();

        const double seconds = elapsed_seconds(start, end);
        ASSERT_GT(seconds, 0.0);

        const double messages_per_second = static_cast<double>(test_case.message_count) / seconds;
        const double mib_per_second =
            (static_cast<double>(test_case.message_count) * static_cast<double>(test_case.payload_size)) /
            (1024.0 * 1024.0 * seconds);

        record_metric("RoundTripMessagesPerSecond_" + suffix, messages_per_second);
        record_metric("RoundTripMiBPerSecond_" + suffix, mib_per_second);

        std::cout << "[ PERF ] roundtrip payload=" << test_case.payload_size
                  << " bytes messages=" << test_case.message_count
                  << " messages_per_second=" << messages_per_second
                  << " mib_per_second=" << mib_per_second << std::endl;
    }
}

TEST_F(HardwarePerformanceTest, ZeroCopyRoundTripThroughputOnTarget) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(128 * 1024 * 1024));

    auto pub = dds.create_publisher("rt://hardware/perf/zerocopy");
    auto sub = dds.create_subscriber("rt://hardware/perf/zerocopy");
    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    constexpr size_t payload_size = 4096;
    constexpr int message_count = 3000;
    std::vector<uint8_t> buffer(payload_size, 0);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < message_count; ++i) {
        auto msg = pub->begin_message(payload_size);
        ASSERT_TRUE(msg.valid());

        auto* data = static_cast<uint8_t*>(msg.data());
        ASSERT_NE(data, nullptr);
        std::memset(data, static_cast<int>(i & 0xFF), payload_size);
        ASSERT_TRUE(msg.commit(payload_size));

        const size_t read = sub->read(buffer.data(), buffer.size(), false);
        ASSERT_EQ(read, payload_size);
        ASSERT_EQ(buffer[0], static_cast<uint8_t>(i & 0xFF));
    }
    const auto end = std::chrono::steady_clock::now();

    const double seconds = elapsed_seconds(start, end);
    ASSERT_GT(seconds, 0.0);

    const double messages_per_second = static_cast<double>(message_count) / seconds;
    const double mib_per_second =
        (static_cast<double>(message_count) * static_cast<double>(payload_size)) /
        (1024.0 * 1024.0 * seconds);

    record_metric("ZeroCopyRoundTripMessagesPerSecond", messages_per_second);
    record_metric("ZeroCopyRoundTripMiBPerSecond", mib_per_second);

    std::cout << "[ PERF ] zerocopy payload=" << payload_size
              << " bytes messages=" << message_count
              << " messages_per_second=" << messages_per_second
              << " mib_per_second=" << mib_per_second << std::endl;
}

TEST_F(HardwarePerformanceTest, WaitForMessageLatencyOnTarget) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(64 * 1024 * 1024));

    auto pub = dds.create_publisher("rt://hardware/perf/latency");
    auto sub = dds.create_subscriber("rt://hardware/perf/latency");
    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    constexpr int message_count = 256;
    std::atomic<bool> consumer_ready{false};
    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(message_count);

    std::thread consumer([&]() {
        consumer_ready.store(true);
        for (int i = 0; i < message_count; ++i) {
            LatencyPayload payload {};
            const int32_t read = sub->read(&payload, sizeof(payload), static_cast<uint32_t>(500000));
            if (read != static_cast<int32_t>(sizeof(payload))) {
                break;
            }
            latencies_ns.push_back(steady_now_ns() - payload.sent_ns);
        }
    });

    while (!consumer_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool publish_ok = true;
    for (int i = 0; i < message_count; ++i) {
        LatencyPayload payload {};
        payload.sequence = static_cast<uint64_t>(i);
        payload.sent_ns = steady_now_ns();

        if (dds.data_write(pub, &payload, sizeof(payload)) == 0) {
            publish_ok = false;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    consumer.join();

    ASSERT_TRUE(publish_ok);
    ASSERT_EQ(latencies_ns.size(), static_cast<size_t>(message_count));

    std::sort(latencies_ns.begin(), latencies_ns.end());
    uint64_t sum_ns = 0;
    for (const auto value : latencies_ns) {
        sum_ns += value;
    }

    const double average_us = static_cast<double>(sum_ns) / static_cast<double>(latencies_ns.size()) / 1000.0;
    const double p95_us = static_cast<double>(latencies_ns[(latencies_ns.size() * 95) / 100]) / 1000.0;
    const double max_us = static_cast<double>(latencies_ns.back()) / 1000.0;

    record_metric("WaitLatencyAverageUs", average_us);
    record_metric("WaitLatencyP95Us", p95_us);
    record_metric("WaitLatencyMaxUs", max_us);

    std::cout << "[ PERF ] wait_latency messages=" << message_count
              << " average_us=" << average_us
              << " p95_us=" << p95_us
              << " max_us=" << max_us << std::endl;
}

TEST_F(HardwarePerformanceTest, RealtimePinnedOneToOneWaitLatency64BOnTarget) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(64 * 1024 * 1024));

    auto pub = dds.create_publisher("rt://hardware/perf/latency/rt_pinned_64b");
    auto sub = dds.create_subscriber("rt://hardware/perf/latency/rt_pinned_64b");
    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    const int producer_cpu = read_int_env_or_default(
        "MB_DDF_RT_LATENCY_PRODUCER_CPU",
        1);
    const int consumer_cpu = read_int_env_or_default(
        "MB_DDF_RT_LATENCY_CONSUMER_CPU",
        2);
    constexpr int message_count = 1000;
    constexpr uint32_t read_timeout_us = 500000;

    const int realtime_priority = sched_get_priority_max(SCHED_FIFO);
    ASSERT_GT(realtime_priority, 0);
    ASSERT_GT(sysconf(_SC_NPROCESSORS_ONLN), std::max(producer_cpu, consumer_cpu));

    std::atomic<bool> start{false};
    std::atomic<bool> producer_ready{false};
    std::atomic<bool> consumer_ready{false};
    std::atomic<bool> producer_setup_ok{false};
    std::atomic<bool> consumer_setup_ok{false};
    std::atomic<int> waiting_sequence{-1};
    std::atomic<int> received_count{0};
    std::atomic<int> publish_failures{0};
    std::atomic<int> read_failures{0};
    std::atomic<int> sequence_failures{0};
    std::atomic<int> ack_timeouts{0};

    std::string producer_error;
    std::string consumer_error;
    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(message_count);

    std::thread consumer([&]() {
        const bool setup_ok = set_current_thread_realtime_affinity(
            consumer_cpu,
            realtime_priority,
            consumer_error);
        consumer_setup_ok.store(setup_ok, std::memory_order_release);
        consumer_ready.store(true, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (!setup_ok || !producer_setup_ok.load(std::memory_order_acquire)) {
            return;
        }

        for (int i = 0; i < message_count; ++i) {
            waiting_sequence.store(i, std::memory_order_release);

            Latency64Payload payload {};
            const int32_t read = sub->read(
                &payload,
                sizeof(payload),
                read_timeout_us);
            if (read != static_cast<int32_t>(sizeof(payload))) {
                read_failures.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
            if (payload.sequence != static_cast<uint64_t>(i) || payload.sent_ns == 0) {
                sequence_failures.fetch_add(1, std::memory_order_acq_rel);
                return;
            }

            latencies_ns.push_back(steady_now_ns() - payload.sent_ns);
            received_count.store(i + 1, std::memory_order_release);
        }
    });

    std::thread producer([&]() {
        const bool setup_ok = set_current_thread_realtime_affinity(
            producer_cpu,
            realtime_priority,
            producer_error);
        producer_setup_ok.store(setup_ok, std::memory_order_release);
        producer_ready.store(true, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (!setup_ok || !consumer_setup_ok.load(std::memory_order_acquire)) {
            return;
        }

        for (int i = 0; i < message_count; ++i) {
            const auto wait_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(1);
            while (waiting_sequence.load(std::memory_order_acquire) != i) {
                if (std::chrono::steady_clock::now() >= wait_deadline) {
                    ack_timeouts.fetch_add(1, std::memory_order_acq_rel);
                    return;
                }
                std::this_thread::yield();
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));

            Latency64Payload payload {};
            payload.sequence = static_cast<uint64_t>(i);
            payload.sent_ns = steady_now_ns();
            std::memset(payload.padding, 0xA5, sizeof(payload.padding));
            if (dds.data_write(pub, &payload, sizeof(payload)) == 0) {
                publish_failures.fetch_add(1, std::memory_order_acq_rel);
                return;
            }

            const auto ack_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(1);
            while (received_count.load(std::memory_order_acquire) < i + 1) {
                if (std::chrono::steady_clock::now() >= ack_deadline) {
                    ack_timeouts.fetch_add(1, std::memory_order_acq_rel);
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    while (!producer_ready.load(std::memory_order_acquire) ||
           !consumer_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    ASSERT_TRUE(producer_setup_ok.load(std::memory_order_acquire)) << producer_error;
    ASSERT_TRUE(consumer_setup_ok.load(std::memory_order_acquire)) << consumer_error;
    ASSERT_EQ(publish_failures.load(std::memory_order_acquire), 0);
    ASSERT_EQ(read_failures.load(std::memory_order_acquire), 0);
    ASSERT_EQ(sequence_failures.load(std::memory_order_acquire), 0);
    ASSERT_EQ(ack_timeouts.load(std::memory_order_acquire), 0);
    ASSERT_EQ(latencies_ns.size(), static_cast<size_t>(message_count));

    std::sort(latencies_ns.begin(), latencies_ns.end());
    uint64_t sum_ns = 0;
    for (const auto value : latencies_ns) {
        sum_ns += value;
    }

    const auto percentile = [&](size_t percent) {
        const size_t index = std::min(
            latencies_ns.size() - 1,
            (latencies_ns.size() * percent) / 100);
        return static_cast<double>(latencies_ns[index]) / 1000.0;
    };

    const double average_us =
        static_cast<double>(sum_ns) / static_cast<double>(latencies_ns.size()) / 1000.0;
    const double p95_us = percentile(95);
    const double p99_us = percentile(99);
    const double max_us = static_cast<double>(latencies_ns.back()) / 1000.0;

    record_metric("RealtimePinned64BWaitLatencyAverageUs", average_us);
    record_metric("RealtimePinned64BWaitLatencyP95Us", p95_us);
    record_metric("RealtimePinned64BWaitLatencyP99Us", p99_us);
    record_metric("RealtimePinned64BWaitLatencyMaxUs", max_us);

    std::cout << "[ PERF ] wait_latency_rt_pinned payload=64 bytes messages="
              << message_count
              << " producer_cpu=" << producer_cpu
              << " consumer_cpu=" << consumer_cpu
              << " policy=SCHED_FIFO priority=" << realtime_priority
              << " average_us=" << average_us
              << " p95_us=" << p95_us
              << " p99_us=" << p99_us
              << " max_us=" << max_us << std::endl;
}

TEST_F(HardwarePerformanceTest, RealtimePinnedOneToOnePollLatency64BOnTarget) {
    auto& dds = DDSCore::instance();
    ASSERT_TRUE(dds.initialize(64 * 1024 * 1024));

    auto pub = dds.create_publisher("rt://hardware/perf/latency/rt_pinned_poll_64b");
    auto sub = dds.create_subscriber("rt://hardware/perf/latency/rt_pinned_poll_64b");
    ASSERT_NE(pub, nullptr);
    ASSERT_NE(sub, nullptr);

    const int producer_cpu = read_int_env_or_default(
        "MB_DDF_RT_LATENCY_PRODUCER_CPU",
        1);
    const int consumer_cpu = read_int_env_or_default(
        "MB_DDF_RT_LATENCY_CONSUMER_CPU",
        2);
    constexpr int message_count = 1000;

    const int realtime_priority = sched_get_priority_max(SCHED_FIFO);
    ASSERT_GT(realtime_priority, 0);
    ASSERT_GT(sysconf(_SC_NPROCESSORS_ONLN), std::max(producer_cpu, consumer_cpu));

    std::atomic<bool> start{false};
    std::atomic<bool> running{true};
    std::atomic<bool> producer_ready{false};
    std::atomic<bool> consumer_ready{false};
    std::atomic<bool> producer_setup_ok{false};
    std::atomic<bool> consumer_setup_ok{false};
    std::atomic<int> waiting_sequence{-1};
    std::atomic<int> received_count{0};
    std::atomic<int> publish_failures{0};
    std::atomic<int> read_failures{0};
    std::atomic<int> sequence_failures{0};
    std::atomic<int> ack_timeouts{0};
    std::atomic<int> poll_timeouts{0};

    std::string producer_error;
    std::string consumer_error;
    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(message_count);

    std::thread consumer([&]() {
        const bool setup_ok = set_current_thread_realtime_affinity(
            consumer_cpu,
            realtime_priority,
            consumer_error);
        consumer_setup_ok.store(setup_ok, std::memory_order_release);
        consumer_ready.store(true, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
            cpu_relax();
        }
        if (!setup_ok || !producer_setup_ok.load(std::memory_order_acquire)) {
            running.store(false, std::memory_order_release);
            return;
        }

        for (int i = 0; i < message_count && running.load(std::memory_order_acquire); ++i) {
            waiting_sequence.store(i, std::memory_order_release);
            const auto read_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(1);
            bool received = false;

            while (running.load(std::memory_order_acquire)) {
                Latency64Payload payload {};
                const int32_t read = sub->read(
                    &payload,
                    sizeof(payload),
                    ReadStrategy::Polling,
                    0,
                    true);
                if (read == 0) {
                    if (std::chrono::steady_clock::now() >= read_deadline) {
                        poll_timeouts.fetch_add(1, std::memory_order_acq_rel);
                        running.store(false, std::memory_order_release);
                        return;
                    }
                    cpu_relax();
                    continue;
                }
                if (read != static_cast<int32_t>(sizeof(payload))) {
                    read_failures.fetch_add(1, std::memory_order_acq_rel);
                    running.store(false, std::memory_order_release);
                    return;
                }
                if (payload.sequence != static_cast<uint64_t>(i) || payload.sent_ns == 0) {
                    sequence_failures.fetch_add(1, std::memory_order_acq_rel);
                    running.store(false, std::memory_order_release);
                    return;
                }

                latencies_ns.push_back(steady_now_ns() - payload.sent_ns);
                received_count.store(i + 1, std::memory_order_release);
                received = true;
                break;
            }

            if (!received) {
                return;
            }
        }
        running.store(false, std::memory_order_release);
    });

    std::thread producer([&]() {
        const bool setup_ok = set_current_thread_realtime_affinity(
            producer_cpu,
            realtime_priority,
            producer_error);
        producer_setup_ok.store(setup_ok, std::memory_order_release);
        producer_ready.store(true, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
            cpu_relax();
        }
        if (!setup_ok || !consumer_setup_ok.load(std::memory_order_acquire)) {
            running.store(false, std::memory_order_release);
            return;
        }

        for (int i = 0; i < message_count && running.load(std::memory_order_acquire); ++i) {
            const auto wait_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(1);
            while (waiting_sequence.load(std::memory_order_acquire) != i) {
                if (!running.load(std::memory_order_acquire)) {
                    return;
                }
                if (std::chrono::steady_clock::now() >= wait_deadline) {
                    ack_timeouts.fetch_add(1, std::memory_order_acq_rel);
                    running.store(false, std::memory_order_release);
                    return;
                }
                cpu_relax();
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));

            Latency64Payload payload {};
            payload.sequence = static_cast<uint64_t>(i);
            payload.sent_ns = steady_now_ns();
            std::memset(payload.padding, 0xA5, sizeof(payload.padding));
            if (dds.data_write(pub, &payload, sizeof(payload)) == 0) {
                publish_failures.fetch_add(1, std::memory_order_acq_rel);
                running.store(false, std::memory_order_release);
                return;
            }

            const auto ack_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(1);
            while (received_count.load(std::memory_order_acquire) < i + 1) {
                if (!running.load(std::memory_order_acquire)) {
                    return;
                }
                if (std::chrono::steady_clock::now() >= ack_deadline) {
                    ack_timeouts.fetch_add(1, std::memory_order_acq_rel);
                    running.store(false, std::memory_order_release);
                    return;
                }
                cpu_relax();
            }
        }
    });

    while (!producer_ready.load(std::memory_order_acquire) ||
           !consumer_ready.load(std::memory_order_acquire)) {
        cpu_relax();
    }
    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    ASSERT_TRUE(producer_setup_ok.load(std::memory_order_acquire)) << producer_error;
    ASSERT_TRUE(consumer_setup_ok.load(std::memory_order_acquire)) << consumer_error;
    ASSERT_EQ(publish_failures.load(std::memory_order_acquire), 0);
    ASSERT_EQ(read_failures.load(std::memory_order_acquire), 0);
    ASSERT_EQ(sequence_failures.load(std::memory_order_acquire), 0);
    ASSERT_EQ(ack_timeouts.load(std::memory_order_acquire), 0);
    ASSERT_EQ(poll_timeouts.load(std::memory_order_acquire), 0);
    ASSERT_EQ(latencies_ns.size(), static_cast<size_t>(message_count));

    std::sort(latencies_ns.begin(), latencies_ns.end());
    uint64_t sum_ns = 0;
    for (const auto value : latencies_ns) {
        sum_ns += value;
    }

    const auto percentile = [&](size_t percent) {
        const size_t index = std::min(
            latencies_ns.size() - 1,
            (latencies_ns.size() * percent) / 100);
        return static_cast<double>(latencies_ns[index]) / 1000.0;
    };

    const double average_us =
        static_cast<double>(sum_ns) / static_cast<double>(latencies_ns.size()) / 1000.0;
    const double p95_us = percentile(95);
    const double p99_us = percentile(99);
    const double max_us = static_cast<double>(latencies_ns.back()) / 1000.0;

    record_metric("RealtimePinned64BPollLatencyAverageUs", average_us);
    record_metric("RealtimePinned64BPollLatencyP95Us", p95_us);
    record_metric("RealtimePinned64BPollLatencyP99Us", p99_us);
    record_metric("RealtimePinned64BPollLatencyMaxUs", max_us);

    std::cout << "[ PERF ] poll_latency_rt_pinned payload=64 bytes messages="
              << message_count
              << " producer_cpu=" << producer_cpu
              << " consumer_cpu=" << consumer_cpu
              << " policy=SCHED_FIFO priority=" << realtime_priority
              << " polling=busy_cpu"
              << " average_us=" << average_us
              << " p95_us=" << p95_us
              << " p99_us=" << p99_us
              << " max_us=" << max_us << std::endl;
}

TEST_F(HardwarePerformanceTest, ForkedIpcPublishThroughputOnTarget) {
    auto& parent_dds = DDSCore::instance();
    ASSERT_TRUE(parent_dds.initialize(64 * 1024 * 1024));

    const std::string topic = "rt://hardware/perf/ipc";
    auto parent_pub = parent_dds.create_publisher(topic);
    ASSERT_NE(parent_pub, nullptr);

    int ready_pipe[2] = {-1, -1};
    ASSERT_EQ(pipe(ready_pipe), 0);

    pid_t child = fork();
    ASSERT_GE(child, 0);

    constexpr size_t payload_size = 256;
    constexpr int message_count = 2048;

    if (child == 0) {
        close(ready_pipe[0]);

        auto& child_dds = DDSCore::instance();
        auto child_sub = child_dds.create_subscriber(topic);
        if (!child_sub) {
            _exit(30);
        }

        const char ready = 'R';
        if (::write(ready_pipe[1], &ready, 1) != 1) {
            _exit(31);
        }
        close(ready_pipe[1]);

        std::vector<uint8_t> buffer(payload_size, 0);
        int received = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (received < message_count && std::chrono::steady_clock::now() < deadline) {
            const int32_t read = child_sub->read(buffer.data(), buffer.size(), static_cast<uint32_t>(500000));
            if (read == static_cast<int32_t>(payload_size)) {
                ++received;
            }
        }

        child_dds.shutdown();
        _exit(received == message_count ? 0 : 32);
    }

    close(ready_pipe[1]);

    char ready = 0;
    const ssize_t ready_read = ::read(ready_pipe[0], &ready, 1);
    close(ready_pipe[0]);

    bool publish_ok = ready_read == 1 && ready == 'R';
    std::vector<uint8_t> payload(payload_size, 0xA5);

    const auto start = std::chrono::steady_clock::now();
    if (publish_ok) {
        for (int i = 0; i < message_count; ++i) {
            payload[0] = static_cast<uint8_t>(i & 0xFF);
            if (parent_dds.data_write(parent_pub, payload.data(), payload.size()) == 0) {
                publish_ok = false;
                break;
            }
        }
    }
    const auto end = std::chrono::steady_clock::now();

    int status = 0;
    const bool child_done = wait_for_child(child, &status, 8);

    ASSERT_TRUE(publish_ok);
    ASSERT_TRUE(child_done) << "child process timed out";
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    const double seconds = elapsed_seconds(start, end);
    ASSERT_GT(seconds, 0.0);

    const double messages_per_second = static_cast<double>(message_count) / seconds;
    const double mib_per_second =
        (static_cast<double>(message_count) * static_cast<double>(payload_size)) /
        (1024.0 * 1024.0 * seconds);

    record_metric("ForkedIpcPublishMessagesPerSecond", messages_per_second);
    record_metric("ForkedIpcPublishMiBPerSecond", mib_per_second);

    std::cout << "[ PERF ] forked_ipc_publish payload=" << payload_size
              << " bytes messages=" << message_count
              << " messages_per_second=" << messages_per_second
              << " mib_per_second=" << mib_per_second << std::endl;
}
