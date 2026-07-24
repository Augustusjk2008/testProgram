#include "MB_DDF_HW/Transport/XdmaTransport.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace MB_DDF::HW {
namespace {

Result<Os::Fd> open_fd(const std::string& path, int flags) {
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        return Status::error(StatusCode::OpenFailed, errno, "open failed: " + path);
    }
    return Os::Fd(fd);
}

std::string channel_path(const std::string& device_path, const char* suffix, int channel) {
    return device_path + suffix + std::to_string(channel);
}

} // namespace

XdmaTransport::XdmaTransport(XdmaConfig config) : config_(std::move(config)) {}

Result<void> XdmaTransport::open() {
    close();

    auto user_fd = open_fd(config_.device_path + "_user", O_RDWR | O_SYNC);
    if (!user_fd) {
        return user_fd.status();
    }
    user_fd_ = std::move(user_fd.value());

    auto user_map = Os::MmapRegion::map(user_fd_.get(), config_.map_length,
                                        static_cast<off_t>(config_.user_offset));
    if (!user_map) {
        close();
        return user_map.status();
    }
    user_map_ = std::move(user_map.value());

    if (config_.h2c_channel >= 0) {
        auto fd = open_fd(channel_path(config_.device_path, "_h2c_", config_.h2c_channel),
                          O_WRONLY | O_SYNC);
        if (!fd) {
            close();
            return fd.status();
        }
        h2c_fd_ = std::move(fd.value());
    }

    if (config_.c2h_channel >= 0) {
        auto fd = open_fd(channel_path(config_.device_path, "_c2h_", config_.c2h_channel),
                          O_RDONLY | O_SYNC);
        if (!fd) {
            close();
            return fd.status();
        }
        c2h_fd_ = std::move(fd.value());
    }

    if (config_.event_number >= 0) {
        auto fd = open_fd(channel_path(config_.device_path, "_events_", config_.event_number),
                          O_RDONLY | O_NONBLOCK);
        if (!fd) {
            close();
            return fd.status();
        }
        event_fd_ = std::move(fd.value());
        auto epoll_status = event_epoll_.open(event_fd_.get());
        if (!epoll_status) {
            close();
            return epoll_status.status();
        }
    }

    return {};
}

void XdmaTransport::close() noexcept {
    event_epoll_.close();
    event_fd_.reset();
    c2h_fd_.reset();
    h2c_fd_.reset();
    user_map_.reset();
    user_fd_.reset();
}

bool XdmaTransport::is_open() const {
    return user_fd_.valid() && user_map_.valid();
}

uint8_t* XdmaTransport::map_bytes() noexcept {
    return static_cast<uint8_t*>(user_map_.data());
}

const uint8_t* XdmaTransport::map_bytes() const noexcept {
    return static_cast<const uint8_t*>(user_map_.data());
}

Result<void> XdmaTransport::check_register_access(uint64_t offset, size_t size,
                                                  size_t alignment) const {
    if (!is_open()) {
        return Status::error(StatusCode::NotOpen, 0, "transport is not open");
    }

    if (alignment > 1 && (offset % alignment) != 0) {
        return Status::error(StatusCode::InvalidArgument, 0, "register access is misaligned");
    }

    const uint64_t map_size = static_cast<uint64_t>(user_map_.size());
    const uint64_t access_size = static_cast<uint64_t>(size);
    if (offset > map_size || access_size > map_size - offset) {
        return Status::error(StatusCode::InvalidArgument, 0, "register access is out of range");
    }

    return {};
}

Result<uint8_t> XdmaTransport::read8(uint64_t offset) const {
    auto status = check_register_access(offset, sizeof(uint8_t), alignof(uint8_t));
    if (!status) {
        return status.status();
    }

    // MMIO 必须按寄存器原生宽度执行单次 volatile 访问。
    const uint8_t result = *reinterpret_cast<const volatile uint8_t*>(map_bytes() + offset);
    std::atomic_thread_fence(std::memory_order_acquire);
    return result;
}

Result<uint16_t> XdmaTransport::read16(uint64_t offset) const {
    auto status = check_register_access(offset, sizeof(uint16_t), sizeof(uint16_t));
    if (!status) {
        return status.status();
    }

    const uint16_t result = *reinterpret_cast<const volatile uint16_t*>(map_bytes() + offset);
    std::atomic_thread_fence(std::memory_order_acquire);
    return result;
}

Result<uint32_t> XdmaTransport::read32(uint64_t offset) const {
    auto status = check_register_access(offset, sizeof(uint32_t), sizeof(uint32_t));
    if (!status) {
        return status.status();
    }

    const uint32_t result = *reinterpret_cast<const volatile uint32_t*>(map_bytes() + offset);
    std::atomic_thread_fence(std::memory_order_acquire);
    return result;
}

