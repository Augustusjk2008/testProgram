#include "MB_DDF/DDS/ProcessSharedSync.h"
#include "MB_DDF/Debug/Logger.h"

#include <cerrno>
#include <cstring>

namespace MB_DDF {
namespace DDS {
namespace Sync {

bool init_process_shared_mutex(pthread_mutex_t& mutex, bool robust) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return false;
    }

    bool ok = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0;
#if defined(SYLIXOS)
    // SylixOS provides process-shared mutexes, but not the POSIX robust-mutex API.
    (void)robust;
#else
    if (ok && robust) {
        const int rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (rc != 0) {
            LOG_WARN << "pthread robust mutex unsupported: " << strerror(rc);
        }
    }
#endif
    if (ok) {
        ok = pthread_mutex_init(&mutex, &attr) == 0;
    }

    pthread_mutexattr_destroy(&attr);
    return ok;
}

bool init_process_shared_cond(pthread_cond_t& cond, ClockKind& clock_kind) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        return false;
    }

    bool ok = pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0;
    clock_kind = ClockKind::Monotonic;
    if (ok) {
        const int rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (rc != 0) {
            LOG_WARN << "pthread cond CLOCK_MONOTONIC unsupported: " << strerror(rc);
            clock_kind = ClockKind::Realtime;
        }
    }
    if (ok) {
        ok = pthread_cond_init(&cond, &attr) == 0;
    }

    pthread_condattr_destroy(&attr);
    return ok;
}

bool mutex_is_usable(pthread_mutex_t& mutex) {
    const int rc = pthread_mutex_trylock(&mutex);
    if (rc == 0) {
        unlock_mutex(mutex);
        return true;
    }
    if (rc == EBUSY) {
        return true;
    }
#if !defined(SYLIXOS)
    if (rc == EOWNERDEAD) {
        LOG_WARN << "recovering robust mutex during startup probe";
        if (pthread_mutex_consistent(&mutex) == 0) {
            unlock_mutex(mutex);
            return true;
        }
    }
#endif

    LOG_WARN << "process-shared mutex unusable: " << strerror(rc);
    return false;
}

bool lock_mutex(pthread_mutex_t& mutex) {
    const int rc = pthread_mutex_lock(&mutex);
    if (rc == 0) {
        return true;
    }
#if !defined(SYLIXOS)
    if (rc == EOWNERDEAD) {
        LOG_WARN << "recovering robust mutex after owner death";
        return pthread_mutex_consistent(&mutex) == 0;
    }
#endif

    LOG_ERROR << "pthread_mutex_lock failed: " << strerror(rc);
    return false;
}

void unlock_mutex(pthread_mutex_t& mutex) {
    const int rc = pthread_mutex_unlock(&mutex);
    if (rc != 0) {
        LOG_ERROR << "pthread_mutex_unlock failed: " << strerror(rc);
    }
}

timespec make_abs_timeout(uint32_t timeout_ms, ClockKind clock_kind) {
    timespec ts{};
    const clockid_t clock_id =
        clock_kind == ClockKind::Monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    clock_gettime(clock_id, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

MutexGuard::MutexGuard(pthread_mutex_t* mutex) : mutex_(mutex), locked_(false) {
    if (mutex_) {
        locked_ = lock_mutex(*mutex_);
    }
}

MutexGuard::~MutexGuard() {
    if (locked_ && mutex_) {
        unlock_mutex(*mutex_);
    }
}

} // namespace Sync
} // namespace DDS
} // namespace MB_DDF
