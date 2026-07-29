#include "post_run_analysis_coordinator.h"

#include "analysis_result_store.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace hwtest::app {
namespace {

using namespace hwtest::algorithm::mbddf;

ActionResult analysisFailure(const QString& code, const QString& message)
{
    return ActionResult{false,
                        code.isEmpty() ? QStringLiteral("analysis_failed") : code,
                        message.isEmpty() ? QStringLiteral("Post-run analysis failed")
                                          : message};
}

bool isTerminalState(const QString& state)
{
    return state == QStringLiteral("completed") ||
        state == QStringLiteral("partial") ||
        state == QStringLiteral("unavailable") ||
        state == QStringLiteral("failed") ||
        state == QStringLiteral("cancelled");
}

QString boundedWarning(QString warning)
{
    while (warning.toUtf8().size() > 512 && !warning.isEmpty()) warning.chop(1);
    return warning;
}

bool isSourceSummaryMetric(const QString& key)
{
    return key == QStringLiteral("raw_sample_count") ||
        key == QStringLiteral("raw_duration_s") ||
        key == QStringLiteral("analysis_sample_count") ||
        key == QStringLiteral("analysis_duration_s") ||
        key == QStringLiteral("sampling_frequency_hz");
}

QVariantMap sourceSummaryForResult(const AnalysisResult& result)
{
    QVariantMap summary = result.diagnostics;
    summary.insert(QStringLiteral("acceptedSampleCount"),
                   QVariant::fromValue<qulonglong>(result.acceptedSampleCount));
    summary.insert(QStringLiteral("lateSampleCount"),
                   QVariant::fromValue<qulonglong>(result.lateSampleCount));
    summary.insert(QStringLiteral("analyzerVersion"), result.analyzerVersion);
    summary.insert(QStringLiteral("reproducible"), result.reproducible);
    summary.insert(QStringLiteral("sourceArtifactPath"),
                   result.sourceArtifactPath);
    summary.insert(QStringLiteral("sourceArtifactSha256"),
                   QString::fromLatin1(result.sourceArtifactSha256.toHex()));
    return summary;
}

AnalysisMetric projectMetric(
    const hwtest::algorithm::mbddf::AnalysisMetric& source)
{
    AnalysisMetric projected;
    projected.key = source.key;
    projected.label = source.label;
    projected.unit = source.unit;
    projected.status = analysisMetricStatusName(source.status);
    projected.hasValue = source.hasValue && std::isfinite(source.value);
    projected.value = projected.hasValue ? source.value : 0.0;
    projected.detail = source.detail;
    return projected;
}

QJsonObject metricLimitObject(
    const hwtest::algorithm::mbddf::AnalysisMetric& metric,
    bool* valid)
{
    QJsonValue value = QJsonValue::Null;
    if (metric.hasValue) {
        if (!std::isfinite(metric.value)) {
            if (valid != nullptr) *valid = false;
        } else {
            value = metric.value;
        }
    }
    return QJsonObject{
        {QStringLiteral("key"), metric.key},
        {QStringLiteral("label"), metric.label},
        {QStringLiteral("unit"), metric.unit},
        {QStringLiteral("status"), analysisMetricStatusName(metric.status)},
        {QStringLiteral("value"), value},
        {QStringLiteral("detail"), metric.detail},
    };
}

bool isValidBodeSource(const AnalysisChannelResult& channel)
{
    double previousFrequency = 0.0;
    for (int index = 0; index < channel.bodePoints.size(); ++index) {
        const AnalysisBodePoint& point = channel.bodePoints.at(index);
        if (!std::isfinite(point.frequencyHz) || point.frequencyHz <= 0.0 ||
            (index > 0 && point.frequencyHz <= previousFrequency) ||
            (point.hasMagnitude && !std::isfinite(point.magnitudeDb)) ||
            (point.hasPhase && !std::isfinite(point.phaseDeg))) {
            return false;
        }
        previousFrequency = point.frequencyHz;
    }
    return true;
}

bool selectBodeProjectionPoints(
    const AnalysisChannelResult& channel,
    int maximumPoints,
    QVector<AnalysisBodePoint>* selected)
{
    if (selected == nullptr || maximumPoints <= 0) return false;
    selected->clear();
    const QVector<AnalysisBodePoint>& points = channel.bodePoints;
    if (points.size() <= maximumPoints) {
        *selected = points;
        return true;
    }

    QSet<int> required;
    const auto pointHasData = [](const AnalysisBodePoint& point) {
        return point.status == AnalysisPointStatus::Valid &&
            point.hasMagnitude && point.hasPhase;
    };
    for (int index = 0; index < points.size(); ++index) {
        if (pointHasData(points.at(index))) {
            required.insert(index);
            break;
        }
    }
    for (int index = points.size() - 1; index >= 0; --index) {
        if (pointHasData(points.at(index))) {
            required.insert(index);
            break;
        }
    }
    for (int index = 1; index < points.size(); ++index) {
        if (pointHasData(points.at(index - 1)) != pointHasData(points.at(index))) {
            required.insert(index - 1);
            required.insert(index);
        }
    }

    const auto addFrequencyNeighbours = [&](double frequencyHz) {
        if (!std::isfinite(frequencyHz) || frequencyHz < points.first().frequencyHz ||
            frequencyHz > points.last().frequencyHz) {
            return;
        }
        const auto position = std::lower_bound(
            points.cbegin(), points.cend(), frequencyHz,
            [](const AnalysisBodePoint& point, double frequency) {
                return point.frequencyHz < frequency;
            });
        const int upper = static_cast<int>(position - points.cbegin());
        if (upper < points.size()) required.insert(upper);
        if (upper > 0) required.insert(upper - 1);
    };
    addFrequencyNeighbours(5.0);
    addFrequencyNeighbours(10.0);
    addFrequencyNeighbours(20.0);
    for (const auto& metric : channel.waveformMetrics) {
        if (!metric.hasValue || !std::isfinite(metric.value)) continue;
        if (metric.key == QStringLiteral("bandwidth_minus_1db_hz") ||
            metric.key == QStringLiteral("bandwidth_minus_3db_hz") ||
            metric.key == QStringLiteral("resonance_frequency_hz")) {
            addFrequencyNeighbours(metric.value);
        }
    }
    if (required.size() > maximumPoints) return false;

    if (maximumPoints > 1) {
        for (int slot = 0; slot < maximumPoints; ++slot) {
            const int index = qRound(
                static_cast<double>(slot) * (points.size() - 1) /
                static_cast<double>(maximumPoints - 1));
            if (required.size() < maximumPoints) required.insert(index);
        }
    }
    for (int index = 0; required.size() < maximumPoints && index < points.size(); ++index) {
        required.insert(index);
    }
    QVector<int> indices;
    indices.reserve(required.size());
    for (int index : required) indices.push_back(index);
    std::sort(indices.begin(), indices.end());
    selected->reserve(indices.size());
    for (int index : indices) selected->push_back(points.at(index));
    return true;
}

bool validateProjectionLimits(const AnalysisResult& result,
                              const PostRunAnalysisConfig& resources,
                              const QString& resultFilePath,
                              QString* code,
                              QString* message)
{
    QJsonArray summaries;
    QVariantMap sourceSummary = sourceSummaryForResult(result);
    for (const AnalysisChannelResult& channel : result.channels) {
        bool valid = channel.channel >= 0 && channel.channel < 4;
        QJsonArray warnings;
        const int warningCount = std::min(16, channel.warnings.size());
        for (int index = 0; index < warningCount; ++index) {
            warnings.push_back(boundedWarning(channel.warnings.at(index)));
        }
        QJsonArray commonMetrics;
        for (const auto& metric : channel.commonMetrics) {
            if (isSourceSummaryMetric(metric.key)) {
                if (metric.hasValue && std::isfinite(metric.value) &&
                    !sourceSummary.contains(metric.key)) {
                    sourceSummary.insert(metric.key, metric.value);
                }
                continue;
            }
            commonMetrics.push_back(metricLimitObject(metric, &valid));
        }
        QJsonArray waveformMetrics;
        for (const auto& metric : channel.waveformMetrics) {
            waveformMetrics.push_back(metricLimitObject(metric, &valid));
        }
        if (!isValidBodeSource(channel)) {
            if (code != nullptr) *code = QStringLiteral("analysis_projection_invalid");
            if (message != nullptr) {
                *message = QStringLiteral("Analysis projection contains invalid values");
            }
            return false;
        }
        QVector<AnalysisBodePoint> projectedPoints;
        if (!selectBodeProjectionPoints(channel, resources.maxProjectedPoints,
                                        &projectedPoints)) {
            if (code != nullptr) *code = QStringLiteral("analysis_projection_limit");
            if (message != nullptr) {
                *message = QStringLiteral("Analysis projection exceeds the configured point limit");
            }
            return false;
        }
        QJsonArray frequency;
        QJsonArray magnitude;
        QJsonArray phase;
        QJsonArray pointStatus;
        for (const AnalysisBodePoint& point : projectedPoints) {
            if (!std::isfinite(point.frequencyHz) || point.frequencyHz <= 0.0 ||
                (point.hasMagnitude && !std::isfinite(point.magnitudeDb)) ||
                (point.hasPhase && !std::isfinite(point.phaseDeg))) {
                valid = false;
                break;
            }
            frequency.push_back(point.frequencyHz);
            magnitude.push_back(point.hasMagnitude
                                    ? QJsonValue(point.magnitudeDb)
                                    : QJsonValue(QJsonValue::Null));
            phase.push_back(point.hasPhase
                                ? QJsonValue(point.phaseDeg)
                                : QJsonValue(QJsonValue::Null));
            pointStatus.push_back(analysisPointStatusName(point.status));
        }
        if (!valid) {
            if (code != nullptr) *code = QStringLiteral("analysis_projection_invalid");
            if (message != nullptr) {
                *message = QStringLiteral("Analysis projection contains invalid values");
            }
            return false;
        }
        const QJsonObject summary{
            {QStringLiteral("channel"), channel.channel},
            {QStringLiteral("enabled"), channel.enabled},
            {QStringLiteral("status"), analysisChannelStateName(channel.state)},
            {QStringLiteral("warnings"), warnings},
            {QStringLiteral("omittedWarningCount"),
             std::max(0, channel.warnings.size() - warningCount)},
            {QStringLiteral("commonMetrics"), commonMetrics},
            {QStringLiteral("waveformMetrics"), waveformMetrics},
            {QStringLiteral("bodeAvailable"), !projectedPoints.isEmpty()},
            {QStringLiteral("bodePointCount"), projectedPoints.size()},
            {QStringLiteral("reasonCode"), channel.reasonCode},
            {QStringLiteral("message"), channel.message},
        };
        const QJsonObject projection{
            {QStringLiteral("channelSummary"), summary},
            {QStringLiteral("bode"),
             QJsonObject{{QStringLiteral("frequencyHz"), frequency},
                         {QStringLiteral("magnitudeDb"), magnitude},
                         {QStringLiteral("phaseDeg"), phase},
                         {QStringLiteral("pointStatus"), pointStatus}}},
        };
        if (QJsonDocument(projection).toJson(QJsonDocument::Compact).size() >
            resources.maxProjectedBytes) {
            if (code != nullptr) *code = QStringLiteral("analysis_projection_limit");
            if (message != nullptr) {
                *message = QStringLiteral("Analysis projection exceeds the configured byte limit");
            }
            return false;
        }
        summaries.push_back(summary);
    }
    const QJsonObject summary{
        {QStringLiteral("supported"), true},
        {QStringLiteral("analyzerId"), result.analyzerId},
        {QStringLiteral("schemaVersion"), result.schemaVersion},
        {QStringLiteral("taskId"), result.identity.taskId},
        {QStringLiteral("analysisGeneration"),
         static_cast<double>(result.identity.analysisGeneration)},
        {QStringLiteral("state"), analysisStateName(result.state)},
        {QStringLiteral("progress"), 100},
        {QStringLiteral("stage"), analysisStateName(result.state)},
        {QStringLiteral("reasonCode"), result.reasonCode},
        {QStringLiteral("message"), result.message},
        {QStringLiteral("resultFilePath"), resultFilePath},
        {QStringLiteral("diagnosticInputFilePath"), QString{}},
        {QStringLiteral("sourceSummary"), QJsonObject::fromVariantMap(sourceSummary)},
        {QStringLiteral("channelSummaries"), summaries},
    };
    const int summaryBytes =
        QJsonDocument(summary).toJson(QJsonDocument::Compact).size();
    if (summaryBytes > resources.maxAnalysisSummaryBytes) {
        if (code != nullptr) *code = QStringLiteral("analysis_summary_limit");
        if (message != nullptr) {
            *message = QStringLiteral("Analysis summary is %1 bytes and exceeds the configured %2-byte limit")
                           .arg(summaryBytes)
                           .arg(resources.maxAnalysisSummaryBytes);
        }
        return false;
    }
    return true;
}

struct WorkerOutcome {
    AnalysisIdentity identity;
    AnalysisResult result;
    ActionResult action;
    QString resultFilePath;
    QString diagnosticInputFilePath;
};

} // namespace

