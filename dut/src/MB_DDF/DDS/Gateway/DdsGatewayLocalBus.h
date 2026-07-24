#pragma once

/**
 * @file DdsGatewayLocalBus.h
 * @brief 基于DDSCore实现的网关本地总线适配器
 */

#include "MB_DDF/DDS/Gateway/GatewayLocalBus.h"

#include <memory>
#include <mutex>
#include <vector>

namespace MB_DDF {
namespace DDS {

class Subscriber;

/**
 * @brief GatewayLocalBus的DDSCore实现。
 *
 * 该类把DomainGateway需要的Topic枚举、观察订阅和本地发布能力映射到DDSCore。
 */
class DdsGatewayLocalBus : public GatewayLocalBus {
public:
    DdsGatewayLocalBus() = default;
    ~DdsGatewayLocalBus() override = default;

    /// 通过DDSCore枚举本地域已经注册的Topic。
    std::vector<LocalTopicInfo> list_topics() override;

    /// 创建内部观察订阅者，并保存Subscriber对象以维持订阅生命周期。
    bool subscribe_topic(const std::string& topic_name, LocalMessageCallback callback) override;

    /// 将网关收到的远端payload发布到本地域，并返回DDSCore分配的本地序列号。
    uint64_t publish_topic(
        const std::string& topic_name,
        const void* data,
        size_t size,
        const LocalSequenceAssignedCallback& before_visible = {}) override;

private:
    std::mutex observers_mutex_;                         ///< 保护observers_，允许扫描线程安全追加订阅者。
    std::vector<std::shared_ptr<Subscriber>> observers_; ///< 持有观察订阅者，防止回调订阅提前析构。
};

} // namespace DDS
} // namespace MB_DDF
