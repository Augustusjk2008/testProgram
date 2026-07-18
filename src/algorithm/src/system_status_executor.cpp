#include <algorithm/system_status_executor.h>

#include <logging/log_types.h>

#include <QDateTime>
#include <QFileInfo>

#include <cmath>

namespace hwtest::algorithm::mbddf {

namespace {

using hwtest::biz::CmpOp;
using hwtest::biz::ErrorCode;
using hwtest::biz::Result;
using hwtest::biz::Status;
using hwtest::biz::TestResult;
using hwtest::biz::TestStep;
using hwtest::biz::TestVerdict;

Status makeStatus(ErrorCode code, const QString& message, const QString& operation = {})
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    status.error.operation = operation;
    return status;
}

QString mapString(const QVariantMap& map, const QString& key, const QString& fallback = {})
{
    const QVariant value = map.value(key);
    return value.isValid() ? value.toString() : fallback;
}

QVariantMap nestedMap(const QVariantMap& map, const QString& key)
{
    return map.value(key).toMap();
}

bool finiteNumber(const QVariant& value, double* number)
{
    bool ok = false;
    const double converted = value.toDouble(&ok);
    if (!ok || !std::isfinite(converted)) {
        return false;
    }
    if (number != nullptr) {
        *number = converted;
    }
    return true;
}

QString effectiveAssetRoot(const QVariantMap& executionConfig)
{
    QString root = executionConfig.value(QStringLiteral("protocolAssetRoot")).toString();
    if (root.isEmpty()) {
        root = nestedMap(executionConfig, QStringLiteral("protocolBundle"))
                   .value(QStringLiteral("assetRoot"))
                   .toString();
    }
    if (root.startsWith(QStringLiteral("${")) && root.endsWith(QLatin1Char('}'))) {
        const QString variable = root.mid(2, root.size() - 3);
        root = qEnvironmentVariable(variable.toUtf8().constData());
    }
    if (root.isEmpty()) {
        root = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    }
    return root;
}

} // namespace

SystemStatusAlgorithmExecutor::SystemStatusAlgorithmExecutor(
    std::unique_ptr<IByteTransport> transport)
    : m_transport(std::move(transport))
{
}

SystemStatusAlgorithmExecutor::~SystemStatusAlgorithmExecutor()
{
    shutdown(2000);
}

Status SystemStatusAlgorithmExecutor::makeStatus(ErrorCode code,
                                                 const QString& message,
                                                 const QString& operation) const
{
    return ::hwtest::algorithm::mbddf::makeStatus(code, message, operation);
}

