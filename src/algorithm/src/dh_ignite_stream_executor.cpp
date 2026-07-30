#include <algorithm/dh_ignite_stream_executor.h>

#include <algorithm/run_parameter_schema.h>

#include <QDateTime>
#include <QFileInfo>

#include <algorithm>
#include <limits>

namespace hwtest::algorithm::mbddf {
namespace {

using hwtest::biz::ErrorCode;
using hwtest::biz::Result;
using hwtest::biz::Status;
using hwtest::biz::TestResult;
using hwtest::biz::TestStep;
using hwtest::biz::TestVerdict;

constexpr qint64 kMaxJsonSafeInteger = (qint64{1} << 53) - 1;
constexpr quint16 kParamOutOfRange = 0x0102;

Status status(ErrorCode code, const QString& message, const QString& operation)
{
    Status result;
    result.code = code;
    result.error.code = code;
    result.error.message = message;
    result.error.operation = operation;
    return result;
}

QVariantMap nestedMap(const QVariantMap& map, const QString& key)
{
    return map.value(key).toMap();
}

QString profileName(const QVariantMap& protocol,
                    const QString& key,
                    const QString& fallback)
{
    const QString configured = protocol.value(key).toString().trimmed();
    return configured.isEmpty() ? fallback : configured;
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

bool positiveInt(const QVariantMap& map,
                 const QString& key,
                 int fallback,
                 int* output)
{
    if (output == nullptr) return false;
    bool ok = true;
    const int value = map.contains(key) ? map.value(key).toInt(&ok) : fallback;
    if (!ok || value <= 0) return false;
    *output = value;
    return true;
}

bool parseInitialSequence(const QVariant& value, quint16* sequence)
{
    if (sequence == nullptr) return false;
    if (!value.isValid()) {
        *sequence = 0;
        return true;
    }
    bool ok = false;
    uint parsed = 0;
    const QString text = value.toString().trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        parsed = text.mid(2).toUInt(&ok, 16);
    } else {
        parsed = text.toUInt(&ok, 10);
    }
    if (!ok || parsed > std::numeric_limits<quint16>::max()) return false;
    *sequence = static_cast<quint16>(parsed);
    return true;
}

ErrorCode transportErrorCode(const TransportResult& result)
{
    return result.errorCode == TransportResult::Error::Timeout
        ? ErrorCode::BusTimeout
        : ErrorCode::RemoteCommandError;
}

qint64 nowUs()
{
    return QDateTime::currentMSecsSinceEpoch() * 1000;
}

TestResult baseResult(const TestStep& step, qint64 startedAtUs)
{
    TestResult result;
    result.stepId = step.stepId;
    result.testItemId = step.testItemId;
    result.algorithmId = step.algorithmId;
    result.startTimeUs = startedAtUs;
    result.endTimeUs = nowUs();
    result.attempts = 1;
    return result;
}

Result<TestResult> executionFailure(const TestStep& step,
                                    ErrorCode code,
                                    const QString& message,
                                    const QString& operation,
                                    qint64 startedAtUs,
                                    quint32 sampleCount)
{
    TestResult result = baseResult(step, startedAtUs);
    result.verdict = TestVerdict::Error;
    result.errorCode = code;
    result.message = message;
    result.rawData.insert(QStringLiteral("sampleCount"), sampleCount);
    return {status(code, message, operation), result};
}

hwtest::biz::MeasurementRecord measurement(const QString& name,
                                           const QVariant& actual,
                                           const QString& unit = {})
{
    hwtest::biz::MeasurementRecord result;
    result.name = name;
    result.actual = actual;
    result.unit = unit;
    return result;
}

} // namespace

DhIgniteStreamAlgorithmExecutor::DhIgniteStreamAlgorithmExecutor(
    std::unique_ptr<IByteTransport> transport)
    : m_transport(std::move(transport))
{
}

DhIgniteStreamAlgorithmExecutor::~DhIgniteStreamAlgorithmExecutor() = default;

Status DhIgniteStreamAlgorithmExecutor::prepare(
    const hwtest::biz::TestPlan& plan,
    const hwtest::biz::TestContext& context,
    const QVariantMap& executionConfig)
{
    constexpr auto operation = "mbddf.dh_ignite_stream.prepare";
    if (m_transport == nullptr) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("A DH ignition stream byte transport is required"),
                      QString::fromLatin1(operation));
    }
    if (context.tags.value(QStringLiteral("runMode")).toString() !=
        QStringLiteral("device_stream")) {
        return status(ErrorCode::CapabilityUnsupported,
                      QStringLiteral("DH ignition only supports device_stream mode"),
                      QString::fromLatin1(operation));
    }
    if (m_requestAccepted.load()) {
        return status(ErrorCode::ResourceBusy,
                      QStringLiteral("An accepted DH ignition request is still active"),
                      QString::fromLatin1(operation));
    }

    if (m_prepared) {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    m_request = nullptr;
    m_response = nullptr;

    QVariantMap configuredDefaults;
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.dh_ignite_stream"));
    if (schema == nullptr) {
        return status(ErrorCode::InternalError,
                      QStringLiteral("DH ignition run parameter schema is missing"),
                      QString::fromLatin1(operation));
    }
    for (const TestStep& configuredStep : plan.steps) {
        if (configuredStep.algorithmId != QStringLiteral("mbddf.dh_ignite_stream")) {
            continue;
        }
        const QVariantMap requestValues = nestedMap(
            nestedMap(configuredStep.parameters, QStringLiteral("protocol")),
            QStringLiteral("requestValues"));
        for (const RunParameterDescriptor& descriptor : schema->parameters) {
            if (requestValues.contains(descriptor.id)) {
                configuredDefaults.insert(descriptor.id,
                                          requestValues.value(descriptor.id));
            }
        }
        break;
    }
    const Result<QVariantMap> normalized = normalizeRunParameters(
        QStringLiteral("mbddf.dh_ignite_stream"), configuredDefaults,
        context.runParameters);
    if (!normalized.ok()) return normalized.status;
    m_effectiveRunParameters = normalized.value;

    quint32 channelMask = 0;
    for (int channel = 0; channel < 23; ++channel) {
        if (m_effectiveRunParameters
                .value(QStringLiteral("channel_enabled[%1]").arg(channel))
                .toBool()) {
            channelMask |= quint32{1} << static_cast<unsigned>(channel);
        }
    }
    m_reportCount = m_effectiveRunParameters
                        .value(QStringLiteral("report_count"))
                        .toUInt();
    m_intervalUs = m_effectiveRunParameters
                       .value(QStringLiteral("interval_us"))
                       .toUInt();
    m_delayFrames = m_effectiveRunParameters
                        .value(QStringLiteral("delay_frames"))
                        .toUInt();
    m_requestValues = {
        {QStringLiteral("power_enable"),
         m_effectiveRunParameters.value(QStringLiteral("power_enable"))},
        {QStringLiteral("return_enable"),
         m_effectiveRunParameters.value(QStringLiteral("return_enable"))},
        {QStringLiteral("channel[0]"), channelMask},
        {QStringLiteral("channel[1]"), 0u},
        {QStringLiteral("report_count"), m_reportCount},
        {QStringLiteral("interval_us"), m_intervalUs},
        {QStringLiteral("delay_frames"), m_delayFrames},
    };

    const QString assetRoot = effectiveAssetRoot(executionConfig);
    if (assetRoot.isEmpty() || !QFileInfo(assetRoot).isDir()) {
        return status(ErrorCode::ConfigParseError,
                      QStringLiteral("MB_DDF protocolAssetRoot is missing or not a directory"),
                      QString::fromLatin1(operation));
    }
    QString error;
    if (!m_catalog.loadFromDirectory(assetRoot, &error)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("Invalid MB_DDF protocol catalog: %1").arg(error),
                      QString::fromLatin1(operation));
    }
    const QVariantMap protocol = nestedMap(executionConfig,
                                           QStringLiteral("protocol"));
    m_request = m_catalog.findByName(profileName(
        protocol, QStringLiteral("requestProfileId"),
        QStringLiteral("dh_control_request")));
    m_response = m_catalog.findByName(profileName(
        protocol, QStringLiteral("responseProfileId"),
        QStringLiteral("dh_control_response")));
    if (m_request == nullptr || m_response == nullptr ||
        m_request->direction != Direction::Request ||
        m_response->direction != Direction::Response ||
        m_request->typeGroup != m_response->typeGroup ||
        m_request->subType != m_response->subType ||
        m_request->findField(QStringLiteral("delay_frames")) == nullptr) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("DH control profiles are missing, mismatched, or lack delay_frames"),
                      QString::fromLatin1(operation));
    }

    const QVariantMap stream = nestedMap(executionConfig,
                                         QStringLiteral("stream"));
    if (!positiveInt(stream, QStringLiteral("readTimeoutMs"), 2000,
                     &m_readTimeoutMs) ||
        !positiveInt(stream, QStringLiteral("writeTimeoutMs"), 2000,
                     &m_writeTimeoutMs)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("DH stream timeouts must be positive integers"),
                      QString::fromLatin1(operation));
    }
    if (!parseInitialSequence(executionConfig.value(
                                  QStringLiteral("initialSequence")),
                              &m_nextSequence)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("initialSequence must be a 16-bit integer"),
                      QString::fromLatin1(operation));
    }

    QVariantMap transportOptions = nestedMap(executionConfig,
                                              QStringLiteral("transport"));
    const QVariantMap serial = nestedMap(executionConfig,
                                         QStringLiteral("serial"));
    if (!serial.isEmpty()) {
        const int baudRate = serial.value(QStringLiteral("baudRate"), 614400).toInt();
        const int dataBits = serial.value(QStringLiteral("dataBits"), 8).toInt();
        const QString parity = serial.value(QStringLiteral("parity"),
                                             QStringLiteral("Even")).toString();
        const int stopBits = serial.value(QStringLiteral("stopBits"), 1).toInt();
        const QString flowControl = serial.value(QStringLiteral("flowControl"),
                                                  QStringLiteral("None")).toString();
        if (baudRate != 614400 || dataBits != 8 ||
            parity.compare(QStringLiteral("Even"), Qt::CaseInsensitive) != 0 ||
            stopBits != 1 ||
            flowControl.compare(QStringLiteral("None"), Qt::CaseInsensitive) != 0) {
            return status(ErrorCode::ConfigSchemaError,
                          QStringLiteral("MB_DDF control serial settings must be 614400/8E1/no-flow-control"),
                          QString::fromLatin1(operation));
        }
    }
    for (auto it = serial.cbegin(); it != serial.cend(); ++it) {
        if (!transportOptions.contains(it.key())) {
            transportOptions.insert(it.key(), it.value());
        }
    }
    m_transport->setRequestId(context.requestId);
    if (!m_transport->configure(transportOptions, &error)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("Invalid MB_DDF transport settings: %1").arg(error),
                      QString::fromLatin1(operation));
    }
    {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        if (!m_transport->open(&error)) {
            return status(ErrorCode::DriverMissing,
                          QStringLiteral("Unable to open DH ignition transport: %1")
                              .arg(error),
                          QString::fromLatin1(operation));
        }
    }

    m_context = context;
    m_prepared = true;
    return {};
}

