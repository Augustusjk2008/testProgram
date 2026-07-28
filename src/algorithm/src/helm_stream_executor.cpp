#include <algorithm/helm_stream_executor.h>

#include <algorithm/run_parameter_schema.h>

#include <logging/log_types.h>

#include <QDateTime>
#include <QElapsedTimer>
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
        root = qEnvironmentVariable(root.mid(2, root.size() - 3).toUtf8().constData());
    }
    if (root.isEmpty()) root = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    return root;
}

bool positiveInt(const QVariantMap& map,
                 const QString& key,
                 int fallback,
                 int* output)
{
    bool ok = true;
    const int value = map.contains(key) ? map.value(key).toInt(&ok) : fallback;
    if (!ok || value <= 0 || output == nullptr) return false;
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
    const QString text = value.toString().trimmed();
    const uint parsed = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
        ? text.mid(2).toUInt(&ok, 16)
        : text.toUInt(&ok, 10);
    if (!ok || parsed > 0xFFFFu) return false;
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
                       const Status& failure,
                       qint64 startedAtUs,
                       quint64 sampleCount)
{
    TestResult result;
    result.stepId = step.stepId;
    result.testItemId = step.testItemId;
    result.algorithmId = step.algorithmId;
    result.verdict = TestVerdict::Error;
    result.errorCode = failure.code;
    result.message = failure.error.message;
    result.startTimeUs = startedAtUs;
    result.endTimeUs = nowUs();
    result.rawData.insert(QStringLiteral("sampleCount"),
                          QVariant::fromValue<qulonglong>(sampleCount));
    return result;
}

quint16 missingBefore(quint16 previous, quint16 current)
{
    const quint16 expected = static_cast<quint16>(previous + 1u);
    return current == expected ? 0u : static_cast<quint16>(current - expected);
}

} // namespace

HelmStreamAlgorithmExecutor::HelmStreamAlgorithmExecutor(
    std::unique_ptr<IByteTransport> transport)
    : m_transport(std::move(transport))
{
}

HelmStreamAlgorithmExecutor::~HelmStreamAlgorithmExecutor()
{
    (void)shutdown(2000);
}

