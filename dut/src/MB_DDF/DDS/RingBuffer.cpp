/**
 * @file RingBuffer.cpp
 * @brief 进程共享同步环形缓冲区实现
 * @date 2025-08-03
 * @author Jiangkai
 */

#include "MB_DDF/DDS/RingBuffer.h"
#include "MB_DDF/DDS/BeforeVisibleContext.h"
#include "MB_DDF/Debug/Logger.h"
#include "MB_DDF/DDS/SemaphoreGuard.h"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <semaphore.h>
#include <stdexcept>
#include <unistd.h>

namespace MB_DDF {
namespace DDS {

namespace {

uint64_t current_process_id() noexcept {
    return static_cast<uint64_t>(static_cast<unsigned long>(::getpid()));
}

bool process_is_definitely_dead(uint64_t owner_pid) noexcept {
    if (owner_pid == 0 ||
        owner_pid > static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

    errno = 0;
    const int result = ::kill(static_cast<pid_t>(owner_pid), 0);
    return result == -1 && errno == ESRCH;
}

void clear_subscriber_state(SubscriberState& state) noexcept {
    state.subscriber_id = 0;
    state.owner_pid = 0;
    state.read_pos.store(0, std::memory_order_release);
    state.last_read_sequence.store(0, std::memory_order_release);
    state.timestamp.store(0, std::memory_order_release);
    state.subscriber_name[0] = '\0';
}

} // namespace

RingBuffer::RingBuffer(void* buffer, size_t size, sem_t* sem, bool enable_checksum)
    : sem_(sem), enable_checksum_(enable_checksum) {
    const size_t metadata_size = sizeof(RingHeader) + sizeof(SubscriberRegistry) + sizeof(RingSyncState);
    if (buffer == nullptr) {
        LOG_ERROR << "RingBuffer construction failed, buffer is null";
        throw std::invalid_argument("RingBuffer buffer is null");
    }
    if (size <= metadata_size) {
        LOG_ERROR << "RingBuffer construction failed, buffer too small: size=" << size
                  << ", required>" << metadata_size;
        throw std::invalid_argument("RingBuffer buffer too small");
    }

    // 计算各部分在共享内存中的布局
    char* base = static_cast<char*>(buffer);
    LOG_DEBUG << "RingBuffer buffer address: " << (void*)base;
    
    // 头部结构
    header_ = reinterpret_cast<RingHeader*>(base);
    
    // 订阅者注册表
    registry_ = reinterpret_cast<SubscriberRegistry*>(base + sizeof(RingHeader));

    // 同步状态
    sync_ = reinterpret_cast<RingSyncState*>(base + sizeof(RingHeader) + sizeof(SubscriberRegistry));
    
    // 数据存储区
    data_ = base + metadata_size;
    capacity_ = size - metadata_size;
    
    {
        SemaphoreGuard guard(sem_);
        if (!guard.acquired()) {
            LOG_ERROR << "RingBuffer construction failed, semaphore acquire failed";
            throw std::runtime_error("RingBuffer semaphore acquire failed");
        }

        // 初始化头部（仅在首次创建时）
        if (header_->magic_number != RingHeader::MAGIC) {
            new (header_) RingHeader();
            header_->capacity = capacity_;
            header_->data_offset = metadata_size;

            // 初始化订阅者注册表和同步状态
            new (registry_) SubscriberRegistry();
            if (!initialize_sync_state()) {
                LOG_ERROR << "RingBuffer sync init failed";
                throw std::runtime_error("RingBuffer sync init failed");
            }
        } else if (sync_->magic != RingSyncState::MAGIC || !sync_state_usable()) {
            LOG_WARN << "RingBuffer sync state invalid, reinitializing";
            if (!initialize_sync_state()) {
                LOG_ERROR << "RingBuffer sync reinit failed";
                throw std::runtime_error("RingBuffer sync reinit failed");
            }
        }
    }

    if (sync_->magic != RingSyncState::MAGIC) {
        LOG_ERROR << "RingBuffer sync state not initialized";
        throw std::runtime_error("RingBuffer sync state not initialized");
    }

    LOG_DEBUG << "RingBuffer created with capacity " << capacity_ << " and data offset " << header_->data_offset;
}

bool RingBuffer::initialize_sync_state() {
    new (sync_) RingSyncState();

#if defined(SYLIXOS)
    // SylixOS accepts PTHREAD_PROCESS_SHARED attributes but its pthread mutex
    // and condition implementations retain process-owned kernel handles. The
    // creator exiting invalidates those handles in every remaining process.
    // Keep the shared layout stable, but use the global named semaphore for
    // writes and sequence polling for waits instead of initializing handles
    // that cannot satisfy the required lifetime contract.
    sync_->clock_kind = static_cast<uint32_t>(Sync::ClockKind::Realtime);
    sync_->generation.store(0, std::memory_order_release);
    sync_->waiter_count.store(0, std::memory_order_release);
    sync_->magic = RingSyncState::MAGIC;
    return true;
#else
    Sync::ClockKind clock_kind = Sync::ClockKind::Monotonic;
    if (!Sync::init_process_shared_mutex(sync_->write_mutex, true) ||
        !Sync::init_process_shared_mutex(sync_->notify_mutex, true) ||
        !Sync::init_process_shared_cond(sync_->notify_cond, clock_kind)) {
        return false;
    }

    sync_->clock_kind = static_cast<uint32_t>(clock_kind);
    sync_->generation.store(0, std::memory_order_release);
    sync_->waiter_count.store(0, std::memory_order_release);
    sync_->magic = RingSyncState::MAGIC;
    return true;
#endif
}

bool RingBuffer::sync_state_usable() {
#if defined(SYLIXOS)
    return sync_->magic == RingSyncState::MAGIC;
#else
    return Sync::mutex_is_usable(sync_->write_mutex) &&
           Sync::mutex_is_usable(sync_->notify_mutex);
#endif
}

bool RingBuffer::reinitialize_sync_state_guarded(const char* reason) {
#if defined(SYLIXOS)
    // Runtime replacement cannot be made safe while other processes may be
    // publishing or waiting. The SylixOS backend never consumes these pthread
    // objects, so there is nothing to repair here.
    LOG_ERROR << "RingBuffer runtime sync reinitialization disabled on SylixOS: "
              << reason;
    return false;
#else
    SemaphoreGuard guard(sem_);
    if (!guard.acquired()) {
        LOG_ERROR << "RingBuffer sync reinit failed, semaphore acquire failed";
        return false;
    }

    LOG_WARN << "RingBuffer sync state invalid, reinitializing: " << reason;
    return initialize_sync_state();
#endif
}

RingBuffer::WriteLock::WriteLock(RingBuffer* rb)
    : rb_(rb), locked_(false) {
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "RingBuffer write rejected during before_visible callback";
        return;
    }
    if (rb_ && rb_->sync_) {
#if defined(SYLIXOS)
        if (rb_->sem_ == nullptr || rb_->sem_ == SEM_FAILED) {
            LOG_ERROR << "RingBuffer write lock has an invalid named semaphore";
            return;
        }
        int rc = 0;
        do {
            rc = ::sem_wait(rb_->sem_);
        } while (rc != 0 && errno == EINTR);
        if (rc == 0) {
            locked_ = true;
        } else {
            LOG_ERROR << "RingBuffer named semaphore wait failed: " << strerror(errno);
        }
#else
        locked_ = Sync::lock_mutex(rb_->sync_->write_mutex);
#endif
    }
}

RingBuffer::WriteLock::~WriteLock() {
#if defined(SYLIXOS)
    if (locked_ && rb_ && rb_->sem_ != nullptr && rb_->sem_ != SEM_FAILED &&
        ::sem_post(rb_->sem_) != 0) {
        LOG_ERROR << "RingBuffer named semaphore post failed: " << strerror(errno);
    }
#else
    if (locked_ && rb_ && rb_->sync_) {
        Sync::unlock_mutex(rb_->sync_->write_mutex);
    }
#endif
}

RingBuffer::WriteLock RingBuffer::acquire_write_lock() {
    return WriteLock(this);
}

RingBuffer::ReserveToken RingBuffer::reserve_locked(size_t max_size, size_t alignment) {
    return reserve(max_size, alignment);
}

bool RingBuffer::commit_locked(const ReserveToken& token, size_t used, uint32_t topic_id) {
    return commit(token, used, topic_id);
}

bool RingBuffer::commit_locked(const ReserveToken& token,
                               size_t used,
                               uint32_t topic_id,
                               uint64_t* out_sequence) {
    return commit(token, used, topic_id, out_sequence);
}

void RingBuffer::abort_locked(const ReserveToken& token) {
    abort(token);
}

bool RingBuffer::publish_message(const void* data, size_t size) {
    return publish_message(data, size, nullptr);
}

bool RingBuffer::publish_message(const void* data, size_t size, uint64_t* out_sequence) {
    return publish_message(data, size, out_sequence, {});
}

bool RingBuffer::publish_message(
    const void* data,
    size_t size,
    uint64_t* out_sequence,
    const std::function<void(uint64_t)>& before_visible) {
    if (out_sequence != nullptr) {
        *out_sequence = 0;
    }

    if (data == nullptr && size > 0) {
        LOG_ERROR << "publish_message failed, data is null for a non-empty payload";
        return false;
    }

    // 使用减法式边界检查，避免 size + header 在极端输入下回绕。
    if (capacity_ < sizeof(MessageHeader) ||
        size > capacity_ - sizeof(MessageHeader)) {
        LOG_ERROR << "publish_message failed, message too large for ring capacity";
        return false;
    }

    auto lock = acquire_write_lock();
    if (!lock.locked()) {
        LOG_ERROR << "publish_message failed, write lock failed";
        return false;
    }

    // 使用零拷贝预留/提交流程以确保连续写入并正确回绕
    ReserveToken token = reserve_locked(size);
    if (!token.valid || token.msg == nullptr) {
        LOG_ERROR << "publish_message failed, reserve returned invalid token";
        return false;
    }

    // 拷贝载荷
    if (data != nullptr && size > 0) {
        std::memcpy(token.msg->get_data(), data, size);
    }

    // 提交，topic_id保持为0以与旧实现一致
    bool ok = commit_impl(token, size, 0, out_sequence, before_visible);
    if (!ok) {
        LOG_ERROR << "publish_message failed, commit failed";
        return false;
    }

    LOG_DEBUG << "publish_message committed with size " << size;
    return true;
}

bool RingBuffer::read_expected(SubscriberState* subscriber, Message*& out_message, uint64_t next_expected_sequence) {
    if (subscriber == nullptr) {
        LOG_ERROR << "read_expected failed, subscriber is nullptr";
        return false;
    }

    uint64_t last_seq = subscriber->last_read_sequence.load(std::memory_order_acquire);
    uint64_t buffer_current_seq = header_->current_sequence.load(std::memory_order_acquire);

    if (next_expected_sequence <= last_seq) {
        return false;
    }
    if (next_expected_sequence > buffer_current_seq) {
        return false;
    }

    size_t search_pos = subscriber->read_pos.load(std::memory_order_acquire);
    for (size_t i = 0; i < capacity_; i += ALIGNMENT) {
        Message* msg = read_message_at(search_pos);

        if (validate_message(msg)) {
            if (msg->header.sequence == next_expected_sequence) {
                out_message = msg;
                subscriber->last_read_sequence.store(msg->header.sequence, std::memory_order_release);
                subscriber->read_pos.store(search_pos, std::memory_order_release);
                subscriber->timestamp.store(msg->header.timestamp, std::memory_order_release);
                return true;
            } else {
                search_pos = (search_pos + msg->msg_size()) % capacity_;
                search_pos = (search_pos + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
            }
        } else {
            search_pos = (search_pos + ALIGNMENT) % capacity_;
        }
    }

    return false;
}

bool RingBuffer::read_next(SubscriberState* subscriber, Message*& out_message) {
    return read_expected(subscriber, out_message, subscriber->last_read_sequence + 1);
}

uint64_t RingBuffer::get_unread_count(SubscriberState* subscriber) {
    if (subscriber == nullptr) {
        LOG_ERROR << "get_unread_count failed, subscriber is nullptr";
        return 0;
    }

    uint64_t buffer_current_seq = header_->current_sequence.load(std::memory_order_acquire);
    
    // 如果缓冲区当前序号小于等于已读序号，说明没有未读消息
    if (buffer_current_seq <= subscriber->last_read_sequence) {
        LOG_DEBUG << "get_unread_count failed, buffer_current_seq " << buffer_current_seq << " <= last_read_sequence " << subscriber->last_read_sequence;
        return 0;
    }
    
    // 计算未读消息数量
    return buffer_current_seq - subscriber->last_read_sequence;
}

bool RingBuffer::read_latest(SubscriberState* subscriber, Message*& out_message) {
    return read_expected(subscriber, out_message, header_->current_sequence.load(std::memory_order_acquire));
}

bool RingBuffer::set_publisher(uint64_t publisher_id, const std::string& publisher_name) {
    auto lock = acquire_write_lock();
    if (!lock.locked()) {
        LOG_ERROR << "set_publisher failed, write lock failed";
        return false;
    }

    // 多发布者允许共用Topic，此处仅保留最近发布者信息用于诊断。
    header_->publisher_id = publisher_id;
    size_t name_len = std::min(publisher_name.length(), sizeof(header_->publisher_name) - 1);
    std::strncpy(header_->publisher_name, publisher_name.c_str(), name_len);
    header_->publisher_name[name_len] = '\0';

    LOG_INFO << "set_publisher " << publisher_id << " " << publisher_name;
    return true;
}

void RingBuffer::remove_publisher() {
    header_->publisher_id = 0;
    header_->publisher_name[0] = '\0';
    LOG_INFO << "remove_publisher";
}

SubscriberState* RingBuffer::register_subscriber(
    uint64_t subscriber_id,
    const std::string& subscriber_name) {
    return register_subscriber_after_sequence(
        subscriber_id, subscriber_name, uint64_t{0});
}

SubscriberState* RingBuffer::register_subscriber(
    uint64_t subscriber_id,
    const std::string& subscriber_name,
    bool start_from_latest) {
    if (!start_from_latest) {
        return register_subscriber_after_sequence(
            subscriber_id, subscriber_name, uint64_t{0});
    }

    uint64_t start_after_sequence = 0;
    {
        // 保留旧 bool 接口的快照语义：先等待正在提交的写入完成，再捕获当前
        // 可见序列。快照后释放写锁，避免在后续注册时形成 write_mutex ->
        // 全局 semaphore 的持锁链；期间新提交的消息仍会由显式边界路径补读。
        auto snapshot_lock = acquire_write_lock();
        if (!snapshot_lock.locked()) {
            LOG_ERROR << "register_subscriber failed, latest snapshot write lock failed";
            return nullptr;
        }
        start_after_sequence = header_->current_sequence.load(std::memory_order_acquire);
    }

    return register_subscriber_after_sequence(
        subscriber_id, subscriber_name, start_after_sequence);
}

SubscriberState* RingBuffer::register_subscriber_after_sequence(
    uint64_t subscriber_id,
    const std::string& subscriber_name,
    uint64_t start_after_sequence) {
    // before_visible 在本 Topic 写锁内执行。注册/Topic API 若在该回调中重入，
    // Linux 会自锁，SylixOS 则因默认递归 mutex 继续执行，造成平台语义分叉。
    // 在获取共享 semaphore 前统一快速失败。
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "register_subscriber rejected during before_visible callback";
        return nullptr;
    }

    // 使用RAII守护对象保护订阅者注册
    SemaphoreGuard guard(sem_);
    if (!guard.acquired()) {
        LOG_ERROR << "register_subscriber failed, semaphore acquire failed";
        return nullptr;
    }

    uint32_t count = registry_->count.load(std::memory_order_acquire);
    if (count > SubscriberRegistry::MAX_SUBSCRIBERS) {
        uint32_t repaired = 0;
        for (uint32_t i = 0; i < SubscriberRegistry::MAX_SUBSCRIBERS; ++i) {
            if (registry_->subscribers[i].subscriber_id != 0) ++repaired;
        }
        registry_->count.store(repaired, std::memory_order_release);
        count = repaired;
        LOG_WARN << "register_subscriber repaired registry count to " << count;
    }

    SubscriberState* id_match = nullptr;
    uint32_t free_index = SubscriberRegistry::MAX_SUBSCRIBERS;
    uint32_t stale_index = SubscriberRegistry::MAX_SUBSCRIBERS;
    const uint64_t owner_pid = current_process_id();
    for (uint32_t i = 0; i < SubscriberRegistry::MAX_SUBSCRIBERS; ++i) {
        auto& s = registry_->subscribers[i];
        if (s.subscriber_id != 0 && s.owner_pid != owner_pid &&
            stale_index == SubscriberRegistry::MAX_SUBSCRIBERS &&
            process_is_definitely_dead(s.owner_pid)) {
            stale_index = i;
            continue;
        }
        if (s.subscriber_id == subscriber_id && s.owner_pid == owner_pid) {
            id_match = &s;
            break;
        }
        if (free_index == SubscriberRegistry::MAX_SUBSCRIBERS && s.subscriber_id == 0) {
            free_index = i;
        }
    }

    // 起点由 Gateway 扫描时显式捕获，数值边界注册阶段不获取 Topic 写锁。只考虑
    // current_sequence 快照内已经正式可见的消息；before_visible 中已经写好消息头但
    // 尚未推进 current_sequence 的消息不属于本次扫描，稍后会作为新消息被读取。
    const uint64_t visible_sequence =
        header_->current_sequence.load(std::memory_order_acquire);
    uint64_t initial_sequence = std::max(start_after_sequence, visible_sequence);
    size_t initial_read_pos =
        header_->write_pos.load(std::memory_order_acquire) % capacity_;

    bool retained_message_found = false;
    uint64_t earliest_retained_sequence = 0;
    size_t earliest_retained_pos = initial_read_pos;
    if (start_after_sequence < visible_sequence) {
        for (size_t pos = 0; pos < capacity_; pos += ALIGNMENT) {
            if (capacity_ - pos < sizeof(MessageHeader)) {
                break;
            }
            Message* msg = read_message_at(pos);
            // 扫描会经过载荷中的对齐字节。先验证头部及连续区间边界，避免把载荷
            // 中偶然出现的 magic 当成消息后按伪造 data_size 越界计算校验和。
            if (msg == nullptr || msg->header.magic != MessageHeader::MAGIC_NUMBER ||
                msg->header.data_size > capacity_ - pos - sizeof(MessageHeader)) {
                continue;
            }
            if (!validate_message(msg)) {
                continue;
            }

            const uint64_t sequence = msg->header.sequence;
            if (sequence <= start_after_sequence || sequence > visible_sequence) {
                continue;
            }
            if (!retained_message_found || sequence < earliest_retained_sequence) {
                retained_message_found = true;
                earliest_retained_sequence = sequence;
                earliest_retained_pos = pos;
            }
        }
    }

    if (retained_message_found) {
        // sequence 严格大于 start_after_sequence，因此减一不会下溢。
        initial_sequence = earliest_retained_sequence - 1U;
        initial_read_pos = earliest_retained_pos;
    }

    auto initialize_read_state = [&](SubscriberState& state) {
        state.read_pos.store(initial_read_pos, std::memory_order_release);
        state.last_read_sequence.store(initial_sequence, std::memory_order_release);
        state.timestamp.store(0, std::memory_order_release);
        state.owner_pid = owner_pid;
    };

    auto update_subscriber_name = [&](SubscriberState& state) {
        const size_t name_len =
            std::min(subscriber_name.length(), sizeof(state.subscriber_name) - 1);
        std::strncpy(state.subscriber_name, subscriber_name.c_str(), name_len);
        state.subscriber_name[name_len] = '\0';
    };

    if (id_match) {
        // 保留旧接口的幂等返回行为，但每次成功注册都必须兑现本次请求的起点。
        // read_pos 先于 last_read_sequence 发布，避免活跃读取者观察到新边界和旧位置。
        update_subscriber_name(*id_match);
        initialize_read_state(*id_match);
        LOG_DEBUG << "register_subscriber " << subscriber_id << " " << subscriber_name
                  << " start_sequence=" << initial_sequence << " (id reused)";
        return id_match;
    }

    const uint32_t target_index =
        stale_index != SubscriberRegistry::MAX_SUBSCRIBERS ? stale_index : free_index;
    if (target_index == SubscriberRegistry::MAX_SUBSCRIBERS) {
        LOG_ERROR << "register_subscriber failed, no free subscriber slot";
        return nullptr;
    }

    SubscriberState& new_sub = registry_->subscribers[target_index];
    if (stale_index != SubscriberRegistry::MAX_SUBSCRIBERS) {
        LOG_WARN << "register_subscriber reclaiming exited owner pid=" << new_sub.owner_pid
                 << " subscriber_id=" << new_sub.subscriber_id;
        clear_subscriber_state(new_sub);
    }
    new_sub.subscriber_id = subscriber_id;
    update_subscriber_name(new_sub);
    initialize_read_state(new_sub);

    uint32_t active = 0;
    for (uint32_t i = 0; i < SubscriberRegistry::MAX_SUBSCRIBERS; ++i) {
        if (registry_->subscribers[i].subscriber_id != 0) ++active;
    }
    registry_->count.store(active, std::memory_order_release);

    LOG_DEBUG << "register_subscriber " << subscriber_id << " " << subscriber_name
              << " start_sequence=" << initial_sequence;
    return &registry_->subscribers[target_index];
}

void RingBuffer::unregister_subscriber(SubscriberState* subscriber) {
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "unregister_subscriber rejected during before_visible callback";
        return;
    }
    // 使用RAII守护对象保护订阅者注销
    SemaphoreGuard guard(sem_);
    if (!guard.acquired()) {
        LOG_ERROR << "unregister_subscriber failed, semaphore acquire failed";
        return;
    }

