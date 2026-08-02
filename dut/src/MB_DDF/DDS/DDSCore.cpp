/**
 * @file DDSCore.cpp
 * @brief DDSCore主接口类实现
 * @date 2025-08-03
 * @author Jiangkai
 * 
 * 实现DDSCore（数据分发服务）的主要接口，支持发布者-订阅者模式的消息传递。
 * 基于共享内存和进程共享同步的环形缓冲区实现进程间通信。
 */

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/BeforeVisibleContext.h"
#include "MB_DDF/Debug/Logger.h"

#include <iostream>
#include <fstream>
#include <mutex>
#include <string>
#include <unistd.h>
#include <utility>

namespace MB_DDF {
namespace DDS {

#if !defined(MB_DDF_SHARED_MEMORY_NAME)
#define MB_DDF_SHARED_MEMORY_NAME "/MB_DDF_V2_SHM"
#endif

// 定义静态成员变量
const uint32_t DDSCore::VERSION;

DDSCore& DDSCore::instance() {
    static DDSCore instance;
    return instance;
}

std::shared_ptr<Publisher> DDSCore::create_publisher(const std::string& topic_name,
                                                     bool enable_checksum) {
    return create_publisher_impl(topic_name, enable_checksum, true);
}

std::shared_ptr<Publisher> DDSCore::create_publisher_if_initialized(
    const std::string& topic_name,
    bool enable_checksum) {
    return create_publisher_impl(topic_name, enable_checksum, false);
}

std::shared_ptr<Publisher> DDSCore::create_publisher_impl(
    const std::string& topic_name,
    bool enable_checksum,
    bool initialize_if_needed) {
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "DDS Topic/entity creation rejected during before_visible callback: "
                  << topic_name;
        return nullptr;
    }

    TopicBinding binding;
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!bind_topic_locked(
                topic_name, enable_checksum, initialize_if_needed, binding)) {
            LOG_ERROR << "failed to create or get topic buffer, topic name: " << topic_name;
            return nullptr;
        }
    }

    LOG_DEBUG << "created publisher, topic name: " << topic_name;
    auto publisher = std::make_shared<Publisher>(
        binding.metadata,
        binding.ring_buffer,
        binding.runtime_state->process_name,
        ExternalEndpointRef{},
        binding.runtime_state);
    if (!publisher->is_runtime_active()) {
        return nullptr;
    }
    if (!binding.ring_buffer->set_publisher(
            publisher->get_id(), publisher->get_name())) {
        LOG_ERROR << "failed to set publisher, publisher id: " << publisher->get_id()
                  << ", name: " << publisher->get_name();
        return nullptr;
    }
    if (!publisher->is_runtime_active()) {
        return nullptr;
    }
    return publisher;
}

std::shared_ptr<Publisher> DDSCore::create_writer(const std::string& topic_name, bool enable_checksum) {   
    return create_publisher(topic_name, enable_checksum);
} 

std::shared_ptr<Publisher> DDSCore::create_publisher(const std::string& topic_name,
                                                     ExternalEndpointRef external_io) {
    if (external_io == nullptr) {
        LOG_ERROR << "failed to create external publisher, external endpoint is null, topic name: " << topic_name;
        return nullptr;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const std::string process_name = get_process_name();
    LOG_DEBUG << "created external publisher, topic name: " << topic_name;
    return std::make_shared<Publisher>(
        nullptr, nullptr, process_name, std::move(external_io));
}

std::shared_ptr<Publisher> DDSCore::create_writer(const std::string& topic_name,
                                                  ExternalEndpointRef external_io) {
    return create_publisher(topic_name, std::move(external_io));
}

std::shared_ptr<Subscriber> DDSCore::create_subscriber(
    const std::string& topic_name,
    bool enable_checksum,
    const MessageCallback& callback) {
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "DDS Topic/entity creation rejected during before_visible callback: "
                  << topic_name;
        return nullptr;
    }

    TopicBinding binding;
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!bind_topic_locked(topic_name, enable_checksum, true, binding)) {
            LOG_ERROR << "failed to create or get topic buffer, topic name: " << topic_name;
            return nullptr;
        }
    }

    LOG_DEBUG << "created subscriber, topic name: " << topic_name;
    auto subscriber = std::make_shared<Subscriber>(
        binding.metadata,
        binding.ring_buffer,
        binding.runtime_state->process_name,
        ExternalEndpointRef{},
        binding.runtime_state);
    if (!subscriber->is_runtime_active() || !subscriber->subscribe(callback)) {
        return nullptr;
    }
    if (!subscriber->is_runtime_active()) {
        subscriber->unsubscribe();
        return nullptr;
    }
    return subscriber;
}