class PostRunAnalysisCoordinator::Impl {
public:
    PostRunAnalysisDependencies dependencies;
    AnalysisResultStore store;
    PostRunAnalysisCapability capability;
    PostRunAnalysisStartSpec startSpec;
    PostRunAnalysisSnapshot snapshot;
    QVector<AnalysisChannelProjection> projections;
    std::unique_ptr<IPostRunAnalysisSession> session;
    AnalysisIdentity identity;
    AnalysisInputSeal seal;
    AnalysisTermination termination;
    AnalysisCancelToken cancelToken;
    QTimer analysisTimer;
    std::unique_ptr<QThread> worker;
    std::function<void()> updateCallback;
    QString sourceArtifactPath;
    quint64 generation = 0;
    quint64 callbackEpoch = 0;
    bool terminalRequested = false;
    bool sealed = false;
    bool stopCompletionRequired = false;
    bool stopCompleted = false;
    bool workerQueued = false;
    bool analysisTimedOut = false;
};

PostRunAnalysisCoordinator::PostRunAnalysisCoordinator(
    PostRunAnalysisDependencies dependencies,
    QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    if (!dependencies.sessionFactory) {
        dependencies.sessionFactory = [](const AnalysisSessionSpec& spec,
                                         AnalysisError* error) {
            return createPostRunAnalysisSession(spec, error);
        };
    }
    if (!dependencies.analyzerFactory) {
        dependencies.analyzerFactory = [](const QString& algorithmId,
                                          AnalysisError* error) {
            return createPostRunAnalyzer(algorithmId, error);
        };
    }
    m_impl->dependencies = std::move(dependencies);
    m_impl->analysisTimer.setSingleShot(true);
    QObject::connect(&m_impl->analysisTimer, &QTimer::timeout, this, [this] {
        if (m_impl->worker == nullptr || isTerminalState(m_impl->snapshot.state)) {
            return;
        }
        m_impl->analysisTimedOut = true;
        m_impl->cancelToken.requestCancel();
        m_impl->snapshot.reasonCode = QStringLiteral("analysis_timeout");
        m_impl->snapshot.message =
            QStringLiteral("Post-run analysis exceeded the configured timeout");
        publishUpdate();
    });
}

