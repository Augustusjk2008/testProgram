#pragma once

#include "MB_DDF_HW/Transport/ITransport.h"
#include <deque>
#include <initializer_list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace MB_DDF::HW::Test {

/// 单次寄存器访问记录，用于验证地址、宽度、值和顺序。
struct Access {
    bool write;
    uint64_t offset;
    unsigned width;
    uint32_t value;
};

/// 无锁的测试 Transport，可预置寄存器并记录全部访问。
class RecordingTransport final : public ITransport {
public:
    Result<void> open() override {
        open_ = true;
        return {};
    }
    void close() noexcept override {
        open_ = false;
    }
    bool is_open() const override {
        return open_;
    }
    void preset(uint64_t offset, uint32_t value) {
        registers_[offset] = value;
    }
    /// 为同一读地址预置按顺序返回的值，适用于读写语义复用的异步状态寄存器。
    void queue_reads(uint64_t offset, std::initializer_list<uint32_t> values) {
        auto& queued = queued_reads_[offset];
        queued.insert(queued.end(), values.begin(), values.end());
    }
    void set_event(int value) {
        event_ = value;
    }
    void fail_next_write_at(uint64_t offset) {
        fail_next_write_offset_ = offset;
    }
    const std::vector<Access>& accesses() const {
        return accesses_;
    }
    const std::vector<Timeout>& waited_timeouts() const {
        return waited_timeouts_;
    }
    void clear_accesses() {
        accesses_.clear();
    }
    Result<uint8_t> read8(uint64_t o) const override {
        return read<uint8_t>(o, 8);
    }
    Result<uint16_t> read16(uint64_t o) const override {
        return read<uint16_t>(o, 16);
    }
    Result<uint32_t> read32(uint64_t o) const override {
        return read<uint32_t>(o, 32);
    }
    Result<void> write8(uint64_t o, uint8_t v) override {
        return write(o, 8, v);
    }
    Result<void> write16(uint64_t o, uint16_t v) override {
        return write(o, 16, v);
    }
    Result<void> write32(uint64_t o, uint32_t v) override {
        return write(o, 32, v);
    }
    Result<int> wait_event(Timeout timeout) override {
        waited_timeouts_.push_back(timeout);
        return event_;
    }

private:
    template <class T> Result<T> read(uint64_t o, unsigned width) const {
        if (!open_) {
            return Status::error(StatusCode::NotOpen, 0, "not open");
        }
        uint32_t value = 0;
        auto queued = queued_reads_.find(o);
        if (queued != queued_reads_.end() && !queued->second.empty()) {
            value = queued->second.front();
            queued->second.pop_front();
        } else {
            const auto it = registers_.find(o);
            value = it == registers_.end() ? 0 : it->second;
        }
        accesses_.push_back({false, o, width, value});
        return static_cast<T>(value);
    }
    Result<void> write(uint64_t o, unsigned width, uint32_t value) {
        if (!open_) {
            return Status::error(StatusCode::NotOpen, 0, "not open");
        }
        accesses_.push_back({true, o, width, value});
        if (fail_next_write_offset_ && *fail_next_write_offset_ == o) {
            fail_next_write_offset_.reset();
            return Status::error(StatusCode::IoError, 0,
                                 "injected register write failure");
        }
        registers_[o] = value;
        return {};
    }
    bool open_{false};
    int event_{0};
    mutable std::vector<Access> accesses_;
    mutable std::unordered_map<uint64_t, std::deque<uint32_t>> queued_reads_;
    mutable std::unordered_map<uint64_t, uint32_t> registers_;
    std::optional<uint64_t> fail_next_write_offset_;
    std::vector<Timeout> waited_timeouts_;
};
} // namespace MB_DDF::HW::Test
