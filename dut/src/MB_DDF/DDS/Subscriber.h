/**
 * @file Subscriber.h
 * @brief 订阅者类定义
 * @date 2025-08-03
 * @author Jiangkai
 * 
 * 提供消息订阅功能，支持从指定Topic的环形缓冲区中接收消息。
 * 采用异步回调机制，在独立线程中处理消息接收和分发。
 */

#pragma once

#include "MB_DDF/DDS/ExternalEndpoint.h"
#include "MB_DDF/DDS/Gateway/GatewayLocalBus.h"
#include "MB_DDF/DDS/RingBuffer.h"
#include "MB_DDF/DDS/TopicRegistry.h"
#include <cstddef>
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <cstdint>
#include <vector>
#include <sched.h>

namespace MB_DDF {
namespace DDS {

struct RuntimeState;

/**
 * @typedef MessageCallback
 * @brief 消息回调函数类型定义
 * @param data 消息数据指针
 * @param size 消息数据大小
 * @param timestamp 消息时间戳（纳秒）
 */
using MessageCallback = std::function<void(const void* data, size_t size, uint64_t timestamp)>;

/**
 * @brief 同步读取策略
 *
 * Blocking 使用 RingBuffer::wait_for_message，可按 timeout_us 超时或无限等待。
 * Polling 只检查当前是否存在未读数据，不等待、不睡眠、不进入 pthread_cond_wait。
 */
enum class ReadStrategy {
    Blocking,
    Polling
};

/**
 * @class Subscriber
 * @brief 消息订阅者类
 * 
 * 负责从指定Topic的环形缓冲区中接收消息，并通过回调函数异步通知用户。
 * 使用独立的工作线程进行消息轮询，避免阻塞主线程。
 */
class Subscriber {
public:
    /**
     * @brief 构造函数
     * @param metadata Topic元数据指针
     * @param ring_buffer 关联的环形缓冲区指针
     * @param subscriber_name 订阅者名称（可选，默认为空）
     */
    Subscriber(TopicMetadata* metadata,
               RingBuffer* ring_buffer,
               const std::string& subscriber_name = "",
               ExternalEndpointRef external_io = nullptr);

    /**
     * @brief 构造由 DDSCore 运行期资源托管的订阅者。
     *
     * 原四参数构造符号保留给直接管理 RingBuffer 的旧调用方。
     */
    Subscriber(TopicMetadata* metadata,
               RingBuffer* ring_buffer,
               const std::string& subscriber_name,
               ExternalEndpointRef external_io,
               std::shared_ptr<RuntimeState> runtime_state);
    
    /**
     * @brief 析构函数，自动取消订阅并清理资源
     */
    ~Subscriber();

    /**
     * @brief 开始订阅消息
     * @param callback 消息接收回调函数
     * @return 订阅成功返回true，失败返回false
     */
    bool subscribe(MessageCallback callback = nullptr);

    /**
     * @brief 从本地订阅建立时可见的当前序列之后开始接收消息
     * @param callback 消息接收回调函数
     * @return 订阅成功返回true；外部端点或注册失败返回false
     */
    bool subscribe_after_current_sequence(MessageCallback callback = nullptr);

    /**
     * @brief 开始观察消息，回调包含Topic名称和本地序列号
     * @param callback 本地消息观察回调
     * @return 订阅成功返回true，失败返回false
     */
    bool subscribe_observer(LocalMessageCallback callback);

    /**
     * @brief 从指定序列号之后开始观察消息
     * @param callback 本地消息观察回调
     * @param start_after_sequence 跳过不大于该序列号的消息；0 表示补读仍保留的消息
     * @return 订阅成功返回true，失败返回false
     */
    bool subscribe_observer(LocalMessageCallback callback,
                            uint64_t start_after_sequence);

    /**
     * @brief 取消订阅，停止接收消息
     */
    void unsubscribe();
    
    /**
     * @brief 检查是否正在订阅
     * @return 正在订阅返回true，否则返回false
     */
    bool is_subscribed() const { return subscribed_.load(); }

    /// 当前订阅者绑定的 DDSCore epoch 是否仍接受新的本地操作。
    bool is_runtime_active() const;

    /**
     * @brief 绑定订阅者工作线程到指定CPU核心
     * @param cpu_id CPU核心ID（从0开始）
     * @return 绑定成功返回true，失败返回false
     */
    bool bind_to_cpu(int cpu_id, int priority = -1, int policy = SCHED_FIFO);

