#include <gtest/gtest.h>

#include "MB_DDF_Demo/HardwareExamples.h"
#include "hw_unit/support/MemorySpiFlashTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>

using namespace MB_DDF;
using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

namespace {

constexpr uint32_t kTestAddress = 0x00001000u;

std::array<uint8_t, SpiFlashDevice::SubsectorSize> original_contents() {
    std::array<uint8_t, SpiFlashDevice::SubsectorSize> data{};
    for (size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<uint8_t>((index * 37u + 3u) & 0xFFu);
    }
    return data;
}

} // namespace

TEST(HwSpiFlashWorkflow, RestoresTheCompleteSubsectorAfterSuccessfulTest) {
    MemorySpiFlashTransport transport(kTestAddress);
    const auto original = original_contents();
    transport.set_contents(original);
    ASSERT_TRUE(transport.open());

    const bool result =
        Demo::TestHooks::run_spi_flash_workflow(transport, kTestAddress);

    EXPECT_TRUE(result);
    EXPECT_EQ(transport.contents(), original);
    EXPECT_EQ(transport.erase_count(), 2u);
    EXPECT_GT(transport.mutation_count(), transport.erase_count());
    EXPECT_EQ(transport.jedec_read_count(), 1u);
}

TEST(HwSpiFlashWorkflow, RejectsUnexpectedJedecIdBeforeAddressedRead) {
    MemorySpiFlashTransport transport(kTestAddress);
    const auto original = original_contents();
    transport.set_contents(original);
    transport.set_jedec_id({0xEF, 0x40, 0x19});
    ASSERT_TRUE(transport.open());

    const bool result =
        Demo::TestHooks::run_spi_flash_workflow(transport, kTestAddress);

    EXPECT_FALSE(result);
    EXPECT_EQ(transport.jedec_read_count(), 1u);
    EXPECT_EQ(transport.mutation_count(), 0u);
    EXPECT_EQ(transport.read_count(), 0u);
    EXPECT_EQ(transport.contents(), original);
}

TEST(HwSpiFlashWorkflow, WaitsForBothDiesBeforeFirstAddressedRead) {
    MemorySpiFlashTransport transport(kTestAddress);
    const auto original = original_contents();
    transport.set_contents(original);
    // 上电接管时：第一轮 die0 Busy、die1 Ready；第二轮两个 die 均 Ready。
    transport.set_flag_status_sequence({0x00, 0x80, 0x80, 0x80});
    ASSERT_TRUE(transport.open());

    const bool result =
        Demo::TestHooks::run_spi_flash_workflow(transport, kTestAddress);

    EXPECT_TRUE(result);
    EXPECT_GT(transport.mutation_count(), 0u);
    EXPECT_GT(transport.read_count(), 0u);
    EXPECT_EQ(transport.contents(), original);
}

TEST(HwSpiFlashWorkflow, RejectsAllHighStatusBeforeMutation) {
    MemorySpiFlashTransport transport(kTestAddress);
    const auto original = original_contents();
    transport.set_contents(original);
    transport.set_register_read_override(0xFF, 0xFF);
    ASSERT_TRUE(transport.open());

    const bool result =
        Demo::TestHooks::run_spi_flash_workflow(transport, kTestAddress);

    EXPECT_FALSE(result);
    EXPECT_EQ(transport.mutation_count(), 0u);
    EXPECT_EQ(transport.read_count(), 0u);
    EXPECT_EQ(transport.contents(), original);
}

TEST(HwSpiFlashWorkflow, RestoresDataAfterTheTestProgramIoctlFails) {
    MemorySpiFlashTransport transport(kTestAddress);
    const auto original = original_contents();
    transport.set_contents(original);
    transport.fail_opcode_on_occurrence(0x12, 1, true);
    ASSERT_TRUE(transport.open());

    const bool result =
        Demo::TestHooks::run_spi_flash_workflow(transport, kTestAddress);

    EXPECT_FALSE(result);
    EXPECT_EQ(transport.contents(), original);
}

TEST(HwSpiFlashWorkflow, RecoveryRetryRestoresDataButDoesNotHideFailure) {
    MemorySpiFlashTransport transport(kTestAddress);
    const auto original = original_contents();
    transport.set_contents(original);
    // 第一次12h是测试图样，第二次12h是首个恢复页。
    transport.fail_opcode_on_occurrence(0x12, 2, true);
    ASSERT_TRUE(transport.open());

    const bool result =
        Demo::TestHooks::run_spi_flash_workflow(transport, kTestAddress);

    EXPECT_FALSE(result);
    EXPECT_EQ(transport.contents(), original);
    EXPECT_EQ(transport.erase_count(), 3u);
}
