#include "MB_DDF/DDS/Gateway/DdsGatewayLocalBus.h"

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/Subscriber.h"

#include <utility>

namespace MB_DDF {
namespace DDS {

std::vector<LocalTopicInfo> DdsGatewayLocalBus::list_topics() {
    // DDSCore负责维护Topic注册表，这里只把信息原样暴露给DomainGateway。
    return DDSCore::instance().list_topics();
}

bool DdsGatewayLocalBus::subscribe_topic(const std::string& topic_name,
                                         LocalMessageCallback callback) {
    // create_observer返回的Subscriber需要持续存活，否则底层观察线程会随对象析构停止。
    auto subscriber = DDSCore::instance().create_observer(topic_name, std::move(callback));
    if (!subscriber) {
        return false;
    }

    std::lock_guard<std::mutex> lock(observers_mutex_);
    observers_.push_back(std::move(subscriber));
    return true;
}

uint64_t DdsGatewayLocalBus::publish_topic(const std::string& topic_name,
                                           const void* data,
                                           size_t size,
                                           const LocalSequenceAssignedCallback& before_visible) {
    // 返回本地序列号给DomainGateway，用于抑制“远端消息回灌后再次被本地观察到”的回环。
    return DDSCore::instance().publish_and_get_sequence(
        topic_name, data, size, before_visible);
}

} // namespace DDS
} // namespace MB_DDF
