#include "MB_DDF/DDS/Gateway/DomainGateway.h"

#include "MB_DDF/DDS/EntityId.h"
#include "MB_DDF/Debug/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace MB_DDF {
namespace DDS {

DomainGateway::DomainGateway(DomainGatewayConfig config,
                             std::shared_ptr<GatewayLocalBus> local_bus)
    : config_(std::move(config)),
      local_bus_(std::move(local_bus)),
      callback_gate_(std::make_shared<LocalCallbackGate>()) {
    callback_gate_->owner = this;
}

DomainGateway::~DomainGateway() {
    // stop()会关闭共享回调门、等待在途回调并停止网关线程。
    stop();
}

bool DomainGateway::add_endpoint(const GatewayEndpointConfig& endpoint) {
    // 端点为空无法收发；运行期间禁止追加端点，避免接收线程槽位和端点列表不同步。
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!endpoint.endpoint) {
        LOG_ERROR << "DomainGateway add_endpoint failed, endpoint is null";
        return false;
    }
    if (running_.load(std::memory_order_acquire)) {
        LOG_ERROR << "DomainGateway add_endpoint failed, gateway is running";
        return false;
    }

    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    endpoints_.push_back(std::make_shared<EndpointState>(endpoint));
    return true;
}

bool DomainGateway::start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    // local_bus_是网关访问本地域DDS的唯一入口，缺失时无法扫描、订阅或发布。
    if (!local_bus_) {
        LOG_ERROR << "DomainGateway start failed, local bus is null";
        return false;
    }

    // compare_exchange保证多次start()只有第一次真正启动线程。
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }

    if (config_.gateway_id == 0) {
        // SylixOS 的 random_device 可能跨进程返回相同序列。网关重启后 message_id
        // 会重新从 1 计数，因此 gateway_id 必须包含 PID/时间，避免对端误判为旧包。
        config_.gateway_id = generate_entity_id();
    }

    activate_local_callbacks();

    // 启动前先扫描一次。已有Topic以枚举快照序列为边界，只桥接快照后的消息；
    // 这样既不重放历史，也不会丢失list_topics()到observer注册之间的并发发布。
    scan_topics_once(true);

    {
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        for (size_t i = 0; i < endpoints_.size(); ++i) {
            auto endpoint = endpoints_[i];
            // 禁用端点不启动接收线程；已有线程说明该端点已经启动过。
            if (!endpoint->config.enabled || endpoint->receive_thread.joinable()) {
                continue;
            }
            endpoint->running.store(true, std::memory_order_release);
            endpoint->receive_thread =
                std::thread(&DomainGateway::receive_loop, this, i, endpoint);
        }
    }

    scan_thread_ = std::thread(&DomainGateway::scan_loop, this);
    return true;
}

void DomainGateway::stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    // scan_topics_once()允许在未start时建立observer，因此无论running_旧值如何，
    // stop()都必须关闭并排空回调，不能提前返回。
    running_.store(false, std::memory_order_release);

    std::vector<std::shared_ptr<EndpointState>> endpoints;
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        endpoints = endpoints_;
    }

    // 先阻止接收线程进入下一轮，再关闭本地回调入口。
    for (auto& endpoint : endpoints) {
        if (endpoint) {
            endpoint->running.store(false, std::memory_order_release);
        }
    }
    deactivate_local_callbacks_and_wait();

    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }

    for (auto& endpoint : endpoints) {
        if (endpoint && endpoint->receive_thread.joinable()) {
            endpoint->receive_thread.join();
        }
    }

    // 每次stopped -> running都是新的实时桥接会话。下一次启动前重新建立
    // Topic快照边界，避免把停机期间创建Topic的历史当成实时消息转发。
    {
        std::lock_guard<std::mutex> scan_lock(scan_mutex_);
        startup_scan_completed_ = false;
        std::lock_guard<std::mutex> topics_lock(topics_mutex_);
        pending_start_boundaries_.clear();
    }
}

void DomainGateway::scan_topics_once() {
    scan_topics_once(false);
}

