#pragma once

#include "MB_DDF_HW/Device/SpiFlashDevice.h"
#include "MB_DDF_HW/Transport/ISpiTransport.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace MB_DDF::HW::Test {

/// 只模拟 Demo 所用 N25Q512A 命令的内存 SPI Transport，用于完整恢复流程测试。
class MemorySpiFlashTransport final : public ISpiTransport {
public:
    explicit MemorySpiFlashTransport(uint32_t base_address)
        : base_address_(base_address) {
        storage_.fill(0xFF);
    }

    Result<void> open() override {
        open_ = true;
        return {};
    }

    void close() noexcept override {
        open_ = false;
    }

    bool is_open() const override {
        return open_;
    }

    void set_jedec_id(SpiFlashDevice::JedecId id) {
        jedec_id_ = id;
    }

    void set_register_read_override(uint8_t status, uint8_t flag_status) {
        status_override_ = status;
        flag_status_override_ = flag_status;
    }

    void set_flag_status_sequence(std::vector<uint8_t> values) {
        flag_status_sequence_ = std::move(values);
        flag_status_sequence_index_ = 0;
    }

    void set_contents(const std::array<uint8_t, SpiFlashDevice::SubsectorSize>& data) {
        storage_ = data;
    }

    const std::array<uint8_t, SpiFlashDevice::SubsectorSize>& contents() const {
        return storage_;
    }

    void fail_opcode_on_occurrence(uint8_t opcode, unsigned occurrence,
                                   bool apply_side_effect) {
        failure_ = FailureRule{opcode, occurrence, apply_side_effect, 0, false};
    }

    size_t mutation_count() const {
        return mutation_count_;
    }

    size_t erase_count() const {
        return erase_count_;
    }

    size_t read_count() const {
        return read_count_;
    }

    size_t jedec_read_count() const {
        return jedec_read_count_;
    }

    Result<size_t> transfer(BufferView tx, MutableBufferView rx) override {
        if (!open_) {
            return Status::error(StatusCode::NotOpen, 0, "memory SPI is not open");
        }
        if (tx.size == 0 || tx.size != rx.size || tx.data == nullptr || rx.data == nullptr) {
            return Status::error(StatusCode::InvalidArgument, 0,
                                 "invalid memory SPI transfer");
        }
        std::fill_n(rx.data, rx.size, uint8_t{0});

        const uint8_t opcode = tx.data[0];
        bool fail_this_transfer = false;
        bool apply_side_effect = false;
        if (failure_ && !failure_->consumed && failure_->opcode == opcode) {
            ++failure_->seen;
            if (failure_->seen == failure_->occurrence) {
                failure_->consumed = true;
                fail_this_transfer = true;
                apply_side_effect = failure_->apply_side_effect;
            }
        }
        if (fail_this_transfer && !apply_side_effect) {
            return Status::error(StatusCode::IoError, EIO,
                                 "injected SPI command failure");
        }

        auto command = execute_command(opcode, tx, rx);
        if (!command) {
            return command.status();
        }
        if (fail_this_transfer) {
            return Status::error(StatusCode::IoError, EINTR,
                                 "injected SPI command failure after side effect");
        }
        return tx.size;
    }

private:
    struct FailureRule {
        uint8_t opcode;
        unsigned occurrence;
        bool apply_side_effect;
        unsigned seen;
        bool consumed;
    };

    static uint32_t decode_address(BufferView tx) {
        return (static_cast<uint32_t>(tx.data[1]) << 24) |
               (static_cast<uint32_t>(tx.data[2]) << 16) |
               (static_cast<uint32_t>(tx.data[3]) << 8) |
               static_cast<uint32_t>(tx.data[4]);
    }

    bool contains(uint32_t address, size_t size) const {
        const uint64_t begin = address;
        const uint64_t end = begin + size;
        return begin >= base_address_ &&
               end <= static_cast<uint64_t>(base_address_) + storage_.size();
    }

