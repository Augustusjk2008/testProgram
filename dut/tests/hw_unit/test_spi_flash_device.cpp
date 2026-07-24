#include <gtest/gtest.h>

#include "MB_DDF_HW/Device/SpiFlashDevice.h"
#include "MB_DDF_HW/Transport/SpidevTransport.h"
#include "hw_unit/support/RecordingSpiTransport.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

namespace {

void queue_write_enable(RecordingSpiTransport& transport, uint8_t status = 0x02) {
    transport.queue_response({0x00});
    transport.queue_response({0x00, status});
}

void queue_successful_mutation(RecordingSpiTransport& transport) {
    transport.queue_response({0x00}); // CLFSR
    queue_write_enable(transport);
}

void queue_ready_and_write_disabled(RecordingSpiTransport& transport) {
    transport.queue_response({0x00, 0x80}); // RDFSR: ready
    transport.queue_response({0x00, 0x00}); // RDSR: WEL cleared
}

} // namespace

TEST(HwSpiFlashDevice, RequiresAnOpenTransport) {
    RecordingSpiTransport transport;
    SpiFlashDevice flash(transport);

    const auto result = flash.read_jedec_id();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::NotOpen);
    EXPECT_TRUE(transport.transfers().empty());
}

TEST(HwSpiFlashDevice, ReadsMicronIdentificationAndStatusRegisters) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00, 0x20, 0xBA, 0x20});
    transport.queue_response({0x00, 0x02});
    transport.queue_response({0x00, 0x80});
    SpiFlashDevice flash(transport);

    const auto id = flash.read_jedec_id();
    const auto status = flash.read_status();
    const auto flag_status = flash.read_flag_status();

    ASSERT_TRUE(id);
    EXPECT_EQ(id.value(), SpiFlashDevice::ExpectedJedecId);
    ASSERT_TRUE(status);
    EXPECT_EQ(status.value(), 0x02);
    ASSERT_TRUE(flag_status);
    EXPECT_EQ(flag_status.value(), 0x80);
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x9F, 0xFF, 0xFF, 0xFF},
                                                 {0x05, 0xFF},
                                                 {0x70, 0xFF}}));
}

TEST(HwSpiFlashDevice, PermanentlyDisablesHoldThroughNonvolatileConfiguration) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00});
    transport.queue_response({0x00, 0x00, 0x00});
    SpiFlashDevice flash(transport);

    const auto result = flash.disable_hold();

    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x06},
                                                 {0xB1, 0xEF, 0xFF}}));
}

TEST(HwSpiFlashDevice, WriteEnableRequiresWelToBecomeSet) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    queue_write_enable(transport, 0x00);
    SpiFlashDevice flash(transport);

    const auto result = flash.write_enable();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::ProtocolError);
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x06}, {0x05, 0xFF}}));
}

TEST(HwSpiFlashDevice, WriteDisableRequiresWelToClear) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00});
    transport.queue_response({0x00, 0x00});
    SpiFlashDevice flash(transport);

    const auto result = flash.write_disable();

    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x04}, {0x05, 0xFF}}));
}

TEST(HwSpiFlashDevice, ErasesOneSubsectorWithDedicatedFourByteOpcode) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    queue_successful_mutation(transport);
    transport.queue_response({0x00, 0x00, 0x00, 0x00, 0x00});
    queue_ready_and_write_disabled(transport);
    SpiFlashDevice flash(transport);

    const auto result = flash.erase_subsector(0x03FFF000u, Timeout::poll());

    ASSERT_TRUE(result) << result.status().message;
    EXPECT_TRUE(flash.last_mutation_command_attempted());
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x50},
                                                 {0x06},
                                                 {0x05, 0xFF},
                                                 {0x21, 0x03, 0xFF, 0xF0, 0x00},
                                                 {0x70, 0xFF},
                                                 {0x05, 0xFF}}));
}

TEST(HwSpiFlashDevice, DoesNotReportMutationWhenWriteEnableFails) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00});       // CLFSR
    queue_write_enable(transport, 0x00);    // WEL remains clear
    SpiFlashDevice flash(transport);

    const auto result = flash.erase_subsector(0x00001000u, Timeout::poll());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::ProtocolError);
    EXPECT_FALSE(flash.last_mutation_command_attempted());
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x50},
                                                 {0x06},
                                                 {0x05, 0xFF}}));
}

TEST(HwSpiFlashDevice, DoesNotReportMutationWhenPreparationTransferFails) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_failure(
        Status::error(StatusCode::IoError, EIO, "CLFSR transfer failed"));
    SpiFlashDevice flash(transport);

    const auto result = flash.erase_subsector(0x00001000u, Timeout::poll());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::IoError);
    EXPECT_FALSE(flash.last_mutation_command_attempted());
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x50}}));
}