    if (subscriber == nullptr) {
        LOG_ERROR << "unregister_subscriber failed, subscriber is nullptr";
        return;
    }

    // 不同进程允许使用相同的实体 ID，因此不能仅按 subscriber_id 查找。
    // subscriber 应直接指向本 RingBuffer 的共享注册槽；先按地址精确定位，再校验
    // owner_pid，避免 fork 后的子进程或错误调用清掉其他活进程的槽。
    SubscriberState* registered_subscriber = nullptr;
    for (uint32_t i = 0; i < SubscriberRegistry::MAX_SUBSCRIBERS; ++i) {
        if (&registry_->subscribers[i] == subscriber) {
            registered_subscriber = &registry_->subscribers[i];
            break;
        }
    }

    if (registered_subscriber == nullptr) {
        LOG_WARN << "unregister_subscriber ignored pointer outside subscriber registry";
        return;
    }

    const uint64_t subscriber_id = registered_subscriber->subscriber_id;
    if (subscriber_id == 0) {
        LOG_WARN << "unregister_subscriber ignored empty subscriber slot";
        return;
    }

    const uint64_t owner_pid = current_process_id();
    if (registered_subscriber->owner_pid != owner_pid) {
        LOG_WARN << "unregister_subscriber ignored foreign owner pid="
                 << registered_subscriber->owner_pid
                 << " current_pid=" << owner_pid
                 << " subscriber_id=" << subscriber_id;
        return;
    }

