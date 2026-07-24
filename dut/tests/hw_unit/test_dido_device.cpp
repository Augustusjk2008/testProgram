#include <gtest/gtest.h>
#include "MB_DDF_HW/Device/DidoDevice.h"
#include "hw_unit/support/RecordingTransport.h"
using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;
TEST(HwDidoDevice, OnlyWritesUpdatedChannels) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    DidoDevice d(t);
    ASSERT_TRUE(d.set_outputs(0x21, 0x21));
    ASSERT_EQ(t.accesses().size(), 2u);
    EXPECT_EQ(t.accesses()[0].offset, 4u);
    EXPECT_EQ(t.accesses()[1].offset, 0x18u);
}
TEST(HwDidoDevice, ConvertsHighAndLowActiveInputs) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(0x80, 0xAA);
    t.preset(0xA0, 0xAA);
    DidoDevice d(t);
    auto r = d.read_inputs();
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value() & 0x101u, 0x101u);
}
