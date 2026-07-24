#pragma once

#include <cstdint>

namespace MB_DDF::HW {

struct Timeout {
    uint32_t microseconds{0};
    bool infinite{false};

    static Timeout poll() { return Timeout{0, false}; }
    static Timeout after_us(uint32_t us) { return Timeout{us, false}; }
    static Timeout forever() { return Timeout{0, true}; }
};

} // namespace MB_DDF::HW
