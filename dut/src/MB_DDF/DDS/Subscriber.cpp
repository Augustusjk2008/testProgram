/**
 * @file Subscriber.cpp
 * @brief 订阅者类实现
 * @date 2025-08-03
 * @author Jiangkai
 * 
 * 实现消息订阅功能，包括异步消息接收、回调处理和线程管理。
 */
#include "MB_DDF/DDS/Subscriber.h"
#include "MB_DDF/DDS/EntityId.h"
#include "MB_DDF/DDS/RingBuffer.h"
#include "MB_DDF/DDS/RuntimeState.h"
#include "MB_DDF/DDS/Message.h"
#include "MB_DDF/Debug/Logger.h"
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <utility>

namespace MB_DDF {
namespace DDS {

Subscriber::Subscriber(TopicMetadata* metadata,
                       RingBuffer* ring_buffer,
                       const std::string& subscriber_name,
                       ExternalEndpointRef external_io)
    : Subscriber(metadata,
                 ring_buffer,
                 subscriber_name,
                 std::move(external_io),
                 nullptr) {}

Subscriber::Subscriber(TopicMetadata* metadata,
                       RingBuffer* ring_buffer,
                       const std::string& subscriber_name,
                       ExternalEndpointRef external_io,
                       std::shared_ptr<RuntimeState> runtime_state)
    : metadata_(metadata), ring_buffer_(ring_buffer), callback_(nullptr),
      observer_callback_(nullptr),
      subscribed_(false), running_(false), worker_thread_(),
      external_io_(std::move(external_io)),
      runtime_state_(std::move(runtime_state)),
      subscriber_name_(subscriber_name) {
    // ID 必须跨进程唯一；SylixOS 的 random_device 可能在每个进程返回相同序列。
    subscriber_id_ = generate_entity_id();
    
    // 如果没有提供订阅者名称，生成默认名称
    if (subscriber_name_.empty()) {
        subscriber_name_ = "subscriber_" + std::to_string(subscriber_id_);
    }

    if (external_io_ != nullptr) {
        const size_t capacity = external_io_->mtu();
        receive_buffer_.resize(capacity > 0 ? capacity : 1);
    }
}

Subscriber::~Subscriber() {
    unsubscribe();
}

bool Subscriber::subscribe(MessageCallback callback) {
    if (subscribed_.load()) {
        LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_ << " already subscribed";
        return false; // 已经订阅
    }
    
    if (external_io_ == nullptr) {
        if (ring_buffer_ == nullptr || !local_runtime_active()) {
            LOG_ERROR << "Subscriber " << subscriber_id_ << " " << subscriber_name_ << " ring buffer is null";
            return false;
        }

        // 在RingBuffer中注册订阅者
        subscriber_state_ = ring_buffer_->register_subscriber(subscriber_id_, subscriber_name_);
        if (!subscriber_state_) {
            LOG_DEBUG << "Failed to register subscriber " << subscriber_id_ << " " << subscriber_name_;
            return false;
        }
    }
    
    callback_ = callback;
    observer_callback_ = nullptr;
    subscribed_.store(true);
    running_.store(callback_ != nullptr);
    
    // 如需实时检测，则设置回调函数，启动工作线程
    if (callback_) {
        LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_ << " subscribed with callback";
        worker_thread_ = std::thread(&Subscriber::worker_loop, this);
    }
    
    LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_ << " subscribed successfully";
    return true;
}

bool Subscriber::subscribe_observer(LocalMessageCallback callback) {
    if (!local_runtime_active()) {
        return false;
    }
    const uint64_t start_after_sequence = ring_buffer_ ? ring_buffer_->current_sequence() : 0U;
    return subscribe_observer(std::move(callback), start_after_sequence);
}

bool Subscriber::subscribe_observer(LocalMessageCallback callback,
                                    uint64_t start_after_sequence) {
    if (!callback) {
        LOG_ERROR << "Subscriber " << subscriber_id_ << " " << subscriber_name_
                  << " observer callback is null";
        return false;
    }
    if (subscribed_.load()) {
        LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_
                  << " already subscribed";
        return false;
    }
    if (external_io_ != nullptr || ring_buffer_ == nullptr || metadata_ == nullptr ||
        !local_runtime_active()) {
        LOG_ERROR << "Subscriber " << subscriber_id_ << " " << subscriber_name_
                  << " observer requires local ring buffer";
        return false;
    }

    subscriber_state_ = ring_buffer_->register_subscriber_after_sequence(
        subscriber_id_, subscriber_name_, start_after_sequence);
    if (!subscriber_state_) {
        LOG_DEBUG << "Failed to register observer subscriber " << subscriber_id_ << " "
                  << subscriber_name_;
        return false;
    }

    callback_ = nullptr;
    observer_callback_ = std::move(callback);
    subscribed_.store(true);
    running_.store(true);
    worker_thread_ = std::thread(&Subscriber::worker_loop, this);

    LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_
              << " subscribed as observer";
    return true;
}

void Subscriber::unsubscribe() {
    if (!subscribed_.load()) {
        LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_ << " not subscribed";
        return;
    }

    // 停止工作线程
    running_.store(false);
    
    // 唤醒可能在条件变量中阻塞的线程，防止 join 挂死
    if (callback_ || observer_callback_) {
        LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_ << " notifies subscribers";
        if (ring_buffer_) ring_buffer_->notify_subscribers();
    }

    // 标记为未订阅
    subscribed_.store(false);
    
    if (worker_thread_.joinable()) {
        worker_thread_.join();
        LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_ << " worker thread joined";
    }
    
    // 从RingBuffer中注销订阅者
    if (ring_buffer_ && subscriber_state_) ring_buffer_->unregister_subscriber(subscriber_state_);
    subscriber_state_ = nullptr;
    callback_ = nullptr;
    observer_callback_ = nullptr;
    LOG_DEBUG << "Subscriber " << subscriber_id_ << " " << subscriber_name_ << " unregistered from ring buffer";
}

void Subscriber::worker_loop() { 
    while (running_.load() &&
           (external_io_ != nullptr || local_runtime_active())) {
        if (external_io_ != nullptr) {
            const int32_t n = external_io_->receive(
                receive_buffer_.data(),
                receive_buffer_.size(),
                static_cast<uint32_t>(10000));

            if (n > 0 && callback_) {
                callback_(receive_buffer_.data(), static_cast<size_t>(n), 0);
            } else if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            continue;
        }

        bool handled_message = false;
        while (running_.load() && local_runtime_active() &&
               ring_buffer_->get_unread_count(subscriber_state_) > 0) {
            Message* msg = nullptr;
            bool read_ok = ring_buffer_->read_next(subscriber_state_, msg);
            if (!read_ok) {
                read_ok = ring_buffer_->read_latest(subscriber_state_, msg);
            }
            if (read_ok) {
                handled_message = true;
                const size_t received_size = msg->msg_size();
                LOG_DEBUG << "Subscriber " << subscriber_name_ << " received message of total size: " << received_size;
                if (received_size >= sizeof(MessageHeader)) {
                    if (msg->is_valid(ring_buffer_->is_checksum_enabled())) {
                        if (callback_) {
                            LOG_DEBUG << "msg->get_data(): " << msg->get_data();
                            LOG_DEBUG << "msg->msg_data_size(): " << msg->msg_data_size();
                            LOG_DEBUG << "msg->header.timestamp: " << msg->header.timestamp;
                            callback_(msg->get_data(), msg->msg_data_size(), msg->header.timestamp);
                        }
                        if (observer_callback_) {
                            LocalMessageView view;
                            view.topic_name = metadata_ ? metadata_->topic_name : "";
                            view.sequence = msg->header.sequence;
                            view.timestamp = msg->header.timestamp;
                            view.data = msg->get_data();
                            view.size = msg->msg_data_size();
                            observer_callback_(view);
                        }
                    } else {
                        LOG_ERROR << "Invalid message received on topic: " << metadata_->topic_name;
                    }
                } else {
                    LOG_ERROR << "Invalid message size received on topic: " << metadata_->topic_name;
                }
            }
        }

        if (!handled_message && running_.load() && local_runtime_active()) {
            // 等待通知以避免忙等待
            if (!ring_buffer_->wait_for_message(subscriber_state_, 100)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
}

bool Subscriber::bind_to_cpu(int cpu_id, int priority, int policy) {
    if (external_io_ == nullptr && !local_runtime_active()) {
        return false;
    }

    // 获取系统CPU核心数
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_id < 0 || cpu_id >= num_cpus) {
        LOG_ERROR << "Invalid CPU ID: " << cpu_id << ", available CPUs: 0-" << (num_cpus - 1);
        return false;
    }

    // 检查工作线程是否已启动
    if (!worker_thread_.joinable()) {
        LOG_ERROR << "Worker thread is not running, cannot bind to CPU";
        return false;
    }

    // 设置CPU亲和性
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    pthread_t thread_handle = worker_thread_.native_handle();
    const int min_priority = sched_get_priority_min(policy);
    const int max_priority = sched_get_priority_max(policy);
    if (min_priority == -1 || max_priority == -1) {
        LOG_ERROR << "Invalid scheduling policy: " << policy;
        return false;
    }

    sched_param sp{};
    if (priority < 0) {
        sp.sched_priority = max_priority;
    } else if (priority < min_priority) {
        sp.sched_priority = min_priority;
    } else if (priority > max_priority) {
        sp.sched_priority = max_priority;
    } else {
        sp.sched_priority = priority;
    }

    int sched_result = pthread_setschedparam(thread_handle, policy, &sp);
    if (sched_result != 0) {
        LOG_ERROR << "Failed to set subscriber worker thread scheduling policy=" << policy
                  << ", priority=" << sp.sched_priority << ": " << strerror(sched_result);
        return false;
    }

    int result = pthread_setaffinity_np(thread_handle, sizeof(cpu_set_t), &cpuset);
    
    if (result != 0) {
        LOG_ERROR << "Failed to bind subscriber worker thread to CPU " << cpu_id << ": " << strerror(result);
        return false;
    }

    LOG_DEBUG << "Subscriber worker thread bound to CPU " << cpu_id
              << ", policy=" << policy
              << ", priority=" << sp.sched_priority;
    return true;
}

size_t Subscriber::read_next(void* data, size_t size) {
    if (!subscribed_.load() || !local_runtime_active()) {
        return 0; // 未订阅
    }
    
    // 读消息
    Message* msg = nullptr;
    if (ring_buffer_->read_next(subscriber_state_, msg)) {
        // 比较数据大小
        if (msg->msg_data_size() < size) {
            size = msg->msg_data_size();
        }
        
        // 复制数据到用户缓冲区
        memcpy(data, msg->get_data(), size);
        return size;
    }

    return 0; // 无消息
}

size_t Subscriber::read_latest(void* data, size_t size) {
    if (!subscribed_.load() || !local_runtime_active()) {
        return 0; // 未订阅
    }
    
    // 读最新消息
    Message* msg = nullptr;
    if (ring_buffer_->read_latest(subscriber_state_, msg)) {
        // 比较数据大小
        if (msg->msg_data_size() < size) {
            size = msg->msg_data_size();
        }
        
        // 复制数据到用户缓冲区
        memcpy(data, msg->get_data(), size);
        return size;
    }

    return 0; // 无消息
}

size_t Subscriber::poll(void* data, size_t size, bool latest) {
    // 未订阅不许读
    if (!subscribed_.load()) {
        return 0;
    }
    // 消息回调模式下不允许自行读取，避免与工作线程重复消费
    if (callback_ || observer_callback_) {
        return 0; 
    }
    if (external_io_ != nullptr) {
        const int32_t ret = external_io_->receive(static_cast<uint8_t*>(data), size);
        return ret > 0 ? static_cast<size_t>(ret) : 0;
    }
    if (!local_runtime_active() || ring_buffer_ == nullptr || subscriber_state_ == nullptr) {
        return 0;
    }
    if (ring_buffer_->get_unread_count(subscriber_state_) == 0) {
        return 0;
    }
    if (latest) {
        return read_latest(data, size);
    } else {
        return read_next(data, size);
    }
}

size_t Subscriber::read(void* data, size_t size, bool latest) {
    return poll(data, size, latest);
}

int32_t Subscriber::read_blocking(void* data, size_t size, uint32_t timeout_us, bool latest) {
    if (!subscribed_.load() || callback_ || observer_callback_) {
        return 0; // 未订阅
    }

    if (external_io_ != nullptr) {
        return external_io_->receive(static_cast<uint8_t*>(data), size, timeout_us);
    }

    if (!local_runtime_active() || ring_buffer_ == nullptr || subscriber_state_ == nullptr) {
        return 0;
    }

    const size_t immediate = poll(data, size, latest);
    if (immediate > 0) {
        return static_cast<int32_t>(immediate);
    }

    const uint32_t timeout_ms = timeout_us == 0 ? 0 : (timeout_us + 999) / 1000;
    if (!ring_buffer_->wait_for_message(subscriber_state_, timeout_ms)) {
        return 0;
    }

    const size_t n = poll(data, size, latest);
    return static_cast<int32_t>(n);
}

int32_t Subscriber::read(void* data,
                         size_t size,
                         ReadStrategy strategy,
                         uint32_t timeout_us,
                         bool latest) {
    if (strategy == ReadStrategy::Polling) {
        return static_cast<int32_t>(poll(data, size, latest));
    }
    return read_blocking(data, size, timeout_us, latest);
}

int32_t Subscriber::read(void* data, size_t size, uint32_t timeout_us) {
    return read_blocking(data, size, timeout_us, true);
}

bool Subscriber::is_runtime_active() const {
    return external_io_ != nullptr || local_runtime_active();
}

bool Subscriber::local_runtime_active() const {
    return runtime_state_ == nullptr || runtime_state_->is_active();
}

} // namespace DDS
} // namespace MB_DDF
