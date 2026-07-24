#pragma once

#include <cstdint>
#include <pthread.h>
#include <time.h>

namespace MB_DDF {
namespace DDS {
namespace Sync {

enum class ClockKind : uint32_t {
    Monotonic = 0,
    Realtime = 1,
};

bool init_process_shared_mutex(pthread_mutex_t& mutex, bool robust);
bool init_process_shared_cond(pthread_cond_t& cond, ClockKind& clock_kind);
bool mutex_is_usable(pthread_mutex_t& mutex);
bool lock_mutex(pthread_mutex_t& mutex);
void unlock_mutex(pthread_mutex_t& mutex);
timespec make_abs_timeout(uint32_t timeout_ms, ClockKind clock_kind);

class MutexGuard {
public:
    explicit MutexGuard(pthread_mutex_t* mutex);
    ~MutexGuard();

    bool locked() const { return locked_; }

    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    pthread_mutex_t* mutex_;
    bool locked_;
};

} // namespace Sync
} // namespace DDS
} // namespace MB_DDF
