#include <biz/i_test_run_service.h>

#include <biz/i_algorithm_executor.h>
#include <biz/i_report_generator.h>
#include <biz/test_config_manager.h>
#include <biz/test_plan_builder.h>

#include <QHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QUuid>
#include <QWaitCondition>

#include <memory>
#include <functional>
#include <mutex>
#include <thread>

namespace hwtest::biz {

IReportGenerator* createReportGeneratorImplementation();

namespace {

Status makeStatus(ErrorCode code, const QString& message)
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

template <typename T>
Result<T> failure(ErrorCode code, const QString& message)
{
    Result<T> result;
    result.status = makeStatus(code, message);
    return result;
}

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

class TestRunService final : public ITestRunService, private IAlgorithmObserver {
public:
    TestRunService(IAlgorithmExecutor* executor, QObject* parent)
        : ITestRunService(parent)
        , m_executor(executor)
        , m_runControl(this)
        , m_reportGenerator(createReportGeneratorImplementation())
    {
    }

    ~TestRunService() override
    {
        shutdown();
    }

    Status initialize() override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
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
                m_state = TestState::Idle;
                taskId = m_taskId;
                notify = true;
            }
        }
        if (notify) {
            emit stateChanged(taskId, TestState::Idle);
        }
        return Status{};
    }

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
            if (m_executorShutdown) {
                return Status{};
            }

            m_shuttingDown = true;
            executor = m_executor;
            active = isActiveState(m_state);
            if (active) {
                m_stopRequested = true;
                m_stopResponseKnown = false;
                m_stopAccepted = false;
                m_control = RunControl::Stop;
                ++m_controlVersion;
                m_state = TestState::Stopping;
                m_controlChanged.wakeAll();
            }
        }

        Status stopStatus;
        if (active) {
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
            if (notify) {
                emit stateChanged(taskId, state);
            }
        }

        joinWorker();
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

        if (!stopStatus.ok()) {
            return stopStatus;
        }
        return executorStatus;
    }

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
        if (!loaded.ok()) {
            return loaded.status;
        }

        QMutexLocker locker(&m_mutex);
        if (!m_initialized || (m_state != TestState::Idle && m_state != TestState::Finished)) {
            return makeStatus(ErrorCode::ResourceBusy,
                              QStringLiteral("Configuration changed while loading"));
        }
        m_config = loaded.value;
        m_configLoaded = true;
        return Status{};
    }

    Result<TaskId> startTest(const QStringList& testItems, int priority) override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        reapFinishedWorker();

        TestConfig config;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_initialized) {
                return failure<TaskId>(ErrorCode::NotInitialized,
                                       QStringLiteral("Service has not been initialized"));
            }
            if (!m_configLoaded) {
                return failure<TaskId>(ErrorCode::ConfigNotLoaded,
                                       QStringLiteral("No configuration is loaded"));
            }
            if (m_shuttingDown || (m_state != TestState::Idle && m_state != TestState::Finished)) {
                return failure<TaskId>(ErrorCode::ResourceBusy,
                                        QStringLiteral("A task cannot start in the current state"));
            }
            config = m_config;
        }

        if (priority < -1) {
            return failure<TaskId>(ErrorCode::ParameterRangeError,
                                   QStringLiteral("Task priority must be -1 or in the range 1..3"));
        }
        const int effectivePriority = priority == -1
            ? config.runtimeConfig.taskPriorityDefault
            : priority;
        if (effectivePriority < 1 || effectivePriority > 3) {
            return failure<TaskId>(ErrorCode::ParameterRangeError,
                                   QStringLiteral("Task priority must be in the range 1..3"));
        }

        const Result<TestPlan> built = m_planBuilder.build(config);
        if (!built.ok()) {
            return failure<TaskId>(built.status.code, built.status.error.message);
        }
        const Result<TestPlan> selected = selectSteps(built.value, testItems);
        if (!selected.ok()) {
            return failure<TaskId>(selected.status.code, selected.status.error.message);
        }

        reapFinishedWorker();

        const TaskId taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        TestContext context;
        context.runId = taskId;
        context.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        context.productModel = config.productModel;
        context.tags = config.runtimeConfig.tags;

        {
            QMutexLocker locker(&m_mutex);
            if (m_shuttingDown || isActiveState(m_state)) {
                return failure<TaskId>(ErrorCode::ResourceBusy,
                                       QStringLiteral("A task is already running"));
            }
            if (m_worker.joinable()) {
                return failure<TaskId>(ErrorCode::ResourceBusy,
                                       QStringLiteral("The previous task is still being finalized"));
            }
            m_taskId = taskId;
            m_context = context;
            m_results.clear();
            m_resultsByStep.clear();
            m_samples.clear();
            m_control = RunControl::Run;
            ++m_controlVersion;
            m_runControl.reset();
            m_stopRequested = false;
            m_stopResponseKnown = false;
            m_stopAccepted = false;
            m_workerDone = false;
            m_workerCanRun = false;
            m_workerTerminalState = TestState::Finished;
            m_priority = effectivePriority;
            m_state = TestState::Running;
            m_worker = std::thread(&TestRunService::runTask,
                                   this,
                                   selected.value,
                                   context,
                                   config.executionConfig);
        }
        emit stateChanged(taskId, TestState::Running);
        {
            QMutexLocker locker(&m_mutex);
            if (m_taskId == taskId) {
                m_workerCanRun = true;
                m_controlChanged.wakeAll();
            }
        }
        return Result<TaskId>{Status{}, taskId};
    }

    Status pauseTest() override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            if (m_state != TestState::Running) {
                return makeStatus(ErrorCode::InvalidState,
                                  QStringLiteral("Task is not running"));
            }
            m_control = RunControl::Pause;
            ++m_controlVersion;
            m_state = TestState::Paused;
            taskId = m_taskId;
            m_controlChanged.wakeAll();
        }
        emit stateChanged(taskId, TestState::Paused);
        return Status{};
    }

    Status resumeTest() override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            if (m_state != TestState::Paused) {
                return makeStatus(ErrorCode::InvalidState,
                                  QStringLiteral("Task is not paused"));
            }
            m_control = RunControl::Run;
            ++m_controlVersion;
            m_state = TestState::Running;
            taskId = m_taskId;
            m_controlChanged.wakeAll();
        }
        emit stateChanged(taskId, TestState::Running);
        return Status{};
    }

    Status stopTest(int timeoutMs) override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        if (timeoutMs < 0) {
            return makeStatus(ErrorCode::ParameterRangeError,
                              QStringLiteral("Stop timeout must not be negative"));
        }

        IAlgorithmExecutor* executor = nullptr;
        TaskId taskId;
        {
            QMutexLocker locker(&m_mutex);
            if (m_state == TestState::Idle || m_state == TestState::Finished) {
                return Status{};
            }
            if (!isActiveState(m_state)) {
                return makeStatus(ErrorCode::InvalidState,
                                  QStringLiteral("Task is not active"));
            }
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

        const Status stopStatus = executor->requestStop(timeoutMs);
        TestState completionState = TestState::Stopping;
        bool notify = false;
        {
            QMutexLocker locker(&m_mutex);
            m_stopResponseKnown = true;
            m_stopAccepted = stopStatus.ok();
            if (m_workerDone) {
                completionState = m_stopAccepted ? TestState::Idle : m_workerTerminalState;
                m_state = completionState;
                notify = true;
            }
        }
        if (notify) {
            emit stateChanged(taskId, completionState);
        }
        return stopStatus;
    }

    Status resetHardware() override
    {
        const std::lock_guard<std::recursive_mutex> lifecycleLocker(m_lifecycleMutex);
        IAlgorithmExecutor* executor = nullptr;
        TaskId taskId;
        bool recoverErrorState = false;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_initialized) {
                return makeStatus(ErrorCode::NotInitialized,
                                  QStringLiteral("Service has not been initialized"));
            }
            if (isActiveState(m_state)) {
                return makeStatus(ErrorCode::ResourceBusy,
                                  QStringLiteral("Reset is not allowed while a task is active"));
            }
            executor = m_executor;
            taskId = m_taskId;
            recoverErrorState = m_state == TestState::Error;
        }
        if (executor == nullptr) {
            return makeStatus(ErrorCode::ParameterRangeError,
                              QStringLiteral("An algorithm executor is required"));
        }
        const Status status = executor->reset();
        if (status.ok() && recoverErrorState) {
            {
                QMutexLocker locker(&m_mutex);
                m_state = TestState::Idle;
            }
            emit stateChanged(taskId, TestState::Idle);
        }
        return status;
    }

    Result<TestState> getCurrentState() const override
    {
        QMutexLocker locker(&m_mutex);
        return Result<TestState>{Status{}, m_state};
    }

    Result<ReportPath> generateReport(const ReportOptions& options) override
    {
        QVector<TestResult> results;
        ReportOptions resolved = options;
        {
            QMutexLocker locker(&m_mutex);
            results = m_results;
            if (resolved.taskId.isEmpty()) {
                resolved.taskId = m_taskId;
            }
            if (resolved.outDir.isEmpty() && m_configLoaded) {
                resolved.outDir = m_config.runtimeConfig.reportDir;
            }
        }
        if (!m_reportGenerator) {
            return failure<ReportPath>(ErrorCode::InternalError,
                                       QStringLiteral("Report generator is unavailable"));
        }
        return m_reportGenerator->createReport(results, resolved);
    }

    Result<SystemResource> getResourceStatus() const override
    {
        return Result<SystemResource>{Status{}, SystemResource{}};
    }