void DomainGateway::scan_topics_once(bool complete_startup_scan) {
    std::lock_guard<std::mutex> scan_lock(scan_mutex_);
    if (!local_bus_) {
        if (complete_startup_scan) {
            startup_scan_completed_ = true;
        }
        return;
    }

    const bool startup_phase = !startup_scan_completed_;
    const auto topics = local_bus_->list_topics();
    for (const auto& topic : topics) {
        // 空Topic无法路由；内部Topic用于网关控制或状态，不应被再次跨域传播。
        if (topic.topic_name.empty() || is_internal_topic(topic.topic_name)) {
            continue;
        }

        uint64_t start_after_sequence = 0;
        {
            std::lock_guard<std::mutex> lock(topics_mutex_);
            // monitored_topics_同时承担去重和“已经建立观察订阅”的状态记录。
            if (monitored_topics_.find(topic.topic_name) != monitored_topics_.end()) {
                continue;
            }
            if (startup_phase) {
                if (complete_startup_scan) {
                    pending_start_boundaries_[topic.topic_name] = topic.current_sequence;
                } else {
                    pending_start_boundaries_.try_emplace(
                        topic.topic_name, topic.current_sequence);
                }
            }
            monitored_topics_.insert(topic.topic_name);
            const auto pending = pending_start_boundaries_.find(topic.topic_name);
            if (pending != pending_start_boundaries_.end()) {
                start_after_sequence = pending->second;
            }
        }

        // observer可能比DomainGateway活得更久，只捕获共享门控，不捕获裸this。
        auto gate = callback_gate_;
        const bool subscribed = local_bus_->subscribe_topic(
            topic.topic_name,
            [gate](const LocalMessageView& message) {
                DomainGateway::dispatch_local_message(gate, message);
            },
            start_after_sequence);

        if (!subscribed) {
            // 订阅失败时回滚monitored_topics_，允许后续扫描周期再次尝试。
            std::lock_guard<std::mutex> lock(topics_mutex_);
            monitored_topics_.erase(topic.topic_name);
            LOG_ERROR << "DomainGateway failed to subscribe topic: " << topic.topic_name;
        } else {
            std::lock_guard<std::mutex> lock(topics_mutex_);
            pending_start_boundaries_.erase(topic.topic_name);
        }
    }

    if (complete_startup_scan) {
        startup_scan_completed_ = true;
    }
}

void DomainGateway::poll_once(size_t ep_slot) {
    auto endpoint = get_endpoint(ep_slot);
    if (!endpoint || !endpoint->config.enabled || !endpoint->config.endpoint) {
        return;
    }

    // 使用端点MTU作为接收缓冲区容量；MTU为0表示端点未给出上限，这里至少保留1字节。
    const size_t mtu = endpoint->config.endpoint->mtu();
    const size_t capacity = mtu == 0 ? 1 : mtu;
    if (endpoint->receive_buffer.size() != capacity) {
        endpoint->receive_buffer.resize(capacity);
    }

    // 接收超时使用10ms，便于stop()设置运行标志后线程能较快退出。
    const int32_t received = endpoint->config.endpoint->receive(
        endpoint->receive_buffer.data(),
        endpoint->receive_buffer.size(),
        static_cast<uint32_t>(10000));
    if (received <= 0) {
        return;
    }

    GatewayEnvelope envelope{};
    // 解析失败通常表示链路噪声、协议版本不匹配或payload校验失败，直接丢弃。
    if (!deserialize_gateway_envelope(endpoint->receive_buffer.data(),
                                      static_cast<size_t>(received),
                                      envelope)) {
        LOG_WARN << "DomainGateway dropped invalid envelope from endpoint: "
                 << endpoint->config.name;
        return;
    }

    handle_remote_envelope(ep_slot, envelope);
}

bool DomainGateway::is_internal_topic(const std::string& topic_name) const {
    // rfind(prefix, 0)用于判断字符串是否以prefix开头。
    return !config_.internal_topic_prefix.empty() &&
           topic_name.rfind(config_.internal_topic_prefix, 0) == 0;
}

std::shared_ptr<DomainGateway::EndpointState> DomainGateway::get_endpoint(size_t ep_slot) {
    // 返回shared_ptr副本，确保调用方使用期间EndpointState不会被释放。
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    if (ep_slot >= endpoints_.size()) {
        return nullptr;
    }
    return endpoints_[ep_slot];
}

void DomainGateway::receive_loop(size_t ep_slot,
                                 std::shared_ptr<EndpointState> endpoint) {
    // 端点线程只负责从单个外部端点收包，解析后的业务处理交给poll_once。
    while (running_.load(std::memory_order_acquire) &&
           endpoint &&
           endpoint->running.load(std::memory_order_acquire)) {
        poll_once(ep_slot);
    }
}

