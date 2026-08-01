#pragma once

/**
 * @file DomainGateway.h
 * @brief DDS跨域网关主控制类
 *
 * DomainGateway负责把本地域DDS Topic消息封装成GatewayEnvelope发送到外部端点，
 * 同时从外部端点接收远端信封并重新发布到本地域。它包含Topic扫描、端点线程、
 * TTL转发、远端去重和本地回灌抑制等逻辑。
 */

#include "MB_DDF/DDS/ExternalEndpoint.h"
#include "MB_DDF/DDS/Gateway/GatewayEnvelope.h"
#include "MB_DDF/DDS/Gateway/GatewayLocalBus.h"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MB_DDF {
namespace DDS {

/**
 * @brief 单个DDS域网关的运行配置。
 */
struct DomainGatewayConfig {
    uint32_t domain_id{0};                         ///< 本地域ID，跨域转发时用于判断消息是否回到源域。
    uint64_t gateway_id{0};                        ///< 本网关ID；为0时start()会自动生成非零随机ID。
    uint32_t topic_scan_period_ms{100};            ///< 后台扫描新增Topic的周期，单位毫秒。
    uint8_t default_ttl{16};                       ///< 本地新消息封装后的默认TTL，0表示不向外发送。
    std::string internal_topic_prefix{"gateway://"}; ///< 内部控制Topic前缀，匹配后不会跨域转发。
};

/**
 * @brief 网关连接的外部链路配置。
 */
struct GatewayEndpointConfig {
    std::string name;              ///< 端点名称，仅用于日志和诊断。
    ExternalEndpointRef endpoint;  ///< 外部收发端点，如UDP、串口或自定义链路。
    bool enabled{true};            ///< 是否参与收发；禁用端点会保留配置但不启动线程。
};

/**
 * @brief DDS跨域网关。
 *
 * 启动后会订阅本地域非内部Topic，将本地消息封装发送到所有启用端点；
 * 收到远端信封后会在本地域发布payload，并在TTL允许时继续转发给其他端点。
 */
class DomainGateway {
public:
    /**
     * @brief 构造网关实例。
     * @param config 网关配置。
     * @param local_bus 本地DDS访问接口，生产环境通常为DdsGatewayLocalBus。
     */
    explicit DomainGateway(DomainGatewayConfig config,
                           std::shared_ptr<GatewayLocalBus> local_bus);

    /// 析构时自动停止后台线程，并阻止已注册回调继续访问析构中的对象。
    ~DomainGateway();

    DomainGateway(const DomainGateway&) = delete;
    DomainGateway& operator=(const DomainGateway&) = delete;

    /**
     * @brief 添加一个外部端点。
     * @return 添加成功返回true；端点为空或网关已经运行时返回false。
     */
    bool add_endpoint(const GatewayEndpointConfig& endpoint);

    /**
     * @brief 启动Topic扫描线程和所有启用端点的接收线程。
     * @return 启动成功返回true；重复调用视为成功。
     */
    bool start();

    /// 停止扫描线程和端点接收线程，等待所有线程退出。
    void stop();

    /// 立即扫描一次本地域Topic，发现新Topic时建立观察订阅。
    void scan_topics_once();

    /// 从指定端点轮询一次外部数据，通常由端点接收线程调用，也可用于测试。
    void poll_once(size_t ep_slot);

private:
    /**
     * @brief 单个外部端点的运行状态。
     */
    struct EndpointState {
        explicit EndpointState(GatewayEndpointConfig config_value)
            : config(std::move(config_value)) {}

        GatewayEndpointConfig config;        ///< 静态端点配置。
        std::mutex send_mutex;               ///< 串行化同一端点的send调用，避免底层链路并发写。
        std::atomic<bool> running{false};    ///< 该端点接收线程的运行标志。
        std::thread receive_thread;          ///< 端点接收线程。
        std::vector<uint8_t> receive_buffer; ///< 按端点MTU复用的接收缓冲区。
    };

    /**
     * @brief 订阅回调与DomainGateway生命周期之间的共享门控。
     *
     * 回调只持有该对象，不直接依赖DomainGateway寿命。stop()/析构先关闭入口，
     * 再等待已经进入的回调退出，从而避免check-then-use导致的悬空this访问。
     */
    struct LocalCallbackGate {
        std::mutex mutex;
        std::condition_variable idle_cv;
        DomainGateway* owner{nullptr};
        size_t in_flight{0};
        bool accepting{true};
    };

    /**
     * @brief 本地序列号键，用于抑制远端消息发布到本地后再次被网关转发。
     */
    struct LocalSequenceKey {
        std::string topic_name;  ///< 本地Topic名称。
        uint64_t sequence{0};    ///< DDSCore发布时返回的本地序列号。

        bool operator==(const LocalSequenceKey& other) const {
            return topic_name == other.topic_name && sequence == other.sequence;
        }
    };

    /// LocalSequenceKey的哈希函数，供unordered_set使用。
    struct LocalSequenceKeyHash {
        size_t operator()(const LocalSequenceKey& key) const {
            return std::hash<std::string>{}(key.topic_name) ^
                   (std::hash<uint64_t>{}(key.sequence) << 1);
        }
    };

    /**
     * @brief 远端消息全局去重键。
     *
     * 同一条消息在多端点拓扑中可能从不同路径抵达，必须用源域、源网关和源消息ID去重。
     */
    struct RemoteMessageKey {
        uint32_t origin_domain_id{0};   ///< 首次产生消息的DDS域ID。
        uint64_t origin_gateway_id{0};  ///< 首次封装消息的网关ID。
        uint64_t message_id{0};         ///< 源网关分配的消息ID。

