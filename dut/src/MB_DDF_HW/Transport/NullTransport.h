#pragma once

#include "MB_DDF_HW/Transport/ITransport.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MB_DDF::HW {

class NullTransport final : public ITransport {
public:
    explicit NullTransport(size_t register_space_size = 4096);

    Result<void> open() override;
    void close() noexcept override;
    bool is_open() const override;

    Result<uint8_t> read8(uint64_t offset) const override;
    Result<uint16_t> read16(uint64_t offset) const override;
    Result<uint32_t> read32(uint64_t offset) const override;

    Result<void> write8(uint64_t offset, uint8_t value) override;
    Result<void> write16(uint64_t offset, uint16_t value) override;
    Result<void> write32(uint64_t offset, uint32_t value) override;

    Result<int> wait_event(Timeout timeout) override;

private:
    Result<void> check_access(uint64_t offset, size_t size, size_t alignment) const;

    std::vector<uint8_t> registers_;
    bool open_{false};
};

} // namespace MB_DDF::HW
