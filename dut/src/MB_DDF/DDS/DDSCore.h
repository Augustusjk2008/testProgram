/**
 * @file DDSCore.h
 * @brief DDSCore主接口类定义
 * @date 2025-08-03
 * @author Jiangkai
 * 
 * 提供DDSCore（数据分发服务）的主要接口，支持发布者-订阅者模式的消息传递。
 * 基于共享内存和无锁环形缓冲区实现高性能的进程间通信。
 */

#pragma once

#include "MB_DDF/DDS/TopicRegistry.h" // Topic注册表管理
#include "MB_DDF/DDS/SharedMemory.h" // 共享内存管理器
#include "MB_DDF/DDS/RingBuffer.h" // 环形缓冲区
#include "MB_DDF/DDS/RuntimeState.h" // 单次初始化的资源 epoch
#include "MB_DDF/DDS/Publisher.h" // 发布者
#include "MB_DDF/DDS/Subscriber.h" // 订阅者
#include "MB_DDF/DDS/Gateway/GatewayLocalBus.h" // Gateway内部本地总线类型

#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace MB_DDF {
namespace DDS {

/**
 * @brief 数据写入接口，对应Publisher
 */
using DataWriter = Publisher;

/**
 * @brief 数据读取接口，对应Subscriber
 */
using DataReader = Subscriber;

/**
 * @class DDSCore
 * @brief DDSCore主控制类，采用单例模式
 * 
 * 负责管理整个DDS系统的生命周期，包括共享内存管理、Topic注册、
 * 发布者和订阅者的创建等核心功能。
 */
class DDSCore {
public:
    /**
     * @brief 获取DDSCore单例实例
     * @return DDSCore单例引用
     */
    static DDSCore& instance();

    // 版本号，用于共享内存布局升级
    static const uint32_t VERSION = 0x00005001;

    /**
     * @brief 创建指定Topic的发布者
     * @param topic_name Topic名称
     * @param enable_checksum 是否启用校验和，默认true
     * @return 发布者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Publisher> create_publisher(const std::string& topic_name, bool enable_checksum = true);

    /**
     * @brief 创建绑定外部端点的发布者
     * @param topic_name Topic名称，仅用于日志和API一致性
     * @param external_io 外部端点对象
     * @return 发布者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Publisher> create_publisher(const std::string& topic_name,
                                                ExternalEndpointRef external_io);

    /**
     * @brief 创建指定Topic的发布者（别名）
     * @param topic_name Topic名称
     * @param enable_checksum 是否启用校验和，默认true
     * @return 发布者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Publisher> create_writer(const std::string& topic_name, bool enable_checksum = true);

    /**
     * @brief 创建绑定外部端点的发布者（别名）
     * @param topic_name Topic名称，仅用于日志和API一致性
     * @param external_io 外部端点对象
     * @return 发布者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Publisher> create_writer(const std::string& topic_name,
                                             ExternalEndpointRef external_io);

    /**
     * @brief 创建指定Topic的订阅者
     * @param topic_name Topic名称
     * @param enable_checksum 是否启用校验和，默认true
     * @param callback 消息接收回调函数，默认空函数
     * @return 订阅者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Subscriber> create_subscriber(const std::string& topic_name, bool enable_checksum = true, const MessageCallback& callback = nullptr);

    /**
     * @brief 创建绑定外部端点的订阅者
     * @param topic_name Topic名称，仅用于日志和API一致性
     * @param external_io 外部端点对象
     * @param callback 消息接收回调函数，默认空函数
     * @return 订阅者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Subscriber> create_subscriber(const std::string& topic_name,
                                                  ExternalEndpointRef external_io,
                                                  const MessageCallback& callback = nullptr);

    /**
     * @brief 创建指定Topic的订阅者（别名）
     * @param topic_name Topic名称
     * @param enable_checksum 是否启用校验和，默认true
     * @param callback 消息接收回调函数，默认空函数
     * @return 订阅者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Subscriber> create_reader(const std::string& topic_name, bool enable_checksum = true, const MessageCallback& callback = nullptr);

    /**
     * @brief 创建绑定外部端点的订阅者（别名）
     * @param topic_name Topic名称，仅用于日志和API一致性
     * @param external_io 外部端点对象
     * @param callback 消息接收回调函数，默认空函数
     * @return 订阅者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Subscriber> create_reader(const std::string& topic_name,
                                              ExternalEndpointRef external_io,
                                              const MessageCallback& callback = nullptr);

    /**
     * @brief 发布数据到指定Topic
     * @param publisher 发布者智能指针
     * @param data 数据指针
     * @param size 数据大小（字节）
     * @return 实际写入的数据大小（字节），0表示写入失败
     */
    size_t data_write(std::shared_ptr<Publisher> publisher, const void* data, size_t size);

    /**
     * @brief 从指定Topic的环形缓冲区读取数据
     * @param subscriber 订阅者智能指针
     * @param data 接收数据指针
     * @param size 接收数据大小（字节）
     * @return 实际读取的数据大小（字节），0表示无数据可读
     */
    size_t data_read(std::shared_ptr<Subscriber> subscriber, void* data, size_t size);

