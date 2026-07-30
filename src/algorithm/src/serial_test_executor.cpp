#include <algorithm/serial_test_executor.h>

#include <algorithm/bus_echo_transport.h>
#include <algorithm/mbddf_exchange_executor.h>
#include <algorithm/run_parameter_schema.h>

#include <logging/log_types.h>

#include <QDateTime>
#include <QSet>

#include <utility>

namespace hwtest::algorithm::mbddf {
namespace {

using hwtest::biz::ErrorCode;
using hwtest::biz::IAlgorithmExecutor;
using hwtest::biz::IAlgorithmObserver;
using hwtest::biz::IRunControl;
using hwtest::biz::MeasurementRecord;
using hwtest::biz::RawSample;
using hwtest::biz::Result;
using hwtest::biz::Status;
using hwtest::biz::TestContext;
using hwtest::biz::TestPlan;
using hwtest::biz::TestResult;
using hwtest::biz::TestStep;
using hwtest::biz::TestVerdict;

constexpr int kLoopbackMode = 0;
constexpr int kEchoMode = 1;
constexpr int kEchoRoundTimeoutMs = 5000;
constexpr int kCycleDiagnosticsLimit = 16;

Status makeStatus(ErrorCode code, const QString& message, const QString& operation)
{
    Status result;
    result.code = code;
    result.error.code = code;
    result.error.message = message;
    result.error.operation = operation;
    return result;
}

QVariantMap nestedMap(const QVariantMap& values, const QString& key)
{
    return values.value(key).toMap();
}

class BorrowedByteTransport final : public IByteTransport {
public:
    explicit BorrowedByteTransport(IByteTransport* target)
        : m_target(target)
    {
    }

    void setRequestId(const hwtest::hal::RequestId& requestId) override
    {
        if (m_target != nullptr) m_target->setRequestId(requestId);
    }

    bool configure(const QVariantMap& options, QString* error) override
    {
        if (m_target == nullptr) {
            if (error != nullptr) *error = QStringLiteral("Serial test control transport is unavailable");
            return false;
        }
        return m_target->configure(options, error);
    }

    bool open(QString* error) override
    {
        if (m_target == nullptr) {
            if (error != nullptr) *error = QStringLiteral("Serial test control transport is unavailable");
            return false;
        }
        return m_target->open(error);
    }

    TransportResult transact(const QByteArray& frame, int timeoutMs) override
    {
        return m_target == nullptr
            ? failed()
            : m_target->transact(frame, timeoutMs);
    }

    TransportResult writeFrame(const QByteArray& frame, int timeoutMs) override
    {
        return m_target == nullptr
            ? failed()
            : m_target->writeFrame(frame, timeoutMs);
    }

    TransportResult readFrame(int timeoutMs) override
    {
        return m_target == nullptr
            ? failed()
            : m_target->readFrame(timeoutMs);
    }

    void close() override
    {
        if (m_target != nullptr) m_target->close();
    }

private:
    static TransportResult failed()
    {
        TransportResult result;
        result.errorCode = TransportResult::Error::Io;
        result.error = QStringLiteral("Serial test control transport is unavailable");
        return result;
    }

    IByteTransport* m_target = nullptr;
};

class CycleObserver final : public IAlgorithmObserver {
public:
    CycleObserver(IAlgorithmObserver& target, int cycleIndex, int cycleCount)
        : m_target(target)
        , m_cycleIndex(cycleIndex)
        , m_cycleCount(cycleCount)
    {
    }

    void onProgress(const hwtest::biz::StepId& stepId,
                    const hwtest::biz::TestItemId& testItemId,
                    int progress,
                    const QString& stage) override
    {
        const int bounded = qBound(0, progress, 100);
        const int combined = ((m_cycleIndex - 1) * 100 + bounded) /
            qMax(1, m_cycleCount);
        m_target.onProgress(stepId, testItemId, combined, stage);
    }

    void onSample(const hwtest::biz::StepId& stepId, const RawSample& sample) override
    {
        RawSample forwarded = sample;
        forwarded.cycleIndex = static_cast<quint64>(m_cycleIndex);
        forwarded.tags.insert(QStringLiteral("serialTestCycle"), m_cycleIndex);
        m_target.onSample(stepId, forwarded);
    }