PostRunAnalysisCoordinator::~PostRunAnalysisCoordinator()
{
    m_impl->cancelToken.requestCancel();
    m_impl->analysisTimer.stop();
    if (m_impl->worker != nullptr) {
        m_impl->worker->quit();
        m_impl->worker->wait();
        m_impl->worker.reset();
    }
    m_impl->session.reset();
    m_impl->store.cancelPending();
}

void PostRunAnalysisCoordinator::configureCapability(
    const PostRunAnalysisCapability& capability)
{
    m_impl->capability = capability;
    if (!m_impl->identity.isValid() ||
        m_impl->snapshot.state == QStringLiteral("none") ||
        isTerminalState(m_impl->snapshot.state)) {
        m_impl->snapshot = {};
        m_impl->snapshot.supported = capability.supported;
        m_impl->snapshot.analyzerId = capability.analyzerId;
        m_impl->snapshot.schemaVersion = capability.schemaVersion;
    }
}

ActionResult PostRunAnalysisCoordinator::preparePending(
    const PostRunAnalysisStartSpec& spec)
{
    if (!m_impl->capability.supported) {
        return analysisFailure(QStringLiteral("CapabilityUnsupported"),
                               QStringLiteral("Post-run analysis is not supported"));
    }
    if (blocksWrites()) {
        return analysisFailure(QStringLiteral("command_in_progress"),
                               QStringLiteral("Post-run analysis is still running"));
    }
    if (spec.resources.maxAnalysisDurationS >
        static_cast<quint64>(std::numeric_limits<qint64>::max() / 1000000LL)) {
        return analysisFailure(QStringLiteral("analysis_config"),
                               QStringLiteral("Analysis duration limit is too large"));
    }
    const ActionResult prepared = m_impl->store.preparePending(
        spec.dataStorageDirectory, spec.resources);
    if (!prepared.ok) return prepared;
    const ActionResult closed = m_impl->store.closePendingForSession();
    if (!closed.ok) {
        m_impl->store.cancelPending();
        return closed;
    }

    AnalysisSessionSpec sessionSpec;
    sessionSpec.algorithmId = spec.algorithmId;
    sessionSpec.configId = spec.configId;
    sessionSpec.sourceStepId = spec.sourceStepId;
    sessionSpec.captureFilePath = m_impl->store.pendingCapturePath();
    sessionSpec.schemaVersion = m_impl->capability.schemaVersion;
    sessionSpec.effectiveRunParameters = spec.effectiveRunParameters;
    sessionSpec.metadata = spec.metadata;
    sessionSpec.maxCaptureBytes = spec.resources.maxCaptureBytes;
    sessionSpec.maxInputSamples = spec.resources.maxInputSamples;
    sessionSpec.maxAnalysisDurationUs = static_cast<qint64>(
        spec.resources.maxAnalysisDurationS * 1000000ULL);
    AnalysisError error;
    auto session = m_impl->dependencies.sessionFactory(sessionSpec, &error);
    if (session == nullptr || !error.ok()) {
        m_impl->store.cancelPending();
        return analysisFailure(error.code, error.message);
    }
    m_impl->session = std::move(session);
    m_impl->startSpec = spec;
    return {};
}

