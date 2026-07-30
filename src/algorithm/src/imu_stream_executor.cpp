#include <algorithm/imu_stream_executor.h>

#include <logging/log_types.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace hwtest::algorithm::mbddf {
namespace {

using hwtest::biz::ErrorCode;
using hwtest::biz::Result;
using hwtest::biz::Status;
using hwtest::biz::TestResult;
using hwtest::biz::TestStep;
using hwtest::biz::TestVerdict;

constexpr int kDefaultImuHostTimestampIntervalUs = 2500;
constexpr qint64 kMaxJsonSafeInteger = (qint64{1} << 53) - 1;

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
    bool ok = true;
    const int value = map.contains(key) ? map.value(key).toInt(&ok) : fallback;
    if (!ok || value <= 0 || output == nullptr) {
        return false;
    }
    *output = value;
    return true;
}

bool parseInitialSequence(const QVariant& value, quint16* sequence)
{
    if (sequence == nullptr) {
        return false;
    }
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
    if (!ok || parsed > 0xFFFFu) {
        return false;
    }
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

TestResult errorResult(const TestStep& step,
                       ErrorCode code,
                       const QString& message,
                       qint64 startedAtUs,
                       quint64 sampleCount)
{
    TestResult result;
    result.stepId = step.stepId;
    result.testItemId = step.testItemId;
    result.algorithmId = step.algorithmId;
    result.verdict = TestVerdict::Error;
    result.errorCode = code;
    result.message = message;
    result.startTimeUs = startedAtUs;
    result.endTimeUs = nowUs();
    result.rawData.insert(QStringLiteral("sampleCount"),
                          QVariant::fromValue<qulonglong>(sampleCount));
    return result;
}

} // namespace

ImuStreamAlgorithmExecutor::ImuStreamAlgorithmExecutor(
    std::unique_ptr<IByteTransport> transport)
    : m_transport(std::move(transport))
{
}

ImuStreamAlgorithmExecutor::~ImuStreamAlgorithmExecutor()
{
    (void)shutdown(2000);
}

