#include <biz/i_test_run_service.h>

// 依赖的其他 biz 头文件
#include <biz/i_algorithm_executor.h>
#include <biz/i_report_generator.h>
#include <biz/test_config_manager.h>
#include <biz/test_plan_builder.h>

// Qt 工具类
#include <QDateTime>       // 时间戳
#include <QElapsedTimer>   // 高精度计时器
#include <QMutex>          // 互斥锁
#include <QMutexLocker>    // 锁的 RAII 包装
#include <QThread>         // 工作线程
#include <QUuid>           // 生成唯一ID
#include <QWaitCondition>  // 条件变量（线程间通信）

// 容器
#include <QHash>
#include <QSet>

// 标准库
#include <memory>          // std::unique_ptr
#include <functional>      // std::function
#include <mutex>           // std::recursive_mutex

namespace hwtest::biz {

// 前置声明：报告生成器的具体实现在另一个编译单元中
IReportGenerator* createReportGeneratorImplementation();

namespace { // 匿名命名空间 - 内部辅助

// 构造 Status
Status makeStatus(ErrorCode code, const QString& message)
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

// 构造失败的 Result<T>
template <typename T>
Result<T> failure(ErrorCode code, const QString& message)
{
    Result<T> result;
    result.status = makeStatus(code, message);
    return result;
}

// 判断错误是否可重试
bool retryable(const Result<TestResult>& result)
{
    const ErrorCode code = result.status.ok() ? result.value.errorCode : result.status.code;
    switch (code) {
    case ErrorCode::ResourceBusy:
    case ErrorCode::ResourceTimeout:
    case ErrorCode::BusTimeout:
    case ErrorCode::ChannelOccupied:
    case ErrorCode::SampleFail:
    case ErrorCode::RemoteCommandError:
    case ErrorCode::ProtocolParseError:
        return true;
    default:
        return false;
    }
}

// ========================================================================
// TestRunService - 测试运行服务的具体实现
// 继承 ITestRunService（对外接口）和 IAlgorithmObserver（算法回调）
// ========================================================================
class TestRunService final : public ITestRunService, private IAlgorithmObserver {
public:
    // 构造函数
    TestRunService(IAlgorithmExecutor* executor, QObject* parent)
        : ITestRunService(parent)
        , m_executor(executor)          // 非拥有指针，生命周期由调用者管理
        , m_runControl(this)            // 运行控制适配器
        , m_reportGenerator(createReportGeneratorImplementation())
    {
    }

    // 析构函数：确保关闭
    ~TestRunService() override
    {
        shutdown();
    }

    // ------------------------------------------------------------------
    // ITestRunService 接口实现
    // ------------------------------------------------------------------

    // 初始化服务
    Status initialize() override
    {
        // 使用 recursive_mutex 防止同一线程重复加锁
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        // 注册 Qt 元类型
        registerBizMetaTypes();
        hwtest::logging::registerLogMetaTypes();

        TaskId taskId;
        bool notify = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_executor == nullptr) {
                return makeStatus(ErrorCode::ParameterRangeError,
                                  QStringLiteral("An algorithm executor is required"));
            }
            if (m_shuttingDown) {
                return makeStatus(ErrorCode::InvalidState,
                                  QStringLiteral("Service is shutting down"));
            }
            if (!m_initialized) {
                m_initialized = true;
                m_state = TestState::Idle;   // 初始化后进入空闲状态
                taskId = m_taskId;
                notify = true;
            }
        }
        if (notify) {
            emit stateChanged(taskId, TestState::Idle);
        }
        return Status{};
    }