    LOG_INFO << "unregister_subscriber " << subscriber_id << " "
             << registered_subscriber->subscriber_name;
    clear_subscriber_state(*registered_subscriber);

    uint32_t active = 0;
    for (uint32_t i = 0; i < SubscriberRegistry::MAX_SUBSCRIBERS; ++i) {
        if (registry_->subscribers[i].subscriber_id != 0) ++active;
    }
    registry_->count.store(active, std::memory_order_release);
}

bool RingBuffer::wait_for_message(SubscriberState* subscriber, uint32_t timeout_ms) {
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "wait_for_message rejected during before_visible callback";
        return false;
    }
    if (subscriber == nullptr) {
        LOG_ERROR << "wait_for_message failed, subscriber is nullptr";
        return false;
    }

    auto has_message = [&]() {
        const uint64_t last_read_sequence =
            subscriber->last_read_sequence.load(std::memory_order_acquire);
        const uint64_t current_seq = header_->current_sequence.load(std::memory_order_acquire);
        return current_seq > last_read_sequence;
    };

    if (has_message()) {
        return true;
    }

#if defined(SYLIXOS)
    // SylixOS pthread condition variables are process-owned even when the
    // process-shared attribute is requested. Polling preserves bounded wait
    // behavior and remains valid when the Topic creator exits normally.
    if (timeout_ms == 0) {
        while (!has_message()) {
            ::usleep(1000U);
        }
        return true;
    }