std::shared_ptr<Subscriber> DDSCore::create_reader(const std::string& topic_name, bool enable_checksum, const MessageCallback& callback) {
    return create_subscriber(topic_name, enable_checksum, callback);
}

std::shared_ptr<Subscriber> DDSCore::create_subscriber(const std::string& topic_name,
                                                       ExternalEndpointRef external_io,
                                                       const MessageCallback& callback) {
    if (external_io == nullptr) {
        LOG_ERROR << "failed to create external subscriber, external endpoint is null, topic name: " << topic_name;
        return nullptr;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const std::string process_name = get_process_name();
    LOG_DEBUG << "created external subscriber, topic name: " << topic_name;
    auto subscriber = std::make_shared<Subscriber>(
        nullptr, nullptr, process_name, std::move(external_io));
    if (!subscriber->subscribe(callback)) {
        return nullptr;
    }
    return subscriber;
}

std::shared_ptr<Subscriber> DDSCore::create_reader(const std::string& topic_name,
                                                   ExternalEndpointRef external_io,
                                                   const MessageCallback& callback) {
    return create_subscriber(topic_name, std::move(external_io), callback);
}

size_t DDSCore::data_write(std::shared_ptr<Publisher> publisher, const void* data, size_t size) {
    if (publisher == nullptr) {
        LOG_ERROR << "failed to write data, publisher is null";
        return 0;
    }
    return publisher->write(data, size) ? size : 0;
}

size_t DDSCore::data_read(std::shared_ptr<Subscriber> subscriber, void* data, size_t size) {
    if (subscriber == nullptr) {
        LOG_ERROR << "failed to read data, subscriber is null";
        return 0;
    }
    return subscriber->read(data, size);
}

size_t DDSCore::data_poll(std::shared_ptr<Subscriber> subscriber,
                          void* data,
                          size_t size,
                          bool latest) {
    if (subscriber == nullptr) {
        LOG_ERROR << "failed to poll data, subscriber is null";
        return 0;
    }
    return subscriber->poll(data, size, latest);
}

int32_t DDSCore::data_read(std::shared_ptr<Subscriber> subscriber,
                           void* data,
                           size_t size,
                           ReadStrategy strategy,
                           uint32_t timeout_us,
                           bool latest) {
    if (subscriber == nullptr) {
        LOG_ERROR << "failed to read data, subscriber is null";
        return 0;
    }
    return subscriber->read(data, size, strategy, timeout_us, latest);
}

std::vector<LocalTopicInfo> DDSCore::list_topics() const {
    std::vector<LocalTopicInfo> topics;
    (void)try_list_topics(topics);
    return topics;
}

bool DDSCore::try_list_topics(std::vector<LocalTopicInfo>& topics) const {
    topics.clear();
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    const auto state = runtime_state_;
    if (!state || !state->is_active() || !state->topic_registry ||
        !state->shm_manager) {
        return false;
    }

    // 状态判断、注册表枚举和序列快照全部位于同一生命周期临界区内，避免把“未初始化”
    // 误当成有效空快照，也不会观察到正在构造或已被 shutdown 摘除的 RuntimeState。
    for (const auto* metadata : state->topic_registry->get_all_topics()) {
        if (metadata == nullptr) {
            continue;
        }
        LocalTopicInfo info;
        info.topic_id = metadata->topic_id;
        info.topic_name = metadata->topic_name;
        info.ring_buffer_size = metadata->ring_buffer_size;
        if (state->shm_manager->get_address() &&
            metadata->ring_buffer_offset + sizeof(RingHeader) <=
                state->shm_manager->get_size()) {
            const auto* header = reinterpret_cast<const RingHeader*>(
                static_cast<const char*>(state->shm_manager->get_address()) +
                metadata->ring_buffer_offset);
            if (header->magic_number == RingHeader::MAGIC) {
                info.current_sequence =
                    header->current_sequence.load(std::memory_order_acquire);
            }
        }
        topics.push_back(std::move(info));
    }
    return true;
}

bool DDSCore::is_initialized() const {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return runtime_state_ && runtime_state_->is_active();
}

std::shared_ptr<Subscriber> DDSCore::create_observer(const std::string& topic_name,
                                                     const LocalMessageCallback& callback) {
    return create_observer_impl(topic_name, callback, 0U, true, true);
}

std::shared_ptr<Subscriber> DDSCore::create_observer(
    const std::string& topic_name,
    const LocalMessageCallback& callback,
    uint64_t start_after_sequence) {
    return create_observer_impl(
        topic_name, callback, start_after_sequence, false, true);
}

std::shared_ptr<Subscriber> DDSCore::create_observer_if_initialized(
    const std::string& topic_name,
    const LocalMessageCallback& callback,
    uint64_t start_after_sequence) {
    return create_observer_impl(
        topic_name, callback, start_after_sequence, false, false);
}

std::shared_ptr<Subscriber> DDSCore::create_observer_impl(
    const std::string& topic_name,
    const LocalMessageCallback& callback,
    uint64_t start_after_sequence,
    bool capture_current_sequence,
    bool initialize_if_needed) {
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "DDS Topic/entity creation rejected during before_visible callback: "
                  << topic_name;
        return nullptr;
    }

    TopicBinding binding;
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!bind_topic_locked(topic_name, true, initialize_if_needed, binding)) {
            LOG_ERROR << "failed to create observer buffer, topic name: " << topic_name;
            return nullptr;
        }
    }

    const uint64_t boundary = capture_current_sequence
                                  ? binding.ring_buffer->current_sequence()
                                  : start_after_sequence;

    auto subscriber = std::make_shared<Subscriber>(
        binding.metadata,
        binding.ring_buffer,
        binding.runtime_state->process_name + "_gateway_observer",
        ExternalEndpointRef{},
        binding.runtime_state);
    if (!subscriber->is_runtime_active() ||
        !subscriber->subscribe_observer(callback, boundary)) {
        return nullptr;
    }
    if (!subscriber->is_runtime_active()) {
        subscriber->unsubscribe();
        return nullptr;
    }
    return subscriber;
}

