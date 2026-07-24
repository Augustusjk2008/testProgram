#include <gtest/gtest.h>
#include "MB_DDF_HW/Device/Ad7606Device.h"
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
TEST(HwAd7606Device, ConfigureUsesMagicValues) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    Ad7606Device d(t);
    Ad7606Config c{};
    c.acquisition_enabled = true;
    c.filter_enabled = false;
    ASSERT_TRUE(d.configure(c));
    EXPECT_EQ(t.accesses()[0].value, 0xAAAAu);
    EXPECT_EQ(t.accesses()[1].value, 0xFFFFu);
}
