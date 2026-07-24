#pragma once

#include <cstddef>
#include <cstdint>

namespace MB_DDF::HW {

struct BufferView {
    const uint8_t* data{nullptr};
    size_t size{0};
};

struct MutableBufferView {
    uint8_t* data{nullptr};
    size_t size{0};
};

} // namespace MB_DDF::HW
