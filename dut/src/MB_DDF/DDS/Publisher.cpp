/**
 * @file Publisher.cpp
 * @brief 发布者类实现
 * @date 2025-08-03
 * @author Jiangkai
 */

#include "MB_DDF/DDS/Publisher.h"
#include "MB_DDF/Debug/Logger.h"
#include <cstring>
#include <random>
#include <utility>

namespace MB_DDF {
namespace DDS {

Publisher::Publisher(TopicMetadata* metadata,
                     RingBuffer* ring_buffer,
                     const std::string& publisher_name,
                     ExternalEndpointRef external_io)
    : metadata_(metadata),
      ring_buffer_(ring_buffer),
      external_io_(std::move(external_io)),
      publisher_name_(publisher_name) {
    // 构造函数直接绑定metadata，不需要判断topic是否存在
    // 生成唯一的发布者ID
    std::random_device rd;
    std::mt19937_64 gen(rd());
    publisher_id_ = gen();
    
    // 如果没有提供发布者名称，生成默认名称
    if (publisher_name_.empty()) {
        publisher_name_ = "publisher_" + std::to_string(publisher_id_);
    }
}

Publisher::~Publisher() {
    // 清理资源，当前实现中没有需要特别清理的资源
    // ring_buffer_由外部管理，不需要在这里释放
}

// WritableMessage 实现
Publisher::WritableMessage::WritableMessage(
    RingBuffer* rb,
    TopicMetadata* metadata,
    const RingBuffer::ReserveToken& token,
    std::unique_ptr<RingBuffer::WriteLock> lock)
    : rb_(rb), metadata_(metadata), token_(token), committed_(false), lock_(std::move(lock)) {}

Publisher::WritableMessage::~WritableMessage() {
    if (!committed_ && token_.valid && rb_) {
        rb_->abort_locked(token_);
    }
    lock_.reset();
}

Publisher::WritableMessage::WritableMessage(WritableMessage&& other) noexcept
    : rb_(other.rb_),
      metadata_(other.metadata_),
      token_(other.token_),
      committed_(other.committed_),
      lock_(std::move(other.lock_)) {
    other.rb_ = nullptr;
    other.metadata_ = nullptr;
    other.token_.valid = false;
    other.committed_ = true;
}

Publisher::WritableMessage& Publisher::WritableMessage::operator=(WritableMessage&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (!committed_ && token_.valid && rb_) {
        rb_->abort_locked(token_);
    }
    lock_.reset();

    rb_ = other.rb_;
    metadata_ = other.metadata_;
    token_ = other.token_;
    committed_ = other.committed_;
    lock_ = std::move(other.lock_);

    other.rb_ = nullptr;
    other.metadata_ = nullptr;
    other.token_.valid = false;
    other.committed_ = true;
    return *this;
}

void* Publisher::WritableMessage::data() {
    return token_.valid && token_.msg ? token_.msg->get_data() : nullptr;
}

size_t Publisher::WritableMessage::capacity() const {
    return token_.capacity;
}

bool Publisher::WritableMessage::commit(size_t used) {
    if (!token_.valid || committed_ || rb_ == nullptr || metadata_ == nullptr ||
        !lock_ || !lock_->locked()) {
        return false;
    }
    bool ok = rb_->commit_locked(token_, used, metadata_->topic_id);
    if (!ok) {
        rb_->abort_locked(token_);
    }
    committed_ = true;
    lock_.reset();
    return ok;
}

void Publisher::WritableMessage::cancel() {
    if (token_.valid && rb_ && !committed_) {
        rb_->abort_locked(token_);
    }
    committed_ = true;
    lock_.reset();
}

bool Publisher::WritableMessage::valid() const {
    return token_.valid && token_.msg != nullptr;
}

Publisher::WritableMessage Publisher::begin_message(size_t max_size) {
    if (ring_buffer_ == nullptr) {
        return WritableMessage(nullptr, nullptr, RingBuffer::ReserveToken(), nullptr);
    }

    auto lock = std::make_unique<RingBuffer::WriteLock>(ring_buffer_);
    if (!lock->locked()) {
        return WritableMessage(nullptr, nullptr, RingBuffer::ReserveToken(), nullptr);
    }

    auto token = ring_buffer_->reserve_locked(max_size);
    if (!token.valid) {
        lock.reset();
    }
    return WritableMessage(ring_buffer_, metadata_, token, std::move(lock));
}

bool Publisher::publish_fill(size_t max_size, const std::function<size_t(void* buffer, size_t capacity)>& fill) {
    if (ring_buffer_ == nullptr || metadata_ == nullptr || !fill) {
        LOG_ERROR << "Publisher " << publisher_name_ << " publish_fill invalid parameters";
        return false;
    }
    auto lock = std::make_unique<RingBuffer::WriteLock>(ring_buffer_);
    if (!lock->locked()) {
        LOG_ERROR << "Publisher " << publisher_name_ << " publish_fill write lock failed";
        return false;
    }

    auto token = ring_buffer_->reserve_locked(max_size);
    if (!token.valid || token.msg == nullptr) {
        LOG_ERROR << "Publisher " << publisher_name_ << " publish_fill reserve token invalid";
        return false;
    }
    void* buf = token.msg->get_data();
    size_t cap = token.capacity;
    size_t written = 0;
    try {
        written = fill(buf, cap);
    } catch (...) {
        LOG_ERROR << "Publisher " << publisher_name_ << " publish_fill exception";
        ring_buffer_->abort_locked(token);
        return false;
    }
    if (written == 0 || written > cap) {
        ring_buffer_->abort_locked(token);
        LOG_ERROR << "Publisher " << publisher_name_ << " publish_fill invalid written size: " << written;
        return false;
    }
    const bool ok = ring_buffer_->commit_locked(token, written, metadata_->topic_id);
    if (!ok) {
        ring_buffer_->abort_locked(token);
    }
    return ok;
}

bool Publisher::publish(const void* data, size_t size) {
    if (external_io_ != nullptr) {
        if (data == nullptr && size > 0) {
            return false;
        }
        return external_io_->send(static_cast<const uint8_t*>(data), size);
    }

    if (ring_buffer_ == nullptr) {
        return false;
    }
    
    // 调用RingBuffer的publish_message方法发布消息
    return ring_buffer_->publish_message(data, size);
}

uint64_t Publisher::publish_and_get_sequence(const void* data, size_t size) {
    return publish_and_get_sequence(data, size, {});
}

uint64_t Publisher::publish_and_get_sequence(
    const void* data,
    size_t size,
    const std::function<void(uint64_t)>& before_visible) {
    if (external_io_ != nullptr || ring_buffer_ == nullptr) {
        return 0;
    }

    uint64_t sequence = 0;
    if (!ring_buffer_->publish_message(data, size, &sequence, before_visible)) {
        return 0;
    }
    return sequence;
}

bool Publisher::write(const void* data, size_t size) {
    return publish(data, size);
}

uint32_t Publisher::get_topic_id() const {
    if (metadata_ != nullptr) {
        return metadata_->topic_id;
    }
    return 0; // metadata为空时返回0
}

std::string Publisher::get_topic_name() const {
    if (metadata_ != nullptr) {
        return std::string(metadata_->topic_name);
    }
    return ""; // metadata为空时返回空字符串
}

uint64_t Publisher::get_id() const {
    return publisher_id_;
}

std::string Publisher::get_name() const {
    return publisher_name_;
}

} // namespace DDS
} // namespace MB_DDF
