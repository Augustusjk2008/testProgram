#include "MB_DDF_HW_Test/ProductProtocol.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace MB_DDF::HWTest {
namespace Generated = GeneratedProductProtocol;
namespace {

uint64_t width_mask(size_t byte_length) {
    return byte_length >= sizeof(uint64_t)
               ? std::numeric_limits<uint64_t>::max()
               : ((uint64_t{1} << (byte_length * 8)) - 1u);
}

bool parse_unsigned_default(const char* text, uint64_t& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_float_default(const char* text, float& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

ProductParseError header_error(std::span<const uint8_t> bytes,
                               ProductErrorCode code, uint32_t detail) {
    ProductParseError error{};
    error.code = code;
    error.detail = detail;
    if (bytes.size() > 1) {
        error.orig_type_group = bytes[1];
    }
    if (bytes.size() > 2) {
        error.orig_sub_type = bytes[2];
    }
    if (bytes.size() > 4) {
        error.orig_sequence = static_cast<uint16_t>(bytes[3]) |
                              (static_cast<uint16_t>(bytes[4]) << 8);
    }
    return error;
}

} // namespace

const Generated::FieldDescriptor* ProductMessage::find_field(
    std::string_view field_name) const noexcept {
    if (descriptor_ == nullptr) {
        return nullptr;
    }
    for (size_t index = 0; index < descriptor_->field_count; ++index) {
        const auto& field = descriptor_->fields[index];
        if (field_name == field.name) {
            return &field;
        }
    }
    return nullptr;
}

ProductMessage ProductMessage::from_descriptor(std::string_view name,
                                               std::span<const uint8_t> bytes) {
    const auto* descriptor = Generated::find_message(name);
    if (descriptor == nullptr || bytes.size() != descriptor->data_length) {
        return {};
    }
    return ProductMessage(descriptor, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

std::string_view ProductMessage::name() const noexcept {
    return descriptor_ == nullptr ? std::string_view{} : descriptor_->name;
}

std::optional<size_t> ProductMessage::data_offset(std::string_view field_name) const {
    const auto* field = find_field(field_name);
    if (field == nullptr || field->data_offset < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(field->data_offset);
}

bool ProductMessage::set_unsigned(std::string_view field_name, uint64_t value) {
    const auto* field = find_field(field_name);
    if (field == nullptr || field->data_offset < 0 || field->byte_length == 0 ||
        field->byte_length > sizeof(uint64_t)) {
        return false;
    }
    const size_t offset = static_cast<size_t>(field->data_offset);
    if (offset + field->byte_length > bytes_.size()) {
        return false;
    }

    if (field->type == Generated::FieldType::Bit) {
        if (field->bit_length == 0 || field->bit_length > 64 ||
            field->bit_offset + field->bit_length > field->byte_length * 8) {
            return false;
        }
        const uint64_t bit_mask = field->bit_length == 64
                                      ? std::numeric_limits<uint64_t>::max()
                                      : ((uint64_t{1} << field->bit_length) - 1u);
        if ((value & ~bit_mask) != 0) {
            return false;
        }
        uint64_t container = 0;
        for (size_t index = 0; index < field->byte_length; ++index) {
            container |= static_cast<uint64_t>(bytes_[offset + index]) << (8 * index);
        }
        const uint64_t shifted_mask = bit_mask << field->bit_offset;
        container = (container & ~shifted_mask) | ((value & bit_mask) << field->bit_offset);
        value = container;
    } else if ((value & ~width_mask(field->byte_length)) != 0) {
        return false;
    }

    for (size_t index = 0; index < field->byte_length; ++index) {
        bytes_[offset + index] = static_cast<uint8_t>(value >> (8 * index));
    }
    return true;
}

bool ProductMessage::set_signed(std::string_view field_name, int64_t value) {
    const auto* field = find_field(field_name);
    if (field == nullptr || field->byte_length == 0 || field->byte_length > sizeof(int64_t)) {
        return false;
    }
    if (field->byte_length < sizeof(int64_t)) {
        const unsigned bits = field->byte_length * 8;
        const int64_t minimum = -(int64_t{1} << (bits - 1));
        const int64_t maximum = (int64_t{1} << (bits - 1)) - 1;
        if (value < minimum || value > maximum) {
            return false;
        }
    }
    return set_unsigned(field_name, static_cast<uint64_t>(value) & width_mask(field->byte_length));
}

bool ProductMessage::set_scaled_signed(std::string_view field_name, double value) {
    const auto* field = find_field(field_name);
    if (field == nullptr ||
        (field->type != Generated::FieldType::S16Fixed &&
         field->type != Generated::FieldType::S32Fixed) ||
        !field->has_lsb || !std::isfinite(field->lsb) || field->lsb <= 0.0 ||
        !std::isfinite(value)) {
        return false;
    }

    const double scaled = value / field->lsb;
    if (!std::isfinite(scaled)) {
        return false;
    }
    const unsigned bits = field->byte_length * 8;
    const double minimum = -std::ldexp(1.0, bits - 1);
    const double maximum = std::ldexp(1.0, bits - 1) - 1.0;
    if (scaled <= minimum - 0.5 || scaled >= maximum + 0.5) {
        return false;
    }
    return set_signed(field_name, std::llround(scaled));
}

bool ProductMessage::set_float(std::string_view field_name, float value) {
    const auto* field = find_field(field_name);
    if (field == nullptr || field->type != Generated::FieldType::F32 ||
        !std::isfinite(value) ||
        field->byte_length != sizeof(float)) {
        return false;
    }
    return set_unsigned(field_name, std::bit_cast<uint32_t>(value));
}

std::optional<uint64_t> ProductMessage::get_unsigned(std::string_view field_name) const {
    const auto* field = find_field(field_name);
    if (field == nullptr || field->data_offset < 0 || field->byte_length == 0 ||
        field->byte_length > sizeof(uint64_t)) {
        return std::nullopt;
    }
    const size_t offset = static_cast<size_t>(field->data_offset);
    if (offset + field->byte_length > bytes_.size()) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (size_t index = 0; index < field->byte_length; ++index) {
        value |= static_cast<uint64_t>(bytes_[offset + index]) << (8 * index);
    }
    if (field->type == Generated::FieldType::Bit) {
        const uint64_t bit_mask = field->bit_length == 64
                                      ? std::numeric_limits<uint64_t>::max()
                                      : ((uint64_t{1} << field->bit_length) - 1u);
        value = (value >> field->bit_offset) & bit_mask;
    }
    return value;
}

std::optional<int64_t> ProductMessage::get_signed(std::string_view field_name) const {
    const auto* field = find_field(field_name);
    const auto raw = get_unsigned(field_name);
    if (field == nullptr || !raw || field->type == Generated::FieldType::Bit) {
        return std::nullopt;
    }
    const unsigned bits = field->byte_length * 8;
    uint64_t value = *raw;
    if (bits < 64 && (value & (uint64_t{1} << (bits - 1))) != 0) {
        value |= ~width_mask(field->byte_length);
    }
    return static_cast<int64_t>(value);
}

std::optional<float> ProductMessage::get_float(std::string_view field_name) const {
    const auto* field = find_field(field_name);
    const auto raw = get_unsigned(field_name);
    if (field == nullptr || !raw || field->type != Generated::FieldType::F32 ||
        field->byte_length != sizeof(float)) {
        return std::nullopt;
    }
    return std::bit_cast<float>(static_cast<uint32_t>(*raw));
}

bool ProductMessage::reserved_bytes_are_zero() const {
    if (descriptor_ == nullptr) {
        return false;
    }
    for (size_t field_index = 0; field_index < descriptor_->field_count; ++field_index) {
        const auto& field = descriptor_->fields[field_index];
        if (field.type != Generated::FieldType::Reserved || field.data_offset < 0) {
            continue;
        }
        const size_t offset = static_cast<size_t>(field.data_offset);
        if (offset + field.byte_length > bytes_.size()) {
            return false;
        }
        for (size_t index = 0; index < field.byte_length; ++index) {
            if (bytes_[offset + index] != 0) {
                return false;
            }
        }
    }
    return true;
}

uint16_t ProductProtocol::next_transmit_sequence() noexcept {
    return transmit_sequence_.fetch_add(1, std::memory_order_relaxed);
}

ProductMessage ProductProtocol::create_message(std::string_view name,
                                               bool allocate_transmit_sequence) {
    const auto* descriptor = Generated::find_message(name);
    if (descriptor == nullptr) {
        return {};
    }
    ProductMessage message(descriptor, std::vector<uint8_t>(descriptor->data_length, 0));
    for (size_t index = 0; index < descriptor->field_count; ++index) {
        const auto& field = descriptor->fields[index];
        if (field.data_offset < 0 || field.default_value == nullptr) {
            continue;
        }
        if (field.type == Generated::FieldType::F32) {
            float value = 0;
            if (parse_float_default(field.default_value, value)) {
                (void)message.set_float(field.name, value);
            }
        } else {
            uint64_t value = 0;
            if (parse_unsigned_default(field.default_value, value)) {
                (void)message.set_unsigned(field.name, value);
            }
        }
    }
    if (allocate_transmit_sequence && descriptor->role == Generated::MessageRole::Response) {
        (void)assign_transmit_sequence(message);
    }
    return message;
}

bool ProductProtocol::assign_transmit_sequence(ProductMessage& message) {
    if (!message || message.descriptor()->role != Generated::MessageRole::Response) {
        return false;
    }
    return message.set_unsigned("seq", next_transmit_sequence());
}

ProductParseResult ProductProtocol::parse_request(
    std::span<const uint8_t> data_segment) const {
    if (data_segment.empty() || data_segment.size() > 255) {
        return ProductParseResult(header_error(data_segment, ProductErrorCode::LenMismatch,
                                               static_cast<uint32_t>(data_segment.size())));
    }
    if (data_segment[0] != 0x11) {
        return ProductParseResult(header_error(data_segment, ProductErrorCode::CmdUnknown,
                                               data_segment[0]));
    }

    const Generated::MessageDescriptor* descriptor = nullptr;
    bool known_command = false;
    for (size_t index = 0; index < Generated::kMessageCount; ++index) {
        const auto& candidate = Generated::kMessages[index];
        if (candidate.role != Generated::MessageRole::Request ||
            candidate.type_group != data_segment[1] ||
            candidate.sub_type != data_segment[2]) {
            continue;
        }
        known_command = true;
        if (candidate.data_length == data_segment.size()) {
            descriptor = &candidate;
            break;
        }
    }
    if (descriptor == nullptr) {
        if (known_command) {
            return ProductParseResult(
                header_error(data_segment, ProductErrorCode::LenMismatch,
                             static_cast<uint32_t>(data_segment.size())));
        }
        return ProductParseResult(
            header_error(data_segment, ProductErrorCode::CmdUnknown,
                         (static_cast<uint32_t>(data_segment[1]) << 8) | data_segment[2]));
    }

    ProductMessage message(descriptor,
                           std::vector<uint8_t>(data_segment.begin(), data_segment.end()));
    if (!message.reserved_bytes_are_zero()) {
        return ProductParseResult(
            header_error(data_segment, ProductErrorCode::ParamOutOfRange, 0));
    }

    ProductProtocol defaults;
    auto expected = defaults.create_message(descriptor->name, false);
    for (size_t index = 0; index < descriptor->field_count; ++index) {
        const auto& field = descriptor->fields[index];
        if (field.type != Generated::FieldType::Const || field.data_offset < 0) {
            if (field.type == Generated::FieldType::F32 && field.data_offset >= 0) {
                const auto value = message.get_float(field.name);
                if (!value || !std::isfinite(*value)) {
                    return ProductParseResult(header_error(
                        data_segment, ProductErrorCode::ParamOutOfRange,
                        field.frame_offset));
                }
            }
            continue;
        }
        const size_t offset = static_cast<size_t>(field.data_offset);
        if (!std::equal(message.bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                        message.bytes_.begin() + static_cast<std::ptrdiff_t>(offset + field.byte_length),
                        expected.bytes_.begin() + static_cast<std::ptrdiff_t>(offset))) {
            return ProductParseResult(header_error(data_segment, ProductErrorCode::ParamOutOfRange,
                                                   field.frame_offset));
        }
    }
    return ProductParseResult(std::move(message));
}

ProductMessage ProductProtocol::create_error_response(
    const ProductParseError& error, bool allocate_transmit_sequence) {
    (void)allocate_transmit_sequence;
    auto response = create_message("error_response", false);
    if (!response) {
        return {};
    }
    // ERROR 属于请求对应的异常响应：seq 与 orig_seq 都回显触发错误的请求序号，
    // 不占用板端独立发送序号。
    (void)response.set_unsigned("seq", error.orig_sequence);
    (void)response.set_unsigned("orig_tg", error.orig_type_group);
    (void)response.set_unsigned("orig_st", error.orig_sub_type);
    (void)response.set_unsigned("orig_seq", error.orig_sequence);
    (void)response.set_unsigned("err_code", static_cast<uint16_t>(error.code));
    (void)response.set_unsigned("detail", error.detail);
    return response;
}

} // namespace MB_DDF::HWTest
