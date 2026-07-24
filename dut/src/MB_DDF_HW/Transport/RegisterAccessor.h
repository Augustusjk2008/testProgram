#pragma once

#include "MB_DDF_HW/Core/Result.h"

#include <cstdint>

namespace MB_DDF::HW {

class RegisterAccessor {
public:
    virtual ~RegisterAccessor() = default;

    virtual Result<uint8_t> read8(uint64_t offset) const = 0;
    virtual Result<uint16_t> read16(uint64_t offset) const = 0;
    virtual Result<uint32_t> read32(uint64_t offset) const = 0;

    virtual Result<void> write8(uint64_t offset, uint8_t value) = 0;
    virtual Result<void> write16(uint64_t offset, uint16_t value) = 0;
    virtual Result<void> write32(uint64_t offset, uint32_t value) = 0;
};

} // namespace MB_DDF::HW
