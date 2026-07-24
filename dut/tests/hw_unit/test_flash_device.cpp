#include <gtest/gtest.h>

#include "MB_DDF_HW/Device/FlashDevice.h"
#include "MB_DDF_HW/Device/Registers/FlashRegisters.h"
#include "hw_unit/support/RecordingTransport.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

namespace {

static_assert(Registers::Flash::UserBase == 0x160000u);

constexpr uint64_t kReadRam = 0x000;
constexpr uint64_t kWriteRam = 0x100;
constexpr uint64_t kCommand = 0x200;
constexpr uint64_t kAddress = 0x204;
constexpr uint64_t kCounts = 0x208;
constexpr uint64_t kTriggerStatus = 0x20C;
constexpr uint64_t kClearDone = 0x210;
constexpr uint64_t kClockDivider = 0x300;
constexpr uint32_t kDone = 1u << 16;

bool has_write(const std::vector<Access>& accesses, uint64_t offset, uint32_t value) {
    return std::any_of(accesses.begin(), accesses.end(), [=](const Access& access) {
        return access.write && access.offset == offset && access.value == value;
    });
}

void expect_access(const Access& actual,
                   bool write,
                   uint64_t offset,
                   uint32_t value) {
    EXPECT_EQ(actual.write, write);
    EXPECT_EQ(actual.offset, offset);
    EXPECT_EQ(actual.width, 32u);
    EXPECT_EQ(actual.value, value);
}

} // namespace

TEST(HwFlashDevice, RequiresAnOpenTransport) {
    RecordingTransport transport;
    FlashDevice device(transport);

    const auto result = device.check_communication();

    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::NotOpen);
}

TEST(HwFlashDevice, RunsTheDocumentedOperationSequence) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_reads(kTriggerStatus, {kDone, kDone, kDone, kDone});
    transport.queue_reads(kReadRam, {0x02, 0x00});
    FlashDevice device(transport);

    const auto first_enable = device.write_enable(Timeout::poll());
    const auto first_enabled_status = device.read_flash_status_raw(Timeout::poll());
    const auto erased = device.chip_erase(Timeout::poll());
    const auto erased_status = device.read_flash_status_raw(Timeout::poll());

    ASSERT_TRUE(first_enable);
    ASSERT_TRUE(first_enabled_status);
    EXPECT_EQ(first_enabled_status.value(), 0x02);
    ASSERT_TRUE(erased);
    ASSERT_TRUE(erased_status);
    EXPECT_EQ(erased_status.value(), 0x00);
    const auto& accesses = transport.accesses();
    ASSERT_EQ(accesses.size(), 16u);
    expect_access(accesses[0], true, kCommand, 0x06);
    expect_access(accesses[1], true, kTriggerStatus, 0xA5);
    expect_access(accesses[2], false, kTriggerStatus, kDone);
    expect_access(accesses[3], true, kCommand, 0x05);
    expect_access(accesses[4], true, kTriggerStatus, 0xA5);
    expect_access(accesses[5], false, kTriggerStatus, kDone);
    expect_access(accesses[6], true, kClearDone, 1);
    expect_access(accesses[7], false, kReadRam, 0x02);
    expect_access(accesses[8], true, kCommand, 0x60);
    expect_access(accesses[9], true, kTriggerStatus, 0xA5);
    expect_access(accesses[10], false, kTriggerStatus, kDone);
    expect_access(accesses[11], true, kCommand, 0x05);
    expect_access(accesses[12], true, kTriggerStatus, 0xA5);
    expect_access(accesses[13], false, kTriggerStatus, kDone);
    expect_access(accesses[14], true, kClearDone, 1);
    expect_access(accesses[15], false, kReadRam, 0x00);
    EXPECT_FALSE(has_write(transport.accesses(), kCommand, 0x04));
    EXPECT_FALSE(has_write(transport.accesses(), kCommand, 0x21));
    EXPECT_FALSE(has_write(transport.accesses(), kCommand, 0xDC));
}

TEST(HwFlashDevice, ProgramsRegistersInDocumentedOrder) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_reads(kTriggerStatus, {kDone});
    const std::array<uint8_t, 4> data{};
    FlashDevice device(transport);

    const auto result = device.program(0, {data.data(), data.size()}, Timeout::poll());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), data.size());
    const auto& accesses = transport.accesses();
    ASSERT_EQ(accesses.size(), 7u);
    expect_access(accesses[0], true, kCommand, 0x12);
    expect_access(accesses[1], true, kAddress, 0);
    expect_access(accesses[2], true, kCounts, 4);
    expect_access(accesses[3], true, kWriteRam, 0);
    expect_access(accesses[4], true, kTriggerStatus, 0xA5);
    expect_access(accesses[5], false, kTriggerStatus, kDone);
    expect_access(accesses[6], true, kClearDone, 1);
}