uint64_t DDSCore::publish_and_get_sequence(const std::string& topic_name,
                                           const void* data,
                                           size_t size) {
    return publish_and_get_sequence(topic_name, data, size, {});
}

uint64_t DDSCore::publish_and_get_sequence(
    const std::string& topic_name,
    const void* data,
    size_t size,
    const LocalSequenceAssignedCallback& before_visible) {
    auto publisher = create_publisher(topic_name, true);
    if (!publisher) {
        return 0;
    }
    return publisher->publish_and_get_sequence(data, size, before_visible);
}

bool DDSCore::initialize(size_t shared_memory_size) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return initialize_locked(shared_memory_size);
}

bool DDSCore::initialize_locked(size_t shared_memory_size) {
    if (runtime_state_ && runtime_state_->is_active()) {
        LOG_WARN << "already initialized, shared memory size: "
                 << runtime_state_->shm_manager->get_size();
        return true;
    }
    
    // 参数验证
    if (shared_memory_size < 1024 * 1024) { // 最小1MB
        LOG_ERROR << "Shared_memory_size too small (minimum 1MB), size: " << shared_memory_size;
        return false;
    }
    
    try {
        auto state = std::make_shared<RuntimeState>();

        state->shm_manager = std::make_unique<SharedMemoryManager>(
            MB_DDF_SHARED_MEMORY_NAME, shared_memory_size);

        if (!state->shm_manager || !state->shm_manager->get_address() ||
            state->shm_manager->get_address() == MAP_FAILED ||
            !state->shm_manager->get_semaphore() ||
            state->shm_manager->get_semaphore() == SEM_FAILED) {
            LOG_ERROR << "Failed to create shared memory manager";
            return false;
        }
        LOG_DEBUG << "Shared memory created, size: "
                  << state->shm_manager->get_size();

        state->topic_registry = std::make_unique<TopicRegistry>(
            state->shm_manager->get_address(),
            state->shm_manager->get_size(),
            state->shm_manager.get()
        );
        LOG_DEBUG << "topic registry created";

        if (!state->topic_registry) {
            LOG_ERROR << "Failed to create topic registry";
            return false;
        }
        if (!state->topic_registry->valid()) {
            LOG_ERROR << "Topic registry invalid";
            return false;
        }
        LOG_DEBUG << "topic registry initialized";

        state->process_name = get_process_name();
        state->active.store(true, std::memory_order_release);
        runtime_state_ = std::move(state);

        LOG_INFO << "DDSCore initialized successfully with " << shared_memory_size << " bytes shared memory";
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR << "Exception occurred: " << e.what();
        return false;
    } catch (...) {
        LOG_ERROR << "Unknown exception occurred";
        return false;
    }
}

