#pragma once

#include "MB_DDF_HW/Os/Epoll.h"
#include "MB_DDF_HW/Os/Fd.h"
#include "MB_DDF_HW/Os/MmapRegion.h"
#include "MB_DDF_HW/Transport/ITransport.h"
#include "MB_DDF_HW/Transport/TransportConfig.h"

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HW {

/// XDMA 用户区 MMIO、event 和预留 DMA 通道的 Transport 实现。
class XdmaTransport final : public ITransport {
public:
    explicit XdmaTransport(XdmaConfig config);

    Result<void> open() override;
    void close() noexcept override;
    bool is_open() const override;

    Result<uint8_t> read8(uint64_t offset) const override;
    Result<uint16_t> read16(uint64_t offset) const override;
    Result<uint32_t> read32(uint64_t offset) const override;

    Result<void> write8(uint64_t offset, uint8_t value) override;
    Result<void> write16(uint64_t offset, uint16_t value) override;
    Result<void> write32(uint64_t offset, uint32_t value) override;

    Result<int> event_fd() const override;
    Result<int> wait_event(Timeout timeout) override;

    Result<size_t> dma_write(int channel, BufferView data, uint64_t device_offset) override;
    Result<size_t> dma_read(int channel, MutableBufferView buffer, uint64_t device_offset) override;

private:
    Result<void> check_register_access(uint64_t offset, size_t size, size_t alignment) const;
    uint8_t* map_bytes() noexcept;
    const uint8_t* map_bytes() const noexcept;

    XdmaConfig config_;
    Os::Fd user_fd_;
    Os::Fd h2c_fd_;
    Os::Fd c2h_fd_;
    Os::Fd event_fd_;
    Os::Epoll event_epoll_;
    Os::MmapRegion user_map_;
};

} // namespace MB_DDF::HW