    uint32_t waited_ms = 0;
    while (!has_message() && waited_ms < timeout_ms) {
        ::usleep(1000U);
        ++waited_ms;
    }
    return has_message();
#else
    if (sync_->magic != RingSyncState::MAGIC || !sync_state_usable()) {
        reinitialize_sync_state_guarded("wait_for_message startup probe failed");
        return false;
    }

    if (!Sync::lock_mutex(sync_->notify_mutex)) {
        reinitialize_sync_state_guarded("wait_for_message lock failed");
        return false;
    }

    if (has_message()) {
        Sync::unlock_mutex(sync_->notify_mutex);
        return true;
    }

    bool unlock_required = true;
    bool sync_reinitialized = false;
    sync_->waiter_count.fetch_add(1, std::memory_order_acq_rel);
    while (!has_message()) {
        int rc = 0;
        if (timeout_ms == 0) {
            rc = pthread_cond_wait(&sync_->notify_cond, &sync_->notify_mutex);
        } else {
            const auto clock_kind = static_cast<Sync::ClockKind>(sync_->clock_kind);
            timespec ts = Sync::make_abs_timeout(timeout_ms, clock_kind);
            rc = pthread_cond_timedwait(&sync_->notify_cond, &sync_->notify_mutex, &ts);
        }
        if (rc == ETIMEDOUT) {
            break;
        }
#if !defined(SYLIXOS)
        if (rc == EOWNERDEAD) {
            pthread_mutex_consistent(&sync_->notify_mutex);
            continue;
        }
#endif
        if (rc != 0) {
            LOG_ERROR << "pthread_cond_wait failed: " << strerror(rc);
            unlock_required = false;
            sync_reinitialized =
                reinitialize_sync_state_guarded("wait_for_message cond wait failed");
            break;
        }
    }
    const bool result = has_message();
    if (!sync_reinitialized) {
        sync_->waiter_count.fetch_sub(1, std::memory_order_acq_rel);
    }
    if (unlock_required) {
        Sync::unlock_mutex(sync_->notify_mutex);
    }
    return result;
#endif
}

