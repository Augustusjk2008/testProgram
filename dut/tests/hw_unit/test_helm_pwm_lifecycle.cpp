#include "HelmControl/HelmPwmLifecycle.h"

#include "hw_unit/support/RecordingTransport.h"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using Clock = std::chrono::steady_clock;
using MB_DDF::HW::DidoDevice;
using MB_DDF::HW::PwmDevice;
using MB_DDF::HW::Test::RecordingTransport;

struct LifecycleRig {
    RecordingTransport pwm_transport;
    RecordingTransport dido_transport;
    PwmDevice pwm{pwm_transport};
    DidoDevice dido{dido_transport};
    HelmPwmLifecycle lifecycle{pwm, dido};
};

void open_pwm(LifecycleRig& rig) {
    ASSERT_TRUE(rig.pwm_transport.open());
    rig.pwm_transport.preset(0x30, 100u);
    rig.pwm_transport.preset(0x28, 0x0Fu);
}

void open_all(LifecycleRig& rig) {
    open_pwm(rig);
    ASSERT_TRUE(rig.dido_transport.open());
}

TEST(HelmPwmLifecycleTest, InitializeImmediatelyDisablesPwm) {
    static_assert(kHelmPwmChannelMapping.encoded == 0x0123u);

    LifecycleRig rig;
    open_pwm(rig);

    ASSERT_TRUE(rig.lifecycle.initialize());

    const auto& accesses = rig.pwm_transport.accesses();
    ASSERT_GE(accesses.size(), 2u);
    EXPECT_TRUE(accesses[0].write);
    EXPECT_EQ(accesses[0].offset, 0x28u);
    EXPECT_EQ(accesses[0].value, 0xFFFFu);
    EXPECT_TRUE(accesses[1].write);
    EXPECT_EQ(accesses[1].offset, 0x24u);
    EXPECT_EQ(accesses[1].value, 0xFFFFu);
    ASSERT_TRUE(accesses.back().write);
    EXPECT_EQ(accesses.back().offset, 0x40u);
    EXPECT_EQ(accesses.back().value, 0x0123u);
    EXPECT_FALSE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, NoUnlockRequestDoesNotTouchDidoOrPwm) {
    LifecycleRig rig;
    open_all(rig);
    ASSERT_TRUE(rig.lifecycle.initialize());
    rig.pwm_transport.clear_accesses();
    rig.dido_transport.clear_accesses();

    EXPECT_TRUE(rig.lifecycle.update(false, Clock::time_point{}));

    EXPECT_TRUE(rig.pwm_transport.accesses().empty());
    EXPECT_TRUE(rig.dido_transport.accesses().empty());
    EXPECT_FALSE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, UnlockPrecedesThirtyMillisecondPwmDelay) {
    LifecycleRig rig;
    open_all(rig);
    ASSERT_TRUE(rig.lifecycle.initialize());
    rig.pwm_transport.clear_accesses();
    rig.dido_transport.clear_accesses();
    const Clock::time_point unlock_time{};

    ASSERT_TRUE(rig.lifecycle.update(true, unlock_time));
    EXPECT_TRUE(rig.lifecycle.update(
        true, unlock_time + kHelmPwmEnableDelay - std::chrono::milliseconds{1}));

    const auto& dido_accesses = rig.dido_transport.accesses();
    ASSERT_EQ(dido_accesses.size(), 1u);
    EXPECT_EQ(dido_accesses[0].offset, 0x04u);
    EXPECT_EQ(dido_accesses[0].value, 0xAAAAu);
    EXPECT_TRUE(rig.pwm_transport.accesses().empty());
    EXPECT_FALSE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, EnablesPwmAtExactlyThirtyMilliseconds) {
    static_assert(kHelmPwmEnableDelay == std::chrono::milliseconds{30});

    LifecycleRig rig;
    open_all(rig);
    ASSERT_TRUE(rig.lifecycle.initialize());
    rig.pwm_transport.clear_accesses();
    rig.dido_transport.clear_accesses();
    const Clock::time_point unlock_time{};

    ASSERT_TRUE(rig.lifecycle.update(true, unlock_time));
    ASSERT_TRUE(rig.lifecycle.update(true, unlock_time + kHelmPwmEnableDelay));

    const auto& pwm_accesses = rig.pwm_transport.accesses();
    ASSERT_EQ(pwm_accesses.size(), 1u);
    EXPECT_EQ(pwm_accesses[0].offset, 0x24u);
    EXPECT_EQ(pwm_accesses[0].value, 0xAAAAu);
    EXPECT_TRUE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, RepeatedUnlockDoesNotResetDelayOrWriteDidoAgain) {
    LifecycleRig rig;
    open_all(rig);
    ASSERT_TRUE(rig.lifecycle.initialize());
    rig.pwm_transport.clear_accesses();
    rig.dido_transport.clear_accesses();
    const Clock::time_point unlock_time{};

    ASSERT_TRUE(rig.lifecycle.update(true, unlock_time));
    ASSERT_TRUE(rig.lifecycle.update(
        true, unlock_time + std::chrono::milliseconds{10}));
    ASSERT_TRUE(rig.lifecycle.update(true, unlock_time + kHelmPwmEnableDelay));

    EXPECT_EQ(rig.dido_transport.accesses().size(), 1u);
    EXPECT_TRUE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, FalseAfterEnableDoesNotReverseState) {
    LifecycleRig rig;
    open_all(rig);
    ASSERT_TRUE(rig.lifecycle.initialize());
    const Clock::time_point unlock_time{};
    ASSERT_TRUE(rig.lifecycle.update(true, unlock_time));
    ASSERT_TRUE(rig.lifecycle.update(true, unlock_time + kHelmPwmEnableDelay));
    rig.pwm_transport.clear_accesses();
    rig.dido_transport.clear_accesses();

    EXPECT_TRUE(rig.lifecycle.update(
        false, unlock_time + kHelmPwmEnableDelay + std::chrono::milliseconds{1}));

    EXPECT_TRUE(rig.pwm_transport.accesses().empty());
    EXPECT_TRUE(rig.dido_transport.accesses().empty());
    EXPECT_TRUE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, StartupDisableFailureStopsLifecycle) {
    LifecycleRig rig;

    EXPECT_FALSE(rig.lifecycle.initialize());
    EXPECT_FALSE(rig.lifecycle.update(true, Clock::time_point{}));
    EXPECT_FALSE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, ChannelMappingFailureStopsLifecycle) {
    LifecycleRig rig;
    open_pwm(rig);
    rig.pwm_transport.fail_next_write_at(0x40u);

    EXPECT_FALSE(rig.lifecycle.initialize());
    EXPECT_FALSE(rig.lifecycle.update(true, Clock::time_point{}));
    EXPECT_FALSE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, DidoUnlockFailureStopsLifecycle) {
    LifecycleRig rig;
    open_pwm(rig);
    ASSERT_TRUE(rig.lifecycle.initialize());

    EXPECT_FALSE(rig.lifecycle.update(true, Clock::time_point{}));
    EXPECT_FALSE(rig.lifecycle.pwm_enabled());
}

TEST(HelmPwmLifecycleTest, PwmEnableFailureStopsLifecycle) {
    LifecycleRig rig;
    open_all(rig);
    ASSERT_TRUE(rig.lifecycle.initialize());
    const Clock::time_point unlock_time{};
    ASSERT_TRUE(rig.lifecycle.update(true, unlock_time));
    rig.pwm_transport.close();

    EXPECT_FALSE(rig.lifecycle.update(true, unlock_time + kHelmPwmEnableDelay));
    EXPECT_FALSE(rig.lifecycle.pwm_enabled());
}

} // namespace
