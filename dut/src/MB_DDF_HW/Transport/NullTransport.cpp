#include "MB_DDF_HW/Transport/NullTransport.h"

namespace MB_DDF::HW {

NullTransport::NullTransport(size_t register_space_size) : registers_(register_space_size, 0) {}

Result<void> NullTransport::open() {
    open_ = true;
    return {};
}

void NullTransport::close() noexcept {
    open_ = false;
}

bool NullTransport::is_open() const {
    return open_;
}

Result<void> NullTransport::check_access(uint64_t offset, size_t size, size_t alignment) const {
    if (!open_) {
        return Status::error(StatusCode::NotOpen, 0, "transport is not open");
    }

    if (alignment > 1 && (offset % alignment) != 0) {
        return Status::error(StatusCode::InvalidArgument, 0, "register access is misaligned");
    }

    const uint64_t space_size = static_cast<uint64_t>(registers_.size());
    const uint64_t access_size = static_cast<uint64_t>(size);
    if (offset > space_size || access_size > space_size - offset) {
        return Status::error(StatusCode::InvalidArgument, 0, "register access is out of range");
    }

    return {};
}

Result<uint8_t> NullTransport::read8(uint64_t offset) const {
    auto status = check_access(offset, sizeof(uint8_t), alignof(uint8_t));
    if (!status) {
        return status.status();
    }

    return registers_[static_cast<size_t>(offset)];
}

Result<uint16_t> NullTransport::read16(uint64_t offset) const {
    auto status = check_access(offset, sizeof(uint16_t), sizeof(uint16_t));
    if (!status) {
        return status.status();
    }

    const auto index = static_cast<size_t>(offset);
    const uint16_t value = static_cast<uint16_t>(registers_[index]) |
                           static_cast<uint16_t>(registers_[index + 1] << 8u);
    return value;
}

Result<uint32_t> NullTransport::read32(uint64_t offset) const {
    auto status = check_access(offset, sizeof(uint32_t), sizeof(uint32_t));
    if (!status) {
        return status.status();
    }

    const auto index = static_cast<size_t>(offset);
    const uint32_t value = static_cast<uint32_t>(registers_[index]) |
                           (static_cast<uint32_t>(registers_[index + 1]) << 8u) |
                           (static_cast<uint32_t>(registers_[index + 2]) << 16u) |
                           (static_cast<uint32_t>(registers_[index + 3]) << 24u);
    return value;
}

Result<void> NullTransport::write8(uint64_t offset, uint8_t value) {
    auto status = check_access(offset, sizeof(uint8_t), alignof(uint8_t));
    if (!status) {
        return status.status();
    }

    registers_[static_cast<size_t>(offset)] = value;
    return {};
}

Result<void> NullTransport::write16(uint64_t offset, uint16_t value) {
    auto status = check_access(offset, sizeof(uint16_t), sizeof(uint16_t));
    if (!status) {
        return status.status();
    }

    const auto index = static_cast<size_t>(offset);
    registers_[index] = static_cast<uint8_t>(value & 0xFFu);
    registers_[index + 1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
    return {};
}

Result<void> NullTransport::write32(uint64_t offset, uint32_t value) {
    auto status = check_access(offset, sizeof(uint32_t), sizeof(uint32_t));
    if (!status) {
        return status.status();
    }

    const auto index = static_cast<size_t>(offset);
    registers_[index] = static_cast<uint8_t>(value & 0xFFu);
    registers_[index + 1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
    registers_[index + 2] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
    registers_[index + 3] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
    return {};
}

Result<int> NullTransport::wait_event(Timeout timeout) {
    (void)timeout;
    return 0;
}

} // namespace MB_DDF::HW
