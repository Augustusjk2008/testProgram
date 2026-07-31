#include <gtest/gtest.h>

#include <iterator>

#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW/Device/Registers/PwmRegisters.h"
#include "hw_unit/support/RecordingTransport.h"

using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

TEST(HwPwmDevice, WritesDirectionsBeforeDuty) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(0x30, 100);
    t.preset(0x28, 0);
    PwmDevice d(t);
    PwmRawOutputs o{{1, 2, 3, 4}, 5, 3};
    ASSERT_TRUE(d.apply_outputs(o));
    const auto& a = t.accesses();
    EXPECT_EQ(a[2].offset, 0x14u);
    EXPECT_EQ(a[6].offset, 0x04u);
    EXPECT_EQ(a.back().value, 0xFFAAu);
}
TEST(HwPwmDevice, RejectsDutyAbovePeak) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(0x30, 10);
    PwmDevice d(t);
    PwmRawOutputs o{{11, 0, 0, 0}, 0, 0};
    auto r = d.apply_outputs(o);
    EXPECT_FALSE(r);
    EXPECT_EQ(r.status().code, StatusCode::InvalidArgument);
}

TEST(HwPwmDevice, EncodesDisabledUpdateGateAndAllOutputsOff) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(0x30, 100);
    t.preset(0x28, 0x0F);
    PwmDevice d(t);

    ASSERT_TRUE(d.disable_outputs());
    ASSERT_TRUE(d.set_update_enabled(false));
    ASSERT_TRUE(d.apply_outputs(PwmRawOutputs{}));

    const auto& accesses = t.accesses();
    ASSERT_FALSE(accesses.empty());
    EXPECT_EQ(accesses.front().offset, 0x28u);
    EXPECT_EQ(accesses.front().value, 0xFFFFu);
    EXPECT_EQ(accesses[1].offset, 0x24u);
    EXPECT_EQ(accesses[1].value, 0xFFFFu);
}

TEST(HwPwmDevice, ConfiguresV4DirectionModeAndChannelMapping) {
    static_assert(Registers::Pwm::DirectionMode == 0x3Cu);
    static_assert(Registers::Pwm::ChannelMapping == 0x40u);

    RecordingTransport t;
    ASSERT_TRUE(t.open());
    PwmDevice d(t);
    PwmConfig config{};

    EXPECT_EQ(config.direction_mode, PwmDirectionMode::PositiveToZero);
    EXPECT_EQ(config.channel_mapping.encoded, 0x3210u);
    ASSERT_TRUE(d.configure(config));

    const uint64_t expected_offsets[] = {
        Registers::Pwm::Carrier,
        Registers::Pwm::Peak,
        Registers::Pwm::Waveform,
        Registers::Pwm::DirectionMode,
        Registers::Pwm::ChannelMapping,
    };
    const uint32_t expected_values[] = {0u, 0u, 0xA001u, 0xAAAAu, 0x3210u};
    const auto& accesses = t.accesses();
    ASSERT_EQ(accesses.size(), std::size(expected_offsets));
    for (size_t i = 0; i < std::size(expected_offsets); ++i) {
        EXPECT_TRUE(accesses[i].write);
        EXPECT_EQ(accesses[i].width, 32u);
        EXPECT_EQ(accesses[i].offset, expected_offsets[i]);
        EXPECT_EQ(accesses[i].value, expected_values[i]);
    }
}

TEST(HwPwmDevice, ConfiguresAlternateDirectionModeAndChannelMapping) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    PwmDevice d(t);
    PwmConfig config{};
    config.direction_mode = PwmDirectionMode::PositiveToOne;
    config.channel_mapping.encoded = 0x2301u;

    ASSERT_TRUE(d.configure(config));
    const auto& accesses = t.accesses();
    ASSERT_EQ(accesses.size(), 5u);
    EXPECT_EQ(accesses[3].offset, Registers::Pwm::DirectionMode);
    EXPECT_EQ(accesses[3].value, 0xBBBBu);
    EXPECT_EQ(accesses[4].offset, Registers::Pwm::ChannelMapping);
    EXPECT_EQ(accesses[4].value, 0x2301u);
}

TEST(HwPwmDevice, ReadsDirectionModeAndChannelMappingIntoState) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    for (unsigned i = 0; i < 4; ++i) {
        t.preset(Registers::Pwm::duty(i), i + 10u);
        t.preset(Registers::Pwm::direction(i), i & 1u);
    }
    t.preset(Registers::Pwm::Enable, 0x0Fu);
    t.preset(Registers::Pwm::UpdateEnable, 1u);
    t.preset(Registers::Pwm::Carrier, 250u);
    t.preset(Registers::Pwm::Peak, 1000u);
    t.preset(Registers::Pwm::Waveform, 0xA002u);
    t.preset(Registers::Pwm::DirectionMode, 0xBBBBu);
    t.preset(Registers::Pwm::ChannelMapping, 0x2301u);

    PwmDevice d(t);
    const auto state = d.read_state();

    ASSERT_TRUE(state);
    EXPECT_EQ(state.value().config.direction_mode, PwmDirectionMode::PositiveToOne);
    EXPECT_EQ(state.value().config.channel_mapping.encoded, 0x2301u);
    ASSERT_EQ(t.accesses().size(), 15u);
    EXPECT_EQ(t.accesses()[13].offset, Registers::Pwm::DirectionMode);
    EXPECT_EQ(t.accesses()[14].offset, Registers::Pwm::ChannelMapping);
}

TEST(HwPwmDevice, RejectsInvalidDirectionModeOnReadback) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(Registers::Pwm::DirectionMode, 0x1234u);

    PwmDevice d(t);
    const auto state = d.read_state();

    ASSERT_FALSE(state);
    EXPECT_EQ(state.status().code, StatusCode::HardwareFault);
}

TEST(HwPwmDevice, KeepsUnsignedDutyModeExplicit) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    PwmDevice d(t);

    ASSERT_TRUE(d.set_duty_mode_unsigned());
    ASSERT_EQ(t.accesses().size(), 1u);
    EXPECT_EQ(t.accesses()[0].offset, Registers::Pwm::DutyMode);
    EXPECT_EQ(t.accesses()[0].value, 0xFFFFu);
}