private:
    static constexpr int kShutdownTimeoutMs = 5000;

    class RunControlAdapter final : public IRunControl {
    public:
        explicit RunControlAdapter(const TestRunService* service)
            : m_service(service)
        {
        }

        RunControl current() const override
        {
            QMutexLocker locker(&m_service->m_mutex);
            m_lastSeenControlVersion = m_service->m_controlVersion;
            m_hasObservedControl = true;
            return m_service->m_control;
        }

        bool checkpoint() const override
        {
            QMutexLocker locker(&m_service->m_mutex);
            while (m_service->m_control == RunControl::Pause) {
                m_service->m_controlChanged.wait(&m_service->m_mutex);
            }
            if (m_service->m_control == RunControl::Stop) {
                if (m_hasObservedControl &&
                    m_lastSeenControlVersion != m_service->m_controlVersion) {
                    m_lastSeenControlVersion = m_service->m_controlVersion;
                    m_hasObservedControl = false;
                    return true;
                }
                return false;
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

    static bool isActiveState(TestState state)
    {
        return state == TestState::Running || state == TestState::Paused ||
            state == TestState::Stopping;
    }

    Result<TestPlan> selectSteps(const TestPlan& source, const QStringList& testItems) const
    {
        if (testItems.isEmpty()) {
            return Result<TestPlan>{Status{}, source};
        }

        QHash<StepId, TestStep> stepsById;
        for (const TestStep& step : source.steps) {
            stepsById.insert(step.stepId, step);
        }

        QSet<StepId> selected;
        std::function<bool(const StepId&)> includeWithDependencies;
        includeWithDependencies = [&stepsById, &selected, &includeWithDependencies](const StepId& stepId) {
                if (selected.contains(stepId)) {
                    return true;
                }
                const auto iterator = stepsById.constFind(stepId);
                if (iterator == stepsById.cend()) {
                    return false;
                }
                selected.insert(stepId);
                for (const StepId& dependency : iterator->dependsOn) {
                    if (!includeWithDependencies(dependency)) {
                        return false;
                    }
                }
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
            if (matchingStep.isEmpty() || !includeWithDependencies(matchingStep)) {
                return failure<TestPlan>(ErrorCode::ItemNotFound,
                                         QStringLiteral("Requested test item '%1' was not found").arg(item));
            }
        }

        TestPlan selectedPlan = source;
        selectedPlan.steps.clear();
        for (const TestStep& step : source.steps) {
            if (selected.contains(step.stepId)) {
                selectedPlan.steps.append(step);
            }
        }
        return Result<TestPlan>{Status{}, selectedPlan};
    }

    bool dependenciesPassed(const TestStep& step) const
    {
        QMutexLocker locker(&m_mutex);
        for (const StepId& dependency : step.dependsOn) {
            const auto result = m_resultsByStep.constFind(dependency);
            if (result == m_resultsByStep.cend() || result->verdict != TestVerdict::Pass) {
                return false;
            }
        }
        return true;
    }

    bool waitForRetryInterval(int intervalMs) const
    {
        int remainingMs = intervalMs;
        while (remainingMs > 0) {
            QMutexLocker locker(&m_mutex);
            if (m_control == RunControl::Stop) {
                return false;
            }
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

    void runTask(TestPlan plan, TestContext context, QVariantMap executionConfig)
    {
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

        const Status prepared = m_executor->prepare(plan, context, executionConfig);
        if (!prepared.ok()) {
            emit hardwareError(context.runId,
                               QString(),
                               prepared.code,
                               prepared.error.message);
            completeWorker(TestState::Error);
            return;
        }

        bool executionError = false;
        bool stopAfterFailure = false;
        for (const TestStep& step : plan.steps) {
            if (!m_runControl.checkpoint()) {
                break;
            }

            if (stopAfterFailure) {
                TestResult skipped;
                skipped.stepId = step.stepId;
                skipped.testItemId = step.testItemId;
                skipped.algorithmId = step.algorithmId;
                skipped.verdict = TestVerdict::Skipped;
                skipped.skipReason = SkipReason::Cancelled;
                skipped.message = QStringLiteral("Skipped after a prior failure");
                publishResult(skipped);
                continue;
            }

            if (!dependenciesPassed(step)) {
                TestResult skipped;
                skipped.stepId = step.stepId;
                skipped.testItemId = step.testItemId;
                skipped.algorithmId = step.algorithmId;
                skipped.verdict = TestVerdict::Skipped;
                skipped.skipReason = SkipReason::DependencyFailed;
                skipped.message = QStringLiteral("A dependency did not pass");
                publishResult(skipped);
                continue;
            }

            Result<TestResult> outcome;
            int attempts = 0;
            for (int attempt = 0; attempt <= step.retryCount; ++attempt) {
                if (!m_runControl.checkpoint()) {
                    break;
                }
                outcome = m_executor->executeStep(step, m_runControl, *this);
                ++attempts;
                if (!retryable(outcome) || attempt == step.retryCount ||
                    m_runControl.current() == RunControl::Stop) {
                    break;
                }
                if (plan.runtimeConfig.retryIntervalMs > 0 &&
                    !waitForRetryInterval(plan.runtimeConfig.retryIntervalMs)) {
                    break;
                }
            }

            if (attempts == 0) {
                break;
            }

            TestResult result = outcome.value;
            result.stepId = step.stepId;
            result.testItemId = step.testItemId;
            result.algorithmId = step.algorithmId;
            result.attempts = attempts;
            if (!outcome.status.ok()) {
                result.verdict = TestVerdict::Error;
                result.errorCode = outcome.status.code;
                if (result.message.isEmpty()) {
                    result.message = outcome.status.error.message;
                }
                executionError = true;
                emit hardwareError(context.runId,
                                   step.testItemId,
                                   outcome.status.code,
                                   result.message);
            } else if (result.verdict == TestVerdict::Error) {
                executionError = true;
                if (result.errorCode != ErrorCode::Ok) {
                    emit hardwareError(context.runId,
                                       step.testItemId,
                                       result.errorCode,
                                       result.message);
                }
            }
            publishResult(result);

            if (plan.runtimeConfig.stopOnFirstFailure &&
                (result.verdict == TestVerdict::Fail || result.verdict == TestVerdict::Error)) {
                stopAfterFailure = true;
            }
        }

        completeWorker(executionError ? TestState::Error : TestState::Finished);
    }

    void completeWorker(TestState terminalState)
    {
        TaskId taskId;
        TestState resultingState = terminalState;
        {
            QMutexLocker locker(&m_mutex);
            m_workerDone = true;
            m_workerTerminalState = terminalState;
            if (m_stopRequested) {
                if (m_stopResponseKnown) {
                    resultingState = m_stopAccepted ? TestState::Idle : terminalState;
                } else {
                    resultingState = TestState::Stopping;
                }
            }
            m_state = resultingState;
            taskId = m_taskId;
        }
        emit stateChanged(taskId, resultingState);
    }

    void reapFinishedWorker()
    {
        std::thread worker;
        {
            QMutexLocker locker(&m_mutex);
            if (m_worker.joinable() && m_workerDone) {
                worker = std::move(m_worker);
            }
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    void joinWorker()
    {
        std::thread worker;
        {
            QMutexLocker locker(&m_mutex);
            if (m_worker.joinable()) {
                worker = std::move(m_worker);
            }
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    void onProgress(const StepId& stepId,
                    const TestItemId& testItemId,
                    int progress,
                    const QString& stage) override
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
        QMutexLocker locker(&m_mutex);
        m_samples[stepId].append(sample);
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

    mutable QMutex m_mutex;
    mutable QWaitCondition m_controlChanged;
    mutable std::recursive_mutex m_lifecycleMutex;
    IAlgorithmExecutor* const m_executor;
    TestConfigManager m_configManager;
    TestPlanBuilder m_planBuilder;
    RunControlAdapter m_runControl;
    std::unique_ptr<IReportGenerator> m_reportGenerator;
    std::thread m_worker;
    TestConfig m_config;
    TestContext m_context;
    TaskId m_taskId;
    QVector<TestResult> m_results;
    QHash<StepId, TestResult> m_resultsByStep;
    QHash<StepId, QVector<RawSample>> m_samples;
    TestState m_state = TestState::Uninitialized;
    TestState m_workerTerminalState = TestState::Finished;
    RunControl m_control = RunControl::Run;
    quint64 m_controlVersion = 0;
    int m_priority = 0;
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

} // namespace

ITestRunService* createTestRunServiceImplementation(IAlgorithmExecutor* executor, QObject* parent)
{
    return new TestRunService(executor, parent);
}

} // namespace hwtest::biz
