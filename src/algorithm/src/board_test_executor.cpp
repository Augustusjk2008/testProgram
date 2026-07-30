#include <algorithm/board_test_executor.h>

#include <algorithm/mbddf_protocol.h>
#include <algorithm/run_parameter_schema.h>

#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QVariantList>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>

namespace hwtest::algorithm::mbddf {

namespace {

using hwtest::biz::ErrorCode;
using hwtest::biz::Result;
using hwtest::biz::Status;
using hwtest::biz::TestResult;
using hwtest::biz::TestStep;
using hwtest::biz::TestVerdict;

constexpr double kLowLevelMaxVolts = 0.8;
constexpr double kHighLevelMinVolts = 3.8;
constexpr double kDutyTolerancePercentagePoints = 1.0;
constexpr double kFeedbackToleranceVolts = 0.05;
constexpr double kProtocolPositiveFullScaleVolts = 32767.0 * 5.0 / 32768.0;

enum class BoardKind { DoWrite, HelmBoard };

Status failedStatus(ErrorCode code,
                    const QString& message,
                    const QString& operation)
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    status.error.operation = operation;
    return status;
}

QString assetRoot(const QVariantMap& executionConfig)
{
    QString root = executionConfig.value(QStringLiteral("protocolAssetRoot")).toString();
    if (root.startsWith(QStringLiteral("${")) && root.endsWith(QLatin1Char('}'))) {
        const QString variable = root.mid(2, root.size() - 3);
        root = qEnvironmentVariable(variable.toUtf8().constData());
    }
    if (root.isEmpty()) root = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    return root;
}

QVector<QString> stringVector(const QVariantMap& map, const QString& key)
{
    QVector<QString> result;
    for (const QVariant& value : map.value(key).toList()) {
        const QString item = value.toString().trimmed();
        if (!item.isEmpty()) result.push_back(item);
    }
    return result;
}

bool allUnique(const QVector<QString>& values)
{
    QSet<QString> unique;
    for (const QString& value : values) {
        if (value.isEmpty() || unique.contains(value)) return false;
        unique.insert(value);
    }
    return unique.size() == values.size();
}

bool allFinite(const QVector<double>& values)
{
    return std::all_of(values.cbegin(), values.cend(),
                       [](double value) { return std::isfinite(value); });
}

qint64 nowUs()
{
    return QDateTime::currentMSecsSinceEpoch() * 1000;
}

QVariantList doubleList(const QVector<double>& values)
{
    QVariantList result;
    result.reserve(values.size());
    for (double value : values) result.push_back(value);
    return result;
}

double quantile(QVector<double> values, double fraction)
{
    if (values.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    const int lower = static_cast<int>(std::floor(position));
    const int upper = static_cast<int>(std::ceil(position));
    const double weight = position - lower;
    return values.at(lower) * (1.0 - weight) + values.at(upper) * weight;
}

double crossingPosition(double left, double right, double threshold, int index)
{
    const double span = right - left;
    if (std::abs(span) < 1e-12) return static_cast<double>(index);
    return static_cast<double>(index) + (threshold - left) / span;
}

QString joinReasons(const QStringList& reasons)
{
    return reasons.isEmpty() ? QString{} : reasons.join(QStringLiteral("; "));
}

ErrorCode transportErrorCode(TransportResult::Error error)
{
    return error == TransportResult::Error::Timeout
        ? ErrorCode::BusTimeout
        : ErrorCode::SampleFail;
}

struct ExchangeResult {
    bool ok = false;
    ErrorCode errorCode = ErrorCode::Ok;
    QString message;
    QVariantMap values;
    QByteArray requestFrame;
    QByteArray responseFrame;
};

QVariantMap baseBoardResult(const QString& kind,
                            const QString& mode,
                            int totalPoints)
{
    return {
        {QStringLiteral("schema"),
         QStringLiteral("hwtest.mbddf-board-test-result")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("completedPoints"), 0},
        {QStringLiteral("totalPoints"), totalPoints},
        {QStringLiteral("summary"), QVariantMap{}},
        {QStringLiteral("doSteps"), QVariantList{}},
        {QStringLiteral("directionPoints"), QVariantList{}},
        {QStringLiteral("pwmPoints"), QVariantList{}},
        {QStringLiteral("feedbackPoints"), QVariantList{}},
    };
}

TestResult initialResult(const TestStep& step)
{
    TestResult result;
    result.stepId = step.stepId;
    result.testItemId = step.testItemId;
    result.algorithmId = step.algorithmId;
    result.startTimeUs = nowUs();
    result.attempts = 1;
    return result;
}

void finishResult(TestResult* result,
                  TestVerdict verdict,
                  ErrorCode errorCode,
                  const QString& message,
                  QVariantMap board)
{
    if (result == nullptr) return;
    result->verdict = verdict;
    result->errorCode = errorCode;
    result->message = message;
    result->endTimeUs = nowUs();
    result->rawData.insert(QStringLiteral("boardTest"), std::move(board));
}

void publishPoint(const TestStep& step,
                  hwtest::biz::IAlgorithmObserver& observer,
                  const QVariantMap& point,
                  const QString& phase,
                  int completed,
                  int total)
{
    hwtest::biz::RawSample sample;
    sample.timestampUs = nowUs();
    sample.channelId = QStringLiteral("board_test");
    sample.values = point;
    sample.tags.insert(QStringLiteral("phase"), phase);
    observer.onSample(step.stepId, sample);
    observer.onProgress(step.stepId, step.testItemId,
                        total > 0 ? qBound(0, completed * 100 / total, 100) : 100,
                        phase);
}

bool shouldStop(const BoardTestExecutorState& state,
                const hwtest::biz::IRunControl& control);

} // namespace

class BoardTestExecutorState {
public:
    BoardTestExecutorState(BoardKind kindValue,
                           std::unique_ptr<IByteTransport> byteTransport,
                           IBoardTestFixture* boardFixture)
        : kind(kindValue)
        , transport(std::move(byteTransport))
        , fixture(boardFixture)
    {
    }

