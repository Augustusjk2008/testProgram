#include "MB_DDF_HW/Transport/SpidevTransport.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <string>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace MB_DDF::HW {
namespace {

int ioctl_retry(int fd, unsigned long request, void* argument) {
    int result = -1;
    do {
        result = ::ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

Result<void> ioctl_config(int fd, unsigned long request, void* argument,
                          const char* operation) {
    if (ioctl_retry(fd, request, argument) < 0) {
        const int error = errno;
        return Status::error(StatusCode::IoError, error,
                             std::string(operation) + " failed");
    }
    return {};
}

} // namespace

SpidevTransport::SpidevTransport(SpidevConfig config) : config_(std::move(config)) {}

SpidevTransport::~SpidevTransport() {
    close();
}

Result<void> SpidevTransport::open() {
    close();
    if (config_.device_path.empty()) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "spidev device path is empty");
    }
    if (config_.speed_hz == 0 || config_.bits_per_word == 0 || config_.mode > 3) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "invalid spidev mode, bits, or speed configuration");
    }

    int raw_fd = -1;
    do {
        raw_fd = ::open(config_.device_path.c_str(), O_RDWR | O_CLOEXEC);
    } while (raw_fd < 0 && errno == EINTR);
    if (raw_fd < 0) {
        const int error = errno;
        return Status::error(StatusCode::OpenFailed, error,
                             "open failed: " + config_.device_path);
    }
    fd_.reset(raw_fd);

    if (::flock(fd_.get(), LOCK_EX | LOCK_NB) < 0) {
        const int error = errno;
        close();
        return Status::error(
            (error == EWOULDBLOCK || error == EAGAIN) ? StatusCode::Busy
                                                       : StatusCode::IoError,
            error, "failed to acquire exclusive advisory lock on " +
                       config_.device_path);
    }

    auto captured = capture_configuration();
    if (!captured) {
        close();
        return captured;
    }
    auto configured = configure();
    if (!configured) {
        close();
        return configured;
    }
    return {};
}

Result<void> SpidevTransport::capture_configuration() {
    SavedConfiguration saved{};
    auto status = ioctl_config(fd_.get(), SPI_IOC_RD_MODE, &saved.mode,
                               "SPI_IOC_RD_MODE before configure");
    if (!status) {
        return status;
    }
    status = ioctl_config(fd_.get(), SPI_IOC_RD_LSB_FIRST, &saved.lsb_first,
                          "SPI_IOC_RD_LSB_FIRST before configure");
    if (!status) {
        return status;
    }
    status = ioctl_config(fd_.get(), SPI_IOC_RD_BITS_PER_WORD,
                          &saved.bits_per_word,
                          "SPI_IOC_RD_BITS_PER_WORD before configure");
    if (!status) {
        return status;
    }
    status = ioctl_config(fd_.get(), SPI_IOC_RD_MAX_SPEED_HZ, &saved.speed_hz,
                          "SPI_IOC_RD_MAX_SPEED_HZ before configure");
    if (!status) {
        return status;
    }
    saved_config_ = saved;
    saved_config_valid_ = true;
    return {};
}

Result<void> SpidevTransport::configure() {
    uint8_t mode = config_.mode;
    auto status = ioctl_config(fd_.get(), SPI_IOC_WR_MODE, &mode, "SPI_IOC_WR_MODE");
    if (!status) {
        return status;
    }
    mode = 0;
    status = ioctl_config(fd_.get(), SPI_IOC_RD_MODE, &mode, "SPI_IOC_RD_MODE");
    if (!status) {
        return status;
    }
    if (mode != config_.mode) {
        return Status::error(StatusCode::ProtocolError, 0,
                             "spidev mode readback does not match requested mode");
    }

    uint8_t lsb_first = 0;
    status = ioctl_config(fd_.get(), SPI_IOC_WR_LSB_FIRST, &lsb_first,
                          "SPI_IOC_WR_LSB_FIRST");
    if (!status) {
        return status;
    }
    lsb_first = 1;
    status = ioctl_config(fd_.get(), SPI_IOC_RD_LSB_FIRST, &lsb_first,
                          "SPI_IOC_RD_LSB_FIRST");
    if (!status) {
        return status;
    }
    if (lsb_first != 0) {
        return Status::error(StatusCode::ProtocolError, 0,
                             "spidev remained configured for LSB-first transfers");
    }

    uint8_t bits = config_.bits_per_word;
    status = ioctl_config(fd_.get(), SPI_IOC_WR_BITS_PER_WORD, &bits,
                          "SPI_IOC_WR_BITS_PER_WORD");
    if (!status) {
        return status;
    }
    bits = 0;
    status = ioctl_config(fd_.get(), SPI_IOC_RD_BITS_PER_WORD, &bits,
                          "SPI_IOC_RD_BITS_PER_WORD");
    if (!status) {
        return status;
    }
    if (bits != config_.bits_per_word) {
        return Status::error(StatusCode::ProtocolError, 0,
                             "spidev word size readback does not match requested size");
    }

    uint32_t speed = config_.speed_hz;
    status = ioctl_config(fd_.get(), SPI_IOC_WR_MAX_SPEED_HZ, &speed,
                          "SPI_IOC_WR_MAX_SPEED_HZ");
    if (!status) {
        return status;
    }
    speed = 0;
    status = ioctl_config(fd_.get(), SPI_IOC_RD_MAX_SPEED_HZ, &speed,
                          "SPI_IOC_RD_MAX_SPEED_HZ");
    if (!status) {
        return status;
    }
    if (speed != config_.speed_hz) {
        return Status::error(StatusCode::ProtocolError, 0,
                             "spidev speed readback does not match requested speed");
    }
    return {};
}

