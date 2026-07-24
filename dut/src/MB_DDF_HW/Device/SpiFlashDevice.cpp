#include "MB_DDF_HW/Device/SpiFlashDevice.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>
#include <utility>

namespace MB_DDF::HW {
namespace {

constexpr uint8_t kReadId = 0x9F;
constexpr uint8_t kReadStatus = 0x05;
constexpr uint8_t kWriteEnable = 0x06;
constexpr uint8_t kWriteDisable = 0x04;
constexpr uint8_t kReadFlagStatus = 0x70;
constexpr uint8_t kClearFlagStatus = 0x50;
constexpr uint8_t kRead4Byte = 0x13;
constexpr uint8_t kPageProgram4Byte = 0x12;
constexpr uint8_t kSubsectorErase4Byte = 0x21;

constexpr uint8_t kStatusWel = 1u << 1;
constexpr uint8_t kFlagReady = 1u << 7;
constexpr uint8_t kFlagErrorMask = (1u << 5) | (1u << 4) | (1u << 1);
constexpr size_t kAddressCommandBytes = 5;
constexpr size_t kReadChunkBytes = SpiFlashDevice::PageSize;

void append_address(std::vector<uint8_t>& frame, uint32_t address) {
    frame.push_back(static_cast<uint8_t>(address >> 24));
    frame.push_back(static_cast<uint8_t>(address >> 16));
    frame.push_back(static_cast<uint8_t>(address >> 8));
    frame.push_back(static_cast<uint8_t>(address));
}

} // namespace

Result<void> SpiFlashDevice::check_communication() const {
    if (!transport_.is_open()) {
        return Status::error(StatusCode::NotOpen, 0,
                             "SPI Flash transport is not open");
    }
    return {};
}

Result<std::vector<uint8_t>> SpiFlashDevice::exchange(std::vector<uint8_t> tx) {
    auto open = check_communication();
    if (!open) {
        return open.status();
    }
    if (tx.empty()) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI Flash transaction is empty");
    }

    std::vector<uint8_t> rx(tx.size(), 0);
    auto transferred = transport_.transfer({tx.data(), tx.size()},
                                           {rx.data(), rx.size()});
    if (!transferred) {
        return transferred.status();
    }
    if (transferred.value() != tx.size()) {
        return Status::error(StatusCode::ProtocolError, 0,
                             "SPI Flash transaction was incomplete");
    }
    return rx;
}

Result<void> SpiFlashDevice::send_command(uint8_t command) {
    auto response = exchange({command});
    if (!response) {
        return response.status();
    }
    return {};
}

Result<std::vector<uint8_t>> SpiFlashDevice::read_command_bytes(
    uint8_t command, size_t response_size) {
    // 9Fh/05h/70h 都没有地址阶段。SPI 仍需由主机发送占位字节产生 SCK，器件才能
    // 同步移出响应；这些 00h 只是 TX don't-care，不是 Flash 地址或 dummy cycle。
    std::vector<uint8_t> tx(response_size + 1, 0xFF);
    tx[0] = command;
    auto response = exchange(std::move(tx));
    if (!response) {
        return response.status();
    }
    return std::vector<uint8_t>(response.value().begin() + 1,
                                response.value().end());
}

Result<uint8_t> SpiFlashDevice::read_register(uint8_t command) {
    auto response = read_command_bytes(command, 1);
    if (!response) {
        return response.status();
    }
    return response.value()[0];
}

Result<SpiFlashDevice::JedecId> SpiFlashDevice::read_jedec_id() {
    auto response = read_command_bytes(kReadId, ExpectedJedecId.size());
    if (!response) {
        return response.status();
    }
    return JedecId{response.value()[0], response.value()[1], response.value()[2]};
}

Result<uint8_t> SpiFlashDevice::read_status() {
    return read_register(kReadStatus);
}

Result<uint8_t> SpiFlashDevice::read_flag_status() {
    return read_register(kReadFlagStatus);
}

Result<void> SpiFlashDevice::disable_hold() {
    // 故障接管序列要求两次独立 CS 事务，且在 B1h 前不插入状态读取。
    auto enabled = send_command(kWriteEnable);
    if (!enabled) {
        return enabled;
    }
    auto configured = exchange(
        {0xB1, 0xEF, 0xFF});
    if (!configured) {
        return configured.status();
    }
    return {};
}

Result<void> SpiFlashDevice::clear_flag_status() {
    return send_command(kClearFlagStatus);
}

Result<void> SpiFlashDevice::write_enable() {
    auto sent = send_command(kWriteEnable);
    if (!sent) {
        return sent;
    }
    auto status = read_status();
    if (!status) {
        return status.status();
    }
    if ((status.value() & kStatusWel) == 0) {
        return Status::error(StatusCode::ProtocolError, 0,
                             "N25Q512A did not set WEL after WREN");
    }
    return {};
}

Result<void> SpiFlashDevice::write_disable() {
    auto sent = send_command(kWriteDisable);
    if (!sent) {
        return sent;
    }
    auto status = read_status();
    if (!status) {
        return status.status();
    }
    if ((status.value() & kStatusWel) != 0) {
        return Status::error(StatusCode::ProtocolError, 0,
                             "N25Q512A left WEL set after WRDI");
    }
    return {};
}