    BoardKind kind;
    std::unique_ptr<IByteTransport> transport;
    IBoardTestFixture* fixture = nullptr;
    ProtocolCatalog catalog;
    const MessageDefinition* request = nullptr;
    const MessageDefinition* response = nullptr;
    hwtest::biz::TestContext context;
    QVariantMap parameters;
    QVector<QString> doSenseResources;
    QVector<QString> pwmResources;
    QVector<QString> directionResources;
    QVector<QString> feedbackResources;
    int settlingMs = 100;
    double pwmSampleRateHz = 1250000.0;
    int pwmSamplesPerChannel = 7500;
    double directionSampleRateHz = 10000.0;
    int directionSamplesPerChannel = 100;
    quint16 nextSequence = 0;
    std::atomic_bool stopRequested{false};
    mutable std::mutex transportMutex;
    bool prepared = false;

    ExchangeResult exchange(const QVariantMap& requestValues, int timeoutMs)
    {
        ExchangeResult result;
        if (!prepared || request == nullptr || response == nullptr || !transport) {
            result.errorCode = ErrorCode::InvalidState;
            result.message = QStringLiteral("Board test executor is not prepared");
            return result;
        }
        QString error;
        QByteArray payload;
        const quint16 sequence = nextSequence++;
        if (!encodePayload(*request, requestValues, sequence, &payload, &error) ||
            !encodeFrame(payload, &result.requestFrame, &error)) {
            result.errorCode = ErrorCode::ProtocolParseError;
            result.message = QStringLiteral("Unable to encode board test request: %1")
                                 .arg(error);
            return result;
        }
        TransportResult transported;
        {
            const std::lock_guard<std::mutex> lock(transportMutex);
            transported = transport->transact(result.requestFrame, timeoutMs);
        }
        if (!transported.ok) {
            result.errorCode = transportErrorCode(transported.errorCode);
            result.message = transported.error.isEmpty()
                ? QStringLiteral("Board test transport failed")
                : transported.error;
            return result;
        }
        result.responseFrame = transported.frame;
        QByteArray responsePayload;
        if (!decodeFrame(transported.frame, &responsePayload, &error) ||
            responsePayload.size() < 5) {
            result.errorCode = ErrorCode::ProtocolParseError;
            result.message = QStringLiteral("Invalid board test response frame: %1")
                                 .arg(error);
            return result;
        }
        const MessageDefinition* actual = catalog.findByCommand(
            static_cast<quint8>(responsePayload.at(1)),
            static_cast<quint8>(responsePayload.at(2)), Direction::Response);
        if (actual == nullptr || actual->name != response->name ||
            !decodePayload(*actual, responsePayload, &result.values, &error)) {
            result.errorCode = ErrorCode::ProtocolParseError;
            result.message = QStringLiteral("Unexpected board test response: %1")
                                 .arg(error);
            return result;
        }
        if (result.values.value(QStringLiteral("seq")).toUInt() != sequence) {
            result.errorCode = ErrorCode::ProtocolParseError;
            result.message = QStringLiteral("Board test response sequence mismatch");
            return result;
        }
        const uint status = result.values.value(QStringLiteral("status")).toUInt();
        const uint remoteError = result.values.value(QStringLiteral("err_code")).toUInt();
        if (status != 0 || remoteError != 0) {
            result.errorCode = ErrorCode::RemoteCommandError;
            result.message = QStringLiteral("DUT rejected board test command (status=%1, err_code=%2)")
                                 .arg(status).arg(remoteError);
            return result;
        }
        result.ok = true;
        return result;
    }
};

namespace {

bool shouldStop(const BoardTestExecutorState& state,
                const hwtest::biz::IRunControl& control)
{
    return state.stopRequested.load() ||
        control.current() == hwtest::biz::RunControl::Stop ||
        !control.checkpoint();
}

Status prepareState(BoardTestExecutorState* state,
                    const hwtest::biz::TestContext& context,
                    const QVariantMap& executionConfig)
{
    if (state == nullptr || !state->transport) {
        return failedStatus(ErrorCode::ParameterRangeError,
                            QStringLiteral("A byte transport is required"),
                            QStringLiteral("mbddf.board.prepare"));
    }
    if (context.tags.value(QStringLiteral("runMode")).toString() !=
        QStringLiteral("single")) {
        return failedStatus(ErrorCode::CapabilityUnsupported,
                            QStringLiteral("Board tests only support single mode"),
                            QStringLiteral("mbddf.board.prepare"));
    }
    if (state->prepared) {
        const std::lock_guard<std::mutex> lock(state->transportMutex);
        state->transport->close();
        state->prepared = false;
    }
    state->request = nullptr;
    state->response = nullptr;
    state->context = context;
    state->stopRequested.store(false);

    const QString runParameterAlgorithmId = state->kind == BoardKind::DoWrite
        ? QStringLiteral("mbddf.do_write")
        : QStringLiteral("mbddf.helm_board_test");
    const auto normalized = normalizeRunParameters(
        runParameterAlgorithmId, {}, context.runParameters);
    if (!normalized.ok()) return normalized.status;
    state->parameters = normalized.value;

    const QString root = assetRoot(executionConfig);
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        return failedStatus(ErrorCode::ConfigParseError,
                            QStringLiteral("MB_DDF protocolAssetRoot is missing or not a directory"),
                            QStringLiteral("mbddf.board.prepare"));
    }
    QString error;
    if (!state->catalog.loadFromDirectory(root, &error)) {
        return failedStatus(ErrorCode::ConfigSchemaError,
                            QStringLiteral("Invalid MB_DDF protocol catalog: %1").arg(error),
                            QStringLiteral("mbddf.board.prepare"));
    }
    const QVariantMap protocol = executionConfig.value(QStringLiteral("protocol")).toMap();
    const QString expectedRequestName = state->kind == BoardKind::DoWrite
        ? QStringLiteral("do_write_request")
        : QStringLiteral("helm_board_test_request");
    const QString expectedResponseName = state->kind == BoardKind::DoWrite
        ? QStringLiteral("do_write_response")
        : QStringLiteral("helm_board_test_response");
    const QString requestName = protocol
        .value(QStringLiteral("requestProfileId"),
               expectedRequestName)
        .toString();
    const QString responseName = protocol
        .value(QStringLiteral("responseProfileId"),
               expectedResponseName)
        .toString();
    if (requestName != expectedRequestName ||
        responseName != expectedResponseName) {
        return failedStatus(ErrorCode::ConfigSchemaError,
                            QStringLiteral("Board test protocol profiles are fixed for the algorithm"),
                            QStringLiteral("mbddf.board.prepare"));
    }
    state->request = state->catalog.findByName(requestName);
    state->response = state->catalog.findByName(responseName);
    if (state->request == nullptr || state->response == nullptr ||
        state->request->direction != Direction::Request ||
        state->response->direction != Direction::Response) {
        return failedStatus(ErrorCode::ConfigSchemaError,
                            QStringLiteral("Board test request/response profiles are invalid"),
                            QStringLiteral("mbddf.board.prepare"));
    }

