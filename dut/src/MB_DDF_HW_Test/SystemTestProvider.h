#pragma once

#include "MB_DDF_HW_Test/ProductProtocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace MB_DDF::HWTest {

/// 定时抖动测试可选负载；接口不暴露任何 MB_DDF_HW 类型。
class ITimerLoadExecutor {
public:
    virtual ~ITimerLoadExecutor() = default;
    virtual ProductErrorCode start() = 0;
    virtual ProductErrorCode stop() = 0;
};

/// 由硬件画像提供的 K7 实时温度源，不向系统测试层暴露具体 Device 类型。
class IK7TemperatureSource {
public:
    virtual ~IK7TemperatureSource() = default;
    virtual bool read_k7_temperature(float& celsius) = 0;
};

/// 不依赖 MB_DDF_HW 的板级系统、内存和定时抖动处理器。
class SystemTestProvider {
public:
    struct CpuTimes {
        uint64_t total{0};
        uint64_t idle{0};
    };

    explicit SystemTestProvider(
        ITimerLoadExecutor* timer_load = nullptr,
        IK7TemperatureSource* k7_temperature_source = nullptr) noexcept
        : timer_load_(timer_load),
          k7_temperature_source_(k7_temperature_source) {}

    ProductErrorCode handle(const ProductMessage& request, ProductMessage& response);
    static size_t timer_bucket_for_jitter(double jitter_us) noexcept;
    static bool parse_cpu_times(std::string_view line, CpuTimes& result);
    static bool calculate_cpu_usage(const CpuTimes& before, const CpuTimes& after,
                                    float& usage) noexcept;
    static std::optional<std::string> pci_bdf_from_sysfs_path(
        std::string_view resolved_path);

private:
    ProductErrorCode handle_system_status(ProductMessage& response);
    ProductErrorCode handle_memory(const ProductMessage& request, ProductMessage& response);
    ProductErrorCode handle_timer_start(const ProductMessage& request,
                                        ProductMessage& response);
    ProductErrorCode handle_timer_stop();

    ITimerLoadExecutor* timer_load_{nullptr};
    IK7TemperatureSource* k7_temperature_source_{nullptr};
};

} // namespace MB_DDF::HWTest