void DomainGateway::scan_loop() {
    // 周期最小为1ms，避免配置为0时进入无休眠的忙循环。
    const auto period = std::chrono::milliseconds(
        std::max<uint32_t>(config_.topic_scan_period_ms, 1));
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(period);
        if (running_.load(std::memory_order_acquire)) {
            scan_topics_once(false);
        }
    }
}

void DomainGateway::dispatch_local_message(const std::shared_ptr<LocalCallbackGate>& gate,
                                           const LocalMessageView& message) {
    if (!gate) {
        return;
    }

    DomainGateway* owner = nullptr;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (!gate->accepting || gate->owner == nullptr) {
            return;
        }
        ++gate->in_flight;
        owner = gate->owner;
    }

    auto finish = [&gate]() {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->in_flight > 0) {
            --gate->in_flight;
        }
        if (gate->in_flight == 0) {
            gate->idle_cv.notify_all();
        }
    };

    try {
        owner->on_local_message(message);
    } catch (...) {
        finish();
        throw;
    }
    finish();
}

void DomainGateway::activate_local_callbacks() {
    std::lock_guard<std::mutex> lock(callback_gate_->mutex);
    callback_gate_->owner = this;
    callback_gate_->accepting = true;
}

void DomainGateway::deactivate_local_callbacks_and_wait() {
    std::unique_lock<std::mutex> lock(callback_gate_->mutex);
    callback_gate_->accepting = false;
    callback_gate_->owner = nullptr;
    callback_gate_->idle_cv.wait(lock, [this] {
        return callback_gate_->in_flight == 0;
    });
}

void DomainGateway::on_local_message(const LocalMessageView& message) {
    // 本地消息入口：仅处理有效的用户Topic和有效payload视图。
    if (message.topic_name.empty() || is_internal_topic(message.topic_name)) {
        return;
    }
    if (message.size > 0 && message.data == nullptr) {
        return;
    }
    // 远端消息发布到本地域后会被本地观察者再次看见，命中抑制窗口时不再外发。
    if (consume_suppressed_sequence(message.topic_name, message.sequence)) {
        return;
    }
    // default_ttl为0时本网关只接收远端消息，不主动把本地消息送出域外。
    if (config_.default_ttl == 0) {
        return;
    }

    // 本地原生消息成为一条新的跨域消息，origin_*字段记录源域和源网关。
    GatewayEnvelope envelope{};
    envelope.header.flags = 0;
    envelope.header.origin_domain_id = config_.domain_id;
    envelope.header.sender_domain_id = config_.domain_id;
    envelope.header.origin_gateway_id = config_.gateway_id;
    envelope.header.message_id = local_message_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
    envelope.header.ttl = config_.default_ttl;
    envelope.topic_name = message.topic_name;
    if (message.size > 0) {
        // 外部发送可能晚于本地回调返回，因此必须把payload复制进信封。
        const auto* bytes = static_cast<const uint8_t*>(message.data);
        envelope.payload.assign(bytes, bytes + message.size);
    }

    // 本地新消息没有入口端点，需要发送到所有启用端点。
    send_to_enabled_endpoints(envelope, 0, false);
}

void DomainGateway::handle_remote_envelope(size_t ingress_slot,
                                           const GatewayEnvelope& envelope) {
    // 内部Topic在任一入口都不得跨域传播；远端伪造同样必须在发布和转发前丢弃。
    if (envelope.topic_name.empty() || is_internal_topic(envelope.topic_name)) {
        return;
    }

    // 先做远端全局去重。重复信封不再发布到本地，也不继续转发。
    RemoteMessageKey key{
        envelope.header.origin_domain_id,
        envelope.header.origin_gateway_id,
        envelope.header.message_id};
    if (!mark_remote_seen(key)) {
        return;
    }

    // 如果消息源域就是本地域，说明它绕了一圈回来；去重记录已保留，但不回灌本地。
    if (envelope.header.origin_domain_id == config_.domain_id) {
        return;
    }

    if (!local_bus_) {
        return;
    }

    // 将远端payload发布到同名本地Topic。空payload时传入nullptr，保持本地发布接口约定。
    const uint64_t local_sequence = local_bus_->publish_topic(
        envelope.topic_name,
        envelope.payload.empty() ? nullptr : envelope.payload.data(),
        envelope.payload.size(),
        [this, &envelope](uint64_t sequence) {
            // 在RingBuffer发布序列号前登记，observer不可能先于该记录看到消息。
            add_suppressed_sequence(envelope.topic_name, sequence);
        });
    (void)local_sequence;

    // TTL大于1才允许继续转发；转发时保留origin_*字段，只更新sender和递减TTL。
    if (envelope.header.ttl > 1) {
        GatewayEnvelope forwarded = envelope;
        forwarded.header.sender_domain_id = config_.domain_id;
        forwarded.header.ttl = static_cast<uint8_t>(envelope.header.ttl - 1);
        send_to_enabled_endpoints(forwarded, ingress_slot, true);
    }
}