    const QVariantMap fixtureConfig =
        executionConfig.value(QStringLiteral("boardFixture")).toMap();
    state->doSenseResources = stringVector(fixtureConfig,
                                           QStringLiteral("doSenseResources"));
    state->pwmResources = stringVector(fixtureConfig,
                                       QStringLiteral("pwmResources"));
    state->directionResources = stringVector(fixtureConfig,
                                             QStringLiteral("directionResources"));
    state->feedbackResources = stringVector(fixtureConfig,
                                            QStringLiteral("feedbackResources"));
    state->settlingMs = fixtureConfig.value(QStringLiteral("settlingMs"), 100).toInt();
    state->pwmSampleRateHz = fixtureConfig
        .value(QStringLiteral("pwmSampleRateHz"), 1250000.0).toDouble();
    state->pwmSamplesPerChannel = fixtureConfig
        .value(QStringLiteral("pwmSamplesPerChannel"), 7500).toInt();
    state->directionSampleRateHz = fixtureConfig
        .value(QStringLiteral("directionSampleRateHz"), 10000.0).toDouble();
    state->directionSamplesPerChannel = fixtureConfig
        .value(QStringLiteral("directionSamplesPerChannel"), 100).toInt();
    QSet<QString> helmResources;
    for (const QVector<QString>* group : {&state->pwmResources,
                                          &state->directionResources,
                                          &state->feedbackResources}) {
        for (const QString& resource : *group) helmResources.insert(resource);
    }
    const bool resourceShapeOk = state->kind == BoardKind::DoWrite
        ? state->doSenseResources.size() == 2 &&
              allUnique(state->doSenseResources)
        : state->pwmResources.size() == 4 &&
              state->directionResources.size() == 4 &&
              state->feedbackResources.size() == 4 &&
              allUnique(state->pwmResources) &&
              allUnique(state->directionResources) &&
              allUnique(state->feedbackResources) &&
              helmResources.size() == 12;
    const bool fixedAcquisitionOk = state->settlingMs == 100 &&
        (state->kind == BoardKind::DoWrite ||
         (state->pwmSampleRateHz == 1250000.0 &&
          state->pwmSamplesPerChannel == 7500));
    if (!resourceShapeOk || !fixedAcquisitionOk ||
        !std::isfinite(state->pwmSampleRateHz) ||
        !std::isfinite(state->directionSampleRateHz) ||
        state->pwmSampleRateHz <= 0.0 || state->pwmSamplesPerChannel <= 0 ||
        state->directionSampleRateHz <= 0.0 ||
        state->directionSamplesPerChannel <= 0) {
        return failedStatus(ErrorCode::ConfigSchemaError,
                            QStringLiteral("boardFixture resources or fixed sampling settings are invalid"),
                            QStringLiteral("mbddf.board.prepare"));
    }

