#pragma once

/**
 * @file RuntimeState.h
 * @brief DDSCore 单次初始化所拥有的进程内运行期资源。
 */

#include "MB_DDF/DDS/RingBuffer.h"
#include "MB_DDF/DDS/SharedMemory.h"
#include "MB_DDF/DDS/TopicRegistry.h"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace MB_DDF {
namespace DDS {

/**
 * @brief 一次 DDSCore 初始化对应的资源 epoch。
 *
 * Publisher/Subscriber 持有该对象可保证 shutdown() 与在途操作并发时，RingBuffer
 * 对象、TopicMetadata 所在映射以及命名 semaphore 均保持有效。active 只表示该
 * epoch 是否仍接受新的本地操作；已经进入的操作可以依靠共享所有权安全收尾。
 */
struct RuntimeState {
    RuntimeState() = default;

    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;

    ~RuntimeState() {
        // RingBuffer 内部保存共享映射和 semaphore 的非拥有指针，析构顺序必须固定。
        topic_buffers.clear();
        topic_registry.reset();
        shm_manager.reset();
    }

    bool is_active() const {
        return active.load(std::memory_order_acquire);
    }

    std::atomic<bool> active{false};
    std::unique_ptr<SharedMemoryManager> shm_manager;
    std::unique_ptr<TopicRegistry> topic_registry;
    std::unordered_map<TopicMetadata*, std::unique_ptr<RingBuffer>> topic_buffers;
    std::string process_name;
};

} // namespace DDS
} // namespace MB_DDF