bool DomainGateway::send_to_endpoint(EndpointState& endpoint,
                                     const GatewayEnvelope& envelope) {
    if (!endpoint.config.enabled || !endpoint.config.endpoint) {
        return false;
    }

    // 发送前统一序列化并重新计算长度/CRC等协议字段。
    auto buffer = serialize_gateway_envelope(envelope);
    if (buffer.empty()) {
        LOG_ERROR << "DomainGateway failed to serialize envelope";
        return false;
    }

    // 外部端点声明MTU时，网关不拆包；超长信封直接失败并记录日志。
    const size_t mtu = endpoint.config.endpoint->mtu();
    if (mtu != 0 && buffer.size() > mtu) {
        LOG_ERROR << "DomainGateway envelope exceeds endpoint mtu, endpoint="
                  << endpoint.config.name << ", size=" << buffer.size()
                  << ", mtu=" << mtu;
        return false;
    }

    // 同一端点的send可能来自多个回调/接收线程，使用端点级锁串行化底层写操作。
    std::lock_guard<std::mutex> lock(endpoint.send_mutex);
    return endpoint.config.endpoint->send(buffer.data(), buffer.size());
}

void DomainGateway::send_to_enabled_endpoints(const GatewayEnvelope& envelope,
                                              size_t excluded_slot,
                                              bool has_excluded_endpoint) {
    std::vector<std::pair<size_t, std::shared_ptr<EndpointState>>> endpoints;
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        endpoints.reserve(endpoints_.size());
        for (size_t i = 0; i < endpoints_.size(); ++i) {
            endpoints.emplace_back(i, endpoints_[i]);
        }
    }

    // 复制端点列表后再发送，避免持有endpoints_mutex_执行可能阻塞的外部I/O。
    for (const auto& [index, endpoint] : endpoints) {
        if (!endpoint ||
            (has_excluded_endpoint && index == excluded_slot) ||
            !endpoint->config.enabled) {
            continue;
        }
        (void)send_to_endpoint(*endpoint, envelope);
    }
}

bool DomainGateway::consume_suppressed_sequence(const std::string& topic_name,
                                                uint64_t sequence) {
    std::lock_guard<std::mutex> lock(suppression_mutex_);
    LocalSequenceKey key{topic_name, sequence};
    auto it = suppressed_local_sequences_.find(key);
    if (it == suppressed_local_sequences_.end()) {
        return false;
    }
    // 抑制记录只需要命中一次，消费后删除，避免长期影响后续相同序列号场景。
    suppressed_local_sequences_.erase(it);
    auto order_it = std::find(suppressed_order_.begin(), suppressed_order_.end(), key);
    if (order_it != suppressed_order_.end()) {
        suppressed_order_.erase(order_it);
    }
    return true;
}

void DomainGateway::add_suppressed_sequence(const std::string& topic_name,
                                            uint64_t sequence) {
    std::lock_guard<std::mutex> lock(suppression_mutex_);
    LocalSequenceKey key{topic_name, sequence};
    if (suppressed_local_sequences_.insert(key).second) {
        suppressed_order_.push_back(std::move(key));
    }

    // 窗口限制防止长时间运行后去重集合无限增长。
    while (suppressed_order_.size() > MAX_WINDOW_ENTRIES) {
        suppressed_local_sequences_.erase(suppressed_order_.front());
        suppressed_order_.pop_front();
    }
}

bool DomainGateway::mark_remote_seen(const RemoteMessageKey& key) {
    std::lock_guard<std::mutex> lock(seen_mutex_);
    if (!seen_remote_messages_.insert(key).second) {
        return false;
    }

    seen_order_.push_back(key);
    // 只保留最近一段远端消息ID，兼顾环路去重和内存上限。
    while (seen_remote_messages_.size() > MAX_WINDOW_ENTRIES &&
           !seen_order_.empty()) {
        seen_remote_messages_.erase(seen_order_.front());
        seen_order_.pop_front();
    }
    return true;
}

} // namespace DDS
} // namespace MB_DDF