bool RingBuffer::empty() const {
    return header_->current_sequence.load(std::memory_order_acquire) == 0;
}

bool RingBuffer::full() const {
    // 缓冲区可以被覆盖，所以永远不会满
    return false;
}

size_t RingBuffer::available_space() const {
    // 总是有空间可写（通过覆盖实现）
    return capacity_;
}

size_t RingBuffer::available_data() const {
    return header_->current_sequence.load(std::memory_order_acquire);
}

RingBuffer::Statistics RingBuffer::get_statistics() const {
    Statistics stats;
    stats.current_sequence = header_->current_sequence.load(std::memory_order_acquire);
    stats.total_messages = stats.current_sequence;
    stats.notify_generation = sync_->generation.load(std::memory_order_acquire);
    stats.waiting_subscribers = sync_->waiter_count.load(std::memory_order_acquire);
    
    // 计算可用空间
    size_t write_pos = header_->write_pos.load(std::memory_order_acquire);
    stats.available_space = capacity_ - write_pos;
    
    stats.active_subscribers = 0;
    stats.subscribers.clear();
    for (uint32_t i = 0; i < SubscriberRegistry::MAX_SUBSCRIBERS; ++i) {
        if (registry_->subscribers[i].subscriber_id != 0) {
            ++stats.active_subscribers;
            stats.subscribers.push_back({
                registry_->subscribers[i].subscriber_id,
                registry_->subscribers[i].subscriber_name});
        }
    }
    
    return stats;
}

