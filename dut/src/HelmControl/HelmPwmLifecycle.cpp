#include "HelmControl/HelmPwmLifecycle.h"

#include "MB_DDF/Debug/Logger.h"

HelmPwmLifecycle::HelmPwmLifecycle(
    MB_DDF::HW::PwmDevice& pwm,
    MB_DDF::HW::DidoDevice& dido) noexcept
    : pwm_(pwm), dido_(dido) {}

bool HelmPwmLifecycle::fail(
    const char* action,
    const MB_DDF::HW::Status& status) {
    LOG_ERROR << action << ": " << status.message;
    faulted_ = true;
    return false;
}

bool HelmPwmLifecycle::initialize() {
    if (initialized_ || faulted_) {
        faulted_ = true;
        return false;
    }

    // 第一次 PWM 写操作直接关闭四路输出，避免未解锁时带输出启动。
    auto result = pwm_.disable_outputs();
    if (!result) return fail("PWM startup output disable failed", result.status());

    result = pwm_.set_update_enabled(false);
    if (!result) return fail("PWM startup update disable failed", result.status());

    // 关断后再清零方向/占空比，保证后续使能从可控状态开始。
    result = pwm_.apply_outputs(MB_DDF::HW::PwmRawOutputs{});
    if (!result) return fail("PWM startup zero duty write failed", result.status());

    // FPGA 完成逻辑到物理通道的反序；控制循环始终只使用逻辑下标。
    result = pwm_.set_channel_mapping(kHelmPwmChannelMapping);
    if (!result) return fail("PWM channel mapping write failed", result.status());

    initialized_ = true;
    return true;
}

bool HelmPwmLifecycle::update(
    bool unlock_requested,
    std::chrono::steady_clock::time_point now) {
    if (!initialized_ || faulted_) return false;
    if (pwm_enabled_) return true;

    if (!unlock_completed_) {
        if (!unlock_requested) return true;

        // DO0 高有效表示舵锁解锁；成功后在本进程内不再撤销。
        constexpr uint16_t kHelmLockDo = 0x0001u;
        auto result = dido_.set_outputs(kHelmLockDo, kHelmLockDo);
        if (!result) return fail("Helm unlock DO0 write failed", result.status());

        LOG_INFO << "Helm unlocked through DIDO DO0; waiting 30 ms before PWM enable";
        unlock_completed_ = true;
        pwm_enable_deadline_ = now + kHelmPwmEnableDelay;
        return true;
    }

    if (now < pwm_enable_deadline_) return true;

    auto result = pwm_.set_update_enabled(true);
    if (!result) return fail("PWM update enable after helm unlock failed", result.status());

    LOG_INFO << "PWM enabled after helm unlock delay";
    pwm_enabled_ = true;
    return true;
}