Status HelmStreamAlgorithmExecutor::prepare(const hwtest::biz::TestPlan& plan,
                                            const hwtest::biz::TestContext& context,
                                            const QVariantMap& executionConfig)
{
    Q_UNUSED(plan)
    if (m_transport == nullptr) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("A helm stream byte transport is required"),
                      QStringLiteral("mbddf.helm_stream.prepare"));
    }
    if (context.tags.value(QStringLiteral("runMode")).toString() !=
        QStringLiteral("device_stream")) {
        return status(ErrorCode::CapabilityUnsupported,
                      QStringLiteral("HELM_STREAM only supports device_stream mode"),
                      QStringLiteral("mbddf.helm_stream.prepare"));
    }

    const auto normalized = normalizeRunParameters(
        QStringLiteral("mbddf.helm_stream"), {}, context.runParameters);
    if (!normalized.ok()) return normalized.status;

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
                      QStringLiteral("mbddf.helm_stream.prepare"));
    }
    QString error;
    if (!m_catalog.loadFromDirectory(assetRoot, &error)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("Invalid MB_DDF protocol catalog: %1").arg(error),
                      QStringLiteral("mbddf.helm_stream.prepare"));
    }

    const QVariantMap protocol = nestedMap(executionConfig, QStringLiteral("protocol"));
    m_startRequest = m_catalog.findByName(profileName(
        protocol, QStringLiteral("startRequestProfileId"),
        QStringLiteral("helm_start_request")));
    m_startResponse = m_catalog.findByName(profileName(
        protocol, QStringLiteral("startResponseProfileId"),
        QStringLiteral("helm_start_response")));
    m_feedbackResponse = m_catalog.findByName(profileName(
        protocol, QStringLiteral("feedbackResponseProfileId"),
        QStringLiteral("helm_feedback_response")));
    m_stopRequest = m_catalog.findByName(profileName(
        protocol, QStringLiteral("stopRequestProfileId"),
        QStringLiteral("helm_stop_request")));
    m_stopResponse = m_catalog.findByName(profileName(
        protocol, QStringLiteral("stopResponseProfileId"),
        QStringLiteral("helm_stop_response")));
    if (m_startRequest == nullptr || m_startResponse == nullptr ||
        m_feedbackResponse == nullptr || m_stopRequest == nullptr ||
        m_stopResponse == nullptr ||
        m_startRequest->direction != Direction::Request ||
        m_stopRequest->direction != Direction::Request ||
        m_startResponse->direction != Direction::Response ||
        m_feedbackResponse->direction != Direction::Response ||
        m_stopResponse->direction != Direction::Response) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("Helm stream protocol profiles are missing or have wrong direction"),
                      QStringLiteral("mbddf.helm_stream.prepare"));
    }

    const QVariantMap stream = nestedMap(executionConfig, QStringLiteral("stream"));
    if (!positiveInt(stream, QStringLiteral("readTimeoutMs"), 20, &m_readTimeoutMs) ||
        !positiveInt(stream, QStringLiteral("startTimeoutMs"), 2000, &m_startTimeoutMs) ||
        !positiveInt(stream, QStringLiteral("stopTimeoutMs"), 2000, &m_stopTimeoutMs)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("Helm stream timeouts must be positive integers"),
                      QStringLiteral("mbddf.helm_stream.prepare"));
    }
    if (!parseInitialSequence(executionConfig.value(QStringLiteral("initialSequence")),
                              &m_nextSequence)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("initialSequence must be a 16-bit integer"),
                      QStringLiteral("mbddf.helm_stream.prepare"));
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
                          QStringLiteral("mbddf.helm_stream.prepare"));
        }
    }
    for (auto it = serial.cbegin(); it != serial.cend(); ++it) {
        if (!transportOptions.contains(it.key())) transportOptions.insert(it.key(), it.value());
    }
    m_transport->setRequestId(context.requestId);
    if (!m_transport->configure(transportOptions, &error)) {
        return status(ErrorCode::ConfigSchemaError,
                      QStringLiteral("Invalid MB_DDF transport settings: %1").arg(error),
                      QStringLiteral("mbddf.helm_stream.prepare"));
    }
    {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        if (!m_transport->open(&error)) {
            return status(ErrorCode::DriverMissing,
                          QStringLiteral("Unable to open helm stream transport: %1").arg(error),
                          QStringLiteral("mbddf.helm_stream.prepare"));
        }
    }

    m_context = context;
    m_effectiveParameters = normalized.value;
    m_sampleCount = 0;
    m_lastValues.clear();
    m_lastFeedbackFrame.clear();
    m_hasProductSequence = false;
    m_hasSerialA = false;
    m_hasSerialB = false;
    m_stopRequested.store(false);
    m_prepared = true;
    return {};
}

TransportResult HelmStreamAlgorithmExecutor::writeFrame(const QByteArray& frame,
                                                         int timeoutMs)
{
    const std::lock_guard<std::mutex> lock(m_transportMutex);
    return m_transport->writeFrame(frame, timeoutMs);
}

TransportResult HelmStreamAlgorithmExecutor::readFrame(int timeoutMs)
{
    const std::lock_guard<std::mutex> lock(m_transportMutex);
    return m_transport->readFrame(timeoutMs);
}

Status HelmStreamAlgorithmExecutor::decodeResponse(
    const QByteArray& frame,
    const MessageDefinition** definition,
    QVariantMap* values) const
{
    if (definition == nullptr || values == nullptr) {
        return status(ErrorCode::InternalError,
                      QStringLiteral("Helm response decode outputs are null"),
                      QStringLiteral("mbddf.helm_stream.decode"));
    }
    QByteArray payload;
    QString error;
    if (!decodeFrame(frame, &payload, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot decode helm stream frame: %1").arg(error),
                      QStringLiteral("mbddf.helm_stream.decode"));
    }
    if (payload.size() < 3) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Helm stream response is shorter than command header"),
                      QStringLiteral("mbddf.helm_stream.decode"));
    }
    *definition = m_catalog.findByCommand(
        static_cast<quint8>(payload.at(1)),
        static_cast<quint8>(payload.at(2)), Direction::Response);
    if (*definition == nullptr || !decodePayload(**definition, payload, values, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot decode helm stream payload: %1").arg(error),
                      QStringLiteral("mbddf.helm_stream.decode"));
    }
    return {};
}

