#include <gtest/gtest.h>

#include "MB_DDF_HW/Device/Registers/UpdateImageIpVersionRegisters.h"
#include "MB_DDF_HW/Device/UpdateImageIpVersionDevice.h"
#include "hw_unit/support/RecordingTransport.h"

using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

namespace {

constexpr uint32_t value_for(unsigned index) {
    return 0x10000000u + index;
}

} // namespace

TEST(HwUpdateImageIpVersionDevice, ReadsCompleteSnapshotWithoutWrites) {
    static_assert(Registers::UpdateImageIpVersion::UserBase == 0x160000u);
    static_assert(Registers::UpdateImageIpVersion::WindowSize == 0x10000u);
    static_assert(Registers::UpdateImageIpVersion::FpgaUpdateDate == 0x70u);

    RecordingTransport t;
    ASSERT_TRUE(t.open());
    for (unsigned i = 1; i <= 28; ++i) {
        t.preset(i * 4u, value_for(i));
    }

    UpdateImageIpVersionDevice d(t);
    const auto snapshot = d.read_snapshot();

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().software_type, static_cast<uint16_t>(value_for(1)));
    EXPECT_EQ(snapshot.value().software_status, static_cast<uint16_t>(value_for(2)));
    EXPECT_EQ(snapshot.value().model_software_version, value_for(3));
    EXPECT_EQ(snapshot.value().model_software_date, value_for(4));
    EXPECT_EQ(snapshot.value().dido.base_address, value_for(5));
    EXPECT_EQ(snapshot.value().dido.version, value_for(6));
    EXPECT_EQ(snapshot.value().dido.date, value_for(7));
    EXPECT_EQ(snapshot.value().dh.base_address, value_for(8));
    EXPECT_EQ(snapshot.value().ad7606.version, value_for(12));
    EXPECT_EQ(snapshot.value().pwm.date, value_for(16));
    EXPECT_EQ(snapshot.value().ads1258.base_address, value_for(17));
    EXPECT_EQ(snapshot.value().xadc.version, value_for(21));
    EXPECT_EQ(snapshot.value().com.date, value_for(25));
    EXPECT_EQ(snapshot.value().fpga_update.base_address, value_for(26));
    EXPECT_EQ(snapshot.value().fpga_update.version, value_for(27));
    EXPECT_EQ(snapshot.value().fpga_update.date, value_for(28));

    const auto& accesses = t.accesses();
    ASSERT_EQ(accesses.size(), 28u);
    for (unsigned i = 0; i < accesses.size(); ++i) {
        EXPECT_FALSE(accesses[i].write);
        EXPECT_EQ(accesses[i].width, 32u);
        EXPECT_EQ(accesses[i].offset, (i + 1u) * 4u);
    }
}

TEST(HwUpdateImageIpVersionDevice, ChecksCommunicationSignature) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(Registers::UpdateImageIpVersion::Communication, 0xAAAABBBBu);
    UpdateImageIpVersionDevice d(t);

    ASSERT_TRUE(d.check_communication());
    ASSERT_EQ(t.accesses().size(), 1u);
    EXPECT_FALSE(t.accesses()[0].write);
    EXPECT_EQ(t.accesses()[0].offset, Registers::UpdateImageIpVersion::Communication);
}

TEST(HwUpdateImageIpVersionDevice, RejectsUnexpectedCommunicationSignature) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(Registers::UpdateImageIpVersion::Communication, 0u);
    UpdateImageIpVersionDevice d(t);

    const auto result = d.check_communication();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::HardwareFault);
}