Status ImuStreamAlgorithmExecutor::prepare(const hwtest::biz::TestPlan& plan,
                                           const hwtest::biz::TestContext& context,
                                           const QVariantMap& executionConfig)
{
    Q_UNUSED(plan)
    if (m_transport == nullptr) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("An IMU stream byte transport is required"),
                      QStringLiteral("mbddf.imu_stream.prepare"));
    }
    if (context.tags.value(QStringLiteral("runMode")).toString() !=
        QStringLiteral("device_stream")) {
        return status(ErrorCode::CapabilityUnsupported,
                      QStringLiteral("IMU_STREAM only supports device_stream mode"),
                      QStringLiteral("mbddf.imu_stream.prepare"));
    }

    if (m_prepared) {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    m_streamMayBeActive = false;
    m_stopConfirmed = false;
    m_stopAttempted = false;
    m_startRequest = nullptr;
    m_startResponse = nullptr;
    m_feedbackResponse = nullptr;
    m_stopRequest = nullptr;
    m_stopResponse = nullptr;

    const QString assetRoot = effectiveAssetRoot(executionConfig);
    if (assetRoot.isEmpty() || !QFileInfo(assetRoot).isDir()) {
        return status(ErrorCode::ConfigParseError,
                      QStringLiteral("MB_DDF protocolAssetRoot is missing or not a directory"),
                      QStringLiteral("mbddf.imu_stream.prepare"));
    }
    QString error;
    if (!m_catalog.loadFromDirectory(assetRoot, &error)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("Invalid MB_DDF protocol catalog: %1").arg(error),
                      QStringLiteral("mbddf.imu_stream.prepare"));
    }

    const QVariantMap protocol = nestedMap(executionConfig, QStringLiteral("protocol"));
    m_startRequest = m_catalog.findByName(profileName(
        protocol, QStringLiteral("startRequestProfileId"),
        QStringLiteral("imu_stream_start_request")));
    m_startResponse = m_catalog.findByName(profileName(
        protocol, QStringLiteral("startResponseProfileId"),
        QStringLiteral("imu_stream_start_response")));
    m_feedbackResponse = m_catalog.findByName(profileName(
        protocol, QStringLiteral("feedbackResponseProfileId"),
        QStringLiteral("imu_stream_feedback_response")));
    m_stopRequest = m_catalog.findByName(profileName(
        protocol, QStringLiteral("stopRequestProfileId"),
        QStringLiteral("imu_stream_stop_request")));
    m_stopResponse = m_catalog.findByName(profileName(
        protocol, QStringLiteral("stopResponseProfileId"),
        QStringLiteral("imu_stream_stop_response")));
    if (m_startRequest == nullptr || m_startResponse == nullptr ||
        m_feedbackResponse == nullptr || m_stopRequest == nullptr ||
        m_stopResponse == nullptr ||
        m_startRequest->direction != Direction::Request ||
        m_stopRequest->direction != Direction::Request ||
        m_startResponse->direction != Direction::Response ||
        m_feedbackResponse->direction != Direction::Response ||
        m_stopResponse->direction != Direction::Response) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("IMU stream protocol profiles are missing or have wrong direction"),
                      QStringLiteral("mbddf.imu_stream.prepare"));
    }

    const QVariantMap stream = nestedMap(executionConfig, QStringLiteral("stream"));
    if (!positiveInt(stream, QStringLiteral("hostTimestampIntervalUs"),
                     kDefaultImuHostTimestampIntervalUs,
                     &m_hostTimestampIntervalUs) ||
        !positiveInt(stream, QStringLiteral("readTimeoutMs"), 20, &m_readTimeoutMs) ||
        !positiveInt(stream, QStringLiteral("startTimeoutMs"), 2000, &m_startTimeoutMs) ||
        !positiveInt(stream, QStringLiteral("stopTimeoutMs"), 2000, &m_stopTimeoutMs)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("IMU stream host timestamp interval and timeouts "
                                     "must be positive integers"),
                      QStringLiteral("mbddf.imu_stream.prepare"));
    }
    if (!parseInitialSequence(executionConfig.value(QStringLiteral("initialSequence")),
                              &m_nextSequence)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("initialSequence must be a 16-bit integer"),
                      QStringLiteral("mbddf.imu_stream.prepare"));
    }

    QVariantMap transportOptions = nestedMap(executionConfig, QStringLiteral("transport"));
    const QVariantMap serial = nestedMap(executionConfig, QStringLiteral("serial"));
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
                          QStringLiteral("mbddf.imu_stream.prepare"));
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
                      QStringLiteral("mbddf.imu_stream.prepare"));
    }
    {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        if (!m_transport->open(&error)) {
            return status(ErrorCode::DriverMissing,
                          QStringLiteral("Unable to open IMU stream transport: %1").arg(error),
                          QStringLiteral("mbddf.imu_stream.prepare"));
        }
    }

    m_context = context;
    m_sampleCount = 0;
    m_timestampAnchorUtcUs = 0;
    m_hasTimestampAnchor = false;
    m_lastValues.clear();
    m_lastFeedbackFrame.clear();
    m_stopRequested.store(false);
    m_prepared = true;
    return {};
}

TransportResult ImuStreamAlgorithmExecutor::writeFrame(const QByteArray& frame,
                                                        int timeoutMs)
{
    const std::lock_guard<std::mutex> lock(m_transportMutex);
    return m_transport->writeFrame(frame, timeoutMs);
}

TransportResult ImuStreamAlgorithmExecutor::readFrame(int timeoutMs)
{
    const std::lock_guard<std::mutex> lock(m_transportMutex);
    return m_transport->readFrame(timeoutMs);
}

Status ImuStreamAlgorithmExecutor::decodeResponse(
    const QByteArray& frame,
    const MessageDefinition** definition,
    QVariantMap* values) const
{
    if (definition == nullptr || values == nullptr) {
        return status(ErrorCode::InternalError,
                      QStringLiteral("IMU response decode outputs are null"),
                      QStringLiteral("mbddf.imu_stream.decode"));
    }
    QByteArray payload;
    QString error;
    if (!decodeFrame(frame, &payload, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot decode IMU stream frame: %1").arg(error),
                      QStringLiteral("mbddf.imu_stream.decode"));
    }
    if (payload.size() < 3) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("IMU stream response is shorter than command header"),
                      QStringLiteral("mbddf.imu_stream.decode"));
    }
    *definition = m_catalog.findByCommand(
        static_cast<quint8>(payload.at(1)),
        static_cast<quint8>(payload.at(2)),
        Direction::Response);
    if (*definition == nullptr || !decodePayload(**definition, payload, values, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot decode IMU stream payload: %1").arg(error),
                      QStringLiteral("mbddf.imu_stream.decode"));
    }
    return {};
}