Status HelmStreamAlgorithmExecutor::publishFeedback(
    const QByteArray& frame,
    const QVariantMap& values,
    const TestStep& step,
    hwtest::biz::IAlgorithmObserver& observer)
{
    if (values.value(QStringLiteral("status")).toInt() != 0 ||
        values.value(QStringLiteral("err_code")).toUInt() != 0) {
        return status(ErrorCode::RemoteCommandError,
                      QStringLiteral("Helm stream feedback reported remote error 0x%1")
                          .arg(values.value(QStringLiteral("err_code")).toUInt(),
                               4, 16, QLatin1Char('0')),
                      QStringLiteral("mbddf.helm_stream.feedback"));
    }
    const int count = values.value(QStringLiteral("sample_count")).toInt();
    if (count < 1 || count > 5) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Helm feedback sample_count must be in 1..5"),
                      QStringLiteral("mbddf.helm_stream.feedback"));
    }
    const quint64 baseTimestamp =
        (static_cast<quint64>(values.value(
             QStringLiteral("first_timestamp_us_high")).toUInt()) << 32) |
        values.value(QStringLiteral("first_timestamp_us_low")).toUInt();
    if (baseTimestamp > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Helm feedback timestamp exceeds signed 64-bit range"),
                      QStringLiteral("mbddf.helm_stream.feedback"));
    }

    const quint16 productSequence = static_cast<quint16>(
        values.value(QStringLiteral("seq")).toUInt());
    const quint16 missingProductFrames = m_hasProductSequence
        ? missingBefore(m_lastProductSequence, productSequence) : 0;
    const bool productDiscontinuity = m_hasProductSequence && missingProductFrames != 0;
    m_lastProductSequence = productSequence;
    m_hasProductSequence = true;

    for (int index = 0; index < count; ++index) {
        const QString source = QStringLiteral("sample[%1].").arg(index);
        const quint16 serialA = static_cast<quint16>(
            values.value(source + QStringLiteral("serial_a")).toUInt());
        const quint16 serialB = static_cast<quint16>(
            values.value(source + QStringLiteral("serial_b")).toUInt());
        const quint16 missingA = m_hasSerialA ? missingBefore(m_lastSerialA, serialA) : 0;
        const quint16 missingB = m_hasSerialB ? missingBefore(m_lastSerialB, serialB) : 0;
        const quint64 timestamp = baseTimestamp +
            values.value(source + QStringLiteral("delta_us")).toUInt();
        if (timestamp > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
            return status(ErrorCode::ProtocolParseError,
                          QStringLiteral("Helm feedback sample timestamp overflow"),
                          QStringLiteral("mbddf.helm_stream.feedback"));
        }

        hwtest::biz::RawSample sample;
        sample.timestampUs = static_cast<qint64>(timestamp);
        sample.channelId = QStringLiteral("HELM_STREAM");
        sample.values.insert(QStringLiteral("seq"), productSequence);
        sample.values.insert(QStringLiteral("status"),
                             values.value(QStringLiteral("status")));
        sample.values.insert(QStringLiteral("err_code"),
                             values.value(QStringLiteral("err_code")));
        sample.values.insert(QStringLiteral("dds_timestamp_us"),
                             QVariant::fromValue<qulonglong>(timestamp));
        sample.values.insert(QStringLiteral("serial_a"), serialA);
        sample.values.insert(QStringLiteral("serial_b"), serialB);
        sample.values.insert(QStringLiteral("command_sequence"), serialA);
        sample.values.insert(QStringLiteral("version"),
                             values.value(source + QStringLiteral("version")));
        for (int channel = 0; channel < 4; ++channel) {
            sample.values.insert(QStringLiteral("fdb[%1]").arg(channel),
                                 values.value(source +
                                     QStringLiteral("fdb[%1]").arg(channel)));
            sample.values.insert(QStringLiteral("ins[%1]").arg(channel),
                                 values.value(source +
                                     QStringLiteral("ins[%1]").arg(channel)));
        }
        const QStringList selfCheckNames{
            QStringLiteral("self_check"),
            QStringLiteral("self_check_1"),
            QStringLiteral("self_check_2"),
            QStringLiteral("self_check_3"),
            QStringLiteral("self_check_4"),
            QStringLiteral("self_check_combined"),
        };
        uint selfCheckOr = 0;
        for (const QString& name : selfCheckNames) {
            const QVariant value = values.value(source + name);
            sample.values.insert(name, value);
            selfCheckOr |= value.toUInt();
        }
        sample.values.insert(
            QStringLiteral("self_check_reserved"),
            values.value(source + QStringLiteral("self_check_reserved")));
        const uint timeout = values.value(source + QStringLiteral("timeout")).toUInt();
        sample.values.insert(QStringLiteral("timeout"), timeout);
        sample.values.insert(QStringLiteral("self_check_or"), selfCheckOr);
        sample.values.insert(QStringLiteral("self_check_or_timeout"),
                             selfCheckOr != 0 || timeout != 0);
        sample.values.insert(QStringLiteral("product_frame_sequence"),
                             productSequence);
        sample.values.insert(QStringLiteral("product_frame_discontinuity"),
                             productDiscontinuity);
        sample.values.insert(QStringLiteral("missing_product_frames"),
                             missingProductFrames);
        sample.values.insert(QStringLiteral("serial_a_discontinuity"),
                             m_hasSerialA && missingA != 0);
        sample.values.insert(QStringLiteral("missing_serial_a"), missingA);
        sample.values.insert(QStringLiteral("serial_b_discontinuity"),
                             m_hasSerialB && missingB != 0);
        sample.values.insert(QStringLiteral("missing_serial_b"), missingB);
        sample.values.insert(QStringLiteral("batch_sample_index"), index);
        sample.values.insert(QStringLiteral("batch_sample_count"), count);
        for (auto parameter = m_effectiveParameters.cbegin();
             parameter != m_effectiveParameters.cend(); ++parameter) {
            sample.values.insert(QStringLiteral("parameter.%1").arg(parameter.key()),
                                 parameter.value());
        }

        sample.tags.insert(QStringLiteral("productFrameSequence"), productSequence);
        sample.tags.insert(QStringLiteral("productFrameDiscontinuity"),
                           productDiscontinuity);
        sample.tags.insert(QStringLiteral("missingProductFrames"), missingProductFrames);
        sample.tags.insert(QStringLiteral("serialADiscontinuity"),
                           m_hasSerialA && missingA != 0);
        sample.tags.insert(QStringLiteral("missingSerialA"), missingA);
        sample.tags.insert(QStringLiteral("serialBDiscontinuity"),
                           m_hasSerialB && missingB != 0);
        sample.tags.insert(QStringLiteral("missingSerialB"), missingB);
        sample.tags.insert(QStringLiteral("batchSampleIndex"), index);
        sample.tags.insert(QStringLiteral("batchSampleCount"), count);
        sample.tags.insert(QStringLiteral("effectiveRunParameters"),
                           m_effectiveParameters);
        sample.tags.insert(QStringLiteral("responseFrameHex"),
                           QString::fromLatin1(frame.toHex()));

        m_lastSerialA = serialA;
        m_lastSerialB = serialB;
        m_hasSerialA = true;
        m_hasSerialB = true;
        ++m_sampleCount;
        m_lastValues = sample.values;
        observer.onSample(step.stepId, sample);
    }
    m_lastFeedbackFrame = frame;
    return {};
}

