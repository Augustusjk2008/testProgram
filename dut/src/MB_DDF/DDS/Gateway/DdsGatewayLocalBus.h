#pragma once

/**
 * @file DdsGatewayLocalBus.h
 * @brief 基于DDSCore实现的网关本地总线适配器
 */

#include "MB_DDF/DDS/Gateway/GatewayLocalBus.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace MB_DDF {
namespace DDS {

class Subscriber;
class Publisher;

/**
 * @brief GatewayLocalBus的DDSCore实现。
 *
 * 该类把DomainGateway需要的Topic枚举、观察订阅和本地发布能力映射到DDSCore。
 */
class DdsGatewayLocalBus : public GatewayLocalBus {
public:
    DdsGatewayLocalBus() = default;
    ~DdsGatewayLocalBus() override;

    /// 通过DDSCore枚举本地域已经注册的Topic。
    std::vector<LocalTopicInfo> list_topics() override;

    /// 原子地区分未初始化状态与有效的空 Topic 快照。
    bool try_list_topics(std::vector<LocalTopicInfo>& topics) override;

    /// 释放当前 Gateway 会话建立的全部 observer。
    void reset_subscriptions() override;

    /// 创建指定序列边界之后的内部观察订阅者，并保存Subscriber对象以维持订阅生命周期。
    bool subscribe_topic(const std::string& topic_name,
                         LocalMessageCallback callback,
                         uint64_t start_after_sequence) override;

    /// 将网关收到的远端payload发布到本地域，并返回DDSCore分配的本地序列号。
    uint64_t publish_topic(
        const std::string& topic_name,
        const void* data,
        size_t size,
        const LocalSequenceAssignedCallback& before_visible = {}) override;

private:
    /**
     * Gateway 回灌远端消息时，每个 Topic 只缓存一个 Publisher。Publisher 持有创建
     * 它的 RuntimeState；shutdown 后旧 epoch 会变为 inactive，并在显式重新初始化后
     * 由 publish_topic() 替换。
     */
    std::mutex publishers_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Publisher>> publishers_;

    std::mutex observers_mutex_;                         ///< 保护observers_，允许扫描线程安全追加订阅者。
    std::vector<std::shared_ptr<Subscriber>> observers_; ///< 持有观察订阅者，防止回调订阅提前析构。
};

} // namespace DDS
} // namespace MB_DDF