Status ImuStreamAlgorithmExecutor::publishFeedback(
    const QByteArray& frame,
    const QVariantMap& values,
    const TestStep& step,
    hwtest::biz::IAlgorithmObserver& observer)
{
    if (values.value(QStringLiteral("status")).toInt() != 0 ||
        values.value(QStringLiteral("err_code")).toUInt() != 0) {
        return status(ErrorCode::RemoteCommandError,
                      QStringLiteral("IMU stream feedback reported remote error 0x%1")
                          .arg(values.value(QStringLiteral("err_code")).toUInt(),
                               4, 16, QLatin1Char('0')),
                      QStringLiteral("mbddf.imu_stream.feedback"));
    }
    if (m_sampleCount >
        static_cast<quint64>(kMaxJsonSafeInteger / m_hostTimestampIntervalUs)) {
        return status(ErrorCode::InternalError,
                      QStringLiteral("IMU stream elapsed timestamp overflow"),
                      QStringLiteral("mbddf.imu_stream.feedback"));
    }
    const qint64 streamElapsedUs =
        static_cast<qint64>(m_sampleCount) * m_hostTimestampIntervalUs;
    if (!m_hasTimestampAnchor) {
        m_timestampAnchorUtcUs = nowUs();
        m_hasTimestampAnchor = true;
    }
    if (m_timestampAnchorUtcUs < 0 ||
        m_timestampAnchorUtcUs > kMaxJsonSafeInteger) {
        return status(ErrorCode::InternalError,
                      QStringLiteral("IMU stream UTC timestamp is outside JSON safe integer range"),
                      QStringLiteral("mbddf.imu_stream.feedback"));
    }
    if (m_timestampAnchorUtcUs > kMaxJsonSafeInteger - streamElapsedUs) {
        return status(ErrorCode::InternalError,
                      QStringLiteral("IMU stream UTC timestamp overflow"),
                      QStringLiteral("mbddf.imu_stream.feedback"));
    }
    hwtest::biz::RawSample sample;
    sample.timestampUs = m_timestampAnchorUtcUs + streamElapsedUs;
    sample.channelId = QStringLiteral("IMU_STREAM");
    sample.values = values;
    sample.tags.insert(QStringLiteral("responseFrameHex"),
                       QString::fromLatin1(frame.toHex()));
    sample.streamElapsedUs = streamElapsedUs;
    observer.onSample(step.stepId, sample);
    ++m_sampleCount;
    m_lastValues = values;
    m_lastFeedbackFrame = frame;
    return {};
}