Status HelmStreamAlgorithmExecutor::stopStream(
    const TestStep* step,
    hwtest::biz::IAlgorithmObserver* observer)
{
    if (!m_streamMayBeActive || m_stopConfirmed || m_stopAttempted) return {};
    m_stopAttempted = true;
    QString error;
    QByteArray payload;
    const quint16 sequence = m_nextSequence;
    if (!encodePayload(*m_stopRequest, {}, sequence, &payload, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot encode helm stream STOP: %1").arg(error),
                      QStringLiteral("mbddf.helm_stream.stop"));
    }
    QByteArray frame;
    if (!encodeFrame(payload, &frame, &error)) {
        return status(ErrorCode::ProtocolParseError,
                      QStringLiteral("Cannot frame helm stream STOP: %1").arg(error),
                      QStringLiteral("mbddf.helm_stream.stop"));
    }
    const TransportResult written = writeFrame(frame, m_stopTimeoutMs);
    if (!written.ok) {
        return status(transportErrorCode(written),
                      QStringLiteral("Helm stream STOP write failed: %1").arg(written.error),
                      QStringLiteral("mbddf.helm_stream.stop"));
    }
    ++m_nextSequence;

    QElapsedTimer timer;
    timer.start();
    Status feedbackError;
    while (timer.elapsed() < m_stopTimeoutMs) {
        const int remaining = m_stopTimeoutMs - static_cast<int>(timer.elapsed());
        const TransportResult read = readFrame(qMax(1, qMin(m_readTimeoutMs, remaining)));
        if (!read.ok) {
            if (read.errorCode == TransportResult::Error::Timeout) continue;
            return status(transportErrorCode(read),
                          QStringLiteral("Helm stream STOP read failed: %1").arg(read.error),
                          QStringLiteral("mbddf.helm_stream.stop"));
        }
        const MessageDefinition* definition = nullptr;
        QVariantMap values;
        const Status decoded = decodeResponse(read.frame, &definition, &values);
        if (!decoded.ok()) return decoded;
        if (definition == m_feedbackResponse) {
            if (step != nullptr && observer != nullptr) {
                const Status published = publishFeedback(read.frame, values, *step, *observer);
                if (!published.ok() && feedbackError.ok()) feedbackError = published;
            }
            continue;
        }
        if (definition == m_startResponse) continue;
        if (definition != m_stopResponse ||
            values.value(QStringLiteral("seq")).toUInt() != sequence) {
            return status(ErrorCode::ProtocolParseError,
                          QStringLiteral("Unexpected helm stream STOP response"),
                          QStringLiteral("mbddf.helm_stream.stop"));
        }
        if (values.value(QStringLiteral("status")).toInt() != 0 ||
            values.value(QStringLiteral("err_code")).toUInt() != 0) {
            return status(ErrorCode::RemoteCommandError,
                          QStringLiteral("Helm STOP reported a remote error"),
                          QStringLiteral("mbddf.helm_stream.stop"));
        }
        m_stopConfirmed = true;
        m_streamMayBeActive = false;
        return feedbackError;
    }
    return status(ErrorCode::BusTimeout,
                  QStringLiteral("Timed out waiting for helm STOP response"),
                  QStringLiteral("mbddf.helm_stream.stop"));
}