    Result<void> execute_command(uint8_t opcode, BufferView tx, MutableBufferView rx) {
        switch (opcode) {
        case 0x9F:
            if (tx.size != 4) {
                break;
            }
            std::copy(jedec_id_.begin(), jedec_id_.end(), rx.data + 1);
            ++jedec_read_count_;
            return {};
        case 0x05:
            if (tx.size != 2) {
                break;
            }
            rx.data[1] = status_override_.value_or(write_enabled_ ? 0x02 : 0x00);
            return {};
        case 0x70:
            if (tx.size != 2) {
                break;
            }
            if (flag_status_override_) {
                rx.data[1] = *flag_status_override_;
            } else if (!flag_status_sequence_.empty()) {
                const size_t index = std::min(flag_status_sequence_index_,
                                              flag_status_sequence_.size() - 1);
                rx.data[1] = flag_status_sequence_[index];
                if (flag_status_sequence_index_ + 1 < flag_status_sequence_.size()) {
                    ++flag_status_sequence_index_;
                }
            } else {
                rx.data[1] = flag_status_;
            }
            return {};
        case 0x50:
            flag_status_ = 0x80;
            return {};
        case 0x06:
            write_enabled_ = true;
            return {};
        case 0x04:
            write_enabled_ = false;
            return {};
        case 0xB1:
            if (tx.size != 3 || !write_enabled_ || tx.data[1] != 0xEF ||
                tx.data[2] != 0xFF) {
                break;
            }
            write_enabled_ = false;
            return {};
        case 0x21:
            if (tx.size != 5 || !contains(decode_address(tx), storage_.size())) {
                break;
            }
            if (!write_enabled_) {
                flag_status_ = 0xA0;
                return {};
            }
            storage_.fill(0xFF);
            write_enabled_ = false;
            flag_status_ = 0x80;
            ++mutation_count_;
            ++erase_count_;
            return {};
        case 0x12: {
            if (tx.size <= 5) {
                break;
            }
            const uint32_t address = decode_address(tx);
            const size_t data_size = tx.size - 5;
            if (!contains(address, data_size)) {
                break;
            }
            if (!write_enabled_) {
                flag_status_ = 0x90;
                return {};
            }
            const size_t offset = static_cast<size_t>(address - base_address_);
            for (size_t index = 0; index < data_size; ++index) {
                storage_[offset + index] &= tx.data[5 + index];
            }
            write_enabled_ = false;
            flag_status_ = 0x80;
            ++mutation_count_;
            return {};
        }
        case 0x13: {
            if (tx.size <= 5) {
                break;
            }
            if (!flag_status_sequence_.empty()) {
                const size_t index = std::min(flag_status_sequence_index_,
                                              flag_status_sequence_.size() - 1);
                if ((flag_status_sequence_[index] & 0x80u) == 0) {
                    return Status::error(StatusCode::HardwareFault, 0,
                                         "READ issued while simulated Flash die is busy");
                }
            }
            const uint32_t address = decode_address(tx);
            const size_t data_size = tx.size - 5;
            if (!contains(address, data_size)) {
                break;
            }
            const size_t offset = static_cast<size_t>(address - base_address_);
            std::copy_n(storage_.begin() + static_cast<std::ptrdiff_t>(offset),
                        data_size, rx.data + 5);
            ++read_count_;
            return {};
        }
        default:
            break;
        }
        return Status::error(StatusCode::ProtocolError, 0,
                             "unsupported or malformed memory SPI command");
    }

    bool open_{false};
    bool write_enabled_{false};
    uint8_t flag_status_{0x80};
    uint32_t base_address_;
    SpiFlashDevice::JedecId jedec_id_{SpiFlashDevice::ExpectedJedecId};
    std::array<uint8_t, SpiFlashDevice::SubsectorSize> storage_{};
    std::optional<FailureRule> failure_;
    std::optional<uint8_t> status_override_;
    std::optional<uint8_t> flag_status_override_;
    std::vector<uint8_t> flag_status_sequence_{};
    size_t flag_status_sequence_index_{0};
    size_t mutation_count_{0};
    size_t erase_count_{0};
    size_t read_count_{0};
    size_t jedec_read_count_{0};
};

} // namespace MB_DDF::HW::Test