Status ImuStreamAlgorithmExecutor::stopStream(
    const TestStep* step,
    hwtest::biz::IAlgorithmObserver* observer)
{
    if (!m_streamMayBeActive || m_stopConfirmed || m_stopAttempted) {
        return {};
    }
    m_stopAttempted = true;
    QString error;
    QByteArray payload;
    const quint16 sequence = m_nextSequence;
    if (!encodePayload(*m_stopRequest, QVariantMap{}, sequence, &payload, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot encode IMU stream STOP: %1").arg(error),
                      QStringLiteral("mbddf.imu_stream.stop"));
    }
    QByteArray frame;
    if (!encodeFrame(payload, &frame, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot frame IMU stream STOP: %1").arg(error),
                      QStringLiteral("mbddf.imu_stream.stop"));
    }
    const TransportResult written = writeFrame(frame, m_stopTimeoutMs);
    if (!written.ok) {
        return status(transportErrorCode(written),
                      QStringLiteral("IMU stream STOP write failed: %1").arg(written.error),
                      QStringLiteral("mbddf.imu_stream.stop"));
    }
    ++m_nextSequence;

    QElapsedTimer timer;
    timer.start();
    Status feedbackError;
    while (timer.elapsed() < m_stopTimeoutMs) {
        const int remaining = m_stopTimeoutMs - static_cast<int>(timer.elapsed());
        const TransportResult read = readFrame(qMax(1, qMin(m_readTimeoutMs, remaining)));
        if (!read.ok) {
            if (read.errorCode == TransportResult::Error::Timeout) {
                continue;
            }
            return status(transportErrorCode(read),
                          QStringLiteral("IMU stream STOP read failed: %1").arg(read.error),
                          QStringLiteral("mbddf.imu_stream.stop"));
        }
        const MessageDefinition* definition = nullptr;
        QVariantMap values;
        const Status decoded = decodeResponse(read.frame, &definition, &values);
        if (!decoded.ok()) {
            return decoded;
        }
        if (definition == m_feedbackResponse) {
            if (step != nullptr && observer != nullptr) {
                const Status published = publishFeedback(read.frame, values, *step, *observer);
                if (!published.ok() && feedbackError.ok()) {
                    feedbackError = published;
                }
            }
            continue;
        }
        if (definition != m_stopResponse) {
            // A delayed START ACK can be observed only on an error cleanup path.
            if (definition == m_startResponse) {
                continue;
            }
            return status(ErrorCode::ProtocolParseError,
                          QStringLiteral("Unexpected response while waiting for IMU STOP ACK"),
                          QStringLiteral("mbddf.imu_stream.stop"));
        }
        if (values.value(QStringLiteral("seq")).toUInt() != sequence) {
            return status(ErrorCode::ProtocolParseError,
                          QStringLiteral("IMU STOP response sequence does not echo request"),
                          QStringLiteral("mbddf.imu_stream.stop"));
        }
        if (values.value(QStringLiteral("status")).toInt() != 0 ||
            values.value(QStringLiteral("err_code")).toUInt() != 0) {
            return status(ErrorCode::RemoteCommandError,
                          QStringLiteral("IMU STOP reported a remote error"),
                          QStringLiteral("mbddf.imu_stream.stop"));
        }
        m_stopConfirmed = true;
        m_streamMayBeActive = false;
        return feedbackError;
    }
    return status(ErrorCode::BusTimeout,
                  QStringLiteral("Timed out waiting for IMU STOP response"),
                  QStringLiteral("mbddf.imu_stream.stop"));
}

Result<TestResult> ImuStreamAlgorithmExecutor::executeStep(
    const TestStep& step,
    const hwtest::biz::IRunControl& control,
    hwtest::biz::IAlgorithmObserver& observer)
{
    const qint64 startedAtUs = nowUs();
    if (!m_prepared || m_transport == nullptr || m_startRequest == nullptr ||
        m_startResponse == nullptr || m_feedbackResponse == nullptr ||
        m_stopRequest == nullptr || m_stopResponse == nullptr) {
        const Status failure = status(ErrorCode::NotInitialized,
                                      QStringLiteral("IMU stream executor is not prepared"),
                                      QStringLiteral("mbddf.imu_stream.execute"));
        return {failure, errorResult(step, failure.code, failure.error.message,
                                     startedAtUs, m_sampleCount)};
    }
    if (step.algorithmId != QStringLiteral("mbddf.imu_stream")) {
        const Status failure = status(ErrorCode::CapabilityUnsupported,
                                      QStringLiteral("Unsupported algorithm id '%1'")
                                          .arg(step.algorithmId),
                                      QStringLiteral("mbddf.imu_stream.execute"));
        return {failure, errorResult(step, failure.code, failure.error.message,
                                     startedAtUs, m_sampleCount)};
    }
    if (m_stopRequested.load() ||
        control.current() == hwtest::biz::RunControl::Stop ||
        !control.checkpoint()) {
        const Status failure = status(ErrorCode::Cancelled,
                                      QStringLiteral("IMU stream was cancelled before START"),
                                      QStringLiteral("mbddf.imu_stream.execute"));
        return {failure, errorResult(step, failure.code, failure.error.message,
                                     startedAtUs, m_sampleCount)};
    }

    m_sampleCount = 0;
    m_timestampAnchorUtcUs = 0;
    m_hasTimestampAnchor = false;
    m_lastValues.clear();
    m_lastFeedbackFrame.clear();
    m_streamMayBeActive = false;
    m_stopConfirmed = false;
    m_stopAttempted = false;

    const auto failAfterCleanup = [&](Status failure) {
        const Status cleanup = stopStream(&step, &observer);
        if (!cleanup.ok()) {
            failure.error.message += QStringLiteral("; STOP cleanup failed: %1")
                                         .arg(cleanup.error.message);
        }
        return Result<TestResult>{
            failure,
            errorResult(step, failure.code, failure.error.message,
                        startedAtUs, m_sampleCount)};
    };

    QString error;
    QByteArray payload;
    const quint16 startSequence = m_nextSequence;
    if (!encodePayload(*m_startRequest, QVariantMap{}, startSequence, &payload, &error)) {
        return failAfterCleanup(status(
            ErrorCode::ProtocolParseError,
            QStringLiteral("Cannot encode IMU stream START: %1").arg(error),
            QStringLiteral("mbddf.imu_stream.start")));
    }
    QByteArray startFrame;
    if (!encodeFrame(payload, &startFrame, &error)) {
        return failAfterCleanup(status(
            ErrorCode::ProtocolParseError,
            QStringLiteral("Cannot frame IMU stream START: %1").arg(error),
            QStringLiteral("mbddf.imu_stream.start")));
    }
    const TransportResult startWritten = writeFrame(startFrame, m_startTimeoutMs);
    if (!startWritten.ok) {
        return failAfterCleanup(status(
            transportErrorCode(startWritten),
            QStringLiteral("IMU stream START write failed: %1").arg(startWritten.error),
            QStringLiteral("mbddf.imu_stream.start")));
    }
    ++m_nextSequence;
    m_streamMayBeActive = true;
    observer.onProgress(step.stepId, step.testItemId, 10,
                        QStringLiteral("START sent"));

    QElapsedTimer startTimer;
    startTimer.start();
    bool startAcknowledged = false;
    while (startTimer.elapsed() < m_startTimeoutMs &&
           !m_stopRequested.load() &&
           control.current() != hwtest::biz::RunControl::Stop) {
        const int remaining = m_startTimeoutMs - static_cast<int>(startTimer.elapsed());
        const TransportResult read = readFrame(qMax(1, qMin(m_readTimeoutMs, remaining)));
        if (!read.ok) {
            if (read.errorCode == TransportResult::Error::Timeout) {
                continue;
            }
            return failAfterCleanup(status(
                transportErrorCode(read),
                QStringLiteral("IMU stream START read failed: %1").arg(read.error),
                QStringLiteral("mbddf.imu_stream.start")));
        }
        const MessageDefinition* definition = nullptr;
        QVariantMap values;
        const Status decoded = decodeResponse(read.frame, &definition, &values);
        if (!decoded.ok()) {
            return failAfterCleanup(decoded);
        }
        if (definition != m_startResponse ||
            values.value(QStringLiteral("seq")).toUInt() != startSequence) {
            return failAfterCleanup(status(
                ErrorCode::ProtocolParseError,
                QStringLiteral("Unexpected IMU stream START response"),
                QStringLiteral("mbddf.imu_stream.start")));
        }
        if (values.value(QStringLiteral("status")).toInt() != 0 ||
            values.value(QStringLiteral("err_code")).toUInt() != 0) {
            return failAfterCleanup(status(
                ErrorCode::RemoteCommandError,
                QStringLiteral("IMU START reported a remote error"),
                QStringLiteral("mbddf.imu_stream.start")));
        }
        startAcknowledged = true;
        break;
    }
    if (!startAcknowledged &&
        (m_stopRequested.load() ||
         control.current() == hwtest::biz::RunControl::Stop)) {
        const Status stopped = stopStream(&step, &observer);
        if (!stopped.ok()) {
            return {stopped,
                    errorResult(step, stopped.code, stopped.error.message,
                                startedAtUs, m_sampleCount)};
        }
    } else if (!startAcknowledged) {
        return failAfterCleanup(status(
            ErrorCode::BusTimeout,
            QStringLiteral("Timed out waiting for IMU START response"),
            QStringLiteral("mbddf.imu_stream.start")));
    }
    if (startAcknowledged) {
        observer.onProgress(step.stepId, step.testItemId, 25,
                            QStringLiteral("streaming"));
    }

    while (!m_stopRequested.load() &&
           control.current() != hwtest::biz::RunControl::Stop &&
           control.checkpoint()) {
        const TransportResult read = readFrame(m_readTimeoutMs);
        if (!read.ok) {
            if (read.errorCode == TransportResult::Error::Timeout) {
                continue;
            }
            return failAfterCleanup(status(
                transportErrorCode(read),
                QStringLiteral("IMU stream read failed: %1").arg(read.error),
                QStringLiteral("mbddf.imu_stream.read")));
        }
        const MessageDefinition* definition = nullptr;
        QVariantMap values;
        const Status decoded = decodeResponse(read.frame, &definition, &values);
        if (!decoded.ok()) {
            return failAfterCleanup(decoded);
        }
        if (definition != m_feedbackResponse) {
            return failAfterCleanup(status(
                ErrorCode::ProtocolParseError,
                QStringLiteral("Unexpected response during IMU streaming"),
                QStringLiteral("mbddf.imu_stream.read")));
        }
        const Status published = publishFeedback(read.frame, values, step, observer);
        if (!published.ok()) {
            return failAfterCleanup(published);
        }
    }

    const Status stopped = stopStream(&step, &observer);
    if (!stopped.ok()) {
        return {stopped,
                errorResult(step, stopped.code, stopped.error.message,
                            startedAtUs, m_sampleCount)};
    }

    TestResult result;
    result.stepId = step.stepId;
    result.testItemId = step.testItemId;
    result.algorithmId = step.algorithmId;
    result.startTimeUs = startedAtUs;
    result.endTimeUs = nowUs();
    result.attempts = 1;
    result.rawData.insert(QStringLiteral("sampleCount"),
                          QVariant::fromValue<qulonglong>(m_sampleCount));
    result.rawData.insert(QStringLiteral("lastResponseValues"), m_lastValues);
    result.rawData.insert(QStringLiteral("lastFeedbackFrameHex"),
                          QString::fromLatin1(m_lastFeedbackFrame.toHex()));
    if (m_sampleCount == 0) {
        result.verdict = TestVerdict::Fail;
        result.errorCode = ErrorCode::SampleFail;
        result.message = QStringLiteral("IMU stream ended without a valid feedback frame");
    } else {
        result.verdict = TestVerdict::Pass;
        result.errorCode = ErrorCode::Ok;
        result.message = QStringLiteral("IMU stream received %1 valid frame(s)")
                             .arg(m_sampleCount);
    }
    hwtest::biz::MeasurementRecord count;
    count.name = QStringLiteral("sample_count");
    count.actual = QVariant::fromValue<qulonglong>(m_sampleCount);
    count.unit = QStringLiteral("frame");
    result.measurements.push_back(count);
    observer.onProgress(step.stepId, step.testItemId, 100,
                        QStringLiteral("STOP acknowledged"));
    return {{}, result};
}

