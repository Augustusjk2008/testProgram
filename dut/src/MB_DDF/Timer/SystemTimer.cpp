/**
 * @file SystemTimer.cpp
 * @brief 高精度系统定时器类实现
 * @date 2025-10-19
 * @author Jiangkai
 */

#include "MB_DDF/Timer/SystemTimer.h"
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <condition_variable>
#include <chrono>

namespace MB_DDF {
namespace Timer {

SystemTimer::SystemTimer(std::function<void(void*)> cb, const SystemTimerOptions& opt)
    : signal_no_(opt.signal_no),
      user_data_(opt.user_data),
      callback_(std::move(cb)) {}

SystemTimer::~SystemTimer() {
    stop();
}

void SystemTimer::notifyStartResult(bool ok) {
    std::lock_guard<std::mutex> lk(start_mtx_);
    start_done_ = true;
    start_ok_ = ok;
    start_cv_.notify_all();
}

std::unique_ptr<SystemTimer> SystemTimer::start(const std::string& period_str,
                                                std::function<void(void*)> callback,
                                                const SystemTimerOptions& opt) {
    if (!callback) throw std::invalid_argument("callback must not be empty");

    auto timer = std::unique_ptr<SystemTimer>(new SystemTimer(std::move(callback), opt));

    // 解析周期字符串
    timer->period_ns_ = parsePeriodNs(period_str);
    if (timer->period_ns_ <= 0) {
        throw std::invalid_argument("invalid period string: " + period_str);
    }

    {
        int efd = eventfd(0, EFD_CLOEXEC);
        if (efd >= 0) {
            timer->stop_event_fd_ = efd;
        }
    }

    // 在当前线程先阻塞该实时信号，确保后续只由定时线程接收
    {
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, timer->signal_no_);
        pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
    }

    // 启动定时线程：在线程内同步等待定时信号并执行用户回调
    timer->worker_.emplace([timer_ptr = timer.get(), policy = opt.sched_policy, prio = opt.priority, cpu = opt.cpu]() {
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, timer_ptr->signal_no_);

        try {
            configureThread(pthread_self(), policy, prio, cpu);

            sigevent sev{};
            memset(&sev, 0, sizeof(sev));
            sev.sigev_notify = SIGEV_THREAD_ID;
            sev.sigev_signo = timer_ptr->signal_no_;
            sev.sigev_value.sival_ptr = timer_ptr;
            pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
            sev._sigev_un._tid = tid;

            if (timer_create(CLOCK_MONOTONIC, &sev, &timer_ptr->timer_id_) != 0) {
                timer_ptr->notifyStartResult(false);
                return;
            }

            timespec ts = nsToTimespec(timer_ptr->period_ns_);
            itimerspec its{};
            its.it_value = ts;
            its.it_interval = ts;
            if (timer_settime(timer_ptr->timer_id_, 0, &its, nullptr) != 0) {
                timer_delete(timer_ptr->timer_id_);
                timer_ptr->notifyStartResult(false);
                return;
            }

            timer_ptr->running_.store(true, std::memory_order_release);
            timer_ptr->notifyStartResult(true);
        } catch (...) {
            timer_ptr->notifyStartResult(false);
            return;
        }

        while (timer_ptr->running_.load(std::memory_order_acquire)) {
            siginfo_t info{};
            if (sigwaitinfo(&sigset, &info) == timer_ptr->signal_no_) {
                if (!timer_ptr->running_.load(std::memory_order_acquire)) break;
                if (timer_ptr->callback_) timer_ptr->callback_(timer_ptr->user_data_);
            } else if (errno != EINTR) {
                break;
            }
        }
    });

    // 缓存线程原生句柄以供 const 查询
    timer->worker_handle_ = timer->worker_->native_handle();
    timer->worker_handle_valid_ = true;

    {
        std::unique_lock<std::mutex> lk(timer->start_mtx_);
        bool ok = timer->start_cv_.wait_for(lk, std::chrono::milliseconds(200), [&]() { return timer->start_done_; })
                  && timer->start_ok_;
        if (!ok) {
            timer->stop();
            return nullptr;
        }
    }

    return timer;
}

void SystemTimer::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        // 即便未运行，也要安全地回收线程资源
        if (worker_.has_value()) {
            // 如果线程仍在等待，置运行标志为 false 以促退出
            running_.store(false, std::memory_order_release);
            if (worker_->joinable()) worker_->join();
            worker_.reset();
            worker_handle_valid_ = false;
        }
        return;
    }

    running_.store(false, std::memory_order_release);

    // 停止并删除定时器
    itimerspec its{};
    memset(&its, 0, sizeof(its));
    timer_settime(timer_id_, 0, &its, nullptr);
    timer_delete(timer_id_);

    if (stop_event_fd_ >= 0) {
        uint64_t one = 1;
        ::write(stop_event_fd_, &one, sizeof(one));
    }
    if (worker_handle_valid_) {
        pthread_kill(worker_handle_, signal_no_);
    }

    // 等待线程结束
    if (worker_.has_value()) {
        if (worker_->joinable()) worker_->join();
        worker_.reset();
        worker_handle_valid_ = false;
    }
    if (stop_event_fd_ >= 0) {
        ::close(stop_event_fd_);
        stop_event_fd_ = -1;
    }
}

void SystemTimer::reset() {
    if (!running_.load(std::memory_order_acquire)) return;

    // 重置定时器
    itimerspec its{};
    memset(&its, 0, sizeof(its));
    timespec ts = nsToTimespec(period_ns_);
    its.it_value = ts;     // 首次到期
    its.it_interval = ts;  // 周期
    timer_settime(timer_id_, 0, &its, nullptr);
}

std::optional<pthread_t> SystemTimer::workerHandle() const {
    if (!worker_handle_valid_) return std::nullopt;
    return worker_handle_;
}

long long SystemTimer::parsePeriodNs(const std::string& period) {
    // 允许 "100ms", "1s", "500us", "100000ns" 等
    std::string s;
    s.reserve(period.size());
    for (char c : period) {
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    }
    if (s.empty()) return -1;

    // 找到后缀位置
    size_t pos = s.find_first_not_of("0123456789");
    if (pos == std::string::npos || pos == 0) return -1;

    long long value = 0;
    try {
        value = std::stoll(s.substr(0, pos));
    } catch (...) {
        return -1;
    }
    std::string unit = s.substr(pos);
    if (unit == "s") {
        return value * 1000000000LL;
    } else if (unit == "ms") {
        return value * 1000000LL;
    } else if (unit == "us") {
        return value * 1000LL;
    } else if (unit == "ns") {
        return value;
    }
    return -1;
}

timespec SystemTimer::nsToTimespec(long long ns) {
    timespec ts{};
    ts.tv_sec = ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    return ts;
}

void SystemTimer::configureThread(pthread_t th, int policy, int priority, int cpu) {
    // 设置调度策略与优先级
    sched_param sp{};
    sp.sched_priority = priority;
    if (pthread_setschedparam(th, policy, &sp) != 0) {
        // 可能需要 root 权限；失败则打印提示但不抛异常
        // 可按需改为日志：std::cerr << "pthread_setschedparam failed: " << std::strerror(errno) << std::endl;
    }
    // 绑核
    if (cpu >= 0) {
        int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu >= num_cpus) {
            return;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu, &cpuset);
        pthread_setaffinity_np(th, sizeof(cpu_set_t), &cpuset);
    }
}

} // namespace Timer
} // namespace MB_DDF
