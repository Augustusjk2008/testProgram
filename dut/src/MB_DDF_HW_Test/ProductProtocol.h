#pragma once

#include "ProductProtocolGenerated.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace MB_DDF::HWTest {

enum class ProductErrorCode : uint16_t {
    Ok = 0x0000,
    CmdUnknown = 0x0001,
    LenMismatch = 0x0002,
    CrcError = 0x0003,
    ChannelInvalid = 0x0101,
    ParamOutOfRange = 0x0102,
    SafetyLimit = 0x0103,
    RegReadWriteFailed = 0x0201,
    MemoryAccessFailed = 0x0202,
    TaskExecFailed = 0x0203,
    TaskBusy = 0x0204,
    HelmDdsFailed = 0x0301,
};

struct ProductParseError {
    ProductErrorCode code{ProductErrorCode::Ok};
    uint8_t orig_type_group{0};
    uint8_t orig_sub_type{0};
    uint16_t orig_sequence{0};
    uint32_t detail{0};
};

class ProductMessage {
public:
    ProductMessage() = default;

    static ProductMessage from_descriptor(std::string_view name,
                                          std::span<const uint8_t> bytes);

    explicit operator bool() const noexcept { return descriptor_ != nullptr; }
    std::string_view name() const noexcept;
    std::span<const uint8_t> bytes() const noexcept { return bytes_; }
    std::span<uint8_t> mutable_bytes() noexcept { return bytes_; }

    bool set_unsigned(std::string_view field_name, uint64_t value);
    bool set_signed(std::string_view field_name, int64_t value);
    bool set_scaled_signed(std::string_view field_name, double value);
    bool set_float(std::string_view field_name, float value);
    std::optional<uint64_t> get_unsigned(std::string_view field_name) const;
    std::optional<int64_t> get_signed(std::string_view field_name) const;
    std::optional<float> get_float(std::string_view field_name) const;
    std::optional<size_t> data_offset(std::string_view field_name) const;
    bool reserved_bytes_are_zero() const;

    const GeneratedProductProtocol::MessageDescriptor* descriptor() const noexcept {
        return descriptor_;
    }

private:
    friend class ProductProtocol;
    ProductMessage(const GeneratedProductProtocol::MessageDescriptor* descriptor,
                   std::vector<uint8_t> bytes)
        : descriptor_(descriptor), bytes_(std::move(bytes)) {}

    const GeneratedProductProtocol::FieldDescriptor* find_field(
        std::string_view field_name) const noexcept;

    const GeneratedProductProtocol::MessageDescriptor* descriptor_{nullptr};
    std::vector<uint8_t> bytes_{};
};

class ProductParseResult {
public:
    explicit ProductParseResult(ProductMessage message)
        : message_(std::move(message)) {}
    explicit ProductParseResult(ProductParseError error) : error_(error) {}

    explicit operator bool() const noexcept { return message_.has_value(); }
    const ProductMessage& message() const { return *message_; }
    ProductMessage& message() { return *message_; }
    const ProductParseError& error() const noexcept { return error_; }

private:
    std::optional<ProductMessage> message_{};
    ProductParseError error_{};
};

class ProductProtocol {
public:
    explicit ProductProtocol(uint16_t initial_transmit_sequence = 0)
        : transmit_sequence_(initial_transmit_sequence) {}

    ProductProtocol(const ProductProtocol&) = delete;
    ProductProtocol& operator=(const ProductProtocol&) = delete;

    ProductMessage create_message(std::string_view name,
                                  bool allocate_transmit_sequence = true);
    bool assign_transmit_sequence(ProductMessage& message);
    ProductParseResult parse_request(std::span<const uint8_t> data_segment) const;
    ProductMessage create_error_response(const ProductParseError& error,
                                         bool allocate_transmit_sequence = true);

private:
    uint16_t next_transmit_sequence() noexcept;
    std::atomic<uint16_t> transmit_sequence_;
};

} // namespace MB_DDF::HWTest