void SpidevTransport::close() noexcept {
    if (fd_.valid() && saved_config_valid_) {
        (void)restore_configuration();
    }
    saved_config_valid_ = false;
    fd_.reset();
}

Result<void> SpidevTransport::restore_configuration() {
    if (!fd_.valid()) {
        return Status::error(StatusCode::NotOpen, 0,
                             "spidev transport is not open");
    }
    if (!saved_config_valid_) {
        return {};
    }
    const auto saved = saved_config_;
    bool failed = false;
    Status first_error{};
    const auto apply = [&](Result<void> result) {
        if (!result && !failed) {
            failed = true;
            first_error = result.status();
        }
    };

    auto mode = saved.mode;
    apply(ioctl_config(fd_.get(), SPI_IOC_WR_MODE, &mode,
                       "restore SPI_IOC_WR_MODE"));
    auto lsb_first = saved.lsb_first;
    apply(ioctl_config(fd_.get(), SPI_IOC_WR_LSB_FIRST, &lsb_first,
                       "restore SPI_IOC_WR_LSB_FIRST"));
    auto bits = saved.bits_per_word;
    apply(ioctl_config(fd_.get(), SPI_IOC_WR_BITS_PER_WORD, &bits,
                       "restore SPI_IOC_WR_BITS_PER_WORD"));
    auto speed = saved.speed_hz;
    apply(ioctl_config(fd_.get(), SPI_IOC_WR_MAX_SPEED_HZ, &speed,
                       "restore SPI_IOC_WR_MAX_SPEED_HZ"));

    if (failed) {
        return first_error;
    }
    saved_config_valid_ = false;
    return {};
}

bool SpidevTransport::is_open() const {
    return fd_.valid();
}

Result<size_t> SpidevTransport::transfer(BufferView tx, MutableBufferView rx) {
    if (!is_open()) {
        return Status::error(StatusCode::NotOpen, 0, "spidev transport is not open");
    }
    if (tx.size != rx.size) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI transmit and receive sizes must match");
    }
    if (tx.size == 0) {
        return size_t{0};
    }
    if (tx.data == nullptr || rx.data == nullptr) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI transmit or receive buffer is null");
    }
    if (tx.size > static_cast<size_t>(INT_MAX)) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "SPI transfer is too large for SPI_IOC_MESSAGE");
    }

    spi_ioc_transfer transfer{};
    transfer.tx_buf = static_cast<__u64>(reinterpret_cast<uintptr_t>(tx.data));
    transfer.rx_buf = static_cast<__u64>(reinterpret_cast<uintptr_t>(rx.data));
    transfer.len = static_cast<__u32>(tx.size);
    transfer.speed_hz = config_.speed_hz;
    transfer.bits_per_word = config_.bits_per_word;

    // 不重试该 ioctl：EINTR 时无法证明破坏性 Flash 命令尚未被器件接收。
    const int transferred = ::ioctl(fd_.get(), SPI_IOC_MESSAGE(1), &transfer);
    if (transferred < 0) {
        const int error = errno;
        return Status::error(StatusCode::IoError, error,
                             "SPI_IOC_MESSAGE transfer failed");
    }
    if (transferred != static_cast<int>(tx.size)) {
        return Status::error(StatusCode::IoError, 0,
                             "SPI_IOC_MESSAGE returned an incomplete transfer");
    }
    return static_cast<size_t>(transferred);
}

} // namespace MB_DDF::HW
