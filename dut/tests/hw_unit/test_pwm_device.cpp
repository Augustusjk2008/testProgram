#include <gtest/gtest.h>
#include "MB_DDF_HW/Device/PwmDevice.h"
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
