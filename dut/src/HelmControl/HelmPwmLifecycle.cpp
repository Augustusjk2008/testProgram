#include "HelmControl/HelmPwmLifecycle.h"

bool HelmPwmLifecycle::initialize(IHelmPwmLifecycleIo& io) {
    if (state_ != State::Uninitialized || !io.force_pwm_disabled()) {
        state_ = State::Fault;
        return false;
    }
    state_ = State::AwaitingUnlock;
    return true;
}

bool HelmPwmLifecycle::update(
    IHelmPwmLifecycleIo& io,
    bool unlock_requested,
    std::chrono::steady_clock::time_point now) {
    switch (state_) {
    case State::AwaitingUnlock:
        if (!unlock_requested) return true;
        if (!io.unlock_helm()) {
            state_ = State::Fault;
            return false;
        }
        pwm_enable_deadline_ = now + kHelmPwmEnableDelay;
        state_ = State::WaitingForPwmEnable;
        return true;
    case State::WaitingForPwmEnable:
        if (now < pwm_enable_deadline_) return true;
        if (!io.enable_pwm()) {
            state_ = State::Fault;
            return false;
        }
        state_ = State::Enabled;
        return true;
    case State::Enabled:
        return true;
    case State::Uninitialized:
    case State::Fault:
        state_ = State::Fault;
        return false;
    }
    state_ = State::Fault;
    return false;
}