ActionResult PostRunAnalysisCoordinator::bindSuccessfulTask(const QString& taskId)
{
    if (m_impl->session == nullptr || taskId.trimmed().isEmpty()) {
        return analysisFailure(QStringLiteral("analysis_identity"),
                               QStringLiteral("A prepared analysis session and task ID are required"));
    }
    m_impl->identity = AnalysisIdentity{taskId.trimmed(), ++m_impl->generation};
    const AnalysisError bound = m_impl->session->bindIdentity(m_impl->identity);
    m_impl->projections.clear();
    m_impl->snapshot = {};
    m_impl->snapshot.supported = m_impl->capability.supported;
    m_impl->snapshot.analyzerId = m_impl->capability.analyzerId;
    m_impl->snapshot.schemaVersion = m_impl->capability.schemaVersion;
    m_impl->snapshot.taskId = m_impl->identity.taskId;
    m_impl->snapshot.analysisGeneration = m_impl->identity.analysisGeneration;
    m_impl->terminalRequested = false;
    m_impl->sealed = false;
    m_impl->stopCompletionRequired = false;
    m_impl->stopCompleted = false;
    m_impl->workerQueued = false;
    m_impl->cancelToken = AnalysisCancelToken{};
    m_impl->analysisTimedOut = false;
    ++m_impl->callbackEpoch;
    if (!bound.ok()) {
        m_impl->snapshot.state = QStringLiteral("unavailable");
        m_impl->snapshot.reasonCode = bound.code;
        m_impl->snapshot.message = bound.message;
        m_impl->snapshot.diagnosticInputFilePath =
            m_impl->store.pendingCapturePath();
        publishUpdate();
        return analysisFailure(bound.code, bound.message);
    }
    m_impl->snapshot.state = QStringLiteral("capturing");
    m_impl->snapshot.progress = 0;
    publishUpdate();
    return {};
}