    bool sequenceOk = true;
    const uint sequence = executionConfig.contains(QStringLiteral("initialSequence"))
        ? executionConfig.value(QStringLiteral("initialSequence")).toUInt(&sequenceOk)
        : 0u;
    if (!sequenceOk || sequence > 0xFFFFu) {
        return failedStatus(ErrorCode::ConfigSchemaError,
                            QStringLiteral("initialSequence must be a 16-bit integer"),
                            QStringLiteral("mbddf.board.prepare"));
    }
    state->nextSequence = static_cast<quint16>(sequence);
    state->transport->setRequestId(context.requestId);
    if (!state->transport->configure(
            executionConfig.value(QStringLiteral("transport")).toMap(), &error)) {
        return failedStatus(ErrorCode::ConfigSchemaError,
                            QStringLiteral("Invalid board test transport: %1").arg(error),
                            QStringLiteral("mbddf.board.prepare"));
    }
    {
        const std::lock_guard<std::mutex> lock(state->transportMutex);
        if (!state->transport->open(&error)) {
            return failedStatus(ErrorCode::DriverMissing,
                                QStringLiteral("Unable to open board test transport: %1").arg(error),
                                QStringLiteral("mbddf.board.prepare"));
        }
    }
    state->prepared = true;
    return {};
}

Status stopState(BoardTestExecutorState* state, int timeoutMs)
{
    if (timeoutMs < 0) {
        return failedStatus(ErrorCode::ParameterRangeError,
                            QStringLiteral("Stop timeout must not be negative"),
                            QStringLiteral("mbddf.board.stop"));
    }
    if (state != nullptr) state->stopRequested.store(true);
    return {};
}

Status resetState(BoardTestExecutorState* state)
{
    if (state == nullptr) return {};
    state->stopRequested.store(false);
    if (state->transport) {
        const std::lock_guard<std::mutex> lock(state->transportMutex);
        state->transport->close();
    }
    state->prepared = false;
    state->request = nullptr;
    state->response = nullptr;
    return {};
}

Status shutdownState(BoardTestExecutorState* state, int timeoutMs)
{
    const Status stopped = stopState(state, timeoutMs);
    if (!stopped.ok()) return stopped;
    return resetState(state);
}

QVariantMap helmRequest(const std::array<int, 4>& duties,
                        const std::array<bool, 4>& directions)
{
    QVariantMap values;
    values.insert(QStringLiteral("pwm_command_reserved"), 0);
    for (int channel = 0; channel < 4; ++channel) {
        values.insert(QStringLiteral("pwm_duty_percent[%1]").arg(channel),
                      duties[static_cast<size_t>(channel)]);
        values.insert(QStringLiteral("direction[%1]").arg(channel),
                      directions[static_cast<size_t>(channel)]);
    }
    return values;
}

std::array<quint32, 2> doWriteState(const QVariantMap& parameters)
{
    quint32 state = 0;
    for (int channel = 0; channel < 16; ++channel) {
        if (parameters.value(QStringLiteral("channel_enabled[%1]").arg(channel))
                .toBool()) {
            state |= (1u << channel);
        }
    }
    return {{state, 0u}};
}

QVariantList doWriteStateList(const std::array<quint32, 2>& state)
{
    return {
        QVariant::fromValue(state[0]),
        QVariant::fromValue(state[1]),
    };
}

QVector<double> channelValues(const hwtest::hal::SampleTaskBlock& block,
                              int channel)
{
    QVector<double> result;
    if (block.sampleType != hwtest::hal::SampleValueType::Float64 ||
        channel < 0 || channel >= block.channelCount ||
        block.samplesPerChannel <= 0 ||
        block.analogValues.size() < block.channelCount * block.samplesPerChannel) {
        return result;
    }
    const int start = channel * block.samplesPerChannel;
    result.reserve(block.samplesPerChannel);
    for (int index = 0; index < block.samplesPerChannel; ++index) {
        result.push_back(block.analogValues.at(start + index));
    }
    return result;
}

double mean(const QVector<double>& values)
{
    if (values.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
    return std::accumulate(values.cbegin(), values.cend(), 0.0) /
        static_cast<double>(values.size());
}

QString halMessage(const hwtest::hal::HalStatus& status,
                   const QString& fallback)
{
    return status.error.message.isEmpty() ? fallback : status.error.message;
}

Result<TestResult> cancelledResult(TestResult result,
                                   QVariantMap board,
                                   const QString& message)
{
    result.skipReason = hwtest::biz::SkipReason::Cancelled;
    finishResult(&result, TestVerdict::Skipped, ErrorCode::Cancelled,
                 message, std::move(board));
    return {Status{}, result};
}

} // namespace

PwmMeasurement measurePwm(const QVector<double>& samples,
                          double sampleRateHz,
                          int minimumCompleteCycles)
{
    PwmMeasurement result;
    if (sampleRateHz <= 0.0 || minimumCompleteCycles <= 0 || samples.size() < 3) {
        result.reason = QStringLiteral("invalid PWM sampling parameters");
        return result;
    }
    QVector<double> finiteSamples;
    finiteSamples.reserve(samples.size());
    for (double value : samples) {
        if (!std::isfinite(value)) {
            result.reason = QStringLiteral("PWM capture contains a non-finite sample");
            return result;
        }
        finiteSamples.push_back(value);
    }
    // The sweep deliberately includes 1% and 99%. Plateau estimators must
    // therefore stay inside those shortest plateaus while still rejecting a
    // lone outlier; 0.5% leaves dozens of samples at the configured capture.
    result.lowVolts = quantile(finiteSamples, 0.005);
    result.highVolts = quantile(finiteSamples, 0.995);
    const double threshold = (result.lowVolts + result.highVolts) / 2.0;
    if (result.highVolts - result.lowVolts < 0.5) {
        result.reason = QStringLiteral("PWM capture has no two-level waveform");
        return result;
    }

    QVector<double> rising;
    QVector<double> falling;
    for (int index = 0; index + 1 < finiteSamples.size(); ++index) {
        const double left = finiteSamples.at(index);
        const double right = finiteSamples.at(index + 1);
        if (left < threshold && right >= threshold) {
            rising.push_back(crossingPosition(left, right, threshold, index));
        } else if (left >= threshold && right < threshold) {
            falling.push_back(crossingPosition(left, right, threshold, index));
        }
    }
    QVector<double> periods;
    QVector<double> dutyFractions;
    for (int index = 0; index + 1 < rising.size(); ++index) {
        const double start = rising.at(index);
        const double end = rising.at(index + 1);
        const auto edge = std::find_if(falling.cbegin(), falling.cend(),
                                       [start, end](double position) {
                                           return position > start && position < end;
                                       });
        if (edge == falling.cend() || end <= start) continue;
        periods.push_back(end - start);
        dutyFractions.push_back((*edge - start) / (end - start));
    }
    result.validCycles = dutyFractions.size();
    if (result.validCycles < minimumCompleteCycles) {
        result.reason = QStringLiteral("PWM capture contains only %1 complete cycles; %2 required")
                            .arg(result.validCycles).arg(minimumCompleteCycles);
        return result;
    }
    const double averagePeriod = mean(periods);
    result.frequencyHz = sampleRateHz / averagePeriod;
    result.dutyPercent = mean(dutyFractions) * 100.0;
    result.valid = std::isfinite(result.frequencyHz) &&
        std::isfinite(result.dutyPercent);
    if (!result.valid) result.reason = QStringLiteral("PWM measurement is not finite");
    return result;
}

DoWriteAlgorithmExecutor::DoWriteAlgorithmExecutor(
    std::unique_ptr<IByteTransport> transport,
    IBoardTestFixture* fixture)
    : m_state(std::make_unique<BoardTestExecutorState>(
          BoardKind::DoWrite, std::move(transport), fixture))
{
}

DoWriteAlgorithmExecutor::~DoWriteAlgorithmExecutor()
{
    shutdown(2000);
}

Status DoWriteAlgorithmExecutor::prepare(const hwtest::biz::TestPlan&,
                                         const hwtest::biz::TestContext& context,
                                         const QVariantMap& executionConfig)
{
    return prepareState(m_state.get(), context, executionConfig);
}

Result<TestResult> DoWriteAlgorithmExecutor::executeStep(
    const TestStep& step,
    const hwtest::biz::IRunControl& control,
    hwtest::biz::IAlgorithmObserver& observer)
{
    TestResult result = initialResult(step);
    QVariantMap board = baseBoardResult(QStringLiteral("do_write"),
                                         QStringLiteral("automatic"), 1);
    QVariantList points;
    if (m_state->fixture == nullptr) {
        finishResult(&result, TestVerdict::Error, ErrorCode::FatalHardwareError,
                     QStringLiteral("PXI-6259 board fixture is unavailable"), board);
        return {Status{}, result};
    }
    if (shouldStop(*m_state, control)) {
        board.insert(QStringLiteral("doSteps"), points);
        return cancelledResult(result, board,
                               QStringLiteral("DO_WRITE stopped before command"));
    }
    const std::array<quint32, 2> commandState = doWriteState(m_state->parameters);
    const QVariantMap requestValues{
        {QStringLiteral("channel[0]"), commandState[0]},
        {QStringLiteral("channel[1]"), commandState[1]},
    };
    const ExchangeResult exchange = m_state->exchange(requestValues, step.timeoutMs);
    if (!exchange.ok) {
        board.insert(QStringLiteral("doSteps"), points);
        finishResult(&result, TestVerdict::Error, exchange.errorCode,
                     exchange.message, board);
        return {Status{}, result};
    }
    m_state->fixture->settle(m_state->settlingMs);
    const auto digital = m_state->fixture->read6259Digital(
        m_state->doSenseResources, step.timeoutMs);
    if (!digital.ok()) {
        board.insert(QStringLiteral("doSteps"), points);
        finishResult(&result, TestVerdict::Error, ErrorCode::FatalHardwareError,
                     halMessage(digital.status,
                                QStringLiteral("PXI-6259 digital input failed")),
                     board);
        return {Status{}, result};
    }
    bool digitalShapeOk = digital.value.size() == m_state->doSenseResources.size();
    if (digitalShapeOk) {
        for (int sampleIndex = 0; sampleIndex < digital.value.size(); ++sampleIndex) {
            const auto& sample = digital.value.at(sampleIndex);
            if (sample.channel != m_state->doSenseResources.at(sampleIndex) ||
                sample.level == hwtest::hal::DigitalLevel::Unknown) {
                digitalShapeOk = false;
                break;
            }
        }
    }
    if (!digitalShapeOk) {
        board.insert(QStringLiteral("doSteps"), points);
        finishResult(&result, TestVerdict::Error,
                     ErrorCode::FatalHardwareError,
                     QStringLiteral("PXI-6259 returned malformed digital input data"),
                     board);
        return {Status{}, result};
    }

    const std::array<quint32, 2> appliedState{{
        exchange.values.value(QStringLiteral("applied_state[0]")).toUInt(),
        exchange.values.value(QStringLiteral("applied_state[1]")).toUInt(),
    }};
    const bool expectedTx = (commandState[0] & (1u << 2)) != 0;
    const bool expectedAttenuator = (commandState[0] & (1u << 1)) != 0;
    const bool measuredTx =
        digital.value.at(0).level == hwtest::hal::DigitalLevel::High;
    const bool measuredAttenuator =
        digital.value.at(1).level == hwtest::hal::DigitalLevel::High;
    const bool appliedOk = appliedState == commandState;
    const bool physicalReadbackOk = measuredTx == expectedTx &&
        measuredAttenuator == expectedAttenuator;
    const bool passed = appliedOk && physicalReadbackOk;
    QVariantMap point{
        {QStringLiteral("index"), 0},
        {QStringLiteral("commandMask"), commandState[0]},
        {QStringLiteral("appliedMask"), appliedState[0]},
        {QStringLiteral("commandState"), commandState[0]},
        {QStringLiteral("appliedState"), appliedState[0]},
        {QStringLiteral("commandStateWords"), doWriteStateList(commandState)},
        {QStringLiteral("appliedStateWords"), doWriteStateList(appliedState)},
        {QStringLiteral("appliedStateMatched"), appliedOk},
        {QStringLiteral("expectedTxEnable"), expectedTx},
        {QStringLiteral("measuredTxEnable"), measuredTx},
        {QStringLiteral("expectedAttenuator"), expectedAttenuator},
        {QStringLiteral("measuredAttenuator"), measuredAttenuator},
        {QStringLiteral("externalValidatedBits"), QVariantList{1, 2}},
        {QStringLiteral("passed"), passed},
        {QStringLiteral("reason"), passed ? QString{} :
             QStringLiteral("DUT applied state or PXI digital readback mismatch")},
    };
    points.push_back(point);
    board.insert(QStringLiteral("completedPoints"), points.size());
    publishPoint(step, observer, point, QStringLiteral("do_write"),
                 points.size(), 1);
    board.insert(QStringLiteral("doSteps"), points);
    board.insert(QStringLiteral("summary"), QVariantMap{
        {QStringLiteral("failedPoints"), passed ? 0 : 1},
    });
    finishResult(&result,
                 passed ? TestVerdict::Pass : TestVerdict::Fail,
                 ErrorCode::Ok,
                 passed ? QStringLiteral("Digital output test passed")
                        : QStringLiteral("Digital output mismatch"),
                 board);
    return {Status{}, result};
}

Status DoWriteAlgorithmExecutor::requestStop(int timeoutMs)
{
    return stopState(m_state.get(), timeoutMs);
}

Status DoWriteAlgorithmExecutor::reset()
{
    return resetState(m_state.get());
}

Status DoWriteAlgorithmExecutor::shutdown(int timeoutMs)
{
    return shutdownState(m_state.get(), timeoutMs);
}

Status DoWriteAlgorithmExecutor::finishRun()
{
    return resetState(m_state.get());
}

HelmBoardTestAlgorithmExecutor::HelmBoardTestAlgorithmExecutor(
    std::unique_ptr<IByteTransport> transport,
    IBoardTestFixture* fixture)
    : m_state(std::make_unique<BoardTestExecutorState>(
          BoardKind::HelmBoard, std::move(transport), fixture))
{
}

HelmBoardTestAlgorithmExecutor::~HelmBoardTestAlgorithmExecutor()
{
    shutdown(2000);
}

Status HelmBoardTestAlgorithmExecutor::prepare(
    const hwtest::biz::TestPlan&,
    const hwtest::biz::TestContext& context,
    const QVariantMap& executionConfig)
{
    return prepareState(m_state.get(), context, executionConfig);
}

Result<TestResult> HelmBoardTestAlgorithmExecutor::executeStep(
    const TestStep& step,
    const hwtest::biz::IRunControl& control,
    hwtest::biz::IAlgorithmObserver& observer)
{
    TestResult result = initialResult(step);
    const bool manual = m_state->parameters
                            .value(QStringLiteral("test_mode"), 0).toInt() == 1;
    QVariantMap board = baseBoardResult(QStringLiteral("helm_board_test"),
                                        manual ? QStringLiteral("manual")
                                               : QStringLiteral("automatic"),
                                        manual ? 1 : 85);
    std::array<int, 4> duties{{0, 0, 0, 0}};
    std::array<bool, 4> directions{{false, false, false, false}};
    if (manual) {
        if (shouldStop(*m_state, control)) {
            return cancelledResult(result, board,
                                   QStringLiteral("HELM_BOARD_TEST manual command cancelled"));
        }
        for (int channel = 0; channel < 4; ++channel) {
            duties[static_cast<size_t>(channel)] = m_state->parameters
                .value(QStringLiteral("pwm_duty_percent[%1]").arg(channel)).toInt();
            directions[static_cast<size_t>(channel)] = m_state->parameters
                .value(QStringLiteral("direction[%1]").arg(channel)).toBool();
        }
        const ExchangeResult exchange = m_state->exchange(
            helmRequest(duties, directions), step.timeoutMs);
        if (!exchange.ok) {
            finishResult(&result, TestVerdict::Error, exchange.errorCode,
                         exchange.message, board);
            return {Status{}, result};
        }
        board.insert(QStringLiteral("completedPoints"), 1);
        board.insert(QStringLiteral("manualResponse"), exchange.values);
        board.insert(QStringLiteral("summary"), QVariantMap{
            {QStringLiteral("responseReceived"), true},
        });
        publishPoint(step, observer, exchange.values, QStringLiteral("manual"), 1, 1);
        finishResult(&result, TestVerdict::Pass, ErrorCode::Ok,
                     QStringLiteral("HELM_BOARD_TEST manual command completed"), board);
        return {Status{}, result};
    }

    if (m_state->fixture == nullptr) {
        finishResult(&result, TestVerdict::Error, ErrorCode::FatalHardwareError,
                     QStringLiteral("PXI-6259/PXI-6733 board fixture is unavailable"),
                     board);
        return {Status{}, result};
    }

    QVariantList directionPoints;
    QVariantList pwmPoints;
    QVariantList feedbackPoints;
    int completed = 0;
    int failedPoints = 0;
    double worstDutyError = 0.0;
    double worstFeedbackError = 0.0;
    QString worstPoint;
    bool cleanupRequired = true;
    const auto zeroAo = [&]() {
        QMap<hwtest::hal::ResourceId, double> values;
        for (const QString& resource : m_state->feedbackResources) {
            values.insert(resource, 0.0);
        }
        return m_state->fixture->write6733Analog(values, step.timeoutMs);
    };
    const auto storeLists = [&] {
        board.insert(QStringLiteral("directionPoints"), directionPoints);
        board.insert(QStringLiteral("pwmPoints"), pwmPoints);
        board.insert(QStringLiteral("feedbackPoints"), feedbackPoints);
        board.insert(QStringLiteral("completedPoints"), completed);
    };
    const auto terminal = [&](TestVerdict verdict, ErrorCode code,
                              QString message) -> Result<TestResult> {
        const hwtest::hal::HalStatus cleaned = cleanupRequired ? zeroAo()
                                                               : hwtest::hal::HalStatus{};
        if (!cleaned.ok()) {
            verdict = TestVerdict::Error;
            code = ErrorCode::FatalHardwareError;
            const QString cleanupMessage = QStringLiteral("AO cleanup failed: %1")
                                               .arg(halMessage(
                                                   cleaned,
                                                   QStringLiteral("unknown HAL error")));
            message = message.isEmpty()
                ? cleanupMessage
                : QStringLiteral("%1; %2").arg(message, cleanupMessage);
        }
        storeLists();
        board.insert(QStringLiteral("summary"), QVariantMap{
            {QStringLiteral("maxDutyErrorPercentagePoints"), worstDutyError},
            {QStringLiteral("maxFeedbackErrorVolts"), worstFeedbackError},
            {QStringLiteral("worstPoint"), worstPoint},
            {QStringLiteral("failedPoints"), failedPoints},
        });
        if (code == ErrorCode::Cancelled) {
            result.skipReason = hwtest::biz::SkipReason::Cancelled;
        }
        finishResult(&result, verdict, code, message, board);
        return {Status{}, result};
    };

    const hwtest::hal::HalStatus initialSafe = zeroAo();
    if (!initialSafe.ok()) {
        return terminal(TestVerdict::Error, ErrorCode::FatalHardwareError,
                        QStringLiteral("Unable to establish zero AO state: %1")
                            .arg(halMessage(initialSafe, QStringLiteral("HAL error"))));
    }

    const std::array<int, 5> directionMasks{{0, 1, 2, 4, 8}};
    for (int expectedMask : directionMasks) {
        if (shouldStop(*m_state, control)) {
            return terminal(TestVerdict::Skipped, ErrorCode::Cancelled,
                            QStringLiteral("HELM_BOARD_TEST stopped between points"));
        }
        directions = {{false, false, false, false}};
        for (int channel = 0; channel < 4; ++channel) {
            directions[static_cast<size_t>(channel)] =
                (expectedMask & (1 << channel)) != 0;
        }
        const ExchangeResult exchange = m_state->exchange(
            helmRequest(duties, directions), step.timeoutMs);
        if (!exchange.ok) {
            return terminal(TestVerdict::Error, exchange.errorCode, exchange.message);
        }
        m_state->fixture->settle(m_state->settlingMs);
        const auto captured = m_state->fixture->capture6259Analog(
            m_state->directionResources, m_state->directionSampleRateHz,
            m_state->directionSamplesPerChannel, step.timeoutMs);
        if (!captured.ok()) {
            return terminal(TestVerdict::Error, ErrorCode::FatalHardwareError,
                            halMessage(captured.status,
                                       QStringLiteral("Direction capture failed")));
        }
        QVariantList measuredVoltages;
        int measuredMask = 0;
        bool passed = true;
        QStringList reasons;
        for (int channel = 0; channel < 4; ++channel) {
            const QVector<double> values = channelValues(captured.value, channel);
            if (values.isEmpty() || !allFinite(values)) {
                return terminal(TestVerdict::Error,
                                ErrorCode::FatalHardwareError,
                                QStringLiteral("Direction capture contains invalid samples"));
            }
            const double voltage = mean(values);
            measuredVoltages.push_back(voltage);
            const bool expected = (expectedMask & (1 << channel)) != 0;
            if (voltage >= kHighLevelMinVolts) measuredMask |= (1 << channel);
            const bool channelPassed = expected
                ? voltage >= kHighLevelMinVolts
                : voltage <= kLowLevelMaxVolts;
            if (!channelPassed) {
                passed = false;
                reasons.push_back(QStringLiteral("DIR%1 voltage %2 V")
                                      .arg(channel + 1).arg(voltage, 0, 'f', 3));
            }
        }
        if (!passed) ++failedPoints;
        QVariantMap point{
            {QStringLiteral("expectedMask"), expectedMask},
            {QStringLiteral("measuredMask"), measuredMask},
            {QStringLiteral("measuredVoltages"), measuredVoltages},
            {QStringLiteral("passed"), passed},
            {QStringLiteral("reason"), joinReasons(reasons)},
        };
        directionPoints.push_back(point);
        ++completed;
        publishPoint(step, observer, point, QStringLiteral("direction"), completed, 85);
    }

    const std::array<int, 9> dutyPoints{{1, 5, 10, 25, 50, 75, 90, 95, 99}};
    directions = {{false, false, false, false}};
    for (int channel = 0; channel < 4; ++channel) {
        for (int commandDuty : dutyPoints) {
            if (shouldStop(*m_state, control)) {
                return terminal(TestVerdict::Skipped, ErrorCode::Cancelled,
                                QStringLiteral("HELM_BOARD_TEST stopped between points"));
            }
            duties = {{0, 0, 0, 0}};
            duties[static_cast<size_t>(channel)] = commandDuty;
            const ExchangeResult exchange = m_state->exchange(
                helmRequest(duties, directions), step.timeoutMs);
            if (!exchange.ok) {
                return terminal(TestVerdict::Error, exchange.errorCode,
                                exchange.message);
            }
            m_state->fixture->settle(m_state->settlingMs);
            const auto targetCapture = m_state->fixture->capture6259Analog(
                {m_state->pwmResources.at(channel)}, m_state->pwmSampleRateHz,
                m_state->pwmSamplesPerChannel, step.timeoutMs);
            if (!targetCapture.ok()) {
                return terminal(TestVerdict::Error, ErrorCode::FatalHardwareError,
                                halMessage(targetCapture.status,
                                           QStringLiteral("PWM capture failed")));
            }
            const QVector<double> targetValues =
                channelValues(targetCapture.value, 0);
            if (targetValues.isEmpty() || !allFinite(targetValues)) {
                return terminal(TestVerdict::Error,
                                ErrorCode::FatalHardwareError,
                                QStringLiteral("PWM capture contains invalid samples"));
            }
            const PwmMeasurement measured = measurePwm(
                targetValues, m_state->pwmSampleRateHz, 20);
            double nonTargetMax = 0.0;
            bool crosstalkPassed = true;
            for (int other = 0; other < 4; ++other) {
                if (other == channel) continue;
                const auto quietCapture = m_state->fixture->capture6259Analog(
                    {m_state->pwmResources.at(other)}, m_state->pwmSampleRateHz,
                    m_state->pwmSamplesPerChannel, step.timeoutMs);
                if (!quietCapture.ok()) {
                    return terminal(TestVerdict::Error,
                                    ErrorCode::FatalHardwareError,
                                    halMessage(quietCapture.status,
                                               QStringLiteral("Non-target PWM capture failed")));
                }
                const QVector<double> quiet = channelValues(quietCapture.value, 0);
                if (quiet.isEmpty() || !allFinite(quiet)) {
                    return terminal(TestVerdict::Error,
                                    ErrorCode::FatalHardwareError,
                                    QStringLiteral("Non-target PWM capture contains invalid samples"));
                }
                const double maximum = *std::max_element(quiet.cbegin(), quiet.cend());
                nonTargetMax = qMax(nonTargetMax, maximum);
                crosstalkPassed = crosstalkPassed && maximum <= kLowLevelMaxVolts;
            }
            const double errorPoints = measured.valid
                ? std::abs(measured.dutyPercent - commandDuty)
                : 0.0;
            QStringList reasons;
            if (!measured.valid) reasons.push_back(measured.reason);
            if (measured.lowVolts > kLowLevelMaxVolts)
                reasons.push_back(QStringLiteral("low plateau exceeds 0.8 V"));
            if (measured.highVolts < kHighLevelMinVolts)
                reasons.push_back(QStringLiteral("high plateau is below 3.8 V"));
            if (measured.valid &&
                errorPoints > kDutyTolerancePercentagePoints)
                reasons.push_back(QStringLiteral("duty error exceeds 1.0 percentage point"));
            if (!crosstalkPassed)
                reasons.push_back(QStringLiteral("non-target PWM pulse detected"));
            const bool passed = reasons.isEmpty();
            if (!passed) ++failedPoints;
            if (std::isfinite(errorPoints) && errorPoints >= worstDutyError) {
                worstDutyError = errorPoints;
                worstPoint = QStringLiteral("PWM%1 @ %2%")
                                 .arg(channel + 1).arg(commandDuty);
            }
            QVariantMap point{
                {QStringLiteral("channel"), channel + 1},
                {QStringLiteral("commandPercent"), commandDuty},
                {QStringLiteral("valid"), measured.valid},
                {QStringLiteral("measuredPercent"), measured.dutyPercent},
                {QStringLiteral("errorPercentagePoints"),
                 measured.valid ? QVariant(errorPoints) : QVariant()},
                {QStringLiteral("tolerancePercentagePoints"),
                 kDutyTolerancePercentagePoints},
                {QStringLiteral("lowV"), measured.lowVolts},
                {QStringLiteral("highV"), measured.highVolts},
                {QStringLiteral("frequencyHz"), measured.frequencyHz},
                {QStringLiteral("validCycles"), measured.validCycles},
                {QStringLiteral("nonTargetMaxV"), nonTargetMax},
                {QStringLiteral("passed"), passed},
                {QStringLiteral("reason"), joinReasons(reasons)},
            };
            pwmPoints.push_back(point);
            ++completed;
            publishPoint(step, observer, point, QStringLiteral("pwm"), completed, 85);
        }
    }

    duties = {{0, 0, 0, 0}};
    directions = {{false, false, false, false}};
    const std::array<double, 11> voltagePoints{{0.0, 0.5, 1.0, 1.5, 2.0,
                                                2.5, 3.0, 3.5, 4.0, 4.5, 5.0}};
    for (int channel = 0; channel < 4; ++channel) {
        for (double commandVolts : voltagePoints) {
            if (shouldStop(*m_state, control)) {
                return terminal(TestVerdict::Skipped, ErrorCode::Cancelled,
                                QStringLiteral("HELM_BOARD_TEST stopped between points"));
            }
            QMap<hwtest::hal::ResourceId, double> output;
            for (int outputChannel = 0; outputChannel < 4; ++outputChannel) {
                output.insert(m_state->feedbackResources.at(outputChannel),
                              outputChannel == channel ? commandVolts : 0.0);
            }
            const hwtest::hal::HalStatus written =
                m_state->fixture->write6733Analog(output, step.timeoutMs);
            if (!written.ok()) {
                return terminal(TestVerdict::Error, ErrorCode::FatalHardwareError,
                                halMessage(written,
                                           QStringLiteral("PXI-6733 AO write failed")));
            }
            m_state->fixture->settle(m_state->settlingMs);
            const ExchangeResult exchange = m_state->exchange(
                helmRequest(duties, directions), step.timeoutMs);
            if (!exchange.ok) {
                return terminal(TestVerdict::Error, exchange.errorCode,
                                exchange.message);
            }
            const double expected = qMin(commandVolts,
                                         kProtocolPositiveFullScaleVolts);
            const double measured = exchange.values
                .value(QStringLiteral("helm_AD_value[%1]").arg(channel)).toDouble();
            if (!std::isfinite(measured)) {
                return terminal(TestVerdict::Error,
                                ErrorCode::ProtocolParseError,
                                QStringLiteral("Feedback response contains a non-finite value"));
            }
            const double errorVolts = std::abs(measured - expected);
            double nonTargetMaxError = 0.0;
            for (int other = 0; other < 4; ++other) {
                if (other == channel) continue;
                const double otherValue = exchange.values
                    .value(QStringLiteral("helm_AD_value[%1]").arg(other))
                    .toDouble();
                if (!std::isfinite(otherValue)) {
                    return terminal(TestVerdict::Error,
                                    ErrorCode::ProtocolParseError,
                                    QStringLiteral("Feedback response contains a non-finite value"));
                }
                nonTargetMaxError = qMax(nonTargetMaxError,
                                         std::abs(otherValue));
            }
            const bool passed = errorVolts <= kFeedbackToleranceVolts &&
                nonTargetMaxError <= kFeedbackToleranceVolts;
            if (!passed) ++failedPoints;
            if (errorVolts >= worstFeedbackError) {
                worstFeedbackError = errorVolts;
                worstPoint = QStringLiteral("FK%1 @ %2 V")
                                 .arg(channel + 1).arg(commandVolts, 0, 'f', 1);
            }
            QVariantMap point{
                {QStringLiteral("channel"), channel + 1},
                {QStringLiteral("commandV"), commandVolts},
                {QStringLiteral("expectedV"), expected},
                {QStringLiteral("measuredV"), measured},
                {QStringLiteral("errorV"), errorVolts},
                {QStringLiteral("toleranceV"), kFeedbackToleranceVolts},
                {QStringLiteral("nonTargetMaxErrorV"), nonTargetMaxError},
                {QStringLiteral("passed"), passed},
                {QStringLiteral("reason"), passed ? QString{} :
                    QStringLiteral("feedback error exceeds tolerance")},
            };
            feedbackPoints.push_back(point);
            ++completed;
            publishPoint(step, observer, point, QStringLiteral("feedback"),
                         completed, 85);
        }
    }

    return terminal(failedPoints > 0 ? TestVerdict::Fail : TestVerdict::Pass,
                    ErrorCode::Ok,
                    failedPoints > 0
                        ? QStringLiteral("HELM_BOARD_TEST completed with criteria failures")
                        : QStringLiteral("HELM_BOARD_TEST automatic test passed"));
}

Status HelmBoardTestAlgorithmExecutor::requestStop(int timeoutMs)
{
    return stopState(m_state.get(), timeoutMs);
}

Status HelmBoardTestAlgorithmExecutor::reset()
{
    return resetState(m_state.get());
}

Status HelmBoardTestAlgorithmExecutor::shutdown(int timeoutMs)
{
    return shutdownState(m_state.get(), timeoutMs);
}

Status HelmBoardTestAlgorithmExecutor::finishRun()
{
    return resetState(m_state.get());
}

} // namespace hwtest::algorithm::mbddf
