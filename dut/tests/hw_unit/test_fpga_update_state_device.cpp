#include <gtest/gtest.h>

#include "MB_DDF_HW/Device/FpgaUpdateStateDevice.h"
#include "MB_DDF_HW/Device/Registers/FpgaUpdateStateRegisters.h"
#include "hw_unit/support/RecordingTransport.h"

using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

TEST(HwFpgaUpdateStateDevice, ReadsCompleteSnapshotWithoutWrites) {
    static_assert(Registers::FpgaUpdateState::UserBase == 0x170000u);
    static_assert(Registers::FpgaUpdateState::WindowSize == 0x10000u);
    static_assert(Registers::FpgaUpdateState::CrcValidationStatus == 0x18u);

    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(Registers::FpgaUpdateState::FlashEraseStatus, 0xAAAAu);
    t.preset(Registers::FpgaUpdateState::FlashWriteStatus, 0xFFFFu);
    t.preset(Registers::FpgaUpdateState::FlashReadStatus, 0xAAAAu);
    t.preset(Registers::FpgaUpdateState::ReadDataCrc, 0x12345678u);
    t.preset(Registers::FpgaUpdateState::WriteDataCrc, 0x87654321u);
    t.preset(Registers::FpgaUpdateState::CrcValidationStatus, 0xAAu);

    FpgaUpdateStateDevice d(t);
    const auto snapshot = d.read_snapshot();

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().flash_erase_status, 0xAAAAu);
    EXPECT_EQ(snapshot.value().flash_write_status, 0xFFFFu);
    EXPECT_EQ(snapshot.value().flash_read_status, 0xAAAAu);
    EXPECT_EQ(snapshot.value().read_data_crc, 0x12345678u);
    EXPECT_EQ(snapshot.value().write_data_crc, 0x87654321u);
    EXPECT_EQ(snapshot.value().crc_validation_status, 0xAAu);

    const auto& accesses = t.accesses();
    ASSERT_EQ(accesses.size(), 6u);
    for (unsigned i = 0; i < accesses.size(); ++i) {
        EXPECT_FALSE(accesses[i].write);
        EXPECT_EQ(accesses[i].width, 32u);
        EXPECT_EQ(accesses[i].offset, (i + 1u) * 4u);
    }
}

TEST(HwFpgaUpdateStateDevice, ChecksCommunicationSignature) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(Registers::FpgaUpdateState::Communication, 0xAAAABBBBu);
    FpgaUpdateStateDevice d(t);

    ASSERT_TRUE(d.check_communication());
    ASSERT_EQ(t.accesses().size(), 1u);
    EXPECT_FALSE(t.accesses()[0].write);
    EXPECT_EQ(t.accesses()[0].offset, Registers::FpgaUpdateState::Communication);
}

TEST(HwFpgaUpdateStateDevice, RejectsUnexpectedCommunicationSignature) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(Registers::FpgaUpdateState::Communication, 0u);
    FpgaUpdateStateDevice d(t);

    const auto result = d.check_communication();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::HardwareFault);
}