void DDSCore::shutdown() {
    std::shared_ptr<RuntimeState> retired_state;
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!runtime_state_) {
            return;
        }

        // 先阻止旧 epoch 接受新操作，再从 Core 摘除。实体持有的 shared_ptr 会让
        // 已进入的发布/读取安全收尾，而 shutdown 无需等待 before_visible 回调。
        runtime_state_->active.store(false, std::memory_order_release);
        retired_state = std::move(runtime_state_);
    }
}

bool DDSCore::bind_topic_locked(const std::string& topic_name,
                                bool enable_checksum,
                                bool initialize_if_needed,
                                TopicBinding& binding) {
    binding = TopicBinding{};

    if (!runtime_state_ || !runtime_state_->is_active()) {
        if (!initialize_if_needed || !initialize_locked(128U * 1024U * 1024U)) {
            LOG_ERROR << "Failed to initialize DDSCore while creating topic buffer for " << topic_name;
            return false;
        }
    }

    const auto state = runtime_state_;
    if (!state || !state->is_active() || !state->topic_registry ||
        !state->shm_manager || !state->shm_manager->get_address()) {
        LOG_ERROR << "DDSCore is missing shared state while creating topic buffer for " << topic_name;
        return false;
    }

    if (!state->topic_registry->is_valid_topic_name(topic_name)) {
        LOG_ERROR << "Invalid topic name: " << topic_name;
        return false;
    }

    TopicMetadata* metadata = state->topic_registry->get_topic_metadata(topic_name);
    if (metadata != nullptr) {
        auto it = state->topic_buffers.find(metadata);
        LOG_DEBUG << "Searching for RingBuffer for topic: " << topic_name;
        if (it != state->topic_buffers.end()) {
            LOG_DEBUG << "Retrieved existing RingBuffer for topic: " << topic_name;
            binding.runtime_state = state;
            binding.metadata = metadata;
            binding.ring_buffer = it->second.get();
            return true;
        }

        try {
            void* buffer_addr =
                static_cast<char*>(state->shm_manager->get_address()) +
                metadata->ring_buffer_offset;

            auto ring_buffer = std::make_unique<RingBuffer>(
                buffer_addr,
                metadata->ring_buffer_size,
                state->shm_manager->get_semaphore(),
                enable_checksum);

            RingBuffer* buffer_ptr = ring_buffer.get();
            state->topic_buffers[metadata] = std::move(ring_buffer);
            LOG_DEBUG << "Created RingBuffer for existing topic: " << topic_name;
            binding.runtime_state = state;
            binding.metadata = metadata;
            binding.ring_buffer = buffer_ptr;
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to create RingBuffer for existing topic: " << e.what();
            return false;
        }
    }

    LOG_DEBUG << "Topic '" << topic_name << "' does not exist, creating new topic";
    try {
        const size_t ring_buffer_size = 1024U * 1024U;
        LOG_DEBUG << "Registering new topic: " << topic_name
                  << " with ring buffer size: " << ring_buffer_size;
        metadata = state->topic_registry->register_topic(topic_name, ring_buffer_size);
        if (!metadata) {
            LOG_ERROR << "Failed to register new topic: " << topic_name;
            return false;
        }

        void* buffer_addr =
            static_cast<char*>(state->shm_manager->get_address()) +
            metadata->ring_buffer_offset;

        auto ring_buffer = std::make_unique<RingBuffer>(
            buffer_addr,
            metadata->ring_buffer_size,
            state->shm_manager->get_semaphore(),
            enable_checksum);
        LOG_DEBUG << "Created RingBuffer for new topic: " << topic_name
                  << " with size: " << metadata->ring_buffer_size;

        RingBuffer* buffer_ptr = ring_buffer.get();
        state->topic_buffers[metadata] = std::move(ring_buffer);
        LOG_DEBUG << "Created new topic '" << topic_name << "' with "
                  << ring_buffer_size << " bytes ring buffer";

        binding.runtime_state = state;
        binding.metadata = metadata;
        binding.ring_buffer = buffer_ptr;
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to create new topic: " << e.what() << std::endl;
        return false;
    }
}

std::string DDSCore::get_process_name() {
    std::ifstream comm("/proc/self/comm");
    std::string name;
    if (comm.is_open()) {
        std::getline(comm, name);
    }

    if (name.empty()) {
        return "process";
    }
    return name;
}

} // namespace DDS
} // namespace MB_DDF