TEST(HwFlashDevice, ReadsRegistersInDocumentedOrder) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_reads(kTriggerStatus, {kDone});
    transport.preset(kReadRam, 0);
    std::array<uint8_t, 4> data{};
    FlashDevice device(transport);

    const auto result = device.read_data(0, {data.data(), data.size()}, Timeout::poll());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), data.size());
    EXPECT_EQ(data, (std::array<uint8_t, 4>{}));
    const auto& accesses = transport.accesses();
    ASSERT_EQ(accesses.size(), 7u);
    expect_access(accesses[0], true, kCommand, 0x13);
    expect_access(accesses[1], true, kAddress, 0);
    expect_access(accesses[2], true, kCounts, 4u << 16);
    expect_access(accesses[3], true, kTriggerStatus, 0xA5);
    expect_access(accesses[4], false, kTriggerStatus, kDone);
    expect_access(accesses[5], true, kClearDone, 1);
    expect_access(accesses[6], false, kReadRam, 0);
}

TEST(HwFlashDevice, DoesNotWriteAddressOrCountsForSimpleCommands) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_reads(kTriggerStatus, {kDone, kDone, kDone});
    transport.preset(kReadRam, 0x02);
    FlashDevice device(transport);

    ASSERT_TRUE(device.write_enable(Timeout::poll()));
    ASSERT_TRUE(device.read_flash_status_raw(Timeout::poll()));
    ASSERT_TRUE(device.chip_erase(Timeout::poll()));

    EXPECT_FALSE(std::any_of(transport.accesses().begin(), transport.accesses().end(),
                             [](const Access& access) {
                                 return access.write &&
                                        (access.offset == kAddress || access.offset == kCounts);
                             }));
    EXPECT_EQ(std::count_if(transport.accesses().begin(), transport.accesses().end(),
                            [](const Access& access) {
                                return access.write && access.offset == kClearDone;
                            }),
              1);
}

TEST(HwFlashDevice, WaitsForActiveHighDoneBit16OnReadCommands) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_reads(kTriggerStatus, {0, kDone});
    transport.preset(kReadRam, 0);
    std::array<uint8_t, 4> data{};
    FlashDevice device(transport);

    const auto result =
        device.read_data(0, {data.data(), data.size()}, Timeout::after_us(1000));

    ASSERT_TRUE(result);
    EXPECT_EQ(std::count_if(transport.accesses().begin(), transport.accesses().end(),
                            [](const Access& access) {
                                return !access.write && access.offset == kTriggerStatus;
                            }),
              2);
}

TEST(HwFlashDevice, TimesOutBeforeClearingDoneOrReadingResultRam) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_reads(kTriggerStatus, {0});
    std::array<uint8_t, 4> data{};
    FlashDevice device(transport);

    const auto result = device.read_data(0, {data.data(), data.size()}, Timeout::poll());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::Timeout);
    EXPECT_FALSE(has_write(transport.accesses(), kClearDone, 1));
    EXPECT_FALSE(std::any_of(transport.accesses().begin(), transport.accesses().end(),
                             [](const Access& access) {
                                 return !access.write && access.offset == kReadRam;
                             }));
}

TEST(HwFlashDevice, RejectsUnsafeTransferShapesWithoutHardwareAccess) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    FlashDevice device(transport);
    std::array<uint8_t, 3> unaligned{};

    const auto null_read = device.read_data(0, {nullptr, 4}, Timeout::poll());
    const auto unaligned_program =
        device.program(0, {unaligned.data(), unaligned.size()}, Timeout::poll());

    ASSERT_FALSE(null_read);
    EXPECT_EQ(null_read.status().code, StatusCode::InvalidArgument);
    ASSERT_FALSE(unaligned_program);
    EXPECT_EQ(unaligned_program.status().code, StatusCode::InvalidArgument);
    EXPECT_TRUE(transport.accesses().empty());
}

TEST(HwFlashDevice, ReadsAndWritesClockDivider) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.preset(kClockDivider, 0x12340010u);
    FlashDevice device(transport);

    const auto divider = device.read_clock_divider();
    const auto valid = device.set_clock_divider(32);

    ASSERT_TRUE(divider);
    EXPECT_EQ(divider.value(), 16u);
    EXPECT_TRUE(valid);
    EXPECT_TRUE(has_write(transport.accesses(), kClockDivider, 32));
}
