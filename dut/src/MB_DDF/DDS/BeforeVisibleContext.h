#pragma once

#include <cstdint>

namespace MB_DDF {
namespace DDS {
namespace Detail {

inline thread_local uint32_t before_visible_callback_depth = 0;

class BeforeVisibleCallbackScope {
public:
    BeforeVisibleCallbackScope() noexcept {
        ++before_visible_callback_depth;
    }

    ~BeforeVisibleCallbackScope() {
        --before_visible_callback_depth;
    }

    BeforeVisibleCallbackScope(const BeforeVisibleCallbackScope&) = delete;
    BeforeVisibleCallbackScope& operator=(const BeforeVisibleCallbackScope&) = delete;
};

inline bool before_visible_callback_active() noexcept {
    return before_visible_callback_depth != 0;
}

} // namespace Detail
} // namespace DDS
} // namespace MB_DDF