Result<TestResult> HelmStreamAlgorithmExecutor::executeStep(
    const TestStep& step,
    const hwtest::biz::IRunControl& control,
    hwtest::biz::IAlgorithmObserver& observer)
{
    const qint64 startedAtUs = nowUs();
    if (!m_prepared || m_startRequest == nullptr || m_startResponse == nullptr ||
        m_feedbackResponse == nullptr || m_stopRequest == nullptr ||
        m_stopResponse == nullptr) {
        const Status failure = status(ErrorCode::NotInitialized,
                                      QStringLiteral("Helm stream executor is not prepared"),
                                      QStringLiteral("mbddf.helm_stream.execute"));
        return {failure, errorResult(step, failure, startedAtUs, m_sampleCount)};
    }
    if (step.algorithmId != QStringLiteral("mbddf.helm_stream")) {
        const Status failure = status(ErrorCode::CapabilityUnsupported,
                                      QStringLiteral("Unsupported algorithm id '%1'")
                                          .arg(step.algorithmId),
                                      QStringLiteral("mbddf.helm_stream.execute"));
        return {failure, errorResult(step, failure, startedAtUs, m_sampleCount)};
    }
    if (m_stopRequested.load() ||
        control.current() == hwtest::biz::RunControl::Stop ||
        !control.checkpoint()) {
        const Status failure = status(ErrorCode::Cancelled,
                                      QStringLiteral("Helm stream was cancelled before START"),
                                      QStringLiteral("mbddf.helm_stream.execute"));
        return {failure, errorResult(step, failure, startedAtUs, m_sampleCount)};
    }

    m_sampleCount = 0;
    m_lastValues.clear();
    m_lastFeedbackFrame.clear();
    m_hasProductSequence = false;
    m_hasSerialA = false;
    m_hasSerialB = false;
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
            failure, errorResult(step, failure, startedAtUs, m_sampleCount)};
    };

    QString error;
    QByteArray payload;
    const quint16 startSequence = m_nextSequence;
    if (!encodePayload(*m_startRequest, m_effectiveParameters,
                       startSequence, &payload, &error)) {
        return failAfterCleanup(status(
            ErrorCode::ProtocolParseError,
            QStringLiteral("Cannot encode helm stream START: %1").arg(error),
            QStringLiteral("mbddf.helm_stream.start")));
    }
    QByteArray startFrame;
    if (!encodeFrame(payload, &startFrame, &error)) {
        return failAfterCleanup(status(
            ErrorCode::ProtocolParseError,
            QStringLiteral("Cannot frame helm stream START: %1").arg(error),
            QStringLiteral("mbddf.helm_stream.start")));
    }
    const TransportResult startWritten = writeFrame(startFrame, m_startTimeoutMs);
    if (!startWritten.ok) {
        return failAfterCleanup(status(
            transportErrorCode(startWritten),
            QStringLiteral("Helm stream START write failed: %1").arg(startWritten.error),
            QStringLiteral("mbddf.helm_stream.start")));
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
            if (read.errorCode == TransportResult::Error::Timeout) continue;
            return failAfterCleanup(status(
                transportErrorCode(read),
                QStringLiteral("Helm stream START read failed: %1").arg(read.error),
                QStringLiteral("mbddf.helm_stream.start")));
        }
        const MessageDefinition* definition = nullptr;
        QVariantMap values;
        const Status decoded = decodeResponse(read.frame, &definition, &values);
        if (!decoded.ok()) return failAfterCleanup(decoded);
        if (definition != m_startResponse ||
            values.value(QStringLiteral("seq")).toUInt() != startSequence) {
            return failAfterCleanup(status(
                ErrorCode::ProtocolParseError,
                QStringLiteral("Unexpected helm stream START response '%1' seq=%2 expected=%3")
                    .arg(definition == nullptr ? QStringLiteral("<null>")
                                               : definition->name)
                    .arg(values.value(QStringLiteral("seq")).toUInt())
                    .arg(startSequence),
                QStringLiteral("mbddf.helm_stream.start")));
        }
        if (values.value(QStringLiteral("status")).toInt() != 0 ||
            values.value(QStringLiteral("err_code")).toUInt() != 0) {
            return failAfterCleanup(status(
                ErrorCode::RemoteCommandError,
                QStringLiteral("Helm START reported a remote error"),
                QStringLiteral("mbddf.helm_stream.start")));
        }
        startAcknowledged = true;
        break;
    }
    if (!startAcknowledged &&
        (m_stopRequested.load() ||
         control.current() == hwtest::biz::RunControl::Stop)) {
        const Status stopped = stopStream(&step, &observer);
        if (!stopped.ok()) {
            return {stopped, errorResult(step, stopped, startedAtUs, m_sampleCount)};
        }
    } else if (!startAcknowledged) {
        return failAfterCleanup(status(
            ErrorCode::BusTimeout,
            QStringLiteral("Timed out waiting for helm START response"),
            QStringLiteral("mbddf.helm_stream.start")));
    }
    if (startAcknowledged) {
        observer.onProgress(step.stepId, step.testItemId, 25,
                            QStringLiteral("streaming"));
        hwtest::logging::LogEvent event;
        event.timestampUs = nowUs();
        event.level = QStringLiteral("INFO");
        event.source = QStringLiteral("algorithm");
        event.category = QStringLiteral("mbddf.helm_stream");
        event.message = QStringLiteral("Helm stream START acknowledged");
        event.requestId = m_context.requestId;
        event.context.insert(QStringLiteral("effectiveRunParameters"),
                             m_effectiveParameters);
        observer.onLog(event);
    }

    while (!m_stopRequested.load() &&
           control.current() != hwtest::biz::RunControl::Stop &&
           control.checkpoint()) {
        const TransportResult read = readFrame(m_readTimeoutMs);
        if (!read.ok) {
            if (read.errorCode == TransportResult::Error::Timeout) continue;
            return failAfterCleanup(status(
                transportErrorCode(read),
                QStringLiteral("Helm stream read failed: %1").arg(read.error),
                QStringLiteral("mbddf.helm_stream.read")));
        }
        const MessageDefinition* definition = nullptr;
        QVariantMap values;
        const Status decoded = decodeResponse(read.frame, &definition, &values);
        if (!decoded.ok()) return failAfterCleanup(decoded);
        if (definition != m_feedbackResponse) {
            return failAfterCleanup(status(
                ErrorCode::ProtocolParseError,
                QStringLiteral("Unexpected response during helm streaming"),
                QStringLiteral("mbddf.helm_stream.read")));
        }
        const Status published = publishFeedback(read.frame, values, step, observer);
        if (!published.ok()) return failAfterCleanup(published);
    }

    const Status stopped = stopStream(&step, &observer);
    if (!stopped.ok()) {
        return {stopped, errorResult(step, stopped, startedAtUs, m_sampleCount)};
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
    result.rawData.insert(QStringLiteral("effectiveRunParameters"),
                          m_effectiveParameters);
    result.rawData.insert(QStringLiteral("lastResponseValues"), m_lastValues);
    result.rawData.insert(QStringLiteral("lastFeedbackFrameHex"),
                          QString::fromLatin1(m_lastFeedbackFrame.toHex()));
    if (m_sampleCount == 0) {
        result.verdict = TestVerdict::Fail;
        result.errorCode = ErrorCode::SampleFail;
        result.message = QStringLiteral("Helm stream ended without valid feedback");
    } else {
        result.verdict = TestVerdict::Pass;
        result.errorCode = ErrorCode::Ok;
        result.message = QStringLiteral("Helm stream received %1 valid sample(s)")
                             .arg(m_sampleCount);
    }
    hwtest::biz::MeasurementRecord count;
    count.name = QStringLiteral("sample_count");
    count.actual = QVariant::fromValue<qulonglong>(m_sampleCount);
    count.unit = QStringLiteral("sample");
    result.measurements.push_back(count);
    observer.onProgress(step.stepId, step.testItemId, 100,
                        QStringLiteral("STOP acknowledged"));
    return {{}, result};
}

