#pragma once

#include "MB_DDF_HW/Device/DeviceCommon.h"

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HW {

/// FLASH IP 地址表定义的命令字。
enum class FlashCommand : uint8_t {
    WriteEnable = 0x06,
    WriteDisable = 0x04,
    WriteStatus = 0x01,
    ReadStatus = 0x05,
    SectorErase = 0x21,
    BlockErase = 0xDC,
    ChipErase = 0x60,
    Program = 0x12,
    Read = 0x13,
};

/// FLASH 控制器原始状态；地址表更正后所有命令均以 D16=1 表示完成。
struct FlashControllerStatus {
    uint32_t raw{0};
    bool d16_set{false};
};

/// 基于片上读写 RAM 和命令寄存器的 Flash 访问接口；不拥有 Transport。
class FlashDevice {
public:
    /// 由读 RAM、写 RAM、命令区相邻地址边界得到的软件安全上限。
    static constexpr size_t MaxTransferBytes = 0x100;

    explicit FlashDevice(ITransport& transport) : transport_(transport) {}

    Result<void> check_communication() const;
    Result<FlashControllerStatus> read_controller_status() const;
    Result<uint32_t> read_flash_status_raw(Timeout timeout);
    Result<uint32_t> read_clock_divider() const;
    Result<void> set_clock_divider(uint32_t divider);

    Result<void> write_enable(Timeout timeout);
    /// 执行全片擦除命令；调用方须先完成授权/备份，并按操作表先 WREN、确认 RDSR=2。
    Result<void> chip_erase(Timeout timeout);
    Result<size_t> program(uint32_t address, BufferView data, Timeout timeout);
    Result<size_t> read_data(uint32_t address, MutableBufferView data, Timeout timeout);

private:
    Result<void> validate_transfer(const void* data, size_t size) const;
    Result<void> execute_simple_command(FlashCommand command,
                                        Timeout timeout,
                                        bool clear_after_completion);
    Result<void> trigger_and_complete(Timeout timeout,
                                      bool clear_after_completion);
    Result<void> wait_for_completion(Timeout timeout) const;

    ITransport& transport_;
};

} // namespace MB_DDF::HW