    // 关闭服务（含停止运行中任务）
    Status shutdown() override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        IAlgorithmExecutor* executor = nullptr;
        bool active = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_executor == nullptr) {
                return makeStatus(ErrorCode::ParameterRangeError,
                                  QStringLiteral("An algorithm executor is required"));
            }
            if (m_executorShutdown) return Status{};

            m_shuttingDown = true;
            executor = m_executor;
            active = isActiveState(m_state);
            // 如果有活动任务，先请求停止
            if (active) {
                m_stopRequested = true;
                m_stopResponseKnown = false;
                m_stopAccepted = false;
                m_control = RunControl::Stop;
                ++m_controlVersion;        // 版本递增，让 checkpoint 感知变化
                m_state = TestState::Stopping;
                m_controlChanged.wakeAll(); // 唤醒工作线程
            }
        }

        Status stopStatus;
        if (active) {
            // 调用算法执行器的 requestStop，等待其返回
            stopStatus = executor->requestStop(kShutdownTimeoutMs);
            TaskId taskId;
            TestState state = TestState::Stopping;
            bool notify = false;
            {
                QMutexLocker locker(&m_mutex);
                m_stopResponseKnown = true;
                m_stopAccepted = stopStatus.ok();
                if (m_workerDone) {
                    state = m_stopAccepted ? TestState::Idle : m_workerTerminalState;
                    m_state = state;
                    taskId = m_taskId;
                    notify = true;
                }
            }
            if (notify) emit stateChanged(taskId, state);
        }

        // 等待工作线程结束
        joinWorker();
        // 关闭算法执行器
        const Status executorStatus = executor->shutdown(kShutdownTimeoutMs);

        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            m_executorShutdown = true;
            m_initialized = false;
            m_configLoaded = false;
            m_state = TestState::Uninitialized;
            taskId = m_taskId;
        }
        emit stateChanged(taskId, TestState::Uninitialized);
        return !stopStatus.ok() ? stopStatus : executorStatus;
    }

    // 加载配置文件
    Status loadConfiguration(const ConfigPath& configPath) override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        {
            QMutexLocker locker(&m_mutex);
            if (!m_initialized) {
                return makeStatus(ErrorCode::NotInitialized,
                                  QStringLiteral("Service has not been initialized"));
            }
            if (m_state != TestState::Idle && m_state != TestState::Finished) {
                return makeStatus(ErrorCode::ResourceBusy,
                                  QStringLiteral("Configuration cannot be loaded in the current state"));
            }
        }

        const Result<TestConfig> loaded = m_configManager.load(configPath);
        if (!loaded.ok()) return loaded.status;

        QMutexLocker locker(&m_mutex);
        if (!m_initialized || (m_state != TestState::Idle && m_state != TestState::Finished)) {
            return makeStatus(ErrorCode::ResourceBusy,
                              QStringLiteral("Configuration changed while loading"));
        }
        m_config = loaded.value;
        m_configLoaded = true;
        return Status{};
    }

    // 启动测试（默认选项）
    Result<TaskId> startTest(const QStringList& testItems, int priority) override
    {
        return startTestWithOptions(RunOptions{}, testItems, priority);
    }

    // 启动测试（带运行选项）
    Result<TaskId> startTestWithOptions(const RunOptions& runOptions,
                                        const QStringList& testItems,
                                        int priority) override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        reapFinishedWorker(); // 清理上一轮已结束的工作线程

        // 检查状态、获取配置
        TestConfig config;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_initialized)
                return failure<TaskId>(ErrorCode::NotInitialized, "Service has not been initialized");
            if (!m_configLoaded)
                return failure<TaskId>(ErrorCode::ConfigNotLoaded, "No configuration is loaded");
            if (m_shuttingDown || (m_state != TestState::Idle && m_state != TestState::Finished))
                return failure<TaskId>(ErrorCode::ResourceBusy, "A task cannot start in the current state");
            config = m_config;
        }

        // 校验运行选项
        const Status optionsStatus = validateRunOptions(runOptions);
        if (!optionsStatus.ok())
            return failure<TaskId>(optionsStatus.code, optionsStatus.error.message);

        // 优先级处理
        if (priority < -1)
            return failure<TaskId>(ErrorCode::ParameterRangeError, "Task priority must be -1 or 1..3");
        const int effectivePriority = (priority == -1) ? config.runtimeConfig.taskPriorityDefault : priority;
        if (effectivePriority < 1 || effectivePriority > 3)
            return failure<TaskId>(ErrorCode::ParameterRangeError, "Task priority must be in 1..3");

        // 构建测试计划
        const Result<TestPlan> built = m_planBuilder.build(config);
        if (!built.ok()) return failure<TaskId>(built.status.code, built.status.error.message);
        const Result<TestPlan> selected = selectSteps(built.value, testItems);
        if (!selected.ok()) return failure<TaskId>(selected.status.code, selected.status.error.message);

        // 设备流模式不允许重试
        if (runOptions.mode == RunMode::DeviceStream) {
            for (const TestStep& step : selected.value.steps) {
                if (step.retryCount > 0) {
                    return failure<TaskId>(ErrorCode::ParameterRangeError,
                        QStringLiteral("device_stream does not support retries; step '%1' has retryCount=%2")
                            .arg(step.stepId).arg(step.retryCount));
                }
            }
        }

        reapFinishedWorker();

        // 创建任务ID和上下文
        const TaskId taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        TestContext context;
        context.runId = taskId;
        context.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        context.productModel = config.productModel;
        context.tags = config.runtimeConfig.tags;
        context.tags.insert(QStringLiteral("runMode"), runModeToString(runOptions.mode));
        context.tags.insert(QStringLiteral("intervalMs"), runOptions.intervalMs);
        context.tags.insert(QStringLiteral("maxCycles"),
                            QVariant::fromValue<qulonglong>(runOptions.maxCycles));
        context.runParameters = runOptions.parameters;

        {
            QMutexLocker locker(&m_mutex);
            if (m_shuttingDown || isActiveState(m_state))
                return failure<TaskId>(ErrorCode::ResourceBusy, "A task is already running");
            if (m_worker != nullptr)
                return failure<TaskId>(ErrorCode::ResourceBusy, "Previous task still being finalized");

            // 重置运行状态
            m_taskId = taskId;
            m_context = context;
            m_results.clear();
            m_resultsByStep.clear();
            m_currentCycleIndex = 0;
            m_control = RunControl::Run;
            ++m_controlVersion;
            m_runControl.reset();
            m_stopRequested = false;
            m_stopResponseKnown = false;
            m_stopAccepted = false;
            m_workerDone = false;
            m_workerCanRun = false;
            m_workerTerminalState = TestState::Finished;
            m_state = TestState::Running;

            // 创建工作线程
            m_worker.reset(QThread::create([this, plan = selected.value, context,
                                            executionConfig = config.executionConfig, runOptions] {
                runTask(plan, context, executionConfig, runOptions);
            }));
            m_worker->setObjectName(QStringLiteral("hwtest.biz.worker"));
            m_worker->start();
        }
        emit stateChanged(taskId, TestState::Running);

        // 允许工作线程开始执行
        {
            QMutexLocker locker(&m_mutex);
            if (m_taskId == taskId) {
                m_workerCanRun = true;
                m_controlChanged.wakeAll();
            }
        }
        return Result<TaskId>{Status{}, taskId};
    }

    // 暂停测试
    Status pauseTest() override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            if (m_state != TestState::Running)
                return makeStatus(ErrorCode::InvalidState, "Task is not running");
            m_control = RunControl::Pause;
            ++m_controlVersion;
            m_state = TestState::Paused;
            taskId = m_taskId;
            m_controlChanged.wakeAll();
        }
        emit stateChanged(taskId, TestState::Paused);
        return Status{};
    }

    // 恢复测试
    Status resumeTest() override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            if (m_state != TestState::Paused)
                return makeStatus(ErrorCode::InvalidState, "Task is not paused");
            m_control = RunControl::Run;
            ++m_controlVersion;
            m_state = TestState::Running;
            taskId = m_taskId;
            m_controlChanged.wakeAll();
        }
        emit stateChanged(taskId, TestState::Running);
        return Status{};
    }

    // 停止测试（带超时）
    Status stopTest(int timeoutMs) override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        if (timeoutMs < 0)
            return makeStatus(ErrorCode::ParameterRangeError, "Stop timeout must not be negative");

        IAlgorithmExecutor* executor = nullptr;
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            if (m_state == TestState::Idle || m_state == TestState::Finished) return Status{};
            if (!isActiveState(m_state))
                return makeStatus(ErrorCode::InvalidState, "Task is not active");
            executor = m_executor;
            m_stopRequested = true;
            m_stopResponseKnown = false;
            m_stopAccepted = false;
            m_control = RunControl::Stop;
            ++m_controlVersion;
            m_state = TestState::Stopping;
            taskId = m_taskId;
            m_controlChanged.wakeAll();
        }
        emit stateChanged(taskId, TestState::Stopping);

        QElapsedTimer stopTimer;
        stopTimer.start();
        const Status stopStatus = executor->requestStop(timeoutMs);

        Status completionStatus = stopStatus;
        TestState completionState = TestState::Stopping;
        bool notify = false;
        {
            QMutexLocker locker(&m_mutex);
            m_stopResponseKnown = true;
            m_stopAccepted = stopStatus.ok();
            // 等待工作线程结束（在超时范围内）
            while (stopStatus.ok() && !m_workerDone) {
                const qint64 remaining = static_cast<qint64>(timeoutMs) - stopTimer.elapsed();
                if (remaining <= 0) break;
                m_controlChanged.wait(&m_mutex, static_cast<unsigned long>(remaining));
            }
            if (stopStatus.ok() && !m_workerDone) {
                completionStatus = makeStatus(ErrorCode::ResourceTimeout,
                                              "Timed out waiting for the test worker to stop");
                m_stopAccepted = false;
            }
            if (m_workerDone) {
                completionState = m_stopAccepted ? TestState::Idle : m_workerTerminalState;
                m_state = completionState;
                notify = true;
            }
        }
        if (notify) emit stateChanged(taskId, completionState);
        return completionStatus;
    }

    // 重置硬件
    Status resetHardware() override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        IAlgorithmExecutor* executor = nullptr;
        TaskId taskId;
        bool recoverErrorState = false;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_initialized)
                return makeStatus(ErrorCode::NotInitialized, "Service has not been initialized");
            if (isActiveState(m_state))
                return makeStatus(ErrorCode::ResourceBusy, "Reset not allowed while task active");
            executor = m_executor;
            taskId = m_taskId;
            recoverErrorState = (m_state == TestState::Error);
        }
        if (executor == nullptr)
            return makeStatus(ErrorCode::ParameterRangeError, "An algorithm executor is required");

        const Status status = executor->reset();
        if (status.ok() && recoverErrorState) {
            QMutexLocker locker(&m_mutex);
            m_state = TestState::Idle;
            emit stateChanged(taskId, TestState::Idle);
        }
        return status;
    }

    // 获取当前状态
    Result<TestState> getCurrentState() const override
    {
        QMutexLocker locker(&m_mutex);
        return Result<TestState>{Status{}, m_state};
    }

    // 生成报告
    Result<ReportPath> generateReport(const ReportOptions& options) override
    {
        QVector<TestResult> results;
        ReportOptions resolved = options;
        {
            QMutexLocker locker(&m_mutex);
            results = m_results;
            if (resolved.taskId.isEmpty()) resolved.taskId = m_taskId;
            if (resolved.outDir.isEmpty() && m_configLoaded)
                resolved.outDir = m_config.runtimeConfig.reportDir;
        }
        if (!m_reportGenerator)
            return failure<ReportPath>(ErrorCode::InternalError, "Report generator is unavailable");
        return m_reportGenerator->createReport(results, resolved);
    }

    // 获取系统资源状态（当前为占位实现）
    Result<SystemResource> getResourceStatus() const override
    {
        return Result<SystemResource>{Status{}, SystemResource{}};
    }

