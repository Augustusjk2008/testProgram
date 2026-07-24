#pragma once

#include "MB_DDF_HW/Core/Buffer.h"
#include "MB_DDF_HW/Core/Result.h"
#include "MB_DDF_HW/Core/Timeout.h"
#include "MB_DDF_HW/Transport/ISpiTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MB_DDF::HW {

/// Micron N25Q512A SPI Flash 协议封装；不拥有 Transport。
class SpiFlashDevice {
public:
    using JedecId = std::array<uint8_t, 3>;

    static constexpr uint32_t CapacityBytes = 64u * 1024u * 1024u;
    static constexpr uint32_t DieSizeBytes = 32u * 1024u * 1024u;
    static constexpr size_t PageSize = 256;
    static constexpr size_t SubsectorSize = 4u * 1024u;
    inline static constexpr JedecId ExpectedJedecId{0x20, 0xBA, 0x20};

    explicit SpiFlashDevice(ISpiTransport& transport) : transport_(transport) {}

    Result<void> check_communication() const;
    Result<JedecId> read_jedec_id();
    Result<uint8_t> read_status();
    Result<uint8_t> read_flag_status();
    /// 发送 WREN(06h) + WRNVCR(B1h, EFh FFh)，永久关闭 #HOLD 功能。
    Result<void> disable_hold();
    Result<void> clear_flag_status();
    Result<void> write_enable();
    Result<void> write_disable();
    Result<void> wait_until_ready(Timeout timeout);
    /// 上电或接管未知状态时，分别切换 CS 读取两个 die，直到二者均空闲。
    /// 这里只判断 Ready；历史 sticky error 由调用方随后读取或清除。
    Result<void> wait_until_all_dies_idle(Timeout timeout);

    Result<void> erase_subsector(uint32_t address, Timeout timeout);
    Result<size_t> program_page(uint32_t address, BufferView data, Timeout timeout);
    Result<size_t> read(uint32_t address, MutableBufferView data);

    /// 最近一次 Program/Erase 调用是否已进入命令传输阶段；失败后用于决定是否恢复。
    bool last_mutation_command_attempted() const noexcept {
        return last_mutation_command_attempted_;
    }

private:
    Result<std::vector<uint8_t>> exchange(std::vector<uint8_t> tx);
    Result<std::vector<uint8_t>> read_command_bytes(uint8_t command,
                                                    size_t response_size);
    Result<uint8_t> read_register(uint8_t command);
    Result<void> send_command(uint8_t command);
    Result<void> prepare_mutation();
    Result<void> finish_mutation(Timeout timeout);
    Result<void> validate_range(uint32_t address, size_t size) const;

    ISpiTransport& transport_;
    bool last_mutation_command_attempted_{false};
};

} // namespace MB_DDF::HW