TransportResult DhIgniteStreamAlgorithmExecutor::writeFrame(
    const QByteArray& frame, int timeoutMs)
{
    const std::lock_guard<std::mutex> lock(m_transportMutex);
    return m_transport->writeFrame(frame, timeoutMs);
}

TransportResult DhIgniteStreamAlgorithmExecutor::readFrame(int timeoutMs)
{
    const std::lock_guard<std::mutex> lock(m_transportMutex);
    return m_transport->readFrame(timeoutMs);
}

Status DhIgniteStreamAlgorithmExecutor::decodeResponse(
    const QByteArray& frame, QVariantMap* values) const
{
    constexpr auto operation = "mbddf.dh_ignite_stream.decode";
    if (values == nullptr) {
        return status(ErrorCode::InternalError,
                      QStringLiteral("DH response output is null"),
                      QString::fromLatin1(operation));
    }
    QByteArray payload;
    QString error;
    if (!decodeFrame(frame, &payload, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot decode DH response frame: %1").arg(error),
                      QString::fromLatin1(operation));
    }
    if (payload.size() < 3 ||
        static_cast<quint8>(payload.at(1)) != m_response->typeGroup ||
        static_cast<quint8>(payload.at(2)) != m_response->subType ||
        !decodePayload(*m_response, payload, values, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Unexpected or invalid DH control response: %1")
                          .arg(error),
                      QString::fromLatin1(operation));
    }
    return {};
}

Result<TestResult> DhIgniteStreamAlgorithmExecutor::executeStep(
    const TestStep& step,
    const hwtest::biz::IRunControl& control,
    hwtest::biz::IAlgorithmObserver& observer)
{
    constexpr auto operation = "mbddf.dh_ignite_stream.execute";
    const qint64 startedAtUs = nowUs();
    if (!m_prepared || m_transport == nullptr || m_request == nullptr ||
        m_response == nullptr) {
        return executionFailure(step, ErrorCode::NotInitialized,
                                QStringLiteral("DH ignition stream executor is not prepared"),
                                QString::fromLatin1(operation), startedAtUs, 0);
    }
    if (step.algorithmId != QStringLiteral("mbddf.dh_ignite_stream")) {
        return executionFailure(step, ErrorCode::CapabilityUnsupported,
                                QStringLiteral("Unsupported algorithm id '%1'")
                                    .arg(step.algorithmId),
                                QString::fromLatin1(operation), startedAtUs, 0);
    }
    if (control.current() == hwtest::biz::RunControl::Stop ||
        !control.checkpoint()) {
        return executionFailure(step, ErrorCode::Cancelled,
                                QStringLiteral("DH ignition was cancelled before request acceptance"),
                                QString::fromLatin1(operation), startedAtUs, 0);
    }

    QString error;
    QByteArray payload;
    const quint16 requestSequence = m_nextSequence;
    if (!encodePayload(*m_request, m_requestValues, requestSequence,
                       &payload, &error)) {
        return executionFailure(step, ErrorCode::ProtocolParseError,
                                QStringLiteral("Cannot encode DH ignition request: %1")
                                    .arg(error),
                                QString::fromLatin1(operation), startedAtUs, 0);
    }
    QByteArray frame;
    if (!encodeFrame(payload, &frame, &error)) {
        return executionFailure(step, ErrorCode::ProtocolParseError,
                                QStringLiteral("Cannot frame DH ignition request: %1")
                                    .arg(error),
                                QString::fromLatin1(operation), startedAtUs, 0);
    }
    observer.onProgress(step.stepId, step.testItemId, 5,
                        QStringLiteral("request encoded"));
    const TransportResult written = writeFrame(frame, m_writeTimeoutMs);
    if (!written.ok) {
        return executionFailure(step, transportErrorCode(written),
                                QStringLiteral("DH ignition request write failed: %1")
                                    .arg(written.error),
                                QString::fromLatin1(operation), startedAtUs, 0);
    }
    ++m_nextSequence;
    m_requestAccepted.store(true);
    observer.onProgress(step.stepId, step.testItemId, 10,
                        QStringLiteral("request accepted; stream is non-cancellable"));

    quint32 sampleCount = 0;
    bool hadRemoteError = false;
    quint32 remoteErrorCount = 0;
    quint16 firstRemoteError = 0;
    QVariantMap lastValues;
    QByteArray lastFrame;
    qint64 timestampAnchorUtcUs = 0;
    const quint32 expectedFrames = std::max<quint32>(1, m_reportCount);

    const auto finalize = [&](TestVerdict verdict,
                              ErrorCode errorCode,
                              const QString& message) {
        m_requestAccepted.store(false);
        TestResult result = baseResult(step, startedAtUs);
        result.verdict = verdict;
        result.errorCode = errorCode;
        result.message = message;
        result.rawData.insert(QStringLiteral("sampleCount"), sampleCount);
        result.rawData.insert(QStringLiteral("expectedReportCount"), m_reportCount);
        result.rawData.insert(QStringLiteral("hadRemoteError"), hadRemoteError);
        result.rawData.insert(QStringLiteral("effectiveRunParameters"),
                              m_effectiveRunParameters);
        result.rawData.insert(QStringLiteral("effectiveRequestValues"),
                              m_requestValues);
        result.rawData.insert(QStringLiteral("requestFrameHex"),
                              QString::fromLatin1(frame.toHex()));
        result.rawData.insert(QStringLiteral("lastResponseValues"), lastValues);
        result.rawData.insert(QStringLiteral("lastResponseFrameHex"),
                              QString::fromLatin1(lastFrame.toHex()));
        result.measurements.push_back(measurement(
            QStringLiteral("sample_count"), sampleCount,
            QStringLiteral("frame")));
        result.measurements.push_back(measurement(
            QStringLiteral("remote_error_count"), remoteErrorCount,
            QStringLiteral("frame")));
        observer.onProgress(step.stepId, step.testItemId, 100,
                            QStringLiteral("finite stream completed"));
        return Result<TestResult>{{}, result};
    };

    for (quint32 index = 0; index < expectedFrames; ++index) {
        const TransportResult read = readFrame(m_readTimeoutMs);
        if (!read.ok) {
            if (hadRemoteError) {
                return finalize(
                    TestVerdict::Error, ErrorCode::RemoteCommandError,
                    QStringLiteral("DH control reported remote error 0x%1 before the finite stream ended")
                        .arg(firstRemoteError, 4, 16, QLatin1Char('0')));
            }
            m_requestAccepted.store(false);
            return executionFailure(
                step, transportErrorCode(read),
                QStringLiteral("DH ignition response read failed at frame %1: %2")
                    .arg(index)
                    .arg(read.error),
                QString::fromLatin1(operation), startedAtUs, sampleCount);
        }

        QVariantMap values;
        const Status decoded = decodeResponse(read.frame, &values);
        if (!decoded.ok()) {
            m_requestAccepted.store(false);
            return executionFailure(step, decoded.code, decoded.error.message,
                                    decoded.error.operation, startedAtUs,
                                    sampleCount);
        }
        const quint16 actualSequence = static_cast<quint16>(
            values.value(QStringLiteral("seq")).toUInt());
        const quint16 expectedSequence = static_cast<quint16>(
            requestSequence + static_cast<quint16>(index));
        if (actualSequence != expectedSequence) {
            m_requestAccepted.store(false);
            return executionFailure(
                step, ErrorCode::ProtocolParseError,
                QStringLiteral("DH response sequence mismatch at frame %1: expected %2, actual %3")
                    .arg(index)
                    .arg(expectedSequence)
                    .arg(actualSequence),
                QString::fromLatin1(operation), startedAtUs, sampleCount);
        }

        const qint64 elapsedUs = static_cast<qint64>(index) * m_intervalUs;
        if (index == 0) timestampAnchorUtcUs = nowUs();
        if (timestampAnchorUtcUs < 0 ||
            timestampAnchorUtcUs > kMaxJsonSafeInteger - elapsedUs) {
            m_requestAccepted.store(false);
            return executionFailure(step, ErrorCode::InternalError,
                                    QStringLiteral("DH stream timestamp exceeds JSON-safe range"),
                                    QString::fromLatin1(operation), startedAtUs,
                                    sampleCount);
        }
        const quint16 remoteErrorCode = static_cast<quint16>(
            values.value(QStringLiteral("err_code")).toUInt());
        const bool remoteError =
            values.value(QStringLiteral("status")).toInt() != 0 ||
            remoteErrorCode != 0;
        // DUT 的写前业务校验统一返回 PARAM_OUT_OF_RANGE，且只回请求序号
        // 对应的一帧。它由产品端判定，不在 PC 复制业务范围。
        const bool requestRejected =
            index == 0 && remoteErrorCode == kParamOutOfRange;
        values.insert(QStringLiteral("frame_index"), index);
        values.insert(QStringLiteral("ignition_phase"),
                      requestRejected
                          ? QStringLiteral("request_rejected")
                          : index < m_delayFrames
                              ? QStringLiteral("baseline")
                              : QStringLiteral("post_ignition"));

        hwtest::biz::RawSample sample;
        sample.timestampUs = timestampAnchorUtcUs + elapsedUs;
        sample.streamElapsedUs = elapsedUs;
        sample.channelId = QStringLiteral("DH_IGNITE_STREAM");
        sample.values = values;
        sample.tags.insert(QStringLiteral("responseFrameHex"),
                           QString::fromLatin1(read.frame.toHex()));
        sample.tags.insert(QStringLiteral("effectiveRunParameters"),
                           m_effectiveRunParameters);
        observer.onSample(step.stepId, sample);

        ++sampleCount;
        lastValues = values;
        lastFrame = read.frame;
        if (remoteError) {
            hadRemoteError = true;
            ++remoteErrorCount;
            if (firstRemoteError == 0) {
                firstRemoteError = remoteErrorCode;
            }
            if (requestRejected) {
                return finalize(
                    TestVerdict::Error, ErrorCode::RemoteCommandError,
                    QStringLiteral("DUT rejected the DH ignition request before hardware write: 0x%1")
                        .arg(remoteErrorCode, 4, 16, QLatin1Char('0')));
            }
            if (index < m_delayFrames) {
                return finalize(
                    TestVerdict::Error, ErrorCode::RemoteCommandError,
                    QStringLiteral("DH baseline frame %1 reported remote error 0x%2")
                        .arg(index)
                        .arg(values.value(QStringLiteral("err_code")).toUInt(),
                             4, 16, QLatin1Char('0')));
            }
        }
        const int progress = 10 + static_cast<int>(
            (static_cast<quint64>(index + 1) * 89u) / expectedFrames);
        observer.onProgress(step.stepId, step.testItemId, progress,
                            QStringLiteral("receiving finite stream"));
    }

    if (m_reportCount == 0) {
        return finalize(TestVerdict::Error, ErrorCode::RemoteCommandError,
                        QStringLiteral("DUT unexpectedly accepted report_count=0"));
    }
    if (hadRemoteError) {
        return finalize(
            TestVerdict::Error, ErrorCode::RemoteCommandError,
            QStringLiteral("DH stream completed with remote error 0x%1")
                .arg(firstRemoteError, 4, 16, QLatin1Char('0')));
    }
    return finalize(TestVerdict::Pass, ErrorCode::Ok,
                    QStringLiteral("DH stream received %1 frame(s)")
                        .arg(sampleCount));
}