// 私有方法实现
bool RingBuffer::can_write(size_t message_size) const {
    if (message_size > capacity_) {
        return false;
    } else {
        return true;
    }
}

SubscriberState* RingBuffer::find_subscriber(uint64_t subscriber_id) const {
    uint32_t count = registry_->count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        if (registry_->subscribers[i].subscriber_id == subscriber_id) {
            return &registry_->subscribers[i];
        }
    }
    return nullptr;
}

Message* RingBuffer::read_message_at(size_t pos) const {
    if (pos >= capacity_ || capacity_ - pos < sizeof(MessageHeader)) {
        return nullptr;
    }
    
    return reinterpret_cast<Message*>(data_ + pos);
}

bool RingBuffer::validate_message(const Message* message) const {
    if (message == nullptr) {
        return false;
    }

    const auto data_begin = reinterpret_cast<uintptr_t>(data_);
    const auto message_begin = reinterpret_cast<uintptr_t>(message);
    if (message_begin < data_begin) {
        return false;
    }

    const size_t pos = static_cast<size_t>(message_begin - data_begin);
    if (pos >= capacity_ || capacity_ - pos < sizeof(MessageHeader)) {
        return false;
    }

    const size_t remaining_payload = capacity_ - pos - sizeof(MessageHeader);
    if (message->header.magic != MessageHeader::MAGIC_NUMBER ||
        message->header.data_size > remaining_payload) {
        return false;
    }

    return message->is_valid(enable_checksum_);
}