private:
    // ---- 常量 ----
    static constexpr int kShutdownTimeoutMs = 5000;
    static constexpr int kMinPeriodicIntervalMs = 0;
    static constexpr int kMaxPeriodicIntervalMs = 60 * 60 * 1000;   // 1小时
    static constexpr quint64 kMaxFiniteCycles = 1000000000ULL;      // 10亿

    // 单次循环的执行结果
    enum class CycleOutcome {
        Completed,
        Stopped,
        Error,
    };

    // ------------------------------------------------------------------
    // RunControlAdapter - 运行控制适配器（IRunControl 实现）
    // 将服务层的 RunControl 状态适配为算法执行器所需的 IRunControl 接口
    // ------------------------------------------------------------------
    class RunControlAdapter final : public IRunControl {
    public:
        explicit RunControlAdapter(const TestRunService* service) : m_service(service) {}

        // 获取当前控制状态
        RunControl current() const override
        {
            QMutexLocker locker(&m_service->m_mutex);
            m_lastSeenControlVersion = m_service->m_controlVersion;
            m_hasObservedControl = true;
            return m_service->m_control;
        }

        // 检查点：暂停时阻塞等待，停止时返回 false
        bool checkpoint() const override
        {
            QMutexLocker locker(&m_service->m_mutex);
            // 暂停状态：阻塞等待直到恢复或停止
            while (m_service->m_control == RunControl::Pause) {
                m_service->m_controlChanged.wait(&m_service->m_mutex);
            }
            // 停止状态：首次检测到返回 true（给一次清理机会），之后返回 false
            if (m_service->m_control == RunControl::Stop) {
                if (m_hasObservedControl &&
                    m_lastSeenControlVersion != m_service->m_controlVersion) {
                    m_lastSeenControlVersion = m_service->m_controlVersion;
                    m_hasObservedControl = false;
                    return true;   // 首次通知停止，允许执行一次清理
                }
                return false;      // 后续调用直接返回 false
            }
            m_lastSeenControlVersion = m_service->m_controlVersion;
            m_hasObservedControl = false;
            return true;
        }

        void reset()
        {
            m_lastSeenControlVersion = m_service->m_controlVersion;
            m_hasObservedControl = false;
        }

    private:
        const TestRunService* m_service;
        mutable quint64 m_lastSeenControlVersion = 0;
        mutable bool m_hasObservedControl = false;
    };

    // 是否处于活动状态（Running / Paused / Stopping）
    static bool isActiveState(TestState state)
    {
        return state == TestState::Running || state == TestState::Paused ||
               state == TestState::Stopping;
    }

    // 校验 RunOptions 参数
    static Status validateRunOptions(const RunOptions& options)
    {
        switch (options.mode) {
        case RunMode::Single:
        case RunMode::DeviceStream:
            return Status{};
        case RunMode::PcPeriodic:
            if (options.intervalMs < kMinPeriodicIntervalMs ||
                options.intervalMs > kMaxPeriodicIntervalMs) {
                return makeStatus(ErrorCode::ParameterRangeError,
                    "PC periodic interval must be in the range 0..3600000 ms");
            }
            if (options.maxCycles > kMaxFiniteCycles) {
                return makeStatus(ErrorCode::ParameterRangeError,
                    "PC periodic maxCycles must be 0 or at most 1000000000");
            }
            return Status{};
        }
        return makeStatus(ErrorCode::ParameterRangeError, "Unknown run mode");
    }

    // 根据 testItems 筛选步骤（含依赖链自动包含）
    Result<TestPlan> selectSteps(const TestPlan& source, const QStringList& testItems) const
    {
        if (testItems.isEmpty()) return Result<TestPlan>{Status{}, source};

        QHash<StepId, TestStep> stepsById;
        for (const TestStep& step : source.steps)
            stepsById.insert(step.stepId, step);

        QSet<StepId> selected;
        // 递归包含依赖
        std::function<bool(const StepId&)> includeWithDependencies;
        includeWithDependencies = [&](const StepId& stepId) {
            if (selected.contains(stepId)) return true;
            auto it = stepsById.constFind(stepId);
            if (it == stepsById.cend()) return false;
            selected.insert(stepId);
            for (const StepId& dep : it->dependsOn)
                if (!includeWithDependencies(dep)) return false;
            return true;
        };

        for (const QString& item : testItems) {
            StepId matchingStep;
            for (const TestStep& step : source.steps) {
                if (step.testItemId == item || step.stepId == item) {
                    matchingStep = step.stepId;
                    break;
                }
            }
            if (matchingStep.isEmpty() || !includeWithDependencies(matchingStep))
                return failure<TestPlan>(ErrorCode::ItemNotFound,
                    QStringLiteral("Requested test item '%1' was not found").arg(item));
        }

        TestPlan selectedPlan = source;
        selectedPlan.steps.clear();
        for (const TestStep& step : source.steps)
            if (selected.contains(step.stepId))
                selectedPlan.steps.append(step);
        return Result<TestPlan>{Status{}, selectedPlan};
    }

    // 检查步骤的所有依赖是否均已通过
    bool dependenciesPassed(const TestStep& step) const
    {
        QMutexLocker locker(&m_mutex);
        for (const StepId& dep : step.dependsOn) {
            auto it = m_resultsByStep.constFind(dep);
            if (it == m_resultsByStep.cend() || it->verdict != TestVerdict::Pass)
                return false;
        }
        return true;
    }

    // 重试等待（支持暂停/停止中断）
    bool waitForRetryInterval(int intervalMs) const
    {
        int remainingMs = intervalMs;
        while (remainingMs > 0) {
            QMutexLocker locker(&m_mutex);
            if (m_control == RunControl::Stop) return false;
            if (m_control == RunControl::Pause) {
                m_controlChanged.wait(&m_mutex);
                continue;
            }
            QElapsedTimer elapsed;
            elapsed.start();
            m_controlChanged.wait(&m_mutex, static_cast<unsigned long>(remainingMs));
            remainingMs -= qMax<qint64>(1, elapsed.elapsed());
        }
        QMutexLocker locker(&m_mutex);
        return m_control != RunControl::Stop;
    }

    // 发布测试结果
    void publishResult(const TestResult& result)
    {
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            m_results.append(result);
            m_resultsByStep.insert(result.stepId, result);
            taskId = m_taskId;
        }
        emit resultProduced(taskId, result);
    }

    // 执行一次完整的测试循环（遍历所有步骤）
    CycleOutcome executeCycle(const TestPlan& plan,
                              const TestContext& context,
                              quint64 cycleIndex)
    {
        bool executionError = false;
        bool stopAfterFailure = false;
        for (const TestStep& step : plan.steps) {
            // 检查运行控制
            if (!m_runControl.checkpoint()) return CycleOutcome::Stopped;

            // 前面步骤失败且启用了首次失败停止 → 跳过剩余步骤
            if (stopAfterFailure) {
                TestResult skipped;
                skipped.stepId = step.stepId;
                skipped.testItemId = step.testItemId;
                skipped.algorithmId = step.algorithmId;
                skipped.verdict = TestVerdict::Skipped;
                skipped.skipReason = SkipReason::Cancelled;
                skipped.message = QStringLiteral("Skipped after a prior failure");
                skipped.cycleIndex = cycleIndex;
                publishResult(skipped);
                continue;
            }

            // 依赖未通过 → 跳过
            if (!dependenciesPassed(step)) {
                TestResult skipped;
                skipped.stepId = step.stepId;
                skipped.testItemId = step.testItemId;
                skipped.algorithmId = step.algorithmId;
                skipped.verdict = TestVerdict::Skipped;
                skipped.skipReason = SkipReason::DependencyFailed;
                skipped.message = QStringLiteral("A dependency did not pass");
                skipped.cycleIndex = cycleIndex;
                publishResult(skipped);
                continue;
            }

            // 执行步骤（含重试逻辑）
            Result<TestResult> outcome;
            int attempts = 0;
            for (int attempt = 0; attempt <= step.retryCount; ++attempt) {
                if (!m_runControl.checkpoint()) break;
                outcome = m_executor->executeStep(step, m_runControl, *this);
                ++attempts;
                // 不可重试的错误 或 已达最大重试次数 → 退出重试循环
                if (!retryable(outcome) || attempt == step.retryCount ||
                    m_runControl.current() == RunControl::Stop) {
                    break;
                }
                if (plan.runtimeConfig.retryIntervalMs > 0 &&
                    !waitForRetryInterval(plan.runtimeConfig.retryIntervalMs)) {
                    break;
                }
            }

            if (attempts == 0) return CycleOutcome::Stopped;

            // 填充结果
            TestResult result = outcome.value;
            result.stepId = step.stepId;
            result.testItemId = step.testItemId;
            result.algorithmId = step.algorithmId;
            result.attempts = attempts;
            result.cycleIndex = cycleIndex;

            if (!outcome.status.ok()) {
                result.verdict = TestVerdict::Error;
                result.errorCode = outcome.status.code;
                if (result.message.isEmpty())
                    result.message = outcome.status.error.message;
                executionError = true;
                emit hardwareError(context.runId, step.testItemId,
                                   outcome.status.code, result.message);
            } else if (result.verdict == TestVerdict::Error) {
                executionError = true;
                if (result.errorCode != ErrorCode::Ok) {
                    emit hardwareError(context.runId, step.testItemId,
                                       result.errorCode, result.message);
                }
            }
            publishResult(result);

            if (plan.runtimeConfig.stopOnFirstFailure &&
                (result.verdict == TestVerdict::Fail ||
                 result.verdict == TestVerdict::Error)) {
                stopAfterFailure = true;
            }
        }
        return executionError ? CycleOutcome::Error : CycleOutcome::Completed;
    }

    // 工作线程主函数
    void runTask(TestPlan plan, TestContext context,
                 QVariantMap executionConfig, RunOptions runOptions)
    {
        // 等待主线程允许开始
        {
            QMutexLocker locker(&m_mutex);
            while (!m_workerCanRun && m_control != RunControl::Stop) {
                m_controlChanged.wait(&m_mutex);
            }
            if (m_control == RunControl::Stop) {
                locker.unlock();
                completeWorker(TestState::Finished);
                return;
            }
        }

        // 调用算法执行器的 prepare
        const Status prepared = m_executor->prepare(plan, context, executionConfig);
        if (!prepared.ok()) {
            emit hardwareError(context.runId, QString(), prepared.code, prepared.error.message);
            completeWorker(TestState::Error);
            return;
        }

        // 主循环（支持周期性运行）
        bool executionError = false;
        quint64 cycleIndex = 1;
        while (true) {
            if (!m_runControl.checkpoint()) break;
            {
                QMutexLocker locker(&m_mutex);
                m_resultsByStep.clear();
                m_currentCycleIndex = cycleIndex;
            }
            emit cycleStarted(context.runId, cycleIndex);

            const CycleOutcome outcome = executeCycle(plan, context, cycleIndex);
            if (outcome == CycleOutcome::Stopped) break;
            if (outcome == CycleOutcome::Error) { executionError = true; break; }

            // 非周期性模式 或 已达最大周期 → 退出
            if (runOptions.mode != RunMode::PcPeriodic ||
                (runOptions.maxCycles > 0 && cycleIndex >= runOptions.maxCycles)) {
                break;
            }
            // 周期间隔等待
            if (runOptions.intervalMs > 0) {
                if (!waitForRetryInterval(runOptions.intervalMs)) break;
            } else {
                QThread::yieldCurrentThread();
                if (!m_runControl.checkpoint()) break;
            }
            ++cycleIndex;
        }

        // 算法收尾
        const Status finished = m_executor->finishRun();
        if (!finished.ok()) {
            emit hardwareError(context.runId, QString(), finished.code, finished.error.message);
            executionError = true;
        }
        completeWorker(executionError ? TestState::Error : TestState::Finished);
    }

    // 标记工作线程完成，更新状态
    void completeWorker(TestState terminalState)
    {
        TaskId taskId;
        TestState resultingState = terminalState;
        {
            QMutexLocker locker(&m_mutex);
            m_workerDone = true;
            m_workerTerminalState = terminalState;
            if (m_stopRequested) {
                resultingState = (m_stopResponseKnown && m_stopAccepted)
                    ? TestState::Idle : terminalState;
            }
            m_state = resultingState;
            taskId = m_taskId;
            m_controlChanged.wakeAll();
        }
        emit stateChanged(taskId, resultingState);
    }

    // 收割已完成的工作线程
    void reapFinishedWorker()
    {
        std::unique_ptr<QThread> worker;
        {
            QMutexLocker locker(&m_mutex);
            if (m_worker != nullptr && m_workerDone)
                worker = std::move(m_worker);
        }
        if (worker) worker->wait();
    }

    // 等待工作线程结束
    void joinWorker()
    {
        std::unique_ptr<QThread> worker;
        {
            QMutexLocker locker(&m_mutex);
            if (m_worker != nullptr)
                worker = std::move(m_worker);
        }
        if (worker) worker->wait();
    }

    // ------------------------------------------------------------------
    // IAlgorithmObserver 回调实现（私有继承，不对外暴露）
    // ------------------------------------------------------------------

    void onProgress(const StepId& stepId, const TestItemId& testItemId,
                    int progress, const QString& stage) override
    {
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            taskId = m_taskId;
        }
        Q_UNUSED(stepId);
        emit testProgress(taskId, testItemId, progress, stage);
    }

    void onSample(const StepId& stepId, const RawSample& sample) override
    {
        RawSample forwarded = sample;
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            forwarded.cycleIndex = m_currentCycleIndex;
            if (forwarded.timestampUs == 0) {
                forwarded.timestampUs =
                    static_cast<qint64>(QDateTime::currentMSecsSinceEpoch()) * 1000;
            }
            taskId = m_taskId;
        }
        emit sampleProduced(taskId, stepId, forwarded);
    }

    void onLog(const hwtest::logging::LogEvent& event) override
    {
        hwtest::logging::LogEvent forwarded = event;
        {
            QMutexLocker locker(&m_mutex);
            forwarded.requestId = m_context.requestId;
        }
        emit logProduced(forwarded);
    }

    // ---- 成员变量 ----
    mutable QMutex m_mutex;                     // 保护所有状态字段
    mutable QWaitCondition m_controlChanged;    // 控制状态变化时的条件变量
    mutable std::recursive_mutex m_lifecycleMutex; // 防止公开接口重入
    IAlgorithmExecutor* const m_executor;       // 算法执行器（非拥有）
    TestConfigManager m_configManager;          // 配置管理器
    TestPlanBuilder m_planBuilder;              // 计划构建器
    RunControlAdapter m_runControl;             // 运行控制适配器
    std::unique_ptr<IReportGenerator> m_reportGenerator; // 报告生成器
    std::unique_ptr<QThread> m_worker;          // 工作线程
    TestConfig m_config;                        // 当前加载的配置
    TestContext m_context;                      // 当前任务上下文
    TaskId m_taskId;                            // 当前任务ID
    QVector<TestResult> m_results;              // 所有步骤的结果
    QHash<StepId, TestResult> m_resultsByStep;  // stepId → 结果（用于依赖检查）
    quint64 m_currentCycleIndex = 0;            // 当前周期序号
    TestState m_state = TestState::Uninitialized;
    TestState m_workerTerminalState = TestState::Finished;
    RunControl m_control = RunControl::Run;
    quint64 m_controlVersion = 0;               // 控制版本（用于首次停止通知）
    bool m_initialized = false;
    bool m_configLoaded = false;
    bool m_shuttingDown = false;
    bool m_executorShutdown = false;
    bool m_workerDone = true;
    bool m_workerCanRun = false;
    bool m_stopRequested = false;
    bool m_stopResponseKnown = false;
    bool m_stopAccepted = false;
};

} // 匿名命名空间

// 工厂函数：创建 TestRunService 实例
ITestRunService* createTestRunServiceImplementation(IAlgorithmExecutor* executor, QObject* parent)
{
    return new TestRunService(executor, parent);
}

} // namespace hwtest::biz