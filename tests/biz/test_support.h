#pragma once

#include <biz/biz_types.h>
#include <biz/i_algorithm_executor.h>

#include <logging/log_types.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QMutex>
#include <QThread>
#include <QVariantList>
#include <QWaitCondition>

#include <functional>

namespace hwtest::biz::test {

inline Status makeStatus(ErrorCode code, const QString& message = {})
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

inline TestResult makeResult(const TestStep& step,
                             TestVerdict verdict = TestVerdict::Pass,
                             ErrorCode errorCode = ErrorCode::Ok,
                             const QString& message = {})
{
    TestResult result;
    result.stepId = step.stepId;
    result.testItemId = step.testItemId;
    result.algorithmId = step.algorithmId;
    result.verdict = verdict;
    result.errorCode = errorCode;
    result.message = message;
    result.attempts = 1;
    return result;
}

inline Result<TestResult> successfulResult(const TestStep& step)
{
    return Result<TestResult>{Status{}, makeResult(step)};
}

inline Result<TestResult> failedResult(const TestStep& step,
                                       ErrorCode errorCode,
                                       const QString& message)
{
    return Result<TestResult>{makeStatus(errorCode, message),
                              makeResult(step, TestVerdict::Error, errorCode, message)};
}

inline TestStep makeStep(const StepId& stepId,
                         const TestItemId& testItemId,
                         const QString& name)
{
    TestStep step;
    step.stepId = stepId;
    step.testItemId = testItemId;
    step.name = name;
    step.type = QStringLiteral("EXCHANGE");
    step.board = QStringLiteral("station-a");
    step.algorithmId = QStringLiteral("algorithm.%1").arg(stepId);
    step.parameters.insert(QStringLiteral("mode"), QStringLiteral("deterministic"));
    step.timeoutMs = 1200;
    step.retryCount = 0;
    step.enabled = true;

    Criterion criterion;
    criterion.metric = QStringLiteral("value");
    criterion.op = CmpOp::InRange;
    criterion.ref = 2.0;
    criterion.lo = 1.0;
    criterion.hi = 3.0;
    criterion.tol = 0.1;
    criterion.passIfMatched = false;
    step.criteria.append(criterion);
    return step;
}

inline TestConfig makeCompleteConfig()
{
    TestConfig config;
    config.schemaVersion = QStringLiteral("1.0");
    config.configId = QStringLiteral("config-complete");
    config.productModel = QStringLiteral("MODEL-X");
    config.productName = QStringLiteral("Complete configuration");
    config.configVersion = QStringLiteral("2026.07");

    TestStep first = makeStep(QStringLiteral("STEP_A"),
                              QStringLiteral("ITEM_A"),
                              QStringLiteral("First step"));
    first.dependsOn = {};
    TestStep second = makeStep(QStringLiteral("STEP_B"),
                               QStringLiteral("ITEM_B"),
                               QStringLiteral("Second step"));
    second.dependsOn = {first.stepId};
    second.retryCount = 2;
    config.steps = {first, second};

    HardwareRequirement requirement;
    requirement.requirementId = QStringLiteral("resource-set-a");
    requirement.deviceId = QStringLiteral("station-a");
    requirement.adapterId = QStringLiteral("logical-adapter-a");
    requirement.requiredModules = {QStringLiteral("input"), QStringLiteral("output")};
    requirement.requiredResources = {QStringLiteral("R-A"), QStringLiteral("R-B")};
    requirement.priority = 2;
    requirement.match.insert(QStringLiteral("serial"), QStringLiteral("S-001"));
    config.hardwareRequirements = {requirement};

    ProtocolProfile profile;
    profile.id = QStringLiteral("profile-a");
    profile.busType = QStringLiteral("MESSAGE");
    profile.payloadEncoding = QStringLiteral("json");
    profile.frameFormat.insert(QStringLiteral("version"), 1);
    profile.timing.insert(QStringLiteral("responseTimeoutMs"), 300);
    profile.responseRules.insert(QStringLiteral("required"), true);
    profile.fieldMapping.insert(QStringLiteral("value"), QStringLiteral("payload.value"));
    config.protocolProfiles = {profile};

    // This is deliberately opaque to BIZ: it must be persisted and forwarded,
    // rather than interpreted as a HAL, protocol, or scheduler configuration.
    config.executionConfig.insert(QStringLiteral("executionProfile"),
                                 QStringLiteral("deterministic"));
    config.executionConfig.insert(QStringLiteral("sampleWindow"), 32);
    QVariantMap executorOptions;
    executorOptions.insert(QStringLiteral("strategy"), QStringLiteral("sequential"));
    executorOptions.insert(QStringLiteral("labels"), QVariantList{QStringLiteral("contract"), QStringLiteral("red")});
    config.executionConfig.insert(QStringLiteral("executorOptions"), executorOptions);

    config.safetyPolicy.outputLimits.insert(QStringLiteral("maxValue"), 10.0);
    config.safetyPolicy.safeState.insert(QStringLiteral("output"), 0.0);
    config.safetyPolicy.enterSafeStateOnStop = true;
    config.safetyPolicy.enterSafeStateOnError = true;
    config.safetyPolicy.daMinVoltage = 0.0;
    config.safetyPolicy.daMaxVoltage = 10.0;
    config.safetyPolicy.doMinSwitchIntervalMs = 20;
    config.safetyPolicy.canSendMaxHz = 100;
    config.safetyPolicy.resourceLockTimeoutMs = 2500;

    config.runtimeConfig.parallelEnabled = false;
    config.runtimeConfig.maxParallel = 1;
    config.runtimeConfig.defaultTimeoutMs = 1800;
    config.runtimeConfig.defaultRetryCount = 2;
    config.runtimeConfig.retryIntervalMs = 15;
    config.runtimeConfig.taskPriorityDefault = 2;
    config.runtimeConfig.pauseAutoReleaseMs = 0;
    config.runtimeConfig.stopOnFirstFailure = false;
    config.runtimeConfig.allowResume = true;
    config.runtimeConfig.reportDir = QStringLiteral("reports");
    config.runtimeConfig.logDir = QStringLiteral("logs");
    config.runtimeConfig.logRotateBytes = 4096;
    config.runtimeConfig.logKeepFiles = 3;
    config.runtimeConfig.tags.insert(QStringLiteral("suite"), QStringLiteral("biz"));

    config.reportFields.insert(QStringLiteral("operator"), QStringLiteral("operator-a"));
    config.reportFields.insert(QStringLiteral("includeRawData"), true);
    return config;
}

inline QCoreApplication& ensureQtApplication()
{
    if (QCoreApplication* existing = QCoreApplication::instance()) {
        registerBizMetaTypes();
        return *existing;
    }

    static int argc = 1;
    static char programName[] = "hwtest_biz_tests";
    static char* argv[] = {programName, nullptr};
    static QCoreApplication application(argc, argv);
    registerBizMetaTypes();
    return application;
}

inline bool waitUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (predicate()) {
            return true;
        }
        QThread::msleep(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

class FakeAlgorithmExecutor final : public IAlgorithmExecutor {
public:
    struct PrepareSnapshot {
        TestPlan plan;
        TestContext context;
        QVariantMap executionConfig;
    };

    struct PortCallSnapshot {
        int prepareCalls = 0;
        int executeCalls = 0;
        int requestStopCalls = 0;
        int resetCalls = 0;
        int shutdownCalls = 0;
    };

    Status prepare(const TestPlan& plan,
                   const TestContext& context,
                   const QVariantMap& executionConfig) override
    {
        QMutexLocker locker(&m_mutex);
        ++m_prepareCalls;
        m_prepareSnapshot = PrepareSnapshot{plan, context, executionConfig};
        return m_prepareStatus;
    }

    Result<TestResult> executeStep(const TestStep& step,
                                   const IRunControl& control,
                                   IAlgorithmObserver& observer) override
    {
        {
            QMutexLocker locker(&m_mutex);
            m_executeOrder.append(step.stepId);
            m_executionStarted = true;
            m_executionStartedCondition.wakeAll();
        }

        observer.onProgress(step.stepId, step.testItemId, 50, QStringLiteral("executing"));
        RawSample sample;
        sample.channelId = QStringLiteral("sample");
        sample.values.insert(QStringLiteral("value"), 2.0);
        observer.onSample(step.stepId, sample);

        hwtest::logging::LogEvent event;
        event.level = QStringLiteral("INFO");
        event.source = QStringLiteral("algorithm");
        event.category = QStringLiteral("step");
        event.message = QStringLiteral("executing %1").arg(step.stepId);
        event.requestId = prepareSnapshot().context.requestId;
        observer.onLog(event);

        while (isBlocking()) {
            const RunControl current = control.current();
            recordControl(current);
            if (!ignoresRunControl() &&
                (current == RunControl::Stop || !control.checkpoint())) {
                return failedResult(step, ErrorCode::Cancelled, QStringLiteral("cancelled"));
            }
            QThread::msleep(2);
        }

        return takeScriptedResult(step);
    }

    Status requestStop(int timeoutMs) override
    {
        QMutexLocker locker(&m_mutex);
        m_requestStopTimeouts.append(timeoutMs);
        return m_requestStopStatus;
    }

    Status reset() override
    {
        QMutexLocker locker(&m_mutex);
        ++m_resetCalls;
        return m_resetStatus;
    }

    Status shutdown(int timeoutMs) override
    {
        QMutexLocker locker(&m_mutex);
        m_shutdownTimeouts.append(timeoutMs);
        return m_shutdownStatus;
    }

    void setPrepareStatus(const Status& status)
    {
        QMutexLocker locker(&m_mutex);
        m_prepareStatus = status;
    }

    void setRequestStopStatus(const Status& status)
    {
        QMutexLocker locker(&m_mutex);
        m_requestStopStatus = status;
    }

    void setScript(const StepId& stepId, const QVector<Result<TestResult>>& results)
    {
        QMutexLocker locker(&m_mutex);
        m_scripts.insert(stepId, results);
        m_nextScriptIndex.insert(stepId, 0);
    }

    void blockExecution()
    {
        QMutexLocker locker(&m_mutex);
        m_blockExecution = true;
        m_allowCompletion = false;
    }

    void allowCompletion()
    {
        QMutexLocker locker(&m_mutex);
        m_allowCompletion = true;
    }

    void ignoreRunControl()
    {
        QMutexLocker locker(&m_mutex);
        m_ignoreRunControl = true;
    }

    bool waitForExecutionStart(int timeoutMs)
    {
        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&m_mutex);
        while (!m_executionStarted && timer.elapsed() < timeoutMs) {
            const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
            m_executionStartedCondition.wait(&m_mutex, qMax(1, remaining));
        }
        return m_executionStarted;
    }

    int prepareCalls() const
    {
        QMutexLocker locker(&m_mutex);
        return m_prepareCalls;
    }

    PrepareSnapshot prepareSnapshot() const
    {
        QMutexLocker locker(&m_mutex);
        return m_prepareSnapshot;
    }

    QVector<StepId> executeOrder() const
    {
        QMutexLocker locker(&m_mutex);
        return m_executeOrder;
    }

    int executeCallCount() const
    {
        QMutexLocker locker(&m_mutex);
        return m_executeOrder.size();
    }

    QVector<int> requestStopTimeouts() const
    {
        QMutexLocker locker(&m_mutex);
        return m_requestStopTimeouts;
    }

    PortCallSnapshot portCallSnapshot() const
    {
        QMutexLocker locker(&m_mutex);
        return PortCallSnapshot{m_prepareCalls,
                                m_executeOrder.size(),
                                m_requestStopTimeouts.size(),
                                m_resetCalls,
                                m_shutdownTimeouts.size()};
    }

    bool sawControl(RunControl value) const
    {
        QMutexLocker locker(&m_mutex);
        return m_seenControls.contains(value);
    }

private:
    bool isBlocking() const
    {
        QMutexLocker locker(&m_mutex);
        return m_blockExecution && !m_allowCompletion;
    }

    bool ignoresRunControl() const
    {
        QMutexLocker locker(&m_mutex);
        return m_ignoreRunControl;
    }

    void recordControl(RunControl value)
    {
        QMutexLocker locker(&m_mutex);
        m_seenControls.append(value);
    }

    Result<TestResult> takeScriptedResult(const TestStep& step)
    {
        QMutexLocker locker(&m_mutex);
        const auto scriptIt = m_scripts.constFind(step.stepId);
        if (scriptIt == m_scripts.cend() || scriptIt->isEmpty()) {
            return successfulResult(step);
        }

        const int nextIndex = m_nextScriptIndex.value(step.stepId);
        const int boundedIndex = qMin(nextIndex, scriptIt->size() - 1);
        m_nextScriptIndex.insert(step.stepId, nextIndex + 1);
        return scriptIt->at(boundedIndex);
    }

    mutable QMutex m_mutex;
    QWaitCondition m_executionStartedCondition;
    Status m_prepareStatus;
    Status m_requestStopStatus;
    Status m_resetStatus;
    Status m_shutdownStatus;
    int m_prepareCalls = 0;
    int m_resetCalls = 0;
    bool m_executionStarted = false;
    bool m_blockExecution = false;
    bool m_allowCompletion = false;
    bool m_ignoreRunControl = false;
    PrepareSnapshot m_prepareSnapshot;
    QVector<StepId> m_executeOrder;
    QVector<RunControl> m_seenControls;
    QVector<int> m_requestStopTimeouts;
    QVector<int> m_shutdownTimeouts;
    QHash<StepId, QVector<Result<TestResult>>> m_scripts;
    QHash<StepId, int> m_nextScriptIndex;
};

} // namespace hwtest::biz::test