bool RingBuffer::find_next_valid_message(size_t start_pos, size_t& out_pos) const {
    for (size_t i = 0; i < capacity_; i += ALIGNMENT) {
        size_t pos = (start_pos + i) % capacity_;
        Message* msg = read_message_at(pos);
        
        if (msg != nullptr && validate_message(msg)) {
            out_pos = pos;
            return true;
        }
    }
    
    LOG_DEBUG << "find_next_valid_message failed, no valid message found";
    return false;
}

void RingBuffer::notify_subscribers() {
    header_->notification_count.fetch_add(1, std::memory_order_acq_rel);
    sync_->generation.fetch_add(1, std::memory_order_acq_rel);

#if defined(SYLIXOS)
    return;
#else
    const uint32_t waiters = sync_->waiter_count.load(std::memory_order_acquire);
    if (waiters == 0) {
        return;
    }

    Sync::MutexGuard guard(&sync_->notify_mutex);
    if (!guard.locked()) {
        return;
    }

    const int rc = pthread_cond_broadcast(&sync_->notify_cond);
    if (rc != 0) {
        LOG_ERROR << "pthread_cond_broadcast failed: " << strerror(rc);
    }
#endif
}

size_t RingBuffer::calculate_message_total_size(size_t data_size) {
    // 检查溢出：data_size 是否超过 capacity_ 减去消息头大小
    if (data_size > capacity_ - sizeof(MessageHeader)) {
        return 0;  // 溢出：请求大小超过缓冲区容量
    }

    size_t total = Message::total_size(data_size);

    // 检查溢出：加法是否溢出
    size_t align_mask = ALIGNMENT - 1;
    if (total > SIZE_MAX - align_mask) {
        return 0;  // 溢出：对齐操作将溢出
    }

    // 对齐到ALIGNMENT边界
    size_t aligned = (total + align_mask) & ~align_mask;

    // 再次检查对齐后的值（保险检查）
    if (aligned < total) {
        return 0;  // 溢出：对齐后值回绕
    }

    return aligned;
}

bool RingBuffer::is_checksum_enabled() const {
    return enable_checksum_;
}

