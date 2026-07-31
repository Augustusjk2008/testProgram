#include <gtest/gtest.h>

#include <iterator>

#include "MB_DDF_HW/Device/Ad7606Device.h"
#include "MB_DDF_HW/Device/Registers/Ad7606Registers.h"
#include "hw_unit/support/RecordingTransport.h"

using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

TEST(HwAd7606Device, ReadsEightSignedChannels) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    for (unsigned i = 0; i < 8; ++i) {
        t.preset(4 + i * 4, 0xFFF0u + i);
    }
    Ad7606Device d(t);
    auto r = d.read_snapshot();
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().raw[0], -16);
    EXPECT_EQ(t.accesses().back().offset, 0x20u);
}

TEST(HwAd7606Device, ConfiguresV4DefaultsAndChannelMapping) {
    static_assert(Registers::Ad7606::ChannelMapping == 0x44);

    RecordingTransport t;
    ASSERT_TRUE(t.open());
    Ad7606Device d(t);
    Ad7606Config c{};
    c.acquisition_enabled = true;
    c.filter_enabled = false;

    EXPECT_EQ(c.oversampling, 0u);
    EXPECT_EQ(c.clock_period, 24u);
    EXPECT_EQ(c.conversion_low_cycles, 3u);
    EXPECT_EQ(c.conversion_wait_cycles, 35u);
    EXPECT_EQ(c.reset_cycles, 5u);
    EXPECT_EQ(c.acquisition_count, 3904u);
    EXPECT_EQ(c.channel_mapping, 0x76543210u);

    ASSERT_TRUE(d.configure(c));

    const uint64_t expected_offsets[] = {
        Registers::Ad7606::AcquisitionEnable, Registers::Ad7606::FilterEnable,
        Registers::Ad7606::Oversampling, Registers::Ad7606::ClockPeriod,
        Registers::Ad7606::ConversionLow, Registers::Ad7606::ConversionWait,
        Registers::Ad7606::ResetCycles, Registers::Ad7606::AcquisitionCount,
        Registers::Ad7606::ChannelMapping,
    };
    const uint32_t expected_values[] = {
        0xAAAAu, 0xFFFFu, 0u, 24u, 3u, 35u, 5u, 3904u, 0x76543210u,
    };
    const auto& accesses = t.accesses();
    ASSERT_EQ(accesses.size(), std::size(expected_offsets));
    for (size_t i = 0; i < std::size(expected_offsets); ++i) {
        EXPECT_TRUE(accesses[i].write);
        EXPECT_EQ(accesses[i].width, 32u);
        EXPECT_EQ(accesses[i].offset, expected_offsets[i]);
        EXPECT_EQ(accesses[i].value, expected_values[i]);
    }
}

TEST(HwAd7606Device, ReadsChannelMappingIntoState) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    for (unsigned i = 0; i < 8; ++i) {
        t.preset(Registers::Ad7606::channel(i), i);
    }
    t.preset(Registers::Ad7606::AcquisitionEnable, 1u);
    t.preset(Registers::Ad7606::FilterEnable, 0u);
    t.preset(Registers::Ad7606::Oversampling, 2u);
    t.preset(Registers::Ad7606::ClockPeriod, 25u);
    t.preset(Registers::Ad7606::ConversionLow, 4u);
    t.preset(Registers::Ad7606::ConversionWait, 36u);
    t.preset(Registers::Ad7606::ResetCycles, 6u);
    t.preset(Registers::Ad7606::AcquisitionCount, 3905u);
    t.preset(Registers::Ad7606::ChannelMapping, 0x10325476u);

    Ad7606Device d(t);
    const auto state = d.read_state();

    ASSERT_TRUE(state);
    EXPECT_EQ(state.value().config.channel_mapping, 0x10325476u);
    ASSERT_EQ(t.accesses().size(), 17u);
    EXPECT_EQ(t.accesses().back().offset, Registers::Ad7606::ChannelMapping);
}
