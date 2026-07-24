#include "MB_DDF_HW/Device/FlashDevice.h"
#include "MB_DDF_HW/Device/Registers/FlashRegisters.h"

#include <chrono>
#include <thread>

namespace MB_DDF::HW {
namespace R = Registers::Flash;

Result<void> FlashDevice::check_communication() const {
    if (!transport_.is_open()) {
        return Status::error(StatusCode::NotOpen, 0, "Flash transport is not open");
    }
    return {};
}

Result<FlashControllerStatus> FlashDevice::read_controller_status() const {
    auto value = transport_.read32(R::TriggerStatus);
    if (!value) {
        return value.status();
    }
    return FlashControllerStatus{value.value(),
                                 (value.value() & R::CompletionMask) != 0};
}

Result<uint32_t> FlashDevice::read_clock_divider() const {
    auto value = transport_.read32(R::ClockDivider);
    if (!value) {
        return value.status();
    }
    return value.value() & 0xFFu;
}

Result<void> FlashDevice::set_clock_divider(uint32_t divider) {
    if (divider == 0 || divider > 0xFFu) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "Flash clock divider must be in range 1..255");
    }
    return transport_.write32(R::ClockDivider, divider);
}

Result<void> FlashDevice::validate_transfer(const void* data, size_t size) const {
    if (size == 0) {
        return {};
    }
    if (data == nullptr) {
        return Status::error(StatusCode::InvalidArgument, 0, "Flash buffer is null");
    }
    if (size > MaxTransferBytes) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "Flash transfer exceeds the controller RAM window");
    }
    if ((size % sizeof(uint32_t)) != 0) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "Flash transfer size must be a multiple of 4 bytes");
    }
    return {};
}

Result<void> FlashDevice::wait_for_completion(Timeout timeout) const {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(timeout.microseconds);
    while (true) {
        auto status = read_controller_status();
        if (!status) {
            return status.status();
        }
        if (status.value().d16_set) {
            return {};
        }
        if (!timeout.infinite &&
            (timeout.microseconds == 0 || std::chrono::steady_clock::now() >= deadline)) {
            return Status::error(StatusCode::Timeout, 0,
                                 "Flash controller operation timed out");
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

Result<void> FlashDevice::trigger_and_complete(Timeout timeout,
                                               bool clear_after_completion) {
    auto status = transport_.write32(R::TriggerStatus, R::TriggerValue);
    if (!status) {
        return status;
    }
    status = wait_for_completion(timeout);
    if (!status) {
        return status;
    }
    if (!clear_after_completion) {
        return {};
    }
    return transport_.write32(R::ClearDone, R::ClearDoneValue);
}

Result<void> FlashDevice::execute_simple_command(FlashCommand command,
                                                 Timeout timeout,
                                                 bool clear_after_completion) {
    auto status = check_communication();
    if (!status) {
        return status;
    }
    status = transport_.write32(R::Command, static_cast<uint8_t>(command));
    if (!status) {
        return status;
    }
    return trigger_and_complete(timeout, clear_after_completion);
}

Result<void> FlashDevice::write_enable(Timeout timeout) {
    return execute_simple_command(FlashCommand::WriteEnable, timeout, false);
}

Result<void> FlashDevice::chip_erase(Timeout timeout) {
    return execute_simple_command(FlashCommand::ChipErase, timeout, false);
}

Result<uint32_t> FlashDevice::read_flash_status_raw(Timeout timeout) {
    auto status = execute_simple_command(FlashCommand::ReadStatus, timeout, true);
    if (!status) {
        return status.status();
    }
    return transport_.read32(R::ReadRamBase);
}

Result<size_t> FlashDevice::program(uint32_t address, BufferView data, Timeout timeout) {
    auto valid = validate_transfer(data.data, data.size);
    if (!valid) {
        return valid.status();
    }
    if (data.size == 0) {
        return size_t{0};
    }
    auto status = check_communication();
    if (!status) {
        return status.status();
    }

    status = transport_.write32(R::Command, static_cast<uint8_t>(FlashCommand::Program));
    if (!status) {
        return status.status();
    }
    status = transport_.write32(R::FlashAddress, address);
    if (!status) {
        return status.status();
    }
    status = transport_.write32(R::ByteCounts, static_cast<uint16_t>(data.size));
    if (!status) {
        return status.status();
    }
    for (size_t offset = 0; offset < data.size; offset += sizeof(uint32_t)) {
        uint32_t word = 0;
        for (size_t byte = 0; byte < sizeof(uint32_t); ++byte) {
            word |= static_cast<uint32_t>(data.data[offset + byte]) << (byte * 8);
        }
        status = transport_.write32(R::write_ram_word(offset / sizeof(uint32_t)), word);
        if (!status) {
            return status.status();
        }
    }
    status = trigger_and_complete(timeout, true);
    if (!status) {
        return status.status();
    }
    return data.size;
}

Result<size_t> FlashDevice::read_data(uint32_t address,
                                      MutableBufferView data,
                                      Timeout timeout) {
    auto valid = validate_transfer(data.data, data.size);
    if (!valid) {
        return valid.status();
    }
    if (data.size == 0) {
        return size_t{0};
    }
    auto status = check_communication();
    if (!status) {
        return status.status();
    }

    status = transport_.write32(R::Command, static_cast<uint8_t>(FlashCommand::Read));
    if (!status) {
        return status.status();
    }
    status = transport_.write32(R::FlashAddress, address);
    if (!status) {
        return status.status();
    }
    status = transport_.write32(R::ByteCounts,
                                static_cast<uint32_t>(static_cast<uint16_t>(data.size)) << 16);
    if (!status) {
        return status.status();
    }
    status = trigger_and_complete(timeout, true);
    if (!status) {
        return status.status();
    }
    for (size_t offset = 0; offset < data.size; offset += sizeof(uint32_t)) {
        auto word = transport_.read32(R::read_ram_word(offset / sizeof(uint32_t)));
        if (!word) {
            return word.status();
        }
        for (size_t byte = 0; byte < sizeof(uint32_t); ++byte) {
            data.data[offset + byte] =
                static_cast<uint8_t>(word.value() >> (byte * 8));
        }
    }
    return data.size;
}

} // namespace MB_DDF::HW