Status SystemStatusAlgorithmExecutor::prepare(const hwtest::biz::TestPlan& plan,
                                              const hwtest::biz::TestContext& context,
                                              const QVariantMap& executionConfig)
{
    if (m_transport == nullptr) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("A byte transport is required"),
                          QStringLiteral("mbddf.prepare"));
    }

    // BIZ may reuse one executor for multiple runs. Tear down the previous
    // session before applying a new catalog or transport configuration.
    if (m_prepared) {
        const std::lock_guard<std::mutex> locker(m_transportMutex);
        m_transport->close();
        m_prepared = false;
    }
    m_request = nullptr;
    m_response = nullptr;

    const QString assetRoot = effectiveAssetRoot(executionConfig);
    if (assetRoot.isEmpty() || !QFileInfo(assetRoot).isDir()) {
        return makeStatus(ErrorCode::ConfigParseError,
                          QStringLiteral("MB_DDF protocolAssetRoot is missing or not a directory"),
                          QStringLiteral("mbddf.prepare"));
    }

    QString error;
    if (!m_catalog.loadFromDirectory(assetRoot, &error)) {
        return makeStatus(ErrorCode::ConfigSchemaError,
                          QStringLiteral("Invalid MB_DDF protocol catalog: %1").arg(error),
                          QStringLiteral("mbddf.prepare"));
    }

    const QVariantMap protocol = nestedMap(executionConfig, QStringLiteral("protocol"));
    const QString requestName = mapString(protocol,
                                           QStringLiteral("requestProfileId"),
                                           QStringLiteral("system_status_request"));
    const QString responseName = mapString(protocol,
                                            QStringLiteral("responseProfileId"),
                                            QStringLiteral("system_status_response"));
    m_request = m_catalog.findByName(requestName);
    m_response = m_catalog.findByName(responseName);
    if (m_request == nullptr || m_response == nullptr ||
        m_request->direction != Direction::Request ||
        m_response->direction != Direction::Response) {
        return makeStatus(ErrorCode::ConfigSchemaError,
                          QStringLiteral("SYSTEM_STATUS request/response profiles are missing or have wrong direction"),
                          QStringLiteral("mbddf.prepare"));
    }

    const QVariantMap serial = nestedMap(executionConfig, QStringLiteral("serial"));
    if (!serial.isEmpty()) {
        const int baudRate = serial.value(QStringLiteral("baudRate"), 614400).toInt();
        const int dataBits = serial.value(QStringLiteral("dataBits"), 8).toInt();
        const QString parity = serial.value(QStringLiteral("parity"), QStringLiteral("Even"))
                                   .toString();
        const int stopBits = serial.value(QStringLiteral("stopBits"), 1).toInt();
        const QString flowControl = serial.value(QStringLiteral("flowControl"), QStringLiteral("None"))
                                        .toString();
        if (baudRate != 614400 || dataBits != 8 || parity.compare(QStringLiteral("Even"), Qt::CaseInsensitive) != 0 ||
            stopBits != 1 || flowControl.compare(QStringLiteral("None"), Qt::CaseInsensitive) != 0) {
            return makeStatus(ErrorCode::ConfigSchemaError,
                              QStringLiteral("MB_DDF serial settings must be 614400/8E1/no-flow-control"),
                              QStringLiteral("mbddf.prepare"));
        }

        QString transportConfigError;
        if (!m_transport->configure(serial, &transportConfigError)) {
            return makeStatus(ErrorCode::ConfigSchemaError,
                              QStringLiteral("Invalid MB_DDF transport settings: %1")
                                  .arg(transportConfigError),
                              QStringLiteral("mbddf.prepare"));
        }
    }

    const QString initialSequenceText = executionConfig.value(QStringLiteral("initialSequence")).toString();
    bool sequenceOk = false;
    quint16 initialSequence = 0;
    if (!initialSequenceText.isEmpty()) {
        const uint parsed = initialSequenceText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
            ? initialSequenceText.toUInt(&sequenceOk, 16)
            : initialSequenceText.toUInt(&sequenceOk, 10);
        if (!sequenceOk || parsed > 0xFFFFu) {
            return makeStatus(ErrorCode::ConfigSchemaError,
                              QStringLiteral("initialSequence must be a 16-bit integer"),
                              QStringLiteral("mbddf.prepare"));
        }
        initialSequence = static_cast<quint16>(parsed);
    } else {
        const QVariant sequenceValue = executionConfig.value(QStringLiteral("initialSequence"));
        if (sequenceValue.isValid()) {
            const int parsed = sequenceValue.toInt(&sequenceOk);
            if (!sequenceOk || parsed < 0 || parsed > 0xFFFF) {
                return makeStatus(ErrorCode::ConfigSchemaError,
                                  QStringLiteral("initialSequence must be a 16-bit integer"),
                                  QStringLiteral("mbddf.prepare"));
            }
            initialSequence = static_cast<quint16>(parsed);
        }
    }

    QString transportError;
    if (!m_transport->open(&transportError)) {
        return makeStatus(ErrorCode::DriverMissing,
                          QStringLiteral("Unable to open MB_DDF byte transport: %1").arg(transportError),
                          QStringLiteral("mbddf.prepare"));
    }

    m_plan = plan;
    m_context = context;
    m_nextSequence = initialSequence;
    m_stopRequested.store(false);
    m_prepared = true;
    return Status{};
}