TEST(HwSpiFlashDevice, ReportsMutationWhenEraseTransferItselfFails) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    queue_successful_mutation(transport);
    transport.queue_failure(
        Status::error(StatusCode::IoError, EINTR, "erase transfer interrupted"));
    SpiFlashDevice flash(transport);

    const auto result = flash.erase_subsector(0x00001000u, Timeout::poll());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::IoError);
    EXPECT_TRUE(flash.last_mutation_command_attempted());
    ASSERT_EQ(transport.transfers().size(), 4u);
    EXPECT_EQ(transport.transfers().back(),
              (std::vector<uint8_t>{0x21, 0x00, 0x00, 0x10, 0x00}));
}

TEST(HwSpiFlashDevice, ProgramsOnePageWithDedicatedFourByteOpcode) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    queue_successful_mutation(transport);
    transport.queue_response({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    queue_ready_and_write_disabled(transport);
    const std::array<uint8_t, 3> data{0x12, 0x34, 0x56};
    SpiFlashDevice flash(transport);

    const auto result =
        flash.program_page(0x02000100u, {data.data(), data.size()}, Timeout::poll());

    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(result.value(), data.size());
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x50},
                                                 {0x06},
                                                 {0x05, 0xFF},
                                                 {0x12, 0x02, 0x00, 0x01, 0x00,
                                                  0x12, 0x34, 0x56},
                                                 {0x70, 0xFF},
                                                 {0x05, 0xFF}}));
}

TEST(HwSpiFlashDevice, ReadsWithDedicatedFourByteOpcode) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00, 0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF});
    std::array<uint8_t, 4> data{};
    SpiFlashDevice flash(transport);

    const auto result = flash.read(0x01234567u, {data.data(), data.size()});

    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(result.value(), data.size());
    EXPECT_EQ(data, (std::array<uint8_t, 4>{0xDE, 0xAD, 0xBE, 0xEF}));
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x13, 0x01, 0x23, 0x45, 0x67,
                                                  0x00, 0x00, 0x00, 0x00}}));
}

TEST(HwSpiFlashDevice, SplitsReadAtTheTwoDieBoundary) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22});
    transport.queue_response({0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0x44});
    std::array<uint8_t, 4> data{};
    SpiFlashDevice flash(transport);

    const auto result = flash.read(0x01FFFFFEu, {data.data(), data.size()});

    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(data, (std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0x44}));
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x13, 0x01, 0xFF, 0xFF, 0xFE,
                                                  0x00, 0x00},
                                                 {0x13, 0x02, 0x00, 0x00, 0x00,
                                                  0x00, 0x00}}));
}

TEST(HwSpiFlashDevice, ReportsFlagStatusFailures) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    queue_successful_mutation(transport);
    transport.queue_response({0x00, 0x00, 0x00, 0x00, 0x00});
    transport.queue_response({0x00, 0xA0}); // ready + erase failure
    SpiFlashDevice flash(transport);

    const auto result = flash.erase_subsector(0x00001000u, Timeout::poll());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::HardwareFault);
    EXPECT_TRUE(flash.last_mutation_command_attempted());
}

TEST(HwSpiFlashDevice, TimesOutWhileFlagStatusRemainsBusy) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00, 0x00});
    SpiFlashDevice flash(transport);

    const auto result = flash.wait_until_ready(Timeout::poll());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::Timeout);
}

TEST(HwSpiFlashDevice, DefersFlagErrorUntilTheOperationIsReady) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00, 0x20}); // busy, stale erase-error bit
    transport.queue_response({0x00, 0x80}); // ready, no operation error
    SpiFlashDevice flash(transport);

    const auto result = flash.wait_until_ready(Timeout::after_us(10'000));

    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x70, 0xFF},
                                                 {0x70, 0xFF}}));
}

TEST(HwSpiFlashDevice, WaitsForBothDiesUsingSeparateChipSelectCycles) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00, 0x80}); // first polling round: die 0 ready
    transport.queue_response({0x00, 0x00}); // first polling round: die 1 busy
    transport.queue_response({0x00, 0x80}); // second polling round: die 0 ready
    transport.queue_response({0x00, 0x80}); // second polling round: die 1 ready
    SpiFlashDevice flash(transport);

    const auto result =
        flash.wait_until_all_dies_idle(Timeout::after_us(10'000));

    ASSERT_TRUE(result) << result.status().message;
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x70, 0xFF},
                                                 {0x70, 0xFF},
                                                 {0x70, 0xFF},
                                                 {0x70, 0xFF}}));
}

