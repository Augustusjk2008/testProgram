#pragma once

/**
 * @file EntityId.h
 * @brief 生成跨进程唯一的 Publisher/Subscriber 实体 ID。
 *
 * SylixOS 3.6.5 上 std::random_device 可能是确定性的：不同进程以它播种
 * std::mt19937_64 后会得到相同的第一个值。SubscriberRegistry 又会优先按 ID
 * 匹配，因此重复 ID 会让两个进程误用同一个读游标。这里显式混入 PID、单调时钟
 * 和进程内计数器，不依赖平台熵源。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unistd.h>

namespace MB_DDF {
namespace DDS {

inline uint64_t generate_entity_id() noexcept {
    static std::atomic<uint64_t> local_sequence{0};

    const uint64_t sequence =
        local_sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    const uint64_t monotonic_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const uint64_t pid = static_cast<uint64_t>(
        static_cast<unsigned long>(::getpid()));

    // SplitMix64 的终结混合步骤把 PID、时间和计数器的相邻位充分扩散。
    uint64_t value = monotonic_ns ^ (pid << 32U) ^
                     (sequence * 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return value == 0 ? 1U : value;
}

} // namespace DDS
} // namespace MB_DDF

