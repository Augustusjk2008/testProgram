#include "HelmControl/HelmPwmLifecycle.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class RecordingHelmPwmLifecycleIo final : public IHelmPwmLifecycleIo {
public:
    enum class Operation {
        ForcePwmDisabled,
        UnlockHelm,
        EnablePwm,
    };

    bool force_pwm_disabled() override {
        calls.push_back(Operation::ForcePwmDisabled);
        return force_pwm_disabled_succeeds;
    }

    bool unlock_helm() override {
        calls.push_back(Operation::UnlockHelm);
        return unlock_helm_succeeds;
    }

    bool enable_pwm() override {
        calls.push_back(Operation::EnablePwm);
        return enable_pwm_succeeds;
    }

    bool force_pwm_disabled_succeeds{true};
    bool unlock_helm_succeeds{true};
    bool enable_pwm_succeeds{true};
    std::vector<Operation> calls;
};

void expect_fault(const HelmPwmLifecycle& lifecycle) {
    EXPECT_EQ(lifecycle.state(), HelmPwmLifecycle::State::Fault);
    EXPECT_FALSE(lifecycle.pwm_enabled());
    EXPECT_FALSE(lifecycle.healthy());
}

TEST(HelmPwmLifecycleTest, InitializeForcesPwmDisabledAndAwaitsUnlock) {
    RecordingHelmPwmLifecycleIo io;
    HelmPwmLifecycle lifecycle;

    EXPECT_TRUE(lifecycle.initialize(io));
    EXPECT_EQ(io.calls, (std::vector<RecordingHelmPwmLifecycleIo::Operation>{
                            RecordingHelmPwmLifecycleIo::Operation::ForcePwmDisabled}));
    EXPECT_EQ(lifecycle.state(), HelmPwmLifecycle::State::AwaitingUnlock);
    EXPECT_FALSE(lifecycle.pwm_enabled());
    EXPECT_TRUE(lifecycle.healthy());
}