    /**
     * @brief 从指定Topic非阻塞轮询读取数据
     * @param subscriber 订阅者智能指针
     * @param data 接收数据指针
     * @param size 接收数据大小（字节）
     * @param latest 是否读取最新消息
     * @return 实际读取的数据大小（字节），0表示无数据可读
     */
    size_t data_poll(std::shared_ptr<Subscriber> subscriber,
                     void* data,
                     size_t size,
                     bool latest = true);

    /**
     * @brief 按指定策略从指定Topic读取数据
     * @param subscriber 订阅者智能指针
     * @param data 接收数据指针
     * @param size 接收数据大小（字节）
     * @param strategy 读取策略，Blocking 等待通知，Polling 立即返回
     * @param timeout_us Blocking 超时时间（微秒），0表示无限等待
     * @param latest 是否读取最新消息
     * @return 实际读取的数据大小（字节），0表示无数据可读或超时
     */
    int32_t data_read(std::shared_ptr<Subscriber> subscriber,
                      void* data,
                      size_t size,
                      ReadStrategy strategy,
                      uint32_t timeout_us = 0,
                      bool latest = true);

    /**
     * @brief 列出本地域已注册Topic
     * @return Topic信息列表
     */
    std::vector<LocalTopicInfo> list_topics() const;

    /**
     * @brief 在同一生命周期临界区内检查初始化状态并枚举 Topic。
     * @param topics 成功时接收快照，失败时清空
     * @return 当前存在活动 RuntimeState 时返回 true
     */
    bool try_list_topics(std::vector<LocalTopicInfo>& topics) const;

    /// 当前 DDSCore 是否存在活动的 RuntimeState。
    bool is_initialized() const;

    /**
     * @brief 创建携带本地序列号的内部观察订阅者
     * @param topic_name Topic名称
     * @param callback 本地消息观察回调
     * @return 订阅者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Subscriber> create_observer(const std::string& topic_name,
                                                 const LocalMessageCallback& callback);

    /**
     * @brief 从指定本地序列号之后创建内部观察订阅者
     * @param topic_name Topic名称
     * @param callback 本地消息观察回调
     * @param start_after_sequence 跳过不大于该序列号的消息；0 表示补读仍保留的消息
     * @return 订阅者智能指针，失败时返回nullptr
     */
    std::shared_ptr<Subscriber> create_observer(
        const std::string& topic_name,
        const LocalMessageCallback& callback,
        uint64_t start_after_sequence);

    /**
     * @brief 发布数据并返回本地序列号
     * @param topic_name Topic名称
     * @param data 数据指针
     * @param size 数据大小
     * @return 发布成功返回本地序列号，失败返回0
     */
    uint64_t publish_and_get_sequence(const std::string& topic_name,
                                      const void* data,
                                      size_t size);

    /**
     * @brief 发布数据，并在消息对observer可见前同步报告本地序列号。
     * @param before_visible 序列号已分配但尚未发布可见时调用。
     * @return 发布成功返回本地序列号，失败返回0。
     */
    uint64_t publish_and_get_sequence(
        const std::string& topic_name,
        const void* data,
        size_t size,
        const LocalSequenceAssignedCallback& before_visible);

    /**
     * @brief 初始化DDS系统
     * @param shared_memory_size 共享内存大小，默认128MB
     * @return 初始化成功返回true，失败返回false
     */
    bool initialize(size_t shared_memory_size = 128 * 1024 * 1024);
    
    /**
     * @brief 关闭DDS系统，清理所有资源
     */
    void shutdown();

private:
    friend class DdsGatewayLocalBus;

    DDSCore() = default;
    ~DDSCore() = default;
    
    // 禁止拷贝和赋值
    DDSCore(const DDSCore&) = delete;
    DDSCore& operator=(const DDSCore&) = delete;

    struct TopicBinding {
        std::shared_ptr<RuntimeState> runtime_state;
        TopicMetadata* metadata{nullptr};
        RingBuffer* ring_buffer{nullptr};
    };

    mutable std::mutex lifecycle_mutex_;                ///< 串行化运行期安装、摘除和实体绑定。
    std::shared_ptr<RuntimeState> runtime_state_;        ///< 当前活动的进程内资源 epoch。

    bool initialize_locked(size_t shared_memory_size);
    bool bind_topic_locked(const std::string& topic_name,
                           bool enable_checksum,
                           bool initialize_if_needed,
                           TopicBinding& binding);

    std::shared_ptr<Publisher> create_publisher_impl(
        const std::string& topic_name,
        bool enable_checksum,
        bool initialize_if_needed);
    std::shared_ptr<Publisher> create_publisher_if_initialized(
        const std::string& topic_name,
        bool enable_checksum);

    std::shared_ptr<Subscriber> create_observer_impl(
        const std::string& topic_name,
        const LocalMessageCallback& callback,
        uint64_t start_after_sequence,
        bool capture_current_sequence,
        bool initialize_if_needed);
    std::shared_ptr<Subscriber> create_observer_if_initialized(
        const std::string& topic_name,
        const LocalMessageCallback& callback,
        uint64_t start_after_sequence);
    
    /**
     * @brief 获取当前进程名称
     * @return 进程名称字符串
     */
    std::string get_process_name();
};

} // namespace DDS
} // namespace MB_DDF