Result<void> SpiFlashDevice::wait_until_ready(Timeout timeout) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(timeout.microseconds);
    while (true) {
        auto flags = read_flag_status();
        if (!flags) {
            return flags.status();
        }
        if ((flags.value() & kFlagReady) != 0) {
            if ((flags.value() & kFlagErrorMask) != 0) {
                std::ostringstream message;
                message << "N25Q512A flag status reports program/erase/protection failure: 0x"
                        << std::hex << static_cast<unsigned>(flags.value());
                return Status::error(StatusCode::HardwareFault, 0, message.str());
            }
            return {};
        }
        if (!timeout.infinite &&
            (timeout.microseconds == 0 || std::chrono::steady_clock::now() >= deadline)) {
            return Status::error(StatusCode::Timeout, 0,
                                 "N25Q512A operation timed out while flag status remained busy");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

Result<void> SpiFlashDevice::wait_until_all_dies_idle(Timeout timeout) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(timeout.microseconds);
    while (true) {
        // N25Q512A 是两个 256Mb die 的堆叠器件。手册要求上电/未知状态下用
        // 两条独立 70h 命令读取两个 die，并在命令之间翻转 S#；不能在同一
        // CS 周期内多读一个字节来代替。每次 read_flag_status() 都是独立 transfer。
        auto first_die = read_flag_status();
        if (!first_die) {
            return first_die.status();
        }
        auto second_die = read_flag_status();
        if (!second_die) {
            return second_die.status();
        }
        if ((first_die.value() & kFlagReady) != 0 &&
            (second_die.value() & kFlagReady) != 0) {
            return {};
        }
        if (!timeout.infinite &&
            (timeout.microseconds == 0 || std::chrono::steady_clock::now() >= deadline)) {
            return Status::error(
                StatusCode::Timeout, 0,
                "N25Q512A operation timed out while one or both die remained busy");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

Result<void> SpiFlashDevice::prepare_mutation() {
    auto status = clear_flag_status();
    if (!status) {
        return status;
    }
    return write_enable();
}

Result<void> SpiFlashDevice::finish_mutation(Timeout timeout) {
    auto ready = wait_until_ready(timeout);
    if (!ready) {
        return ready;
    }
    auto status = read_status();
    if (!status) {
        return status.status();
    }
    if ((status.value() & kStatusWel) != 0) {
        return Status::error(StatusCode::ProtocolError, 0,
                             "N25Q512A left WEL set after a program/erase operation");
    }
    return {};
}

Result<void> SpiFlashDevice::validate_range(uint32_t address, size_t size) const {
    const uint64_t end = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    if (end > CapacityBytes) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI Flash access exceeds the 64 MiB device capacity");
    }
    return {};
}

Result<void> SpiFlashDevice::erase_subsector(uint32_t address, Timeout timeout) {
    last_mutation_command_attempted_ = false;
    if ((address % SubsectorSize) != 0) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI Flash subsector erase address must be 4 KiB aligned");
    }
    auto valid = validate_range(address, SubsectorSize);
    if (!valid) {
        return valid;
    }
    auto prepared = prepare_mutation();
    if (!prepared) {
        return prepared;
    }

    std::vector<uint8_t> frame;
    frame.reserve(kAddressCommandBytes);
    frame.push_back(kSubsectorErase4Byte);
    append_address(frame, address);
    // 从 ioctl 调用前起，任何错误都不能证明器件没有接收到擦除命令。
    last_mutation_command_attempted_ = true;
    auto erased = exchange(std::move(frame));
    if (!erased) {
        return erased.status();
    }
    return finish_mutation(timeout);
}

Result<size_t> SpiFlashDevice::program_page(uint32_t address, BufferView data,
                                            Timeout timeout) {
    last_mutation_command_attempted_ = false;
    if (data.data == nullptr || data.size == 0 || data.size > PageSize) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI Flash page program requires 1..256 bytes");
    }
    if ((address % PageSize) + data.size > PageSize) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI Flash page program must not cross a page boundary");
    }
    auto valid = validate_range(address, data.size);
    if (!valid) {
        return valid.status();
    }
    auto prepared = prepare_mutation();
    if (!prepared) {
        return prepared.status();
    }

    std::vector<uint8_t> frame;
    frame.reserve(kAddressCommandBytes + data.size);
    frame.push_back(kPageProgram4Byte);
    append_address(frame, address);
    frame.insert(frame.end(), data.data, data.data + data.size);
    // 即使 ioctl 返回 EINTR/短传，也必须按“编程可能已开始”处理。
    last_mutation_command_attempted_ = true;
    auto programmed = exchange(std::move(frame));
    if (!programmed) {
        return programmed.status();
    }
    auto finished = finish_mutation(timeout);
    if (!finished) {
        return finished.status();
    }
    return data.size;
}

Result<size_t> SpiFlashDevice::read(uint32_t address, MutableBufferView data) {
    if (data.size != 0 && data.data == nullptr) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI Flash read buffer is null");
    }
    auto valid = validate_range(address, data.size);
    if (!valid) {
        return valid.status();
    }
    if (data.size == 0) {
        return size_t{0};
    }

    size_t completed = 0;
    while (completed < data.size) {
        const uint32_t current = address + static_cast<uint32_t>(completed);
        const uint32_t die_end =
            ((current / DieSizeBytes) + 1u) * DieSizeBytes;
        const size_t bytes_to_die_end = static_cast<size_t>(die_end - current);
        const size_t chunk = std::min({kReadChunkBytes,
                                       data.size - completed,
                                       bytes_to_die_end});

        std::vector<uint8_t> frame(kAddressCommandBytes + chunk, 0);
        frame[0] = kRead4Byte;
        frame[1] = static_cast<uint8_t>(current >> 24);
        frame[2] = static_cast<uint8_t>(current >> 16);
        frame[3] = static_cast<uint8_t>(current >> 8);
        frame[4] = static_cast<uint8_t>(current);
        auto response = exchange(std::move(frame));
        if (!response) {
            return response.status();
        }
        std::copy_n(response.value().begin() + kAddressCommandBytes, chunk,
                    data.data + completed);
        completed += chunk;
    }
    return completed;
}

} // namespace MB_DDF::HW
