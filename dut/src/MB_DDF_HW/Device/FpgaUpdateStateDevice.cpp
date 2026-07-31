#include "MB_DDF_HW/Device/FpgaUpdateStateDevice.h"

#include "MB_DDF_HW/Device/Registers/FpgaUpdateStateRegisters.h"

namespace MB_DDF::HW {
namespace R = Registers::FpgaUpdateState;

Result<void> FpgaUpdateStateDevice::check_communication() const {
    return check_device_communication(transport_);
}

Result<FpgaUpdateStateSnapshot> FpgaUpdateStateDevice::read_snapshot() const {
    auto flash_erase_status = transport_.read32(R::FlashEraseStatus);
    if (!flash_erase_status) {
        return flash_erase_status.status();
    }
    auto flash_write_status = transport_.read32(R::FlashWriteStatus);
    if (!flash_write_status) {
        return flash_write_status.status();
    }
    auto flash_read_status = transport_.read32(R::FlashReadStatus);
    if (!flash_read_status) {
        return flash_read_status.status();
    }
    auto read_data_crc = transport_.read32(R::ReadDataCrc);
    if (!read_data_crc) {
        return read_data_crc.status();
    }
    auto write_data_crc = transport_.read32(R::WriteDataCrc);
    if (!write_data_crc) {
        return write_data_crc.status();
    }
    auto crc_validation_status = transport_.read32(R::CrcValidationStatus);
    if (!crc_validation_status) {
        return crc_validation_status.status();
    }
    return FpgaUpdateStateSnapshot{
        static_cast<uint16_t>(flash_erase_status.value()),
        static_cast<uint16_t>(flash_write_status.value()),
        static_cast<uint16_t>(flash_read_status.value()),
        read_data_crc.value(),
        write_data_crc.value(),
        static_cast<uint8_t>(crc_validation_status.value()),
    };
}

} // namespace MB_DDF::HW