    void onLog(const hwtest::logging::LogEvent& event) override
    {
        hwtest::logging::LogEvent forwarded = event;
        forwarded.category = QStringLiteral("mbddf.serial_test");
        forwarded.context.insert(QStringLiteral("serialTestCycle"), m_cycleIndex);
        m_target.onLog(forwarded);
    }

private:
    IAlgorithmObserver& m_target;
    int m_cycleIndex = 1;
    int m_cycleCount = 1;
};

bool stopped(const std::atomic_bool& requested, const IRunControl& control)
{
    return requested.load() || control.current() == hwtest::biz::RunControl::Stop ||
        !control.checkpoint();
}

QVariantMap configuredDefaults(const TestPlan& plan)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.serial_test"));
    if (schema == nullptr) return {};

    for (const TestStep& step : plan.steps) {
        if (step.algorithmId != QStringLiteral("mbddf.serial_test")) continue;
        const QVariantMap values = nestedMap(
            nestedMap(step.parameters, QStringLiteral("protocol")),
            QStringLiteral("requestValues"));
        QVariantMap defaults;
        for (const RunParameterDescriptor& descriptor : schema->parameters) {
            if (values.contains(descriptor.id)) {
                defaults.insert(descriptor.id, values.value(descriptor.id));
            }
        }
        return defaults;
    }
    return {};
}

const TestStep* configuredSerialStep(const TestPlan& plan)
{
    for (const TestStep& step : plan.steps) {
        if (step.algorithmId == QStringLiteral("mbddf.serial_test")) return &step;
    }
    return nullptr;
}

QList<hwtest::biz::Criterion> perRoundCriteria(
    const QList<hwtest::biz::Criterion>& configured,
    bool echo)
{
    const QSet<QString> supported = echo
        ? QSet<QString>{QStringLiteral("status"), QStringLiteral("err_code"),
                        QStringLiteral("link_id")}
        : QSet<QString>{QStringLiteral("status"), QStringLiteral("err_code"),
                        QStringLiteral("link_id"), QStringLiteral("error_count"),
                        QStringLiteral("total_count"), QStringLiteral("elapsed_ms")};
    QList<hwtest::biz::Criterion> result;
    for (const hwtest::biz::Criterion& criterion : configured) {
        if (supported.contains(criterion.metric)) result.push_back(criterion);
    }
    return result;
}

MeasurementRecord measurement(const QString& name, const QVariant& value)
{
    MeasurementRecord record;
    record.name = name;
    record.actual = value;
    return record;
}

QVariantMap cycleRecord(int index, const TestResult& result)
{
    QVariantMap record;
    record.insert(QStringLiteral("cycle_index"), index);
    record.insert(QStringLiteral("verdict"), hwtest::biz::testVerdictToString(result.verdict));
    record.insert(QStringLiteral("error_code"), hwtest::biz::errorCodeToString(result.errorCode));
    record.insert(QStringLiteral("message"), result.message);
    record.insert(QStringLiteral("rawData"), result.rawData);
    return record;
}

bool appendCycleDiagnostic(QVariantList* cycles, QVariantMap record)
{
    if (cycles == nullptr) return false;
    if (cycles->size() < kCycleDiagnosticsLimit) {
        cycles->push_back(std::move(record));
        return false;
    }
    // Preserve the first diagnostics and keep the final slot current so a
    // large run cannot make the terminal snapshot grow without bound.
    (*cycles)[kCycleDiagnosticsLimit - 1] = std::move(record);
    return true;
}

} // namespace

SerialTestAlgorithmExecutor::SerialTestAlgorithmExecutor(
    std::unique_ptr<IByteTransport> controlTransport,
    hwtest::hal::IControlChannel* auxiliaryControlChannel,
    hwtest::hal::ResourceId controlResourceId)
    : m_controlTransport(std::move(controlTransport))
    , m_auxiliaryControlChannel(auxiliaryControlChannel)
    , m_controlResourceId(std::move(controlResourceId))
{
}

SerialTestAlgorithmExecutor::~SerialTestAlgorithmExecutor()
{
    shutdown(2000);
}