void PostRunAnalysisCoordinator::discardPrepared()
{
    if (m_impl->worker == nullptr &&
        (m_impl->snapshot.state == QStringLiteral("none") ||
         isTerminalState(m_impl->snapshot.state))) {
        m_impl->session.reset();
        m_impl->store.cancelPending();
        m_impl->terminalRequested = false;
        m_impl->sealed = false;
        m_impl->workerQueued = false;
    }
}

void PostRunAnalysisCoordinator::append(const ApplicationSample& sample)
{
    if (m_impl->session == nullptr || !m_impl->identity.isValid() ||
        sample.taskId != m_impl->identity.taskId) {
        return;
    }
    PostRunSample input;
    input.streamElapsedUs = sample.streamElapsedUs;
    input.sourceId = sample.stepId;
    input.values = sample.values;
    input.tags = sample.tags;
    const AnalysisAcceptResult accepted = m_impl->session->append(
        m_impl->identity, input);
    if (accepted.disposition == AnalysisAppendDisposition::ResourceLimit) {
        m_impl->snapshot.reasonCode = QStringLiteral("analysis_resource_limit");
        m_impl->snapshot.message = accepted.message;
        publishUpdate();
    } else if (!accepted.error.ok() &&
               accepted.disposition == AnalysisAppendDisposition::Rejected) {
        m_impl->snapshot.message = accepted.error.message;
        publishUpdate();
    }
}

void PostRunAnalysisCoordinator::requestTerminal(
    const AnalysisTermination& termination,
    bool waitForStopCompletion,
    const QString& sourceArtifactPath)
{
    if (m_impl->session == nullptr || m_impl->terminalRequested ||
        !m_impl->identity.isValid()) {
        return;
    }
    m_impl->terminalRequested = true;
    m_impl->termination = termination;
    m_impl->sourceArtifactPath = sourceArtifactPath;
    m_impl->stopCompletionRequired = waitForStopCompletion;
    m_impl->stopCompleted = !waitForStopCompletion;
    const AnalysisIdentity identity = m_impl->identity;
    QMetaObject::invokeMethod(
        this,
        [this, identity] {
            if (m_impl->identity.taskId == identity.taskId &&
                m_impl->identity.analysisGeneration ==
                    identity.analysisGeneration) {
                sealPendingInput();
            }
        },
        Qt::QueuedConnection);
}

void PostRunAnalysisCoordinator::notifyStopCompleted()
{
    m_impl->stopCompleted = true;
    queueWorkerIfReady();
}

void PostRunAnalysisCoordinator::sealPendingInput()
{
    if (m_impl->sealed || m_impl->session == nullptr) return;
    m_impl->seal = m_impl->session->seal(m_impl->termination);
    m_impl->sealed = true;
    if (!m_impl->seal.valid || !m_impl->seal.error.ok()) {
        m_impl->snapshot.state = QStringLiteral("failed");
        m_impl->snapshot.reasonCode = m_impl->seal.error.code;
        m_impl->snapshot.message = m_impl->seal.error.message.isEmpty()
            ? m_impl->seal.message
            : m_impl->seal.error.message;
        m_impl->snapshot.diagnosticInputFilePath =
            m_impl->store.retainPendingCapture();
        publishUpdate();
        return;
    }
    if (m_impl->termination.kind == AnalysisTerminationKind::Error ||
        m_impl->seal.resourceLimitReached) {
        m_impl->snapshot.state = QStringLiteral("unavailable");
        m_impl->snapshot.reasonCode = m_impl->seal.resourceLimitReached
            ? QStringLiteral("analysis_resource_limit")
            : (m_impl->termination.reasonCode.isEmpty()
                   ? QStringLiteral("acquisition_error")
                   : m_impl->termination.reasonCode);
        m_impl->snapshot.message = m_impl->termination.message;
        m_impl->snapshot.diagnosticInputFilePath =
            m_impl->store.retainPendingCapture();
        publishUpdate();
        return;
    }
    m_impl->snapshot.state = QStringLiteral("queued");
    m_impl->snapshot.stage = QStringLiteral("sealed");
    m_impl->snapshot.progress = 0;
    publishUpdate();
    queueWorkerIfReady();
}

