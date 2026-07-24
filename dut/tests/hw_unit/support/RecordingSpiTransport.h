#pragma once

#include "MB_DDF_HW/Transport/ISpiTransport.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <utility>
#include <variant>
#include <vector>

namespace MB_DDF::HW::Test {

class RecordingSpiTransport final : public ISpiTransport {
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

    void queue_response(std::initializer_list<uint8_t> response) {
        script_.emplace_back(std::vector<uint8_t>(response));
    }

    void queue_response(std::vector<uint8_t> response) {
        script_.emplace_back(std::move(response));
    }

    void queue_failure(Status status) {
        script_.emplace_back(std::move(status));
    }

    const std::vector<std::vector<uint8_t>>& transfers() const {
        return transfers_;
    }

    Result<size_t> transfer(BufferView tx, MutableBufferView rx) override {
        if (!open_) {
            return Status::error(StatusCode::NotOpen, 0, "SPI transport is not open");
        }
        if (tx.size != rx.size || (tx.size != 0 && (tx.data == nullptr || rx.data == nullptr))) {
            return Status::error(StatusCode::InvalidArgument, 0, "invalid SPI transfer buffers");
        }

        if (tx.size == 0) {
            transfers_.emplace_back();
            return size_t{0};
        }
        transfers_.emplace_back(tx.data, tx.data + tx.size);
        if (script_.empty()) {
            return Status::error(StatusCode::ProtocolError, 0,
                                 "no scripted SPI response remains");
        }

        auto step = std::move(script_.front());
        script_.pop_front();
        if (std::holds_alternative<Status>(step)) {
            return std::get<Status>(std::move(step));
        }
        auto response = std::get<std::vector<uint8_t>>(std::move(step));
        if (response.size() != rx.size) {
            return Status::error(StatusCode::ProtocolError, 0,
                                 "scripted SPI response has the wrong size");
        }
        std::copy(response.begin(), response.end(), rx.data);
        return tx.size;
    }

private:
    using ScriptStep = std::variant<std::vector<uint8_t>, Status>;

    bool open_{false};
    std::deque<ScriptStep> script_;
    std::vector<std::vector<uint8_t>> transfers_;
};

} // namespace MB_DDF::HW::Test