Status ImuStreamAlgorithmExecutor::requestStop(int timeoutMs)
{
    if (timeoutMs < 0) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("Stop timeout must not be negative"),
                      QStringLiteral("mbddf.imu_stream.requestStop"));
    }
    m_stopRequested.store(true);
    return {};
}

Status ImuStreamAlgorithmExecutor::reset()
{
    m_stopRequested.store(true);
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    m_streamMayBeActive = false;
    m_stopConfirmed = false;
    m_sampleCount = 0;
    m_timestampAnchorUtcUs = 0;
    m_hasTimestampAnchor = false;
    m_lastValues.clear();
    m_lastFeedbackFrame.clear();
    return {};
}

Status ImuStreamAlgorithmExecutor::shutdown(int timeoutMs)
{
    if (timeoutMs < 0) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("Shutdown timeout must not be negative"),
                      QStringLiteral("mbddf.imu_stream.shutdown"));
    }
    m_stopRequested.store(true);
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    m_streamMayBeActive = false;
    m_timestampAnchorUtcUs = 0;
    m_hasTimestampAnchor = false;
    return {};
}

Status ImuStreamAlgorithmExecutor::finishRun()
{
    Status result;
    if (m_streamMayBeActive && m_transport != nullptr) {
        result = stopStream(nullptr, nullptr);
    }
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    m_streamMayBeActive = false;
    m_timestampAnchorUtcUs = 0;
    m_hasTimestampAnchor = false;
    return result;
}

const ProtocolCatalog& ImuStreamAlgorithmExecutor::catalog() const noexcept
{
    return m_catalog;
}

} // namespace hwtest::algorithm::mbddf