// 写槽预留（零拷贝支持）
RingBuffer::ReserveToken RingBuffer::reserve(size_t max_size, size_t alignment) {
    ReserveToken token;
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "reserve rejected during before_visible callback";
        return token;
    }
    if (capacity_ < sizeof(MessageHeader) ||
        max_size > capacity_ - sizeof(MessageHeader)) {
        LOG_ERROR << "reserve failed, requested size too large";
        return token; // invalid
    }

    size_t to_write_pos = header_->write_pos.load(std::memory_order_acquire) % capacity_;
    // 确保对齐
    size_t aligned_pos = (to_write_pos + alignment - 1) & ~(alignment - 1);
    if (aligned_pos >= capacity_) {
        aligned_pos = 0; // 回绕
    }

    // 计算末尾剩余的连续空间（不含消息头）
    size_t tail_space = (aligned_pos <= capacity_) ? (capacity_ - aligned_pos) : 0;
    size_t payload_capacity_tail = (tail_space > sizeof(MessageHeader)) ? (tail_space - sizeof(MessageHeader)) : 0;

    size_t pos = aligned_pos;
    size_t payload_capacity = payload_capacity_tail;

    // 如果末尾空间不足以容纳请求大小，选择从头开始的连续区域
    if (payload_capacity < max_size) {
        pos = 0;
        // 头部对齐处理
        pos = (pos + alignment - 1) & ~(alignment - 1);
        payload_capacity = (capacity_ > pos + sizeof(MessageHeader)) ? (capacity_ - pos - sizeof(MessageHeader)) : 0;
        if (payload_capacity < max_size) {
            // 仍不足以容纳请求大小
            LOG_ERROR << "reserve failed, contiguous region too small";
            return token; // invalid
        }
    }

    Message* msg = reinterpret_cast<Message*>(data_ + pos);
    // 在填充期间设置为不可见
    msg->header.magic = 0;
    msg->header.data_size = 0;

    token.pos = pos;
    token.capacity = payload_capacity;
    token.msg = msg;
    token.valid = true;
    return token;
}

// 提交写槽（更新头并通知订阅者）
bool RingBuffer::commit(const ReserveToken& token, size_t used, uint32_t topic_id) {
    return commit(token, used, topic_id, nullptr);
}

bool RingBuffer::commit(const ReserveToken& token,
                         size_t used,
                         uint32_t topic_id,
                         uint64_t* out_sequence) {
    return commit_impl(token, used, topic_id, out_sequence, {});
}

bool RingBuffer::commit_impl(
    const ReserveToken& token,
    size_t used,
    uint32_t topic_id,
    uint64_t* out_sequence,
    const std::function<void(uint64_t)>& before_visible) {
    if (out_sequence != nullptr) {
        *out_sequence = 0;
    }
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "commit rejected during before_visible callback";
        return false;
    }
    if (!token.valid || token.msg == nullptr) {
        LOG_ERROR << "commit failed, invalid token";
        return false;
    }
    if (used > token.capacity) {
        LOG_ERROR << "commit failed, used > capacity";
        return false;
    }

    Message* buffer_msg = token.msg;

    // 先保证载荷写入对其他线程可见
    std::atomic_thread_fence(std::memory_order_release);

    // 填充消息头（先写头，后发布序列号）
    uint64_t seq = header_->current_sequence.load(std::memory_order_acquire) + 1;
    buffer_msg->header.magic = MessageHeader::MAGIC_NUMBER;
    buffer_msg->header.topic_id = topic_id;
    buffer_msg->header.sequence = seq;
    buffer_msg->header.data_size = static_cast<uint32_t>(used);
    buffer_msg->update(enable_checksum_);

    // 计算新写入位置（包含对齐）
    size_t total_size = calculate_message_total_size(used);
    size_t new_write_pos = (token.pos + total_size) % capacity_;
    new_write_pos = (new_write_pos + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    if (new_write_pos >= capacity_) {
        new_write_pos = 0;
    }

    // 必须严格早于current_sequence.store：observer既可能等待通知，也可能主动轮询。
    if (before_visible) {
        try {
            Detail::BeforeVisibleCallbackScope callback_scope;
            before_visible(seq);
        } catch (const std::exception& e) {
            buffer_msg->header.magic = 0;
            buffer_msg->header.data_size = 0;
            LOG_ERROR << "commit before_visible callback failed: " << e.what();
            return false;
        } catch (...) {
            buffer_msg->header.magic = 0;
            buffer_msg->header.data_size = 0;
            LOG_ERROR << "commit before_visible callback failed with unknown exception";
            return false;
        }
    }

    // 更新环形缓冲区头部（发布序列号，保证写入可见）
    std::atomic_thread_fence(std::memory_order_release);
    header_->current_sequence.store(seq, std::memory_order_release);
    header_->write_pos.store(new_write_pos, std::memory_order_release);
    header_->timestamp.store(buffer_msg->header.timestamp, std::memory_order_release);
    notify_subscribers();

    if (out_sequence != nullptr) {
        *out_sequence = seq;
    }

    LOG_DEBUG << "commit message seq " << seq << " size " << used;
    return true;
}

// 新增：放弃写槽（不推进写指针）
void RingBuffer::abort(const ReserveToken& token) {
    if (Detail::before_visible_callback_active()) {
        LOG_ERROR << "abort rejected during before_visible callback";
        return;
    }
    if (!token.valid || token.msg == nullptr) {
        return;
    }
    // 保持该区域不可见，供后续写操作覆盖
    token.msg->header.magic = 0;
    token.msg->header.data_size = 0;
}

} // namespace DDS
} // namespace MB_DDF
