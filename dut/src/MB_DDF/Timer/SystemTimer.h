/**
 * @file SystemTimer.h
 * @brief 高精度系统定时器类
 * @date 2025-10-19
 * @author Jiangkai
 */

#pragma once
#include <signal.h>
#include <time.h>
#if !defined(SYLIXOS)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <functional>
#include <string>
#include <thread>
#include <optional>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sys/eventfd.h>

namespace MB_DDF {
namespace Timer {

struct SystemTimerOptions {
    int sched_policy = SCHED_FIFO;        // 定时线程调度策略
    int priority = 50;                    // 定时线程优先级（SCHED_FIFO/RR 范围内有效）
    int cpu = -1;                         // 绑核编号，-1 表示不绑核
    int signal_no = SIGRTMIN;             // 使用的实时信号编号
    void* user_data = nullptr;            // 回调函数用户数据指针
};

class SystemTimer {
public:
    // 一次性接口：解析周期字符串、安装信号、创建并启动高精度周期定时器
    static std::unique_ptr<SystemTimer> start(const std::string& period_str,
                                              std::function<void(void*)> callback,
                                              const SystemTimerOptions& opt = {});

    ~SystemTimer();

    // 停止并删除定时器
    void stop();

    // 定时器重新计时
    void reset();

    // 是否正在运行
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // 获取定时线程的线程句柄（如使用）
    std::optional<pthread_t> workerHandle() const;

    // 设定线程调度与绑核
    static void configureThread(pthread_t th, int policy, int priority, int cpu);

private:
    explicit SystemTimer(std::function<void(void*)> cb, const SystemTimerOptions& opt);

    // 解析周期字符串（支持 s/ms/us/ns），返回纳秒
    static long long parsePeriodNs(const std::string& period);
    static timespec nsToTimespec(long long ns);

private:
    void notifyStartResult(bool ok);

    timer_t timer_id_{};
    std::atomic<bool> running_{false};
    int signal_no_ = SIGRTMIN;
    void* user_data_ = nullptr;          // 回调函数用户数据指针

    std::function<void(void*)> callback_;

    std::optional<std::thread> worker_;
    pthread_t worker_handle_{};         // 缓存原生句柄以支持 const 访问
    bool worker_handle_valid_ = false;

    long long period_ns_ = 0;           // 解析后的周期（纳秒）
    int stop_event_fd_ = -1;

    std::mutex start_mtx_{};
    std::condition_variable start_cv_{};
    bool start_done_ = false;
    bool start_ok_ = false;
};

} // namespace Timer
} // namespace MB_DDF