Status SerialTestAlgorithmExecutor::prepare(const TestPlan& plan,
                                            const TestContext& context,
                                            const QVariantMap& executionConfig)
{
    if (m_controlTransport == nullptr) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("A serial test control transport is required"),
                          QStringLiteral("mbddf.serial_test.prepare"));
    }
    if (m_delegate != nullptr) {
        shutdown(2000);
    }

    const Result<QVariantMap> normalized = normalizeRunParameters(
        QStringLiteral("mbddf.serial_test"), configuredDefaults(plan), context.runParameters);
    if (!normalized.ok()) return normalized.status;

    const TestStep* sourceStep = configuredSerialStep(plan);
    if (sourceStep == nullptr) {
        return makeStatus(ErrorCode::ConfigSchemaError,
                          QStringLiteral("Serial test plan does not contain mbddf.serial_test"),
                          QStringLiteral("mbddf.serial_test.prepare"));
    }

    m_testMode = normalized.value.value(QStringLiteral("test_mode")).toInt();
    m_cycleCount = normalized.value.value(QStringLiteral("cycle_count")).toInt();
    const int linkId = normalized.value.value(QStringLiteral("link_id")).toInt();
    const bool echo = m_testMode == kEchoMode;
    if (m_testMode != kLoopbackMode && !echo) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("Serial test mode is not supported"),
                          QStringLiteral("mbddf.serial_test.prepare"));
    }

    const QVariantMap serialTest = nestedMap(executionConfig, QStringLiteral("serialTest"));
    const QVariantMap profile = nestedMap(
        serialTest, echo ? QStringLiteral("echo") : QStringLiteral("loopback"));
    const QString requestProfileId = profile.value(QStringLiteral("requestProfileId")).toString();
    const QString responseProfileId = profile.value(QStringLiteral("responseProfileId")).toString();
    if (requestProfileId.isEmpty() || responseProfileId.isEmpty()) {
        return makeStatus(ErrorCode::ConfigSchemaError,
                          QStringLiteral("Serial test mode protocol profiles are required"),
                          QStringLiteral("mbddf.serial_test.prepare"));
    }

    m_delegateStep = *sourceStep;
    m_delegateStep.algorithmId = echo ? QStringLiteral("mbddf.bus_echo")
                                      : QStringLiteral("mbddf.bus_loop");
    if (echo) m_delegateStep.timeoutMs = kEchoRoundTimeoutMs;
    m_delegateStep.criteria = perRoundCriteria(sourceStep->criteria, echo);
    QVariantMap protocol = nestedMap(m_delegateStep.parameters, QStringLiteral("protocol"));
    QVariantMap requestValues = nestedMap(protocol, QStringLiteral("requestValues"));
    requestValues.remove(QStringLiteral("test_mode"));
    requestValues.remove(QStringLiteral("cycle_count"));
    if (echo) {
        requestValues.remove(QStringLiteral("total_count"));
    } else {
        for (auto iterator = requestValues.begin(); iterator != requestValues.end();) {
            if (iterator.key().startsWith(QStringLiteral("data["))) {
                iterator = requestValues.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    requestValues.insert(QStringLiteral("link_id"), linkId);
    if (!echo) {
        requestValues.insert(QStringLiteral("total_count"), m_cycleCount);
    }
    protocol.insert(QStringLiteral("requestValues"), requestValues);
    m_delegateStep.parameters.insert(QStringLiteral("protocol"), protocol);

    TestPlan delegatePlan = plan;
    delegatePlan.steps.clear();
    delegatePlan.steps.push_back(m_delegateStep);
    TestContext delegateContext = context;
    delegateContext.runParameters = echo
        ? QVariantMap{{QStringLiteral("link_id"), linkId}}
        : QVariantMap{{QStringLiteral("link_id"), linkId},
                      {QStringLiteral("total_count"), m_cycleCount}};
    QVariantMap delegateExecutionConfig = executionConfig;
    delegateExecutionConfig.insert(QStringLiteral("protocol"), profile);

    std::unique_ptr<IByteTransport> delegateTransport;
    if (echo) {
        delegateTransport = std::make_unique<BusEchoTransport>(
            std::make_unique<BorrowedByteTransport>(m_controlTransport.get()),
            m_auxiliaryControlChannel,
            m_controlResourceId);
    } else {
        delegateTransport = std::make_unique<BorrowedByteTransport>(m_controlTransport.get());
    }
    m_delegate = std::make_unique<MbdDfExchangeAlgorithmExecutor>(
        std::move(delegateTransport),
        m_delegateStep.algorithmId,
        requestProfileId,
        responseProfileId,
        echo ? QStringLiteral("BUS_ECHO_TEST") : QStringLiteral("BUS_LOOP_TEST"));

    const Status prepared = m_delegate->prepare(delegatePlan, delegateContext,
                                                 delegateExecutionConfig);
    if (!prepared.ok()) {
        m_delegate.reset();
        return prepared;
    }

    m_stopRequested.store(false);
    m_prepared = true;
    return {};
}

Result<TestResult> SerialTestAlgorithmExecutor::executeStep(
    const TestStep& step,
    const IRunControl& control,
    IAlgorithmObserver& observer)
{
    if (!m_prepared || m_delegate == nullptr) {
        const Status failure = makeStatus(ErrorCode::NotInitialized,
                                          QStringLiteral("Serial test executor is not prepared"),
                                          QStringLiteral("mbddf.serial_test.executeStep"));
        TestResult result;
        result.stepId = step.stepId;
        result.testItemId = step.testItemId;
        result.algorithmId = step.algorithmId;
        result.verdict = TestVerdict::Error;
        result.errorCode = failure.code;
        result.message = failure.error.message;
        return {failure, result};
    }
    if (step.algorithmId != QStringLiteral("mbddf.serial_test")) {
        const Status failure = makeStatus(ErrorCode::CapabilityUnsupported,
                                          QStringLiteral("Unsupported algorithm id '%1'").arg(step.algorithmId),
                                          QStringLiteral("mbddf.serial_test.executeStep"));
        TestResult result;
        result.stepId = step.stepId;
        result.testItemId = step.testItemId;
        result.algorithmId = step.algorithmId;
        result.verdict = TestVerdict::Error;
        result.errorCode = failure.code;
        result.message = failure.error.message;
        return {failure, result};
    }

    const int rounds = m_testMode == kEchoMode ? m_cycleCount : 1;
    QVariantList cycles;
    QVariantMap lastResponseValues;
    QVector<MeasurementRecord> lastMeasurements;
    int completed = 0;
    int failed = 0;
    QString firstFailure;
    bool cycleDiagnosticsTruncated = false;

    const auto aggregate = [&](TestVerdict verdict,
                               ErrorCode errorCode,
                               const QString& message) {
        TestResult result;
        result.stepId = step.stepId;
        result.testItemId = step.testItemId;
        result.algorithmId = QStringLiteral("mbddf.serial_test");
        result.verdict = verdict;
        result.errorCode = errorCode;
        result.message = message;
        result.attempts = 1;
        result.startTimeUs = static_cast<qint64>(QDateTime::currentMSecsSinceEpoch()) * 1000;
        result.endTimeUs = result.startTimeUs;
        result.rawData.insert(QStringLiteral("test_mode"), m_testMode);
        result.rawData.insert(QStringLiteral("link_id"),
                              m_delegateStep.parameters.value(QStringLiteral("protocol")).toMap()
                                  .value(QStringLiteral("requestValues")).toMap()
                                  .value(QStringLiteral("link_id")));
        result.rawData.insert(QStringLiteral("requested_cycle_count"), m_cycleCount);
        result.rawData.insert(QStringLiteral("completed_cycle_count"), completed);
        result.rawData.insert(QStringLiteral("failed_cycle_count"), failed);
        result.rawData.insert(QStringLiteral("cycles"), cycles);
        result.rawData.insert(QStringLiteral("cycle_diagnostics_limit"),
                              kCycleDiagnosticsLimit);
        result.rawData.insert(QStringLiteral("cycle_diagnostics_truncated"),
                              cycleDiagnosticsTruncated);
        QVariantMap summary = lastResponseValues;
        summary.insert(QStringLiteral("test_mode"), m_testMode);
        summary.insert(QStringLiteral("requested_cycle_count"), m_cycleCount);
        summary.insert(QStringLiteral("completed_cycle_count"), completed);
        summary.insert(QStringLiteral("failed_cycle_count"), failed);
        result.rawData.insert(QStringLiteral("responseValues"), summary);
        result.measurements = lastMeasurements;
        result.measurements.push_back(measurement(
            QStringLiteral("requested_cycle_count"), m_cycleCount));
        result.measurements.push_back(measurement(
            QStringLiteral("completed_cycle_count"), completed));
        result.measurements.push_back(measurement(
            QStringLiteral("failed_cycle_count"), failed));
        return result;
    };

    for (int round = 1; round <= rounds; ++round) {
        if (stopped(m_stopRequested, control)) {
            const Status cancelled = makeStatus(ErrorCode::Cancelled,
                                                QStringLiteral("Serial test was cancelled"),
                                                QStringLiteral("mbddf.serial_test.executeStep"));
            return {cancelled, aggregate(TestVerdict::Error, cancelled.code,
                                         cancelled.error.message)};
        }

        CycleObserver cycleObserver(observer, round, rounds);
        const Result<TestResult> perRound = m_delegate->executeStep(
            m_delegateStep, control, cycleObserver);
        cycleDiagnosticsTruncated =
            appendCycleDiagnostic(&cycles, cycleRecord(round, perRound.value)) ||
            cycleDiagnosticsTruncated;
        if (!perRound.value.rawData.isEmpty()) {
            lastResponseValues = perRound.value.rawData.value(
                QStringLiteral("responseValues")).toMap();
            lastMeasurements = perRound.value.measurements;
        }
        if (m_testMode == kLoopbackMode) {
            completed = qMax(0, lastResponseValues
                                    .value(QStringLiteral("total_count"))
                                    .toInt());
            failed = qMax(0, lastResponseValues
                                 .value(QStringLiteral("error_count"))
                                 .toInt());
        } else {
            ++completed;
        }

        if (!perRound.ok()) {
            return {perRound.status,
                    aggregate(TestVerdict::Error, perRound.status.code,
                              perRound.status.error.message)};
        }
        if (perRound.value.verdict == TestVerdict::Error) {
            const Status failure = makeStatus(
                perRound.value.errorCode,
                perRound.value.message.isEmpty()
                    ? QStringLiteral("Serial test round reported an error")
                    : perRound.value.message,
                QStringLiteral("mbddf.serial_test.executeStep"));
            return {failure, aggregate(TestVerdict::Error, failure.code,
                                       failure.error.message)};
        }
        if (perRound.value.verdict == TestVerdict::Fail) {
            if (m_testMode == kEchoMode) ++failed;
            if (firstFailure.isEmpty()) firstFailure = perRound.value.message;
        }
    }

    const TestVerdict verdict = failed == 0 ? TestVerdict::Pass : TestVerdict::Fail;
    const ErrorCode errorCode = failed == 0 ? ErrorCode::Ok : ErrorCode::SampleFail;
    return {{}, aggregate(verdict, errorCode, firstFailure)};
}

Status SerialTestAlgorithmExecutor::requestStop(int timeoutMs)
{
    if (timeoutMs < 0) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("Stop timeout must not be negative"),
                          QStringLiteral("mbddf.serial_test.requestStop"));
    }
    m_stopRequested.store(true);
    return m_delegate == nullptr ? Status{} : m_delegate->requestStop(timeoutMs);
}

Status SerialTestAlgorithmExecutor::reset()
{
    m_stopRequested.store(false);
    m_prepared = false;
    if (m_delegate == nullptr) return {};
    const Status result = m_delegate->reset();
    m_delegate.reset();
    return result;
}

Status SerialTestAlgorithmExecutor::shutdown(int timeoutMs)
{
    if (timeoutMs < 0) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("Shutdown timeout must not be negative"),
                          QStringLiteral("mbddf.serial_test.shutdown"));
    }
    m_stopRequested.store(true);
    m_prepared = false;
    if (m_delegate == nullptr) return {};
    const Status result = m_delegate->shutdown(timeoutMs);
    m_delegate.reset();
    return result;
}

Status SerialTestAlgorithmExecutor::finishRun()
{
    m_prepared = false;
    return m_delegate == nullptr ? Status{} : m_delegate->finishRun();
}

} // namespace hwtest::algorithm::mbddf