void PostRunAnalysisCoordinator::queueWorkerIfReady()
{
    if (!m_impl->sealed || !m_impl->stopCompleted || m_impl->workerQueued ||
        isTerminalState(m_impl->snapshot.state)) {
        return;
    }
    m_impl->workerQueued = true;
    QMetaObject::invokeMethod(this, [this] { startWorker(); }, Qt::QueuedConnection);
}

void PostRunAnalysisCoordinator::startWorker()
{
    if (!m_impl->workerQueued || m_impl->worker != nullptr ||
        isTerminalState(m_impl->snapshot.state)) {
        return;
    }
    AnalysisError creationError;
    auto analyzer = m_impl->dependencies.analyzerFactory(
        m_impl->startSpec.algorithmId, &creationError);
    if (analyzer == nullptr || !creationError.ok()) {
        m_impl->snapshot.state = QStringLiteral("failed");
        m_impl->snapshot.reasonCode = creationError.code;
        m_impl->snapshot.message = creationError.message;
        m_impl->snapshot.diagnosticInputFilePath =
            m_impl->store.retainPendingCapture();
        publishUpdate();
        return;
    }

    m_impl->snapshot.state = QStringLiteral("validating");
    m_impl->snapshot.stage = QStringLiteral("validating");
    publishUpdate();
    const AnalysisInputSeal seal = m_impl->seal;
    const AnalysisIdentity identity = m_impl->identity;
    const AnalysisCancelToken cancel = m_impl->cancelToken;
    const quint64 epoch = m_impl->callbackEpoch;
    const QString analysisDirectory = m_impl->store.analysisDirectory();
    const QString sourceArtifactPath = m_impl->sourceArtifactPath;
    const PostRunAnalysisConfig resources = m_impl->startSpec.resources;
    auto sharedAnalyzer = std::shared_ptr<IPostRunAnalyzer>(std::move(analyzer));
    auto outcome = std::make_shared<WorkerOutcome>();
    outcome->identity = identity;
    m_impl->worker.reset(QThread::create(
        [this, sharedAnalyzer, outcome, seal, identity, cancel, epoch,
         analysisDirectory, sourceArtifactPath, resources] {
            const AnalysisProgressCallback progress =
                [this, identity, epoch](const AnalysisProgress& update) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, identity, epoch, update] {
                            if (epoch != m_impl->callbackEpoch ||
                                identity.taskId != m_impl->identity.taskId ||
                                identity.analysisGeneration !=
                                    m_impl->identity.analysisGeneration ||
                                isTerminalState(m_impl->snapshot.state)) {
                                return;
                            }
                            m_impl->snapshot.state = analysisStateName(update.state);
                            m_impl->snapshot.progress = std::clamp(update.percent, 0, 100);
                            m_impl->snapshot.stage = update.stage;
                            m_impl->snapshot.message = update.message;
                            publishUpdate();
                        },
                        Qt::QueuedConnection);
                };
            outcome->result = sharedAnalyzer->analyze(seal, progress, cancel);
            outcome->result.identity = identity;
            if (cancel.isCancellationRequested()) {
                outcome->result.state = AnalysisState::Cancelled;
                outcome->result.reasonCode = QStringLiteral("analysis_cancelled");
            }
            if (!sourceArtifactPath.isEmpty()) {
                QString hashError;
                const QByteArray digest = AnalysisResultStore::sha256File(
                    sourceArtifactPath, &hashError,
                    [cancel] { return cancel.isCancellationRequested(); });
                outcome->result.sourceArtifactPath = sourceArtifactPath;
                if (hashError.isEmpty() && !digest.isEmpty()) {
                    outcome->result.sourceArtifactSha256 = digest;
                    outcome->result.reproducible = true;
                } else {
                    outcome->result.reproducible = false;
                    outcome->result.diagnostics.insert(
                        QStringLiteral("sourceArtifactHashError"), hashError);
                }
            }
            if (cancel.isCancellationRequested()) {
                outcome->result.state = AnalysisState::Cancelled;
                outcome->result.reasonCode = QStringLiteral("analysis_cancelled");
            }
            if (outcome->result.state != AnalysisState::Cancelled &&
                outcome->result.state != AnalysisState::Failed) {
                QString projectionCode;
                QString projectionMessage;
                const QString candidateResultPath =
                    AnalysisResultStore::resultPathForIdentity(
                        analysisDirectory, identity.taskId,
                        identity.analysisGeneration);
                if (!validateProjectionLimits(outcome->result, resources,
                                              candidateResultPath,
                                              &projectionCode,
                                              &projectionMessage)) {
                    outcome->action = analysisFailure(projectionCode,
                                                      projectionMessage);
                    outcome->diagnosticInputFilePath = seal.captureFilePath;
                } else {
                    QByteArray utf8;
                    AnalysisError serializeError;
                    if (!serializeAnalysisResultJson(outcome->result, &utf8,
                                                     &serializeError)) {
                        outcome->action = analysisFailure(serializeError.code,
                                                          serializeError.message);
                        outcome->diagnosticInputFilePath = seal.captureFilePath;
                    } else {
                        AnalysisProgress persisting;
                        persisting.identity = identity;
                        persisting.state = AnalysisState::Persisting;
                        persisting.percent = 95;
                        persisting.stage = QStringLiteral("persisting");
                        persisting.message =
                            QStringLiteral("Saving post-run analysis result");
                        progress(persisting);
                        if (cancel.isCancellationRequested()) {
                            outcome->result.state = AnalysisState::Cancelled;
                            outcome->result.reasonCode =
                                QStringLiteral("analysis_cancelled");
                            outcome->diagnosticInputFilePath = seal.captureFilePath;
                        } else {
                            AnalysisResultStore store;
                            outcome->action = store.commitResult(
                                analysisDirectory, identity.taskId,
                                identity.analysisGeneration, utf8,
                                resources.maxResultBytes,
                                &outcome->resultFilePath);
                            if (!outcome->action.ok) {
                                outcome->diagnosticInputFilePath =
                                    seal.captureFilePath;
                            } else {
                                QFile::remove(seal.captureFilePath);
                            }
                        }
                    }
                }
            } else {
                outcome->diagnosticInputFilePath = seal.captureFilePath;
            }
            QMetaObject::invokeMethod(
                this,
                [this, outcome, epoch] {
                    m_impl->analysisTimer.stop();
                    if (m_impl->worker != nullptr) {
                        m_impl->worker->wait();
                        m_impl->worker.reset();
                    }
                    if (epoch != m_impl->callbackEpoch ||
                        outcome->identity.taskId != m_impl->identity.taskId ||
                        outcome->identity.analysisGeneration !=
                            m_impl->identity.analysisGeneration) {
                        return;
                    }
                    if (!outcome->resultFilePath.isEmpty()) {
                        m_impl->store.cancelPending();
                    } else if (!outcome->diagnosticInputFilePath.isEmpty()) {
                        outcome->diagnosticInputFilePath =
                            m_impl->store.retainPendingCapture();
                    }
                    if (m_impl->analysisTimedOut) {
                        m_impl->snapshot.state = QStringLiteral("failed");
                        m_impl->snapshot.reasonCode =
                            QStringLiteral("analysis_timeout");
                        m_impl->snapshot.message =
                            QStringLiteral("Post-run analysis exceeded the configured timeout");
                        m_impl->snapshot.diagnosticInputFilePath =
                            outcome->diagnosticInputFilePath;
                        m_impl->snapshot.resultFilePath.clear();
                        m_impl->projections.clear();
                        publishUpdate();
                        return;
                    }
                    if (!outcome->action.ok) {
                        m_impl->snapshot.state = QStringLiteral("failed");
                        m_impl->snapshot.reasonCode = outcome->action.code;
                        m_impl->snapshot.message = outcome->action.message;
                        m_impl->snapshot.diagnosticInputFilePath =
                            outcome->diagnosticInputFilePath;
                        m_impl->projections.clear();
                        publishUpdate();
                        return;
                    }

                    const AnalysisResult& result = outcome->result;
                    m_impl->snapshot.state = analysisStateName(result.state);
                    m_impl->snapshot.progress = isTerminalState(
                        m_impl->snapshot.state) ? 100 : m_impl->snapshot.progress;
                    m_impl->snapshot.stage = m_impl->snapshot.state;
                    m_impl->snapshot.message = result.message;
                    m_impl->snapshot.reasonCode = result.reasonCode;
                    m_impl->snapshot.resultFilePath = outcome->resultFilePath;
                    m_impl->snapshot.diagnosticInputFilePath =
                        outcome->diagnosticInputFilePath;
                    m_impl->snapshot.sourceSummary = sourceSummaryForResult(result);
                    m_impl->snapshot.channelSummaries.clear();
                    m_impl->projections.clear();
                    bool projectionInvalid = false;
                    for (const AnalysisChannelResult& channel : result.channels) {
                        AnalysisChannelProjection projection;
                        projection.channelSummary.channel = channel.channel;
                        projection.channelSummary.enabled = channel.enabled;
                        projection.channelSummary.status =
                            analysisChannelStateName(channel.state);
                        projection.channelSummary.reasonCode = channel.reasonCode;
                        projection.channelSummary.message = channel.message;
                        const int warningCount = std::min(16, channel.warnings.size());
                        for (int index = 0; index < warningCount; ++index) {
                            projection.channelSummary.warnings.push_back(
                                boundedWarning(channel.warnings.at(index)));
                        }
                        projection.channelSummary.omittedWarningCount =
                            std::max(0, channel.warnings.size() - warningCount);
                        for (const auto& metric : channel.commonMetrics) {
                            if (isSourceSummaryMetric(metric.key)) {
                                if (metric.hasValue && std::isfinite(metric.value) &&
                                    !m_impl->snapshot.sourceSummary.contains(metric.key)) {
                                    m_impl->snapshot.sourceSummary.insert(metric.key,
                                                                          metric.value);
                                }
                                continue;
                            }
                            projection.channelSummary.commonMetrics.push_back(
                                projectMetric(metric));
                        }
                        for (const auto& metric : channel.waveformMetrics) {
                            projection.channelSummary.waveformMetrics.push_back(
                                projectMetric(metric));
                        }
                        QVector<AnalysisBodePoint> projectedPoints;
                        if (!selectBodeProjectionPoints(
                                channel,
                                m_impl->startSpec.resources.maxProjectedPoints,
                                &projectedPoints)) {
                            projectionInvalid = true;
                            break;
                        }
                        for (const AnalysisBodePoint& point : projectedPoints) {
                            if (!std::isfinite(point.frequencyHz) ||
                                point.frequencyHz <= 0.0 ||
                                (point.hasMagnitude &&
                                 !std::isfinite(point.magnitudeDb)) ||
                                (point.hasPhase && !std::isfinite(point.phaseDeg))) {
                                projectionInvalid = true;
                                break;
                            }
                            projection.frequencyHz.push_back(point.frequencyHz);
                            projection.magnitudeDb.push_back(
                                AnalysisNullableNumber{point.hasMagnitude,
                                                       point.magnitudeDb});
                            projection.phaseDeg.push_back(
                                AnalysisNullableNumber{point.hasPhase,
                                                       point.phaseDeg});
                            projection.pointStatus.push_back(
                                analysisPointStatusName(point.status));
                        }
                        if (projectionInvalid) break;
                        projection.channelSummary.bodeAvailable =
                            !projection.frequencyHz.isEmpty();
                        projection.channelSummary.bodePointCount =
                            projection.frequencyHz.size();
                        m_impl->snapshot.channelSummaries.push_back(
                            projection.channelSummary);
                        m_impl->projections.push_back(std::move(projection));
                    }
                    if (projectionInvalid) {
                        m_impl->snapshot.state = QStringLiteral("failed");
                        m_impl->snapshot.reasonCode =
                            QStringLiteral("analysis_projection_invalid");
                        m_impl->snapshot.message =
                            QStringLiteral("Analysis projection exceeds limits or contains invalid values");
                        m_impl->snapshot.resultFilePath.clear();
                        m_impl->snapshot.channelSummaries.clear();
                        m_impl->projections.clear();
                    }
                    publishUpdate();
                },
                Qt::QueuedConnection);
        }));
    m_impl->worker->start();
    m_impl->analysisTimer.start(m_impl->startSpec.resources.analysisTimeoutMs);
}