        bool operator==(const RemoteMessageKey& other) const {
            return origin_domain_id == other.origin_domain_id &&
                   origin_gateway_id == other.origin_gateway_id &&
                   message_id == other.message_id;
        }
    };

    /// RemoteMessageKey的组合哈希函数，供unordered_set使用。
    struct RemoteMessageKeyHash {
        size_t operator()(const RemoteMessageKey& key) const {
            size_t h = std::hash<uint32_t>{}(key.origin_domain_id);
            h ^= std::hash<uint64_t>{}(key.origin_gateway_id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<uint64_t>{}(key.message_id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    /// 去重/抑制窗口最大条目数，超过后按插入顺序淘汰旧记录。
    static constexpr size_t MAX_WINDOW_ENTRIES = 4096;

    /// 判断Topic是否为网关内部Topic，内部Topic不参与跨域转发。
    bool is_internal_topic(const std::string& topic_name) const;

    /// 按槽位安全获取端点状态，槽位越界时返回nullptr。
    std::shared_ptr<EndpointState> get_endpoint(size_t ep_slot);

    /// 指定端点的接收线程主循环。
    void receive_loop(size_t ep_slot, std::shared_ptr<EndpointState> endpoint);

    /// 周期性扫描本地域Topic的后台线程主循环。
    void scan_loop();

    /// 扫描Topic；启动边界完成前使用枚举快照，之后的新Topic从保留历史起点观察。
    void scan_topics_once(bool complete_startup_scan);

    /// 经共享门控安全分发本地观察回调。
    static void dispatch_local_message(const std::shared_ptr<LocalCallbackGate>& gate,
                                       const LocalMessageView& message);

    /// 打开本地回调入口，用于构造后及stop()后的重新启动。
    void activate_local_callbacks();

    /// 关闭本地回调入口并等待已经进入的回调全部退出。
    void deactivate_local_callbacks_and_wait();

    /// 本地DDS观察回调入口，将本地消息封装后发往外部端点。
    void on_local_message(const LocalMessageView& message);

    /// 处理远端信封：去重、回灌到本地域，并在TTL允许时继续转发。
    void handle_remote_envelope(size_t ingress_slot, const GatewayEnvelope& envelope);

    /// 序列化信封并发送到指定端点。
    bool send_to_endpoint(EndpointState& endpoint, const GatewayEnvelope& envelope);

    /// 向所有启用端点发送信封，可选择跳过消息来源端点。
    void send_to_enabled_endpoints(const GatewayEnvelope& envelope,
                                   size_t excluded_slot,
                                   bool has_excluded_endpoint);

    /// 如果本地序列号位于回灌抑制窗口中，则消费该记录并返回true。
    bool consume_suppressed_sequence(const std::string& topic_name, uint64_t sequence);

    /// 将本地发布序列号加入回灌抑制窗口。
    void add_suppressed_sequence(const std::string& topic_name, uint64_t sequence);

    /// 标记远端消息已处理；首次出现返回true，重复消息返回false。
    bool mark_remote_seen(const RemoteMessageKey& key);

    DomainGatewayConfig config_;                 ///< 网关运行配置，start()可能补齐自动gateway_id。
    std::shared_ptr<GatewayLocalBus> local_bus_; ///< 本地DDS访问接口。
    std::shared_ptr<LocalCallbackGate> callback_gate_; ///< 跨越observer寿命的安全回调门控。

    std::mutex lifecycle_mutex_;                 ///< 串行化add_endpoint/start/stop生命周期操作。
    std::atomic<bool> running_{false};           ///< 网关整体运行标志。
    std::atomic<uint64_t> local_message_counter_{0}; ///< 源网关消息ID递增计数器。
    std::thread scan_thread_;                    ///< Topic扫描后台线程。

    std::mutex endpoints_mutex_;                 ///< 保护端点列表。
    std::vector<std::shared_ptr<EndpointState>> endpoints_; ///< 外部端点状态列表，槽位即端点索引。

    std::mutex scan_mutex_;                      ///< 串行化启动扫描、后台扫描和手工扫描。
    bool startup_scan_completed_{false};         ///< start()的启动边界枚举是否已经完成。
    std::mutex topics_mutex_;                    ///< 保护Topic订阅状态。
    std::unordered_set<std::string> monitored_topics_; ///< 已建立观察订阅的本地Topic名称。
    std::unordered_map<std::string, uint64_t> pending_start_boundaries_; ///< 启动Topic订阅重试边界。

    std::mutex suppression_mutex_; ///< 保护本地回灌抑制窗口。
    std::unordered_set<LocalSequenceKey, LocalSequenceKeyHash> suppressed_local_sequences_; ///< 可快速查询的抑制键集合。
    std::deque<LocalSequenceKey> suppressed_order_; ///< 抑制键插入顺序，用于窗口淘汰。

    std::mutex seen_mutex_; ///< 保护远端消息去重窗口。
    std::unordered_set<RemoteMessageKey, RemoteMessageKeyHash> seen_remote_messages_; ///< 已处理远端消息集合。
    std::deque<RemoteMessageKey> seen_order_; ///< 远端消息插入顺序，用于窗口淘汰。
};

} // namespace DDS
} // namespace MB_DDF
