#include "MB_DDF_HW_Test/SystemTestProvider.h"

#include "MB_DDF/Debug/Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace MB_DDF::HWTest {
namespace {

using Clock = std::chrono::steady_clock;

bool read_first_line(const std::filesystem::path& path, std::string& value) {
    std::ifstream input(path);
    return input.good() && static_cast<bool>(std::getline(input, value));
}

template <typename Value>
bool read_number(const std::filesystem::path& path, Value& value) {
    std::ifstream input(path);
    return static_cast<bool>(input >> value);
}

bool read_cpu_times(SystemTestProvider::CpuTimes& result) {
    std::ifstream input("/proc/stat");
    std::string line;
    return std::getline(input, line) &&
           SystemTestProvider::parse_cpu_times(line, result);
}

bool read_cpu_usage(float& usage) {
    SystemTestProvider::CpuTimes before{};
    SystemTestProvider::CpuTimes after{};
    if (!read_cpu_times(before)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!read_cpu_times(after)) {
        return false;
    }
    return SystemTestProvider::calculate_cpu_usage(before, after, usage);
}

bool read_memory_usage(float& usage) {
    std::ifstream input("/proc/meminfo");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    uint64_t total = 0;
    uint64_t available = 0;
    while (input >> key >> value >> unit) {
        if (key == "MemTotal:") {
            total = value;
        } else if (key == "MemAvailable:") {
            available = value;
        }
    }
    if (total == 0 || available > total) {
        return false;
    }
    usage = static_cast<float>(100.0 * static_cast<double>(total - available) /
                               static_cast<double>(total));
    return true;
}

bool read_cpu_frequency(unsigned cpu, float& mhz) {
    uint64_t khz = 0;
    const auto base = std::filesystem::path("/sys/devices/system/cpu") /
                      ("cpu" + std::to_string(cpu)) / "cpufreq";
    const auto convert_khz = [&](uint64_t value) {
        const float converted =
            static_cast<float>(static_cast<double>(value) / 1000.0);
        if (value == 0 || !std::isfinite(converted) || converted <= 0.0F) {
            return false;
        }
        mhz = converted;
        return true;
    };
    if (read_number(base / "scaling_cur_freq", khz) &&
        convert_khz(khz)) {
        return true;
    }
    khz = 0;
    if (read_number(base / "cpuinfo_cur_freq", khz) &&
        convert_khz(khz)) {
        return true;
    }

    // 该板内核未注册标准 cpufreq policy，SCMI debugfs 时钟是当前集群时钟。
    std::string_view clock_name;
    if (cpu < 4) {
        clock_name = "scmi_clk_cpul";
    } else if (cpu < 6) {
        clock_name = "scmi_clk_cpub01";
    } else if (cpu < 8) {
        clock_name = "scmi_clk_cpub23";
    } else {
        return false;
    }
    uint64_t hz = 0;
    const auto clock_rate = std::filesystem::path("/sys/kernel/debug/clk") /
                            clock_name / "clk_rate";
    if (!read_number(clock_rate, hz) || hz == 0) {
        return false;
    }
    mhz = static_cast<float>(static_cast<double>(hz) / 1'000'000.0);
    return std::isfinite(mhz) && mhz > 0.0F;
}

bool read_uptime(double& seconds) {
    std::ifstream input("/proc/uptime");
    return static_cast<bool>(input >> seconds) && seconds >= 0.0;
}

std::optional<std::filesystem::path> resolved_xdma_pci_device() {
    const auto device_from_fact = [](const std::filesystem::path& fact) ->
        std::optional<std::filesystem::path> {
        std::error_code error;
        const auto resolved = std::filesystem::canonical(fact, error);
        if (error) {
            return std::nullopt;
        }
        const auto bdf = SystemTestProvider::pci_bdf_from_sysfs_path(
            resolved.generic_string());
        if (!bdf) {
            return std::nullopt;
        }
        return std::filesystem::path("/sys/bus/pci/devices") / *bdf;
    };

    // XDMA Linux 驱动实际创建设备节点为 xdma0_user；保留旧入口兼容旧镜像。
    for (const auto& class_fact : {
             std::filesystem::path("/sys/class/xdma/xdma0_user/device"),
             std::filesystem::path("/sys/class/xdma/xdma0/device")}) {
        if (const auto class_device = device_from_fact(class_fact)) {
            return class_device;
        }
    }

    for (const auto& device_path : {std::filesystem::path("/dev/xdma0_user"),
                                    std::filesystem::path("/dev/xdma0")}) {
        struct stat device_status {};
        if (::stat(device_path.c_str(), &device_status) != 0 ||
            !S_ISCHR(device_status.st_mode)) {
            continue;
        }
        const auto character_device = std::filesystem::path("/sys/dev/char") /
                                      (std::to_string(::major(device_status.st_rdev)) + ":" +
                                       std::to_string(::minor(device_status.st_rdev))) /
                                      "device";
        if (const auto pci_device = device_from_fact(character_device)) {
            return pci_device;
        }
    }
    return std::nullopt;
}

bool read_pcie_link(float& speed, float& width) {
    const auto device = resolved_xdma_pci_device();
    if (!device) {
        return false;
    }
    std::string speed_text;
    uint32_t width_value = 0;
    if (!read_first_line(*device / "current_link_speed", speed_text) ||
        !read_number(*device / "current_link_width", width_value)) {
        return false;
    }
    std::istringstream parser(speed_text);
    if (!(parser >> speed) || speed <= 0.0F || width_value == 0) {
        return false;
    }
    width = static_cast<float>(width_value);
    return true;
}

bool read_thermal_zone(std::initializer_list<std::string_view> names, float& celsius) {
    std::error_code error;
    const std::filesystem::path root("/sys/class/thermal");
    for (std::filesystem::directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        std::string type;
        if (!read_first_line(it->path() / "type", type)) {
            continue;
        }
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        bool matched = false;
        for (const auto name : names) {
            matched = matched || type.find(name) != std::string::npos;
        }
        int64_t millidegrees = 0;
        if (matched && read_number(it->path() / "temp", millidegrees)) {
            celsius = static_cast<float>(static_cast<double>(millidegrees) / 1000.0);
            return true;
        }
    }
    return false;
}

bool read_center_thermal(float& celsius) {
    std::error_code error;
    const std::filesystem::path root("/sys/class/hwmon");
    for (std::filesystem::directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        std::string name;
        if (!read_first_line(it->path() / "name", name) || name != "center_thermal") {
            continue;
        }
        std::error_code child_error;
        for (std::filesystem::directory_iterator child(it->path(), child_error), child_end;
             !child_error && child != child_end; child.increment(child_error)) {
            const auto file_name = child->path().filename().string();
            if (!file_name.starts_with("temp") || !file_name.ends_with("_input")) {
                continue;
            }
            int64_t millidegrees = 0;
            if (read_number(child->path(), millidegrees)) {
                celsius = static_cast<float>(static_cast<double>(millidegrees) / 1000.0);
                return true;
            }
        }
    }
    return false;
}

bool set_float(ProductMessage& message, std::string_view field, float value) {
    return std::isfinite(value) && message.set_float(field, value);
}

bool set_s16_measurement(ProductMessage& message, std::string_view field, float value) {
    if (!std::isfinite(value)) {
        return false;
    }
    const auto rounded = std::llround(value);
    if (rounded < std::numeric_limits<int16_t>::min() ||
        rounded > std::numeric_limits<int16_t>::max()) {
        return false;
    }
    return message.set_signed(field, rounded);
}

double elapsed_seconds(Clock::time_point start, Clock::time_point end) {
    return std::max(1.0e-9, std::chrono::duration<double>(end - start).count());
}

float bandwidth_mb_per_second(size_t bytes, double seconds) {
    return static_cast<float>((static_cast<double>(bytes) / 1'000'000.0) / seconds);
}

uint32_t pattern_value(uint8_t type, uint32_t seed, size_t index) {
    switch (type) {
    case 0:
        return seed;
    case 1:
        return (index & 1u) == 0 ? seed : ~seed;
    default:
        return seed ^ static_cast<uint32_t>(index);
    }
}

void non_temporal_fill(uint64_t* destination, size_t words, uint64_t value) {
#if defined(__aarch64__)
    size_t index = 0;
    for (; index + 1 < words; index += 2) {
        asm volatile("stnp %x[first], %x[second], [%[address]]"
                     :
                     : [address] "r"(destination + index), [first] "r"(value),
                       [second] "r"(value)
                     : "memory");
    }
    if (index < words) {
        destination[index] = value;
    }
    asm volatile("dsb st" ::: "memory");
#else
    std::fill(destination, destination + words, value);
#endif
}

std::atomic<uint64_t> memory_observation_sink{0};

#if defined(__GNUC__)
__attribute__((noinline))
#endif
void observe_memory(const uint32_t* words, size_t count) noexcept {
    const volatile uint32_t* observable_words = words;
    uint64_t checksum = 0;
    for (size_t index = 0; index < count; ++index) {
        checksum = (checksum << 7) ^ (checksum >> 3) ^ observable_words[index];
    }
    memory_observation_sink.store(checksum, std::memory_order_relaxed);
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
void observe_memory(const uint64_t* words, size_t count) noexcept {
    const volatile uint64_t* observable_words = words;
    uint64_t checksum = 0;
    for (size_t index = 0; index < count; ++index) {
        checksum = (checksum << 7) ^ (checksum >> 3) ^ observable_words[index];
    }
    memory_observation_sink.store(checksum, std::memory_order_relaxed);
}

} // namespace

ProductErrorCode SystemTestProvider::handle(const ProductMessage& request,
                                            ProductMessage& response) {
    if (request.name() == "system_status_request") {
        return handle_system_status(response);
    }
    if (request.name() == "memperf_test_request") {
        return handle_memory(request, response);
    }
    if (request.name() == "timer_jitter_start_request") {
        return handle_timer_start(request, response);
    }
    if (request.name() == "timer_jitter_stop_request") {
        return handle_timer_stop();
    }
    return ProductErrorCode::CmdUnknown;
}

size_t SystemTestProvider::timer_bucket_for_jitter(double jitter_us) noexcept {
    constexpr std::array<double, 7> limits_us{2.0, 4.0, 8.0, 16.0,
                                              32.0, 64.0, 100.0};
    return static_cast<size_t>(
        std::upper_bound(limits_us.begin(), limits_us.end(), jitter_us) -
        limits_us.begin());
}

bool SystemTestProvider::parse_cpu_times(std::string_view line, CpuTimes& result) {
    std::istringstream input{std::string(line)};
    std::string label;
    std::array<uint64_t, 10> values{};
    if (!(input >> label) || label != "cpu") {
        return false;
    }
    size_t count = 0;
    while (count < values.size() && input >> values[count]) {
        ++count;
    }
    if (count < 4) {
        return false;
    }
    result = {};
    // Linux 的 guest/guest_nice 已包含在 user/nice 中，不能重复累计。
    const size_t non_guest_count = std::min<size_t>(count, 8);
    for (size_t index = 0; index < non_guest_count; ++index) {
        result.total += values[index];
    }
    result.idle = values[3] + (count > 4 ? values[4] : 0);
    return true;
}

bool SystemTestProvider::calculate_cpu_usage(const CpuTimes& before,
                                             const CpuTimes& after,
                                             float& usage) noexcept {
    if (after.total <= before.total || after.idle < before.idle) {
        return false;
    }
    const uint64_t total = after.total - before.total;
    const uint64_t idle = std::min(total, after.idle - before.idle);
    usage = static_cast<float>(100.0 * static_cast<double>(total - idle) /
                               static_cast<double>(total));
    return std::isfinite(usage);
}

std::optional<std::string> SystemTestProvider::pci_bdf_from_sysfs_path(
    std::string_view resolved_path) {
    const auto is_hex = [](char character) {
        return std::isxdigit(static_cast<unsigned char>(character)) != 0;
    };
    const auto hex_value = [](char character) {
        const auto lower = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
        return static_cast<unsigned>(lower >= 'a' ? lower - 'a' + 10
                                                   : lower - '0');
    };
    const auto is_bdf = [&](std::string_view component) {
        if (component.size() != 12 || component[4] != ':' ||
            component[7] != ':' || component[10] != '.') {
            return false;
        }
        constexpr std::array<size_t, 9> hex_positions{0, 1, 2, 3, 5,
                                                      6, 8, 9, 11};
        for (const size_t index : hex_positions) {
            if (!is_hex(component[index])) {
                return false;
            }
        }
        const unsigned device = hex_value(component[8]) * 16u +
                                hex_value(component[9]);
        const unsigned function = hex_value(component[11]);
        return device <= 0x1Fu && function <= 7u;
    };

    std::optional<std::string> leaf_bdf;
    size_t begin = 0;
    while (begin <= resolved_path.size()) {
        const size_t end = resolved_path.find('/', begin);
        const auto component = resolved_path.substr(
            begin, end == std::string_view::npos ? resolved_path.size() - begin
                                                 : end - begin);
        if (is_bdf(component)) {
            leaf_bdf = std::string(component);
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return leaf_bdf;
}

ProductErrorCode SystemTestProvider::handle_system_status(ProductMessage& response) {
    bool complete = true;
    const auto require = [&](bool available, std::string_view field) {
        if (!available) {
            LOG_ERROR << "[HW-TEST] 系统状态字段缺少可靠测量源：" << field;
        }
        complete = available && complete;
    };

    float value = 0.0F;
    require(read_cpu_usage(value) && set_float(response, "cpu_usage", value),
            "cpu_usage (/proc/stat)");
    require(read_memory_usage(value) && set_float(response, "mem_usage", value),
            "mem_usage (/proc/meminfo)");
    require(read_cpu_frequency(0, value) &&
                set_float(response, "cpu_freq_little", value),
            "cpu_freq_little (CPU0 cpufreq/SCMI clock)");
    require(read_cpu_frequency(4, value) &&
                set_float(response, "cpu_freq_big", value),
            "cpu_freq_big (CPU4 cpufreq/SCMI clock)");

    float pcie_speed = 0.0F;
    float pcie_width = 0.0F;
    const bool pcie_ok = read_pcie_link(pcie_speed, pcie_width);
    require(pcie_ok && set_float(response, "pcie_speed", pcie_speed) &&
                set_float(response, "pcie_width", pcie_width),
            "pcie_speed/pcie_width (XDMA PCIe sysfs)");

    require(set_float(response, "net_init_time", 0.0F),
            "net_init_time (固定为 0 s)");

    float rk_temperature = 0.0F;
    const bool rk_temperature_ok = read_center_thermal(rk_temperature);
    float cpu_temperature = rk_temperature;
    const bool cpu_temperature_ok = rk_temperature_ok ||
                                    read_thermal_zone({"cpu", "soc"},
                                                      cpu_temperature);
    require(cpu_temperature_ok &&
                set_float(response, "cpu_temp", cpu_temperature),
            "cpu_temp (center_thermal 或 CPU/SOC thermal zone)");
    require(rk_temperature_ok &&
                set_s16_measurement(response, "rk_temp", rk_temperature),
            "rk_temp (center_thermal temp*_input)");

    float k7_temperature = 0.0F;
    require(k7_temperature_source_ != nullptr &&
                k7_temperature_source_->read_k7_temperature(k7_temperature) &&
                set_s16_measurement(response, "k7_temp", k7_temperature),
            "k7_temp (XADC 0x150000 + 0x200)");

    double uptime = 0.0;
    require(read_uptime(uptime) &&
                uptime <= std::numeric_limits<uint32_t>::max() &&
                response.set_unsigned("power_on_sec",
                                      static_cast<uint32_t>(uptime)),
            "power_on_sec (/proc/uptime)");

    if (!complete) {
        LOG_ERROR << "[HW-TEST] 系统状态存在缺失的可靠测量源，拒绝伪造字段";
        return ProductErrorCode::TaskExecFailed;
    }
    return ProductErrorCode::Ok;
}

ProductErrorCode SystemTestProvider::handle_memory(const ProductMessage& request,
                                                   ProductMessage& response) {
    const auto type_value = request.get_unsigned("memperf_type");
    const auto length_kib = request.get_unsigned("length");
    const auto seed_value = request.get_unsigned("seed");
    constexpr uint64_t kMaximumLengthKiB = 256u * 1024u;
    if (!type_value || !length_kib || !seed_value || *type_value > 6 ||
        *length_kib == 0 || *length_kib > kMaximumLengthKiB) {
        return ProductErrorCode::ParamOutOfRange;
    }

    const auto type = static_cast<uint8_t>(*type_value);
    const auto seed = static_cast<uint32_t>(*seed_value);
    const size_t bytes = static_cast<size_t>(*length_kib) * 1024u;
    (void)response.set_unsigned("memperf_type", type);

    try {
        std::vector<uint32_t> memory;
        if (type != 6) {
            memory.resize(bytes / sizeof(uint32_t));
        }
        uint32_t errors = 0;
        uint32_t first_failure = 0;
        float write_bandwidth = 0.0F;
        float read_bandwidth = 0.0F;
        const auto overall_start = Clock::now();

        if (type <= 2) {
            // 类型 0~2 只选择三种 seed 派生图样；均测试当前进程实际分配的内存，
            // 不把它们伪称为不同的物理存储介质。
            const auto write_start = Clock::now();
            for (size_t index = 0; index < memory.size(); ++index) {
                memory[index] = pattern_value(type, seed, index);
            }
            const auto write_end = Clock::now();
            const auto read_start = Clock::now();
            for (size_t index = 0; index < memory.size(); ++index) {
                if (memory[index] != pattern_value(type, seed, index)) {
                    if (errors == 0) {
                        first_failure = static_cast<uint32_t>(index * sizeof(uint32_t));
                    }
                    ++errors;
                }
            }
            const auto read_end = Clock::now();
            write_bandwidth = bandwidth_mb_per_second(
                bytes, elapsed_seconds(write_start, write_end));
            read_bandwidth = bandwidth_mb_per_second(
                bytes, elapsed_seconds(read_start, read_end));
        } else if (type == 3) {
            std::fill(memory.begin(), memory.end(), seed);
            const auto start = Clock::now();
            uint64_t checksum = 0;
            for (const auto word : memory) {
                checksum += word;
            }
            asm volatile("" : : "r"(checksum) : "memory");
            read_bandwidth = bandwidth_mb_per_second(
                bytes, elapsed_seconds(start, Clock::now()));
        } else if (type == 4) {
            const auto start = Clock::now();
            std::fill(memory.begin(), memory.end(), seed);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const auto end = Clock::now();
            observe_memory(memory.data(), memory.size());
            write_bandwidth = bandwidth_mb_per_second(
                bytes, elapsed_seconds(start, end));
        } else if (type == 5) {
            std::vector<uint32_t> source(memory.size());
            for (size_t index = 0; index < source.size(); ++index) {
                source[index] = seed ^ static_cast<uint32_t>(index);
            }
            asm volatile("" : : "r"(source.data()) : "memory");
            const auto start = Clock::now();
            std::copy(source.begin(), source.end(), memory.begin());
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const auto end = Clock::now();
            observe_memory(memory.data(), memory.size());
            const double seconds = elapsed_seconds(start, end);
            read_bandwidth = bandwidth_mb_per_second(bytes, seconds);
            write_bandwidth = read_bandwidth;
        } else {
            std::vector<uint64_t> aligned_memory(bytes / sizeof(uint64_t));
            const auto start = Clock::now();
            non_temporal_fill(aligned_memory.data(), aligned_memory.size(),
                              (static_cast<uint64_t>(seed) << 32) | seed);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const auto end = Clock::now();
            observe_memory(aligned_memory.data(), aligned_memory.size());
            write_bandwidth = bandwidth_mb_per_second(
                bytes, elapsed_seconds(start, end));
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - overall_start).count();
        (void)response.set_unsigned("error_count", errors);
        (void)response.set_unsigned("first_fail_addr", first_failure);
        (void)response.set_unsigned(
            "elapsed_ms", static_cast<uint32_t>(std::min<int64_t>(
                              elapsed, std::numeric_limits<uint32_t>::max())));
        (void)response.set_float("write_bandwidth", write_bandwidth);
        (void)response.set_float("read_bandwidth", read_bandwidth);
        return ProductErrorCode::Ok;
    } catch (const std::bad_alloc&) {
        return ProductErrorCode::MemoryAccessFailed;
    } catch (...) {
        return ProductErrorCode::TaskExecFailed;
    }
}

ProductErrorCode SystemTestProvider::handle_timer_start(const ProductMessage& request,
                                                        ProductMessage& response) {
    const auto mode = request.get_unsigned("mode");
    if (!mode || *mode > 1) {
        return ProductErrorCode::ParamOutOfRange;
    }

    ITimerLoadExecutor* active_load = nullptr;
    if (*mode == 1) {
        if (timer_load_ == nullptr) {
            return ProductErrorCode::TaskExecFailed;
        }
        const auto started = timer_load_->start();
        if (started != ProductErrorCode::Ok) {
            return started;
        }
        active_load = timer_load_;
    }

    constexpr auto period = std::chrono::microseconds(250);
    constexpr size_t samples = 250;
    std::array<uint32_t, 8> buckets{};
    double sum = 0.0;
    double maximum = 0.0;

    try {
        auto previous = Clock::now();
        for (size_t sample = 0; sample < samples; ++sample) {
            const auto deadline = previous + period;
            std::this_thread::sleep_until(deadline);
            const auto now = Clock::now();
            const double actual = std::chrono::duration<double, std::micro>(
                                      now - previous).count();
            const double jitter = std::abs(
                actual - static_cast<double>(period.count()));
            sum += jitter;
            maximum = std::max(maximum, jitter);
            const auto bucket = timer_bucket_for_jitter(jitter);
            ++buckets[bucket];
            previous = now;
        }

        if (active_load != nullptr) {
            const auto stopped = active_load->stop();
            active_load = nullptr;
            if (stopped != ProductErrorCode::Ok) {
                return stopped;
            }
        }

        for (size_t index = 0; index < buckets.size(); ++index) {
            (void)response.set_unsigned("buckets[" + std::to_string(index) + "]",
                                        buckets[index]);
        }
        (void)response.set_float("avg_jitter", static_cast<float>(sum / samples));
        (void)response.set_float("max_jitter", static_cast<float>(maximum));
        return ProductErrorCode::Ok;
    } catch (...) {
        if (active_load != nullptr) {
            (void)active_load->stop();
        }
        return ProductErrorCode::TaskExecFailed;
    }
}

ProductErrorCode SystemTestProvider::handle_timer_stop() {
    return timer_load_ != nullptr ? timer_load_->stop()
                                  : ProductErrorCode::Ok;
}

} // namespace MB_DDF::HWTest
