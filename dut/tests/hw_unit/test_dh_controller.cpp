#include <gtest/gtest.h>
#include "MB_DDF_HW/Device/DhController.h"
#include "hw_unit/support/RecordingTransport.h"
using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;
TEST(HwDhController, EncodesAllCommandRanges) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    DhController d(t);
    ASSERT_TRUE(d.fire(0));
    ASSERT_TRUE(d.fire(16));
    ASSERT_TRUE(d.fire(32));
    EXPECT_EQ(t.accesses()[0].value, 0xB000u);
    EXPECT_EQ(t.accesses()[1].value, 0xC000u);
    EXPECT_EQ(t.accesses()[2].value, 0xD000u);
}
TEST(HwDhController, BatchesDeduplicatesAndRepeatPadsFinalBatch) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    DhController d(t);
    const uint8_t c[] = {1, 2, 2, 3, 4, 5};
    ASSERT_TRUE(d.fire_multiple(c));
    ASSERT_EQ(t.accesses().size(), 4u);
    EXPECT_EQ(t.accesses()[0].value, 0x04030201u);
    EXPECT_EQ(t.accesses()[2].value, 0x05050505u);
}
TEST(HwDhController, ExplicitlyControlsGlobalFireEnable) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    DhController d(t);

    ASSERT_TRUE(d.set_fire_enabled(true));
    ASSERT_TRUE(d.set_fire_enabled(false));
    ASSERT_EQ(t.accesses().size(), 2u);
    EXPECT_TRUE(t.accesses()[0].write);
    EXPECT_EQ(t.accesses()[0].offset, 0x1DCu);
    EXPECT_EQ(t.accesses()[0].width, 32u);
    EXPECT_EQ(t.accesses()[0].value, 0xAAAAu);
    EXPECT_TRUE(t.accesses()[1].write);
    EXPECT_EQ(t.accesses()[1].offset, 0x1DCu);
    EXPECT_EQ(t.accesses()[1].width, 32u);
    EXPECT_EQ(t.accesses()[1].value, 0xFFFFu);
}

TEST(HwDhController, ControlsReturnAndPulseConfigRegisters) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    DhController d(t);

    ASSERT_TRUE(d.set_return_enabled(true));
    ASSERT_TRUE(d.set_return_enabled(false));
    ASSERT_TRUE(d.set_pulse_config_enabled(true));
    ASSERT_TRUE(d.set_pulse_config_enabled(false));

    ASSERT_EQ(t.accesses().size(), 4u);
    EXPECT_EQ(t.accesses()[0].offset, 0x04u);
    EXPECT_EQ(t.accesses()[0].value, 0xA000u);
    EXPECT_EQ(t.accesses()[1].offset, 0x04u);
    EXPECT_EQ(t.accesses()[1].value, 0x00A0u);
    EXPECT_EQ(t.accesses()[2].offset, 0x1E0u);
    EXPECT_EQ(t.accesses()[2].value, 0xAAAAu);
    EXPECT_EQ(t.accesses()[3].offset, 0x1E0u);
    EXPECT_EQ(t.accesses()[3].value, 0xFFFFu);
}

TEST(HwDhController, ReadsEnableAndChannelStatusEncodings) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    DhController d(t);
    t.preset(0x1DCu, 0xAAAAu);
    t.preset(0x04u, 0x000000AAu);
    for (unsigned channel = 0; channel < 23; ++channel) {
        t.preset(0x44u + channel * 4u, 0xFFFFu);
    }
    t.preset(0x44u, 0xAAAAu);
    t.preset(0x48u, 0xBBBBu);
    t.preset(0x4Cu, 0xFFFFu);

    ASSERT_TRUE(d.read_fire_enabled());
    ASSERT_TRUE(d.read_return_enabled());
    EXPECT_TRUE(d.read_fire_enabled().value());
    EXPECT_TRUE(d.read_return_enabled().value());
    const auto status = d.read_channel_statuses();
    ASSERT_TRUE(status);
    EXPECT_EQ(status.value()[0], 1u);
    EXPECT_EQ(status.value()[1], 2u);
    EXPECT_EQ(status.value()[2], 0u);

    t.preset(0x1DCu, 0xFFFFu);
    t.preset(0x04u, 0x000000FFu);
    EXPECT_FALSE(d.read_fire_enabled().value());
    EXPECT_FALSE(d.read_return_enabled().value());
}

TEST(HwDhController, AcceptsReturnReadbackOnlyFromLowByte) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    DhController d(t);
    t.preset(0x04u, 0x000000AAu);
    EXPECT_TRUE(d.read_return_enabled());
    t.preset(0x04u, 0x000000FFu);
    EXPECT_FALSE(d.read_return_enabled().value());
}

TEST(HwDhController, LimitsPulseReadbackCountWithoutTouchingExtraChannels) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    DhController d(t);
    const auto widths = d.read_pulse_widths(23);
    ASSERT_TRUE(widths);
    ASSERT_EQ(t.accesses().size(), 23u);
    EXPECT_EQ(t.accesses().back().offset, 0x104u + 22u * 4u);
}

TEST(HwDhController, RejectsInvalidBatteryActivationEncoding) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    for (unsigned channel = 0; channel < 48; ++channel) {
        t.preset(0x44u + channel * 4u, 0xFFFFu);
    }
    t.preset(0x1D8u, 0x0000ABFFu);
    DhController d(t);

    const auto feedback = d.read_feedback();

    ASSERT_FALSE(feedback);
    EXPECT_EQ(feedback.status().code, StatusCode::HardwareFault);
}
