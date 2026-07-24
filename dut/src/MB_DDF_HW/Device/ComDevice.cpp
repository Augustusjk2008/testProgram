#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Device/Registers/ComRegisters.h"
#include <algorithm>
#include <chrono>

namespace MB_DDF::HW {
namespace R = Registers::Com;
// 帧布局寄存器将长度字段、帧尾长度和帧头长度压缩到一个字节。
namespace {
uint8_t layout_byte(uint8_t length, uint8_t tail, uint8_t head) {
    uint8_t code = length == 0 ? 0 : length == 1 ? 1 : 2;
    return static_cast<uint8_t>((code << 6) | ((std::min<uint8_t>(tail, 4) & 7) << 3) |
                                (std::min<uint8_t>(head, 4) & 7));
}
uint8_t length_bytes(uint8_t value) {
    const auto code = (value >> 6) & 3;
    return code == 0 ? 0 : code == 1 ? 1 : 2;
}
} // namespace
ComConfig ComDevice::default_config() {
    return {};
}
Result<void> ComDevice::check_communication() const {
    if (!transport_.is_open()) {
        return Status::error(StatusCode::NotOpen, 0, "transport is not open");
    }
    return {};
}
Result<void> ComDevice::configure(const ComConfig& c) {
    uint8_t control = c.format.receive_control;
    control = c.byte_timeout_returns_idle ? (control | 1u) : (control & ~1u);
    control = c.receive_enabled ? (control | 0x20u) : (control & ~0x20u);
    auto s = transport_.write8(R::Format, c.format.byte_format);
    if (!s) {
        return s;
    }
    s = transport_.write8(R::Format + 1, control);
    if (!s) {
        return s;
    }
    s = transport_.write8(R::Format + 2,
                          layout_byte(c.frame.send_length_bytes, c.frame.send_tail_length,
                                      c.frame.send_header_length));
    if (!s) {
        return s;
    }
    s = transport_.write8(R::Format + 3,
                          layout_byte(c.frame.receive_length_bytes, c.frame.receive_tail_length,
                                      c.frame.receive_header_length));
    if (!s) {
        return s;
    }
    const std::array<std::pair<uint64_t, const std::array<uint8_t, 4>*>, 4> arrays = {
        {{R::SendHeader, &c.frame.send_header},
         {R::SendTail, &c.frame.send_tail},
         {R::ReceiveHeader, &c.frame.receive_header},
         {R::ReceiveTail, &c.frame.receive_tail}}};
    for (auto [base, data] : arrays) {
        for (unsigned i = 0; i < 4; ++i) {
            s = transport_.write8(base + i, (*data)[i]);
            if (!s) {
                return s;
            }
        }
    }
    s = transport_.write8(R::Loopback, c.loopback ? 0xAFu : 0);
    if (!s) {
        return s;
    }
    s = transport_.write8(R::InterruptMode,
                          c.interrupt_mode == ComInterruptMode::Level ? 0xAEu : 0);
    if (!s) {
        return s;
    }
    // 不依赖 IP 复位默认值，显式打开中断输出门控。
    s = transport_.write8(R::PulseEnable, 1u);
    if (!s) {
        return s;
    }
    s = transport_.write16(R::InterruptPulse, c.interrupt_pulse_counter);
    if (!s) {
        return s;
    }
    s = transport_.write32(R::ReceiveTimeout, c.receive_timeout_counter);
    if (!s) {
        return s;
    }
    s = transport_.write16(R::SendBaud, c.baudrate_counter);
    if (!s) {
        return s;
    }
    s = transport_.write16(R::ReceiveBaud, c.baudrate_counter);
    if (!s) {
        return s;
    }
    loopback_enabled_ = c.loopback || (control & 0x18u) == 0x18u;
    if (!loopback_enabled_) {
        loopback_receive_pending_ = false;
    }
    return {};
}
Result<ComConfig> ComDevice::read_config() const {
    ComConfig c{};
    auto f = transport_.read32(R::Format);
    if (!f) {
        return f.status();
    }
    c.format.byte_format = f.value() & 0xFF;
    c.format.receive_control = (f.value() >> 8) & 0xFF;
    c.receive_enabled = (c.format.receive_control & 0x20) != 0;
    c.byte_timeout_returns_idle = (c.format.receive_control & 1) != 0;
    const uint8_t tx = (f.value() >> 16) & 0xFF, rx = (f.value() >> 24) & 0xFF;
    c.frame.send_length_bytes = length_bytes(tx);
    c.frame.receive_length_bytes = length_bytes(rx);
    c.frame.send_header_length = tx & 7;
    c.frame.send_tail_length = (tx >> 3) & 7;
    c.frame.receive_header_length = rx & 7;
    c.frame.receive_tail_length = (rx >> 3) & 7;
    const std::array<std::pair<uint64_t, std::array<uint8_t, 4>*>, 4> arrays = {
        {{R::SendHeader, &c.frame.send_header},
         {R::SendTail, &c.frame.send_tail},
         {R::ReceiveHeader, &c.frame.receive_header},
         {R::ReceiveTail, &c.frame.receive_tail}}};
    for (auto [base, data] : arrays) {
        auto v = transport_.read32(base);
        if (!v) {
            return v.status();
        }
        for (unsigned i = 0; i < 4; ++i) {
            (*data)[i] = (v.value() >> (8 * i)) & 0xFF;
        }
    }
    auto mode = transport_.read32(R::Loopback);
    if (!mode) {
        return mode.status();
    }
    c.loopback = (mode.value() & 0xFF) == 0xAF;
    c.interrupt_mode =
        ((mode.value() >> 8) & 0xFF) == 0xAE ? ComInterruptMode::Level : ComInterruptMode::Pulse;
    auto p = transport_.read32(R::InterruptPulse);
    if (!p) {
        return p.status();
    }
    c.interrupt_pulse_counter = p.value();
    auto t = transport_.read32(R::ReceiveTimeout);
    if (!t) {
        return t.status();
    }
    c.receive_timeout_counter = t.value();
    auto b = transport_.read32(R::SendBaud);
    if (!b) {
        return b.status();
    }
    c.baudrate_counter = b.value() & 0xFFFF;
    return c;
}
Result<size_t> ComDevice::send(BufferView data) {
    if (!transport_.is_open()) {
        return Status::error(StatusCode::NotOpen, 0, "transport is not open");
    }
    if (data.size > mtu() || (data.data == nullptr && data.size)) {
        return Status::error(StatusCode::InvalidArgument, 0, "COM payload is invalid");
    }
    // 与参考驱动一致，空 payload 直接成功，不触发任何硬件命令。
    if (data.size == 0) {
        return size_t{0};
    }
    auto state = transport_.read32(R::Control);
    if (!state) {
        return state.status();
    }
    if ((state.value() & 2u) == 0) {
        return Status::error(StatusCode::Busy, 0, "COM transmitter is busy");
    }

    auto format = transport_.read32(R::Format);
    if (!format) {
        return format.status();
    }
    const size_t prefix = length_bytes(static_cast<uint8_t>((format.value() >> 16) & 0xFFu));
    const size_t send_ram_capacity = static_cast<size_t>(R::Format - R::SendRam);
    if ((prefix == 1 && data.size > 0xFFu) || prefix + data.size > send_ram_capacity) {
        return Status::error(StatusCode::InvalidArgument, 0,
                             "COM payload does not fit the configured frame length");
    }
    const uint32_t encoded_length =
        static_cast<uint32_t>(data.size == send_ram_capacity ? 0 : data.size);
    const size_t framed_size = prefix + data.size;

    // 发送 RAM 只允许 32 位小端访问；长度字段在 payload 前，尾部不足四字节补零。
    for (size_t offset = 0; offset < framed_size; offset += 4) {
        uint32_t word = 0;
        for (size_t i = 0; i < 4 && offset + i < framed_size; ++i) {
            const size_t framed_index = offset + i;
            const uint8_t byte = framed_index < prefix
                                     ? static_cast<uint8_t>(encoded_length >> (8 * framed_index))
                                     : data.data[framed_index - prefix];
            word |= static_cast<uint32_t>(byte) << (8 * i);
        }
        auto s = transport_.write32(R::SendRam + offset, word);
        if (!s) {
            return s.status();
        }
    }
    // 无帧内长度字段时，IP 通过独立长度寄存器确定本次发送长度。
    if (prefix == 0) {
        auto s = transport_.write16(R::SendLength, static_cast<uint16_t>(encoded_length));
        if (!s) {
            return s.status();
        }
    }

    // 必须在完整帧写入 RAM 后最后触发发送。D0=1 表示不执行接收切 RAM 命令；
    // 持久接收使能由模式寄存器 D5 控制，不应在发送时额外切换 RX RAM。
    auto s = transport_.write32(R::Control, 0x81u);
    if (!s) {
        return s.status();
    }
    if (loopback_enabled_) {
        loopback_receive_pending_ = true;
    }
    return data.size;
}
Result<uint32_t> ComDevice::read_error_status() const {
    return transport_.read32(R::Error);
}
Result<void> ComDevice::clear_error_status() {
    return transport_.write32(R::Error, 0xCCCCu);
}
Result<void> ComDevice::reset() {
    auto status = transport_.write32(R::Control, 0);
    if (status) {
        loopback_enabled_ = false;
        loopback_receive_pending_ = false;
    }
    return status;
}
Result<void> ComDevice::enable_receive() {
    return transport_.write32(R::Control, 0x82u);
}
Result<size_t> ComDevice::receive(MutableBufferView buffer, Timeout timeout) {
    if (buffer.data == nullptr && buffer.size) {
        return Status::error(StatusCode::InvalidArgument, 0, "COM receive buffer is null");
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::microseconds(timeout.microseconds);
    for (;;) {
        Timeout remaining = timeout;
        if (!timeout.infinite) {
            const auto now = Clock::now();
            if (timeout.microseconds == 0 || now >= deadline) {
                remaining = Timeout::poll();
            } else {
                const auto remaining_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
                remaining = Timeout::after_us(static_cast<uint32_t>(remaining_us));
            }
        }

        // 优先等待 XDMA event；未接出 COM 中断的 bitstream 在超时后降级检查 RAM。
        auto event = transport_.wait_event(remaining);
        if (!event) {
            return event.status();
        }
        if (event.value() == 0) {
            // 部分 bitstream 未接出 COM 中断；只允许刚发送的内部回环帧降级读取一次。
            if (!loopback_enabled_ || !loopback_receive_pending_) {
                return size_t{0};
            }
        }
        // event 只表示有硬件通知；仍需按参考驱动回读状态，确认完成后才能切换 RX RAM。
        auto state = transport_.read32(R::Control);
        if (!state) {
            return state.status();
        }
        auto error = read_error_status();
        if (!error) {
            return error.status();
        }
        // 文档规定 Error 仅低 8 位有效，高 24 位必须忽略。
        if ((error.value() & 0xFFu) != 0) {
            loopback_receive_pending_ = false;
            auto clear = clear_error_status();
            if (!clear) {
                return clear.status();
            }
            // D0=0 同时可能表示错误帧；清错后切换该帧并重新使能下一次接收。
            auto enable = enable_receive();
            if (!enable) {
                return enable.status();
            }
            return Status::error(StatusCode::ProtocolError, 0, "COM receive error");
        }
        if ((state.value() & 1u) != 0) {
            break;
        }
        // 旧的 XDMA 计数或伪唤醒不能结束本次阻塞接收；在总超时内继续等待真实完成。
        if (!timeout.infinite &&
            (timeout.microseconds == 0 || Clock::now() >= deadline)) {
            return size_t{0};
        }
    }
    loopback_receive_pending_ = false;

    auto config = read_config();
    if (!config) {
        return config.status();
    }

    size_t prefix = config.value().frame.receive_length_bytes;
    size_t length = 0;
    if (prefix == 0) {
        // ReceivedBytes 只保证在接收不忙时有效，必须在 0x82 重新接收前锁存。
        auto count = transport_.read32(R::ReceivedBytes);
        if (!count) {
            return count.status();
        }
        length = count.value();
    }

    // 接收完成后切换乒乓 RAM，并清除中断、重新使能下一帧接收。
    auto enable = enable_receive();
    if (!enable) {
        return enable.status();
    }

    if (prefix != 0) {
        auto word = transport_.read32(R::ReceiveRam);
        if (!word) {
            return word.status();
        }
        length = prefix == 1 ? (word.value() & 0xFF) : (word.value() & 0xFFFF);
    }
    if (length > mtu()) {
        return Status::error(StatusCode::ProtocolError, 0, "COM frame length exceeds MTU");
    }
    if (buffer.size < length) {
        return Status::error(StatusCode::BufferTooSmall, 0, "COM receive buffer is too small");
    }
    for (size_t out = 0; out < length;) {
        const size_t absolute = prefix + out;
        const size_t aligned = absolute & ~size_t{3};
        auto word = transport_.read32(R::ReceiveRam + aligned);
        if (!word) {
            return word.status();
        }
        for (size_t i = absolute - aligned; i < 4 && out < length; ++i) {
            buffer.data[out++] = (word.value() >> (8 * i)) & 0xFF;
        }
    }

    return length;
}
} // namespace MB_DDF::HW