Status HelmStreamAlgorithmExecutor::requestStop(int timeoutMs)
{
    if (timeoutMs < 0) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("Stop timeout must not be negative"),
                      QStringLiteral("mbddf.helm_stream.requestStop"));
    }
    m_stopRequested.store(true);
    return {};
}

Status HelmStreamAlgorithmExecutor::reset()
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
    m_lastValues.clear();
    m_lastFeedbackFrame.clear();
    return {};
}

Status HelmStreamAlgorithmExecutor::shutdown(int timeoutMs)
{
    if (timeoutMs < 0) {
        return status(ErrorCode::ParameterRangeError,
                      QStringLiteral("Shutdown timeout must not be negative"),
                      QStringLiteral("mbddf.helm_stream.shutdown"));
    }
    m_stopRequested.store(true);
    if (m_transport != nullptr) {
        const std::lock_guard<std::mutex> lock(m_transportMutex);
        m_transport->close();
    }
    m_prepared = false;
    m_streamMayBeActive = false;
    return {};
}

Status HelmStreamAlgorithmExecutor::finishRun()
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
    return result;
}

quint64 HelmStreamAlgorithmExecutor::sampleCount() const noexcept
{
    return m_sampleCount;
}

const ProtocolCatalog& HelmStreamAlgorithmExecutor::catalog() const noexcept
{
    return m_catalog;
}

} // namespace hwtest::algorithm::mbddf
