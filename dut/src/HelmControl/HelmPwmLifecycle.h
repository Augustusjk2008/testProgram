#pragma once

#include <chrono>

inline constexpr std::chrono::milliseconds kHelmPwmEnableDelay{30};

class IHelmPwmLifecycleIo {
public:
    virtual ~IHelmPwmLifecycleIo() = default;
    virtual bool force_pwm_disabled() = 0;
    virtual bool unlock_helm() = 0;
    virtual bool enable_pwm() = 0;
};

class HelmPwmLifecycle final {
public:
    enum class State {
        Uninitialized,
        AwaitingUnlock,
        WaitingForPwmEnable,
        Enabled,
        Fault,
    };

    bool initialize(IHelmPwmLifecycleIo& io);
    bool update(IHelmPwmLifecycleIo& io,
                bool unlock_requested,
                std::chrono::steady_clock::time_point now);

    State state() const noexcept { return state_; }
    bool pwm_enabled() const noexcept { return state_ == State::Enabled; }
    bool healthy() const noexcept { return state_ != State::Fault; }

private:
    State state_{State::Uninitialized};
    std::chrono::steady_clock::time_point pwm_enable_deadline_{};
};