TEST(HwSpiFlashDevice, DoesNotAcceptOnlyOneReadyDie) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    transport.queue_response({0x00, 0x80});
    transport.queue_response({0x00, 0x00});
    SpiFlashDevice flash(transport);

    const auto result = flash.wait_until_all_dies_idle(Timeout::poll());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::Timeout);
    EXPECT_EQ(transport.transfers(),
              (std::vector<std::vector<uint8_t>>{{0x70, 0xFF},
                                                 {0x70, 0xFF}}));
}

TEST(HwSpiFlashDevice, RejectsUnsafeRangesBeforeUsingHardware) {
    RecordingSpiTransport transport;
    ASSERT_TRUE(transport.open());
    SpiFlashDevice flash(transport);
    std::array<uint8_t, 4> data{};

    const auto unaligned_erase = flash.erase_subsector(1, Timeout::poll());
    const auto crossing_page =
        flash.program_page(0xFFu, {data.data(), data.size()}, Timeout::poll());
    const auto outside_capacity =
        flash.read(SpiFlashDevice::CapacityBytes - 1,
                   {data.data(), data.size()});
    const auto null_program = flash.program_page(0, {nullptr, 1}, Timeout::poll());

    EXPECT_FALSE(unaligned_erase);
    EXPECT_EQ(unaligned_erase.status().code, StatusCode::InvalidArgument);
    EXPECT_FALSE(crossing_page);
    EXPECT_EQ(crossing_page.status().code, StatusCode::InvalidArgument);
    EXPECT_FALSE(outside_capacity);
    EXPECT_EQ(outside_capacity.status().code, StatusCode::InvalidArgument);
    EXPECT_FALSE(null_program);
    EXPECT_EQ(null_program.status().code, StatusCode::InvalidArgument);
    EXPECT_TRUE(transport.transfers().empty());
}

TEST(HwSpiFlashHardware, ReadsExpectedJedecIdWithoutWriting) {
    const char* enabled = std::getenv("MB_DDF_HW_SPI_FLASH_READ_ID_TEST");
    if (enabled == nullptr || std::string(enabled) != "1") {
        GTEST_SKIP() << "set MB_DDF_HW_SPI_FLASH_READ_ID_TEST=1 on the target board";
    }

    SpidevTransport transport;
    const auto opened = transport.open();
    if (!opened) {
        FAIL() << opened.status().message << ", errno=" << opened.status().errno_value;
        return;
    }

    SpiFlashDevice flash(transport);
    const auto id = flash.read_jedec_id();
    EXPECT_TRUE(id) << (id ? "" : id.status().message);
    if (id) {
        EXPECT_EQ(id.value(), SpiFlashDevice::ExpectedJedecId);
    }
    const auto restored = transport.restore_configuration();
    EXPECT_TRUE(restored) << (restored ? "" : restored.status().message);
    transport.close();
}

TEST(HwSpiFlashHardware, ReadsConfiguredAddressWithoutWriting) {
    const char* enabled = std::getenv("MB_DDF_HW_SPI_FLASH_READ_ADDRESS_TEST");
    if (enabled == nullptr || std::string(enabled) != "1") {
        GTEST_SKIP() << "set MB_DDF_HW_SPI_FLASH_READ_ADDRESS_TEST=1 on the target board";
    }

    constexpr uint32_t address =
        SpiFlashDevice::CapacityBytes - SpiFlashDevice::SubsectorSize;
    SpidevTransport transport;
    const auto opened = transport.open();
    if (!opened) {
        FAIL() << opened.status().message << ", errno=" << opened.status().errno_value;
        return;
    }

    SpiFlashDevice flash(transport);
    std::array<uint8_t, 16> data{};
    const auto read = flash.read(address, {data.data(), data.size()});
    const auto status = flash.read_status();
    const auto flags = flash.read_flag_status();
    EXPECT_TRUE(read) << (read ? "" : read.status().message);
    EXPECT_TRUE(status) << (status ? "" : status.status().message);
    EXPECT_TRUE(flags) << (flags ? "" : flags.status().message);
    if (read && status && flags) {
        std::cout << "address=0x" << std::hex << address << ", data=";
        for (const auto byte : data) {
            std::cout << static_cast<unsigned>(byte) << ' ';
        }
        std::cout << ", status=0x" << static_cast<unsigned>(status.value())
                  << ", flags=0x" << static_cast<unsigned>(flags.value())
                  << std::dec << '\n';
        EXPECT_NE(status.value(), 0xFF);
        EXPECT_EQ(flags.value() & 0x80u, 0x80u);
        EXPECT_EQ(flags.value() & 0x32u, 0u);
    }
    const auto restored = transport.restore_configuration();
    EXPECT_TRUE(restored) << (restored ? "" : restored.status().message);
    transport.close();
}
