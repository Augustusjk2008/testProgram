#include "MB_DDF_Demo/DemoRunner.h"

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/Debug/Logger.h"
#include "MB_DDF_Demo/DdsExamples.h"
#include "MB_DDF_Demo/HardwareExamples.h"

#include <exception>
#include <utility>

namespace MB_DDF::Demo {
namespace {

struct DemoSummary {
    int passed{0};
    int skipped{0};
    int failed{0};
};

class DDSShutdownGuard {
public:
    explicit DDSShutdownGuard(DDS::DDSCore& dds) : dds_(dds) {}
    ~DDSShutdownGuard() { dds_.shutdown(); }
    DDSShutdownGuard(const DDSShutdownGuard&) = delete;
    DDSShutdownGuard& operator=(const DDSShutdownGuard&) = delete;

private:
    DDS::DDSCore& dds_;
};

template <typename Callback>
void run_example(const char* name, Callback&& callback, DemoSummary& summary) {
    LOG_INFO << "========== 开始执行示例：" << name << " ==========";
    try {
        const auto result = std::forward<Callback>(callback)();
        switch (result) {
        case DemoResult::Passed:
            ++summary.passed;
            LOG_INFO << "[DEMO] 示例通过：" << name;
            break;
        case DemoResult::Skipped:
            ++summary.skipped;
            LOG_WARN << "[DEMO] 示例已跳过：" << name;
            break;
        case DemoResult::Failed:
            ++summary.failed;
            LOG_ERROR << "[DEMO] 示例失败：" << name;
            break;
        }
    } catch (const std::exception& exception) {
        ++summary.failed;
        LOG_ERROR << "[DEMO] 示例执行时发生异常：" << name
                  << "，异常信息：" << exception.what();
    } catch (...) {
        ++summary.failed;
        LOG_ERROR << "[DEMO] 示例执行时发生未知异常：" << name;
    }
}

} // namespace

int run_demo() {
    auto& dds = DDS::DDSCore::instance();
    LOG_INFO << "[DEMO] 正在初始化 DDSCore：创建或连接共享内存，并初始化 Topic 注册表和内部同步状态";
    if (!dds.initialize()) {
        LOG_ERROR << "[DEMO] DDSCore 初始化失败：无法创建或连接共享内存及内部资源";
        return 1;
    }
    LOG_INFO << "[DEMO] DDSCore 初始化完成，后续示例将复用同一个 DDSCore 实例";
    DDSShutdownGuard shutdown_guard(dds);
    DemoSummary summary;

    run_example("DDS 同步发布与非阻塞轮询（发布一条消息后立即 poll 读取）",
                [&dds]() { return run_dds_synchronous_example(dds); }, summary);
    run_example("DDS 阻塞读取（等待生产者线程发布消息，并验证超时唤醒）",
                [&dds]() { return run_dds_blocking_example(dds); }, summary);
    run_example("DDS 回调订阅（Subscriber 工作线程异步接收并执行回调）",
                [&dds]() { return run_dds_callback_example(dds); }, summary);
    run_example("DDS 零拷贝发布（演示 begin_message/commit 和 publish_fill 两种方式）",
                [&dds]() { return run_dds_zero_copy_example(dds); }, summary);
    run_example("DDS Observer 与序列号（观察消息通知并读取 DDS 本地序列号）",
                [&dds]() { return run_dds_observer_example(dds); }, summary);
    run_example("DDS Topic 发现（枚举并核对前述示例已注册的 Topic）",
                [&dds]() { return run_dds_topic_discovery_example(dds); }, summary);
    run_example("MB_DDF_HW 直接 Device API（直接访问 XdmaTransport 和具体 Device，不经过 DDS/Adapter）",
                []() { return run_hw_direct_device_example(); }, summary);
    run_example("MB_DDF_HW 全能力 API（执行寄存器访问、回环、DMA 和 DDS Adapter 示例）",
                []() { return run_hw_full_capability_example(); }, summary);

    LOG_INFO << "========== Demo 执行结果汇总 ==========";
    LOG_INFO << "[DEMO] 通过=" << summary.passed << "，跳过=" << summary.skipped
             << "，失败=" << summary.failed
             << "，总计=" << (summary.passed + summary.skipped + summary.failed);
    return summary.failed == 0 ? 0 : 4;
}

} // namespace MB_DDF::Demo