TEST(HelmPwmLifecycleTest, UpdateWithoutUnlockDoesNotIssueFurtherIo) {
    RecordingHelmPwmLifecycleIo io;
    HelmPwmLifecycle lifecycle;
    ASSERT_TRUE(lifecycle.initialize(io));

    EXPECT_TRUE(lifecycle.update(io, false, Clock::time_point{}));
    EXPECT_EQ(io.calls, (std::vector<RecordingHelmPwmLifecycleIo::Operation>{
                            RecordingHelmPwmLifecycleIo::Operation::ForcePwmDisabled}));
    EXPECT_EQ(lifecycle.state(), HelmPwmLifecycle::State::AwaitingUnlock);
    EXPECT_FALSE(lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, UnlockDoesNotEnablePwmBeforeThirtyMilliseconds) {
    RecordingHelmPwmLifecycleIo io;
    HelmPwmLifecycle lifecycle;
    ASSERT_TRUE(lifecycle.initialize(io));
    io.calls.clear();
    const Clock::time_point unlock_time{};

    EXPECT_TRUE(lifecycle.update(io, true, unlock_time));
    EXPECT_TRUE(lifecycle.update(
        io, true, unlock_time + kHelmPwmEnableDelay - std::chrono::milliseconds{1}));

    EXPECT_EQ(io.calls, (std::vector<RecordingHelmPwmLifecycleIo::Operation>{
                            RecordingHelmPwmLifecycleIo::Operation::UnlockHelm}));
    EXPECT_EQ(lifecycle.state(), HelmPwmLifecycle::State::WaitingForPwmEnable);
    EXPECT_FALSE(lifecycle.pwm_enabled());
    EXPECT_TRUE(lifecycle.healthy());
}

TEST(HelmPwmLifecycleTest, EnablesPwmAtExactlyThirtyMillisecondsAfterUnlock) {
    static_assert(kHelmPwmEnableDelay == std::chrono::milliseconds{30});

    RecordingHelmPwmLifecycleIo io;
    HelmPwmLifecycle lifecycle;
    ASSERT_TRUE(lifecycle.initialize(io));
    io.calls.clear();
    const Clock::time_point unlock_time{};

    ASSERT_TRUE(lifecycle.update(io, true, unlock_time));
    EXPECT_TRUE(lifecycle.update(io, true, unlock_time + kHelmPwmEnableDelay));

    EXPECT_EQ(io.calls, (std::vector<RecordingHelmPwmLifecycleIo::Operation>{
                            RecordingHelmPwmLifecycleIo::Operation::UnlockHelm,
                            RecordingHelmPwmLifecycleIo::Operation::EnablePwm}));
    EXPECT_EQ(lifecycle.state(), HelmPwmLifecycle::State::Enabled);
    EXPECT_TRUE(lifecycle.pwm_enabled());
    EXPECT_TRUE(lifecycle.healthy());
}

TEST(HelmPwmLifecycleTest, RepeatedUnlockDoesNotResetDelayOrUnlockAgain) {
    RecordingHelmPwmLifecycleIo io;
    HelmPwmLifecycle lifecycle;
    ASSERT_TRUE(lifecycle.initialize(io));
    io.calls.clear();
    const Clock::time_point unlock_time{};

    ASSERT_TRUE(lifecycle.update(io, true, unlock_time));
    ASSERT_TRUE(lifecycle.update(
        io, true, unlock_time + std::chrono::milliseconds{10}));
    EXPECT_TRUE(lifecycle.update(io, true, unlock_time + kHelmPwmEnableDelay));

    EXPECT_EQ(io.calls, (std::vector<RecordingHelmPwmLifecycleIo::Operation>{
                            RecordingHelmPwmLifecycleIo::Operation::UnlockHelm,
                            RecordingHelmPwmLifecycleIo::Operation::EnablePwm}));
    EXPECT_EQ(lifecycle.state(), HelmPwmLifecycle::State::Enabled);
    EXPECT_TRUE(lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, FalseAfterEnableDoesNotReversePwmState) {
    RecordingHelmPwmLifecycleIo io;
    HelmPwmLifecycle lifecycle;
    ASSERT_TRUE(lifecycle.initialize(io));
    io.calls.clear();
    const Clock::time_point unlock_time{};
    ASSERT_TRUE(lifecycle.update(io, true, unlock_time));
    ASSERT_TRUE(lifecycle.update(io, true, unlock_time + kHelmPwmEnableDelay));

    io.calls.clear();
    EXPECT_TRUE(lifecycle.update(
        io, false, unlock_time + kHelmPwmEnableDelay + std::chrono::milliseconds{1}));

    EXPECT_TRUE(io.calls.empty());
    EXPECT_EQ(lifecycle.state(), HelmPwmLifecycle::State::Enabled);
    EXPECT_TRUE(lifecycle.pwm_enabled());
    EXPECT_TRUE(lifecycle.healthy());
}

TEST(HelmPwmLifecycleTest, ForcePwmDisabledFailureEntersFault) {
    RecordingHelmPwmLifecycleIo io;
    io.force_pwm_disabled_succeeds = false;
    HelmPwmLifecycle lifecycle;

    EXPECT_FALSE(lifecycle.initialize(io));
    EXPECT_EQ(io.calls, (std::vector<RecordingHelmPwmLifecycleIo::Operation>{
                            RecordingHelmPwmLifecycleIo::Operation::ForcePwmDisabled}));
    expect_fault(lifecycle);
}

TEST(HelmPwmLifecycleTest, UnlockFailureEntersFault) {
    RecordingHelmPwmLifecycleIo io;
    HelmPwmLifecycle lifecycle;
    ASSERT_TRUE(lifecycle.initialize(io));
    io.calls.clear();
    io.unlock_helm_succeeds = false;

    EXPECT_FALSE(lifecycle.update(io, true, Clock::time_point{}));
    EXPECT_EQ(io.calls, (std::vector<RecordingHelmPwmLifecycleIo::Operation>{
                            RecordingHelmPwmLifecycleIo::Operation::UnlockHelm}));
    expect_fault(lifecycle);
}

TEST(HelmPwmLifecycleTest, EnablePwmFailureEntersFault) {
    RecordingHelmPwmLifecycleIo io;
    HelmPwmLifecycle lifecycle;
    ASSERT_TRUE(lifecycle.initialize(io));
    io.calls.clear();
    const Clock::time_point unlock_time{};
    ASSERT_TRUE(lifecycle.update(io, true, unlock_time));
    io.enable_pwm_succeeds = false;

    EXPECT_FALSE(lifecycle.update(io, true, unlock_time + kHelmPwmEnableDelay));
    EXPECT_EQ(io.calls, (std::vector<RecordingHelmPwmLifecycleIo::Operation>{
                            RecordingHelmPwmLifecycleIo::Operation::UnlockHelm,
                            RecordingHelmPwmLifecycleIo::Operation::EnablePwm}));
    expect_fault(lifecycle);
}

} // namespace