Result<void> XdmaTransport::write8(uint64_t offset, uint8_t value) {
    auto status = check_register_access(offset, sizeof(uint8_t), alignof(uint8_t));
    if (!status) {
        return status.status();
    }

    // release fence 保证此前的普通内存写在 MMIO 写之前可见。
    std::atomic_thread_fence(std::memory_order_release);
    *reinterpret_cast<volatile uint8_t*>(map_bytes() + offset) = value;
    return {};
}

Result<void> XdmaTransport::write16(uint64_t offset, uint16_t value) {
    auto status = check_register_access(offset, sizeof(uint16_t), sizeof(uint16_t));
    if (!status) {
        return status.status();
    }

    std::atomic_thread_fence(std::memory_order_release);
    *reinterpret_cast<volatile uint16_t*>(map_bytes() + offset) = value;
    return {};
}

Result<void> XdmaTransport::write32(uint64_t offset, uint32_t value) {
    auto status = check_register_access(offset, sizeof(uint32_t), sizeof(uint32_t));
    if (!status) {
        return status.status();
    }

    std::atomic_thread_fence(std::memory_order_release);
    *reinterpret_cast<volatile uint32_t*>(map_bytes() + offset) = value;
    return {};
}

Result<int> XdmaTransport::event_fd() const {
    if (!event_fd_.valid()) {
        return Status::error(StatusCode::Unsupported, 0, "event fd is not configured");
    }
    return event_fd_.get();
}

Result<int> XdmaTransport::wait_event(Timeout timeout) {
    if (!event_fd_.valid()) {
        return Status::error(StatusCode::Unsupported, 0, "event fd is not configured");
    }

    auto wait = event_epoll_.wait(timeout);
    if (!wait) {
        return wait.status();
    }

    if (wait.value() == 0) {
        return 0;
    }
    // XDMA 驱动的 poll 和 read 共用 events_irq：epoll 就绪表示计数非零，read 会原子
    // 取出并清零该计数。只关闭 event fd 不会清除计数，必须在每次就绪后消费它。
    // 每个 events_N 必须保持单消费者，避免其他读者在 epoll 与 read 之间抢先消费。
    uint32_t event_count = 0;
    ssize_t bytes_read = -1;
    do {
        bytes_read = ::read(event_fd_.get(), &event_count, sizeof(event_count));
    } while (bytes_read < 0 && errno == EINTR);
    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (bytes_read < 0) {
        return Status::error(StatusCode::IoError, errno, "event read failed");
    }
    if (bytes_read != static_cast<ssize_t>(sizeof(event_count))) {
        return Status::error(StatusCode::IoError, 0, "event read returned an invalid size");
    }
    return static_cast<int>(event_count);
}

Result<size_t> XdmaTransport::dma_write(int channel, BufferView data, uint64_t device_offset) {
    if (!h2c_fd_.valid() || config_.h2c_channel < 0) {
        return Status::error(StatusCode::Unsupported, 0, "h2c dma channel is not open");
    }
    if (channel != config_.h2c_channel) {
        return Status::error(StatusCode::InvalidArgument, 0, "h2c channel does not match config");
    }
    if (data.data == nullptr && data.size != 0) {
        return Status::error(StatusCode::InvalidArgument, 0, "dma write buffer is null");
    }

    const ssize_t written =
        ::pwrite(h2c_fd_.get(), data.data, data.size, static_cast<off_t>(device_offset));
    if (written < 0) {
        return Status::error(StatusCode::IoError, errno, "pwrite failed");
    }

    return static_cast<size_t>(written);
}

Result<size_t> XdmaTransport::dma_read(int channel, MutableBufferView buffer,
                                       uint64_t device_offset) {
    if (!c2h_fd_.valid() || config_.c2h_channel < 0) {
        return Status::error(StatusCode::Unsupported, 0, "c2h dma channel is not open");
    }
    if (channel != config_.c2h_channel) {
        return Status::error(StatusCode::InvalidArgument, 0, "c2h channel does not match config");
    }
    if (buffer.data == nullptr && buffer.size != 0) {
        return Status::error(StatusCode::InvalidArgument, 0, "dma read buffer is null");
    }

    const ssize_t read =
        ::pread(c2h_fd_.get(), buffer.data, buffer.size, static_cast<off_t>(device_offset));
    if (read < 0) {
        return Status::error(StatusCode::IoError, errno, "pread failed");
    }

    return static_cast<size_t>(read);
}

} // namespace MB_DDF::HW