Result<TestResult> SystemStatusAlgorithmExecutor::failure(const TestStep& step,
                                                           ErrorCode code,
                                                           const QString& message) const
{
    TestResult result;
    result.stepId = step.stepId;
    result.testItemId = step.testItemId;
    result.algorithmId = step.algorithmId;
    result.verdict = TestVerdict::Error;
    result.errorCode = code;
    result.message = message;
    return Result<TestResult>{makeStatus(code, message, QStringLiteral("mbddf.executeStep")), result};
}

Result<TestResult> SystemStatusAlgorithmExecutor::protocolFailure(const TestStep& step,
                                                                   const QString& message) const
{
    return failure(step, ErrorCode::ProtocolParseError, message);
}

Result<TestResult> SystemStatusAlgorithmExecutor::executeStep(
    const TestStep& step,
    const hwtest::biz::IRunControl& control,
    hwtest::biz::IAlgorithmObserver& observer)
{
    if (!m_prepared || m_transport == nullptr || m_request == nullptr || m_response == nullptr) {
        return failure(step, ErrorCode::NotInitialized,
                       QStringLiteral("MB_DDF executor is not prepared"));
    }
    if (m_stopRequested.load() || control.current() == hwtest::biz::RunControl::Stop ||
        !control.checkpoint()) {
        return failure(step, ErrorCode::Cancelled, QStringLiteral("SYSTEM_STATUS was cancelled"));
    }
    if (step.algorithmId != QStringLiteral("mbddf.system_status")) {
        return failure(step, ErrorCode::CapabilityUnsupported,
                       QStringLiteral("Unsupported algorithm id '%1'").arg(step.algorithmId));
    }

    QVariantMap requestValues;
    const QVariantMap protocol = nestedMap(step.parameters, QStringLiteral("protocol"));
    const QVariantMap configuredValues = nestedMap(protocol, QStringLiteral("requestValues"));
    for (auto iterator = configuredValues.cbegin(); iterator != configuredValues.cend(); ++iterator) {
        requestValues.insert(iterator.key(), iterator.value());
    }

    const quint16 sequence = m_nextSequence++;
    QString error;
    QByteArray payload;
    if (!encodePayload(*m_request, requestValues, sequence, &payload, &error)) {
        return protocolFailure(step, QStringLiteral("Cannot encode SYSTEM_STATUS request: %1").arg(error));
    }
    QByteArray frame;
    if (!encodeFrame(payload, &frame, &error)) {
        return protocolFailure(step, QStringLiteral("Cannot frame SYSTEM_STATUS request: %1").arg(error));
    }

    observer.onProgress(step.stepId, step.testItemId, 25, QStringLiteral("request encoded"));
    TransportResult transportResult;
    {
        std::lock_guard<std::mutex> locker(m_transportMutex);
        if (m_stopRequested.load()) {
            return failure(step, ErrorCode::Cancelled, QStringLiteral("SYSTEM_STATUS was cancelled"));
        }
        transportResult = m_transport->transact(frame, qMax(1, step.timeoutMs));
    }
    if (!transportResult.ok) {
        const ErrorCode code = transportResult.errorCode == TransportResult::Error::Timeout ||
                transportResult.error.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
            ? ErrorCode::BusTimeout
            : ErrorCode::RemoteCommandError;
        return failure(step, code,
                       QStringLiteral("SYSTEM_STATUS transport failed: %1").arg(transportResult.error));
    }
    if (m_stopRequested.load() || control.current() == hwtest::biz::RunControl::Stop) {
        return failure(step, ErrorCode::Cancelled, QStringLiteral("SYSTEM_STATUS was cancelled"));
    }

    QByteArray responsePayload;
    if (!decodeFrame(transportResult.frame, &responsePayload, &error)) {
        return protocolFailure(step, QStringLiteral("Cannot decode SYSTEM_STATUS frame: %1").arg(error));
    }
    if (responsePayload.size() < 3) {
        return protocolFailure(step, QStringLiteral("SYSTEM_STATUS response is shorter than command header"));
    }
    const MessageDefinition* responseDefinition =
        m_catalog.findByCommand(static_cast<quint8>(responsePayload.at(1)),
                                static_cast<quint8>(responsePayload.at(2)),
                                Direction::Response);
    if (responseDefinition == nullptr || responseDefinition != m_response) {
        return protocolFailure(step, QStringLiteral("Unexpected SYSTEM_STATUS response command"));
    }
    QVariantMap values;
    if (!decodePayload(*responseDefinition, responsePayload, &values, &error)) {
        return protocolFailure(step, QStringLiteral("Cannot decode SYSTEM_STATUS payload: %1").arg(error));
    }
    if (values.value(QStringLiteral("seq")).toUInt() != sequence) {
        return protocolFailure(step, QStringLiteral("SYSTEM_STATUS response sequence does not echo request"));
    }

    TestResult result;
    result.stepId = step.stepId;
    result.testItemId = step.testItemId;
    result.algorithmId = step.algorithmId;
    result.verdict = TestVerdict::Pass;
    result.errorCode = ErrorCode::Ok;
    result.attempts = 1;
    result.startTimeUs = static_cast<qint64>(QDateTime::currentMSecsSinceEpoch()) * 1000;
    result.endTimeUs = result.startTimeUs;
    result.rawData.insert(QStringLiteral("requestFrameHex"), QString::fromLatin1(frame.toHex()));
    result.rawData.insert(QStringLiteral("responseFrameHex"), QString::fromLatin1(transportResult.frame.toHex()));
    result.rawData.insert(QStringLiteral("responseValues"), values);
    for (auto iterator = values.cbegin(); iterator != values.cend(); ++iterator) {
        if (iterator.key() == QStringLiteral("sync[0]") || iterator.key() == QStringLiteral("sync[1]") ||
            iterator.key() == QStringLiteral("len") || iterator.key() == QStringLiteral("version") ||
            iterator.key() == QStringLiteral("type_group") || iterator.key() == QStringLiteral("sub_type") ||
            iterator.key() == QStringLiteral("seq")) {
            continue;
        }
        result.measurements.append(measurement(iterator.key(), iterator.value()));
    }

    QString criterionError;
    if (!evaluateCriteria(step, values, &criterionError)) {
        result.verdict = TestVerdict::Fail;
        result.errorCode = ErrorCode::SampleFail;
        result.message = criterionError;
    }
    if (values.value(QStringLiteral("status")).toInt() != 0 ||
        values.value(QStringLiteral("err_code")).toInt() != 0) {
        result.verdict = TestVerdict::Error;
        result.errorCode = ErrorCode::RemoteCommandError;
        result.message = QStringLiteral("SYSTEM_STATUS reported a remote error");
    }

    hwtest::biz::RawSample sample;
    sample.timestampUs = result.endTimeUs;
    sample.channelId = QStringLiteral("SYSTEM_STATUS");
    sample.values = values;
    sample.tags.insert(QStringLiteral("requestFrameHex"), result.rawData.value(QStringLiteral("requestFrameHex")));
    observer.onSample(step.stepId, sample);
    observer.onProgress(step.stepId, step.testItemId, 100, QStringLiteral("response decoded"));

    hwtest::logging::LogEvent event;
    event.timestampUs = result.endTimeUs;
    event.level = result.verdict == TestVerdict::Pass ? QStringLiteral("INFO") : QStringLiteral("ERROR");
    event.source = QStringLiteral("algorithm");
    event.category = QStringLiteral("mbddf.system_status");
    event.message = result.message.isEmpty() ? QStringLiteral("SYSTEM_STATUS completed") : result.message;
    event.requestId = m_context.requestId;
    event.context.insert(QStringLiteral("requestFrameHex"), result.rawData.value(QStringLiteral("requestFrameHex")));
    event.context.insert(QStringLiteral("responseFrameHex"), result.rawData.value(QStringLiteral("responseFrameHex")));
    observer.onLog(event);

    return Result<TestResult>{Status{}, result};
}