Status DhIgniteStreamAlgorithmExecutor::requestStop(int timeoutMs)
{
    if (timeoutMs < 0) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("Stop timeout must not be negative"),
                      QStringLiteral("mbddf.dh_ignite_stream.requestStop"));
    }
    if (!m_requestAccepted.load()) return {};
    return status(ErrorCode::CapabilityUnsupported,
                  QStringLiteral("An accepted DH ignition stream cannot be stopped"),
                  QStringLiteral("mbddf.dh_ignite_stream.requestStop"));
}

Status DhIgniteStreamAlgorithmExecutor::reset()
{
    if (m_requestAccepted.load()) {
        return status(ErrorCode::CapabilityUnsupported,
                      QStringLiteral("An accepted DH ignition stream cannot be reset"),
                      QStringLiteral("mbddf.dh_ignite_stream.reset"));
    }
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    m_request = nullptr;
    m_response = nullptr;
    return {};
}

Status DhIgniteStreamAlgorithmExecutor::shutdown(int timeoutMs)
{
    if (timeoutMs < 0) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("Shutdown timeout must not be negative"),
                      QStringLiteral("mbddf.dh_ignite_stream.shutdown"));
    }
    if (m_requestAccepted.load()) {
        return status(ErrorCode::CapabilityUnsupported,
                      QStringLiteral("An accepted DH ignition stream cannot be shut down"),
                      QStringLiteral("mbddf.dh_ignite_stream.shutdown"));
    }
    return reset();
}

Status DhIgniteStreamAlgorithmExecutor::finishRun()
{
    if (m_requestAccepted.load()) {
        return status(ErrorCode::CapabilityUnsupported,
                      QStringLiteral("DH ignition stream is still active"),
                      QStringLiteral("mbddf.dh_ignite_stream.finishRun"));
    }
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    return {};
}

const ProtocolCatalog& DhIgniteStreamAlgorithmExecutor::catalog() const noexcept
{
    return m_catalog;
}

} // namespace hwtest::algorithm::mbddf