ActionResult PostRunAnalysisCoordinator::cancelAndWait(int timeoutMs)
{
    m_impl->cancelToken.requestCancel();
    m_impl->analysisTimer.stop();
    ++m_impl->callbackEpoch;
    if (m_impl->worker != nullptr) {
        const unsigned long waitMs = timeoutMs < 0
            ? std::numeric_limits<unsigned long>::max()
            : static_cast<unsigned long>(timeoutMs);
        if (!m_impl->worker->wait(waitMs)) {
            return analysisFailure(
                QStringLiteral("analysis_shutdown_timeout"),
                QStringLiteral("Post-run analysis did not stop within the configured timeout"));
        }
        m_impl->worker.reset();
    }
    if (m_impl->identity.isValid() && !isTerminalState(m_impl->snapshot.state)) {
        m_impl->snapshot.state = QStringLiteral("cancelled");
        m_impl->snapshot.reasonCode = QStringLiteral("analysis_cancelled");
        m_impl->snapshot.message = QStringLiteral("Post-run analysis was cancelled");
        m_impl->snapshot.diagnosticInputFilePath =
            m_impl->store.retainPendingCapture();
        publishUpdate();
    }
    m_impl->session.reset();
    return {};
}

bool PostRunAnalysisCoordinator::blocksWrites() const
{
    const QString state = m_impl->snapshot.state;
    return state == QStringLiteral("queued") ||
        state == QStringLiteral("validating") ||
        state == QStringLiteral("preprocessing") ||
        state == QStringLiteral("calculating") ||
        state == QStringLiteral("persisting");
}

PostRunAnalysisSnapshot PostRunAnalysisCoordinator::snapshot() const
{
    return m_impl->snapshot;
}

QVector<AnalysisChannelProjection> PostRunAnalysisCoordinator::projections() const
{
    return m_impl->projections;
}

void PostRunAnalysisCoordinator::setUpdateCallback(std::function<void()> callback)
{
    m_impl->updateCallback = std::move(callback);
}

void PostRunAnalysisCoordinator::publishUpdate()
{
    if (m_impl->updateCallback) m_impl->updateCallback();
}

} // namespace hwtest::app
