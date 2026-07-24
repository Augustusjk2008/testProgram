#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace MB_DDF::HW {

struct XdmaConfig {
    std::string device_path{"/dev/xdma0"};
    uint64_t user_offset{0};
    size_t map_length{4096};
    int h2c_channel{-1};
    int c2h_channel{-1};
    int event_number{-1};
};

} // namespace MB_DDF::HW
