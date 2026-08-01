#pragma once

/**
 * @file GatewayLocalBus.h
 * @brief 跨域网关访问本地DDS域的抽象总线接口
 *
 * DomainGateway只依赖该抽象接口，不直接绑定DDSCore实现，便于测试或替换本地总线。
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MB_DDF {
namespace DDS {

/**
 * @brief 本地域中可被网关监控的Topic信息。
 */
struct LocalTopicInfo {
    uint32_t topic_id{0};        ///< DDS本地域内的Topic ID。
    std::string topic_name;      ///< Topic名称，网关按名称订阅和跨域发布。
    size_t ring_buffer_size{0};  ///< Topic对应环形缓冲区大小，当前网关只透传该元信息。
    uint64_t current_sequence{0}; ///< 枚举Topic时已经可见的最新本地序列号。
};

/**
 * @brief 本地DDS消息的只读视图。
 *
 * 回调期间data指向DDS消息负载，网关需要在回调内完成拷贝，不能长期保存该指针。
 */
struct LocalMessageView {
    std::string topic_name;      ///< 消息所属Topic名称。
    uint64_t sequence{0};        ///< 本地域发布序列号，用于识别网关自己回灌的消息。
    uint64_t timestamp{0};       ///< 本地DDS消息时间戳，单位由底层DDS消息头定义。
    const void* data{nullptr};   ///< 用户负载指针；size为0时允许为空。
    size_t size{0};              ///< 用户负载字节数。
};

/// 本地Topic观察回调，DomainGateway用它捕获本地域新发布的消息。
using LocalMessageCallback = std::function<void(const LocalMessageView&)>;

/// 本地发布序列号已分配、但消息尚未对observer可见时调用。
using LocalSequenceAssignedCallback = std::function<void(uint64_t)>;

/**
 * @brief 网关对本地域DDS能力的最小依赖接口。
 */
class GatewayLocalBus {
public:
    virtual ~GatewayLocalBus() = default;

    /**
     * @brief 枚举当前本地域已注册的Topic。
     * @return Topic信息列表。
     */
    virtual std::vector<LocalTopicInfo> list_topics() = 0;

    /**
     * @brief 订阅指定本地Topic并用回调观察后续消息。
     * @param topic_name 需要监控的Topic名称。
     * @param callback 收到本地消息时调用的回调。
     * @param start_after_sequence 仅观察该序列号之后的消息；0表示从保留历史起点观察。
     * @return 成功建立观察返回true，失败返回false。
     */
    virtual bool subscribe_topic(const std::string& topic_name,
                                 LocalMessageCallback callback,
                                 uint64_t start_after_sequence) = 0;

    /**
     * @brief 将远端收到的payload发布到本地Topic。
     * @param topic_name 目标Topic名称。
     * @param data payload指针；size为0时可以为空。
     * @param size payload字节数。
     * @param before_visible 序列号分配后、observer可见前同步调用；必须短时且不可抛异常。
     * @return 发布成功返回本地域序列号，失败返回0。
     */
    virtual uint64_t publish_topic(
        const std::string& topic_name,
        const void* data,
        size_t size,
        const LocalSequenceAssignedCallback& before_visible = {}) = 0;
};

} // namespace DDS
} // namespace MB_DDF
