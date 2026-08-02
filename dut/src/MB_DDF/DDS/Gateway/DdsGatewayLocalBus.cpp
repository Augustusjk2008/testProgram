#include "MB_DDF/DDS/Gateway/DdsGatewayLocalBus.h"

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/Publisher.h"
#include "MB_DDF/DDS/Subscriber.h"

#include <utility>

namespace MB_DDF {
namespace DDS {

DdsGatewayLocalBus::~DdsGatewayLocalBus() {
    reset_subscriptions();

    std::unordered_map<std::string, std::shared_ptr<Publisher>> publishers;
    {
        std::lock_guard<std::mutex> lock(publishers_mutex_);
        publishers.swap(publishers_);
    }
}

std::vector<LocalTopicInfo> DdsGatewayLocalBus::list_topics() {
    // DDSCore负责维护Topic注册表，这里只把信息原样暴露给DomainGateway。
    return DDSCore::instance().list_topics();
}

bool DdsGatewayLocalBus::try_list_topics(std::vector<LocalTopicInfo>& topics) {
    return DDSCore::instance().try_list_topics(topics);
}

void DdsGatewayLocalBus::reset_subscriptions() {
    std::vector<std::shared_ptr<Subscriber>> observers;
    {
        std::lock_guard<std::mutex> lock(observers_mutex_);
        observers.swap(observers_);
    }
    // Subscriber 析构会停止并 join 工作线程，必须在 observers_mutex_ 外执行。
}

bool DdsGatewayLocalBus::subscribe_topic(const std::string& topic_name,
                                         LocalMessageCallback callback,
                                         uint64_t start_after_sequence) {
    // create_observer返回的Subscriber需要持续存活，否则底层观察线程会随对象析构停止。
    auto subscriber = DDSCore::instance().create_observer_if_initialized(
        topic_name, std::move(callback), start_after_sequence);
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
    auto& dds = DDSCore::instance();
    if (!dds.is_initialized()) {
        return 0;
    }

    std::shared_ptr<Publisher> publisher;
    std::shared_ptr<Publisher> retired_publisher;
    {
        // receive() 将来可能由多个网络工作线程并发调用。首次创建也放在锁内，确保
        // 同一 Topic 不会因竞态生成多个 Publisher；命中缓存后这里只复制 shared_ptr。
        std::lock_guard<std::mutex> lock(publishers_mutex_);
        auto found = publishers_.find(topic_name);
        if (found == publishers_.end() || !found->second ||
            !found->second->is_runtime_active()) {
            publisher = dds.create_publisher_if_initialized(topic_name, true);
            if (!publisher) {
                return 0;
            }
            if (found == publishers_.end()) {
                publishers_.emplace(topic_name, publisher);
            } else {
                retired_publisher = std::move(found->second);
                found->second = publisher;
            }
        } else {
            publisher = found->second;
        }
    }

    // 返回本地序列号给DomainGateway，用于抑制“远端消息回灌后再次被本地观察到”的回环。
    // 实际写 RingBuffer 时不持有缓存锁，避免不同 Topic 的网络接收互相串行化。
    return publisher->publish_and_get_sequence(data, size, before_visible);
}

} // namespace DDS
} // namespace MB_DDF