    /**
     * @brief 非阻塞轮询读取消息
     * @param data 接收消息数据的指针
     * @param size 接收消息数据的最大大小
     * @param latest 是否读取最新消息（默认true）
     * @return 实际读取的消息大小（0表示无消息）
     *
     * Polling 路径不会调用 wait_for_message，不会进入 pthread_cond_wait。
     */
    size_t poll(void* data, size_t size, bool latest = true);

    /**
     * @brief 兼容旧同步读取接口，等价于 poll()
     * @param data 接收消息数据的指针
     * @param size 接收消息数据的最大大小
     * @param latest 是否读取最新消息（默认true）
     * @return 实际读取的消息大小（0表示无消息）
     */
    size_t read(void* data, size_t size, bool latest = true);

    /**
     * @brief 阻塞读取消息，支持超时或无限等待
     * @param data 接收消息数据的指针
     * @param size 接收消息数据的最大大小
     * @param timeout_us 超时时间（微秒），0表示无限等待
     * @param latest 是否读取最新消息（默认true）
     * @return 实际读取的消息大小（0表示无消息或超时）
     */
    int32_t read_blocking(void* data, size_t size, uint32_t timeout_us = 0, bool latest = true);

    /**
     * @brief 按指定策略同步读取消息
     * @param data 接收消息数据的指针
     * @param size 接收消息数据的最大大小
     * @param strategy 读取策略，Blocking 等待通知，Polling 立即返回
     * @param timeout_us Blocking 超时时间（微秒），0表示无限等待；Polling 忽略
     * @param latest 是否读取最新消息（默认true）
     * @return 实际读取的消息大小（0表示无消息或超时）
     */
    int32_t read(void* data,
                 size_t size,
                 ReadStrategy strategy,
                 uint32_t timeout_us = 0,
                 bool latest = true);

    /**
     * @brief 兼容旧超时读取接口，等价于 read_blocking(..., latest=true)
     * @param data 接收消息数据的指针
     * @param size 接收消息数据的最大大小
     * @param timeout_us 超时时间（微秒），0表示无限等待
     * @return 实际读取的消息大小（0表示无消息或超时）
     */
    int32_t read(void* data, size_t size, uint32_t timeout_us);

    /**
     * @brief 获取订阅者工作线程
     * @return 工作线程对象引用
     */
    std::thread* get_thread() { 
        if (callback_ || observer_callback_) {
            return &worker_thread_; 
        } else {
            return nullptr;
        }
    }

private:
    bool subscribe_impl(MessageCallback callback, bool start_after_current_sequence);
    bool local_runtime_active() const;

    TopicMetadata* metadata_;       ///< Topic元数据指针
    RingBuffer* ring_buffer_;       ///< 环形缓冲区指针
    MessageCallback callback_;      ///< 消息回调函数
    LocalMessageCallback observer_callback_; ///< 内部观察回调函数
    std::atomic<bool> subscribed_;  ///< 订阅状态标志
    std::atomic<bool> running_;     ///< 工作线程运行状态标志
    std::thread worker_thread_;     ///< 消息接收工作线程
    ExternalEndpointRef external_io_; ///< 外部端点对象
    std::shared_ptr<RuntimeState> runtime_state_; ///< 保持 DDSCore epoch 资源存活。
    std::vector<uint8_t> receive_buffer_; ///< 外部端点接收缓冲区

    // 自身信息
    uint64_t subscriber_id_;        ///< 唯一的订阅者ID
    std::string subscriber_name_;   ///< 订阅者名称
    SubscriberState* subscriber_state_{}; ///< 订阅者状态结构体指针

    /**
     * @brief 工作线程主循环函数
     * 持续从环形缓冲区中读取消息并调用回调函数
     */
    void worker_loop();

    /**
     * @brief 从环形缓冲区读取下一条消息
     * @param data 接收消息数据的指针
     * @param size 接收消息数据的最大大小
     * @return 实际读取的消息大小（0表示无消息）
     */
    size_t read_next(void* data, size_t size);

    /**
     * @brief 从环形缓冲区读取最新消息
     * @param data 接收消息数据的指针
     * @param size 接收消息数据的最大大小
     * @return 实际读取的消息大小（0表示无消息）
     */
    size_t read_latest(void* data, size_t size);
};

} // namespace DDS
} // namespace MB_DDF