Status SystemStatusAlgorithmExecutor::requestStop(int timeoutMs)
{
    if (timeoutMs < 0) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("Stop timeout must not be negative"),
                          QStringLiteral("mbddf.requestStop"));
    }
    m_stopRequested.store(true);
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> locker(m_transportMutex);
        m_transport->close();
    }
    return Status{};
}

Status SystemStatusAlgorithmExecutor::reset()
{
    m_stopRequested.store(false);
    m_prepared = false;
    m_request = nullptr;
    m_response = nullptr;
    m_nextSequence = 0;
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> locker(m_transportMutex);
        m_transport->close();
    }
    return Status{};
}

Status SystemStatusAlgorithmExecutor::shutdown(int timeoutMs)
{
    if (timeoutMs < 0) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("Shutdown timeout must not be negative"),
                          QStringLiteral("mbddf.shutdown"));
    }
    m_stopRequested.store(true);
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> locker(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    return Status{};
}

const ProtocolCatalog& SystemStatusAlgorithmExecutor::catalog() const noexcept
{
    return m_catalog;
}

bool SystemStatusAlgorithmExecutor::evaluateCriteria(const TestStep& step,
                                                     const QVariantMap& values,
                                                     QString* failureMessage) const
{
    for (const hwtest::biz::Criterion& criterion : step.criteria) {
        const auto iterator = values.constFind(criterion.metric);
        if (iterator == values.cend()) {
            if (failureMessage != nullptr) {
                *failureMessage = QStringLiteral("Metric '%1' is not present in SYSTEM_STATUS response")
                                       .arg(criterion.metric);
            }
            return false;
        }
        const bool matched = compare(iterator.value(), criterion);
        if (matched != criterion.passIfMatched) {
            if (failureMessage != nullptr) {
                *failureMessage = QStringLiteral("Criterion '%1' did not match")
                                       .arg(criterion.metric);
            }
            return false;
        }
    }
    return true;
}

