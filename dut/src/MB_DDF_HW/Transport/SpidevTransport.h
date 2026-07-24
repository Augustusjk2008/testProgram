#pragma once

#include "MB_DDF_HW/Os/Fd.h"
#include "MB_DDF_HW/Transport/ISpiTransport.h"

#include <cstdint>
#include <string>

namespace MB_DDF::HW {

struct SpidevConfig {
    std::string device_path{"/dev/spidev0.0"};
    uint32_t speed_hz{1'000'000};
    uint8_t mode{0};
    uint8_t bits_per_word{8};
};

/// Linux spidev 全双工 Transport；每次 transfer 使用一个 SPI_IOC_MESSAGE。
class SpidevTransport final : public ISpiTransport {
public:
    explicit SpidevTransport(SpidevConfig config = {});
    ~SpidevTransport() override;

    Result<void> open() override;
    void close() noexcept override;
    bool is_open() const override;
    Result<size_t> transfer(BufferView tx, MutableBufferView rx) override;

    /// 恢复 open 前保存的 SPI 全局配置；成功后 close 不再重复写配置。
    Result<void> restore_configuration();

    const SpidevConfig& config() const noexcept {
        return config_;
    }

private:
    struct SavedConfiguration {
        uint32_t speed_hz{0};
        uint8_t mode{0};
        uint8_t lsb_first{0};
        uint8_t bits_per_word{0};
    };

    Result<void> capture_configuration();
    Result<void> configure();

    SpidevConfig config_;
    Os::Fd fd_;
    SavedConfiguration saved_config_{};
    bool saved_config_valid_{false};
};

} // namespace MB_DDF::HW
