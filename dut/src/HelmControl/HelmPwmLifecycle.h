#pragma once

#include "MB_DDF_HW/Device/DidoDevice.h"
#include "MB_DDF_HW/Device/PwmDevice.h"

#include <chrono>

/// 舵锁解锁成功后，PWM 使能前必须等待的最短时间。
inline constexpr std::chrono::milliseconds kHelmPwmEnableDelay{30};

/// 逻辑舵 1..4 到 FPGA 实际通道 4..1 的板级映射。
inline constexpr MB_DDF::HW::PwmChannelMapping kHelmPwmChannelMapping{0x0123u};
/// AD7606 逻辑舵反馈同样反序；未使用的逻辑通道 5..8 保持默认映射。
inline constexpr uint32_t kHelmAd7606ChannelMapping{0x76540123u};

/// 管理“启动关 PWM -> 舵锁解锁 -> 延时使能 PWM”的单向流程。
/// 解锁成功后不再反向上锁，STOP/回零也不禁止 PWM。
class HelmPwmLifecycle final {
public:
    HelmPwmLifecycle(MB_DDF::HW::PwmDevice& pwm,
                     MB_DDF::HW::DidoDevice& dido) noexcept;

    /// 程序启动时立即关闭四路 PWM，清零后配置板级通道映射。
    bool initialize();

    /// 在控制线程中推进解锁和 30 ms 延时；函数本身不阻塞。
    bool update(bool unlock_requested,
                std::chrono::steady_clock::time_point now);

    bool pwm_enabled() const noexcept { return pwm_enabled_; }

private:
    /// 记录硬件错误并把流程锁定在故障态。
    bool fail(const char* action, const MB_DDF::HW::Status& status);

    MB_DDF::HW::PwmDevice& pwm_;
    MB_DDF::HW::DidoDevice& dido_;
    bool initialized_{false};       // 启动 PWM 安全态已建立。
    bool unlock_completed_{false};  // DIDO DO0 解锁写入已成功。
    bool pwm_enabled_{false};       // 30 ms 等待已完成，允许输出。
    bool faulted_{false};           // 任一关键硬件操作失败后粘滞保持。
    std::chrono::steady_clock::time_point pwm_enable_deadline_{}; // PWM 最早使能时刻。
};