bool SystemStatusAlgorithmExecutor::compare(const QVariant& actual,
                                            const hwtest::biz::Criterion& criterion)
{
    double actualNumber = 0.0;
    double referenceNumber = 0.0;
    const bool numeric = finiteNumber(actual, &actualNumber) && finiteNumber(criterion.ref, &referenceNumber);
    const double tolerance = std::abs(criterion.tol);
    if (!numeric) {
        if (criterion.op == CmpOp::Equal) {
            return actual.toString() == criterion.ref.toString();
        }
        if (criterion.op == CmpOp::NotEqual) {
            return actual.toString() != criterion.ref.toString();
        }
        return false;
    }

    switch (criterion.op) {
    case CmpOp::GreaterThan: return actualNumber > referenceNumber + tolerance;
    case CmpOp::GreaterOrEqual: return actualNumber + tolerance >= referenceNumber;
    case CmpOp::LessThan: return actualNumber < referenceNumber - tolerance;
    case CmpOp::LessOrEqual: return actualNumber - tolerance <= referenceNumber;
    case CmpOp::Equal: return std::abs(actualNumber - referenceNumber) <= tolerance;
    case CmpOp::NotEqual: return std::abs(actualNumber - referenceNumber) > tolerance;
    case CmpOp::InRange:
        return actualNumber + tolerance >= criterion.lo && actualNumber - tolerance <= criterion.hi;
    }
    return false;
}

hwtest::biz::MeasurementRecord SystemStatusAlgorithmExecutor::measurement(const QString& name,
                                                                          const QVariant& value)
{
    hwtest::biz::MeasurementRecord record;
    record.name = name;
    record.actual = value;
    return record;
}

} // namespace hwtest::algorithm::mbddf
