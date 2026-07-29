#include "helm_performance_analyzer.h"

#include "helm_analysis_capture.h"
#include "helm_sweep_estimator.h"
#include "helm_waveform_analyzers.h"

#include <QDateTime>
#include <QMetaType>

#include <algorithm>
#include <cmath>

namespace hwtest::algorithm::mbddf {
namespace {

const QString kAnalyzerId = QStringLiteral("mbddf.helm.performance");
const QString kAnalyzerVersion =
    QStringLiteral("mbddf.helm.performance/1;cyclesPerEstimate=4;maxDelayMs=100;"
                   "frequencyPointCount=96;candidateSamples=2048;"
                   "candidateSampling=time_stratified;"
                   "minCommandCorrelation=0.95;absoluteExcitationFloor=1e-6;"
                   "relativeExcitationRatio=0.01");

void progress(const AnalysisProgressCallback& callback, const AnalysisIdentity& identity,
              AnalysisState state, int percent, int channel, const QString& stage)
{
    if (!callback) return;
    AnalysisProgress update;
    update.identity = identity;
    update.state = state;
    update.percent = percent;
    update.channel = channel;
    update.stage = stage;
    callback(update);
}

bool finiteParameter(const QVariantMap& values, const QString& key, double* output)
{
    if (output == nullptr || !values.contains(key)) return false;
    bool ok = false;
    const double value = values.value(key).toDouble(&ok);
    if (!ok || !std::isfinite(value)) return false;
    *output = value;
    return true;
}

bool parseParameters(const QVariantMap& values, HelmAnalysisParameters* parameters,
                     quint32* enabled)
{
    if (parameters == nullptr || enabled == nullptr) return false;
    bool ok = false;
    const int waveform = values.value(QStringLiteral("waveform")).toInt(&ok);
    if (!ok || waveform < 0 || waveform > 4) return false;
    HelmAnalysisParameters result;
    result.waveform = waveform;
    if (!finiteParameter(values, QStringLiteral("freq"), &result.frequencyHz) ||
        !finiteParameter(values, QStringLiteral("ampl"), &result.configuredAmplitude) ||
        !finiteParameter(values, QStringLiteral("offset"), &result.offset) ||
        !finiteParameter(values, QStringLiteral("start"), &result.startPhaseRad) ||
        !finiteParameter(values, QStringLiteral("max_freq"), &result.maximumFrequencyHz) ||
        !finiteParameter(values, QStringLiteral("sweep_duration_s"), &result.sweepDurationSeconds)) {
        return false;
    }
    const uint enable = values.value(QStringLiteral("enable")).toUInt(&ok);
    if (!ok || enable > 0x0fu) return false;
    *parameters = result;
    *enabled = static_cast<quint32>(enable);
    return true;
}

AnalysisChannelResult unavailableChannel(int channel, bool enabled,
                                         const QString& reason, const QString& message)
{
    AnalysisChannelResult result;
    result.channel = channel;
    result.enabled = enabled;
    result.state = enabled ? AnalysisChannelState::Unavailable
                           : AnalysisChannelState::NotApplicable;
    result.reasonCode = enabled ? reason : QString{};
    result.message = enabled ? message : QStringLiteral("Channel is disabled");
    return result;
}

quint64 missingBefore(quint32 previous, quint32 current)
{
    const quint32 advance = current - previous;
    if (advance <= 1u || advance >= 0x80000000u) return 0;
    return static_cast<quint64>(advance - 1u);
}

bool channelInputValid(const HelmCaptureData& capture, int channel, QString* reason)
{
    for (const HelmCaptureRecord& record : capture.records) {
        if ((record.flags & 0x1u) != 0u || (record.flags & 0x10u) != 0u ||
            !std::isfinite(record.command[static_cast<size_t>(channel)]) ||
            !std::isfinite(record.feedback[static_cast<size_t>(channel)]) ||
            record.streamElapsedUs < 0) {
            if (reason != nullptr) *reason = QStringLiteral("invalid_input");
            return false;
        }
        if (record.status != 0u || record.errorCode != 0u || record.selfCheck != 0u ||
            record.timeout != 0u) {
            if (reason != nullptr) *reason = QStringLiteral("device_reported_error");
            return false;
        }
    }
    return true;
}

HelmSeries channelSeries(const HelmCaptureData& capture, int channel)
{
    HelmSeries series;
    series.timeSeconds.reserve(capture.records.size());
    series.command.reserve(capture.records.size());
    series.feedback.reserve(capture.records.size());
    for (const HelmCaptureRecord& record : capture.records) {
        series.timeSeconds.push_back(static_cast<double>(record.streamElapsedUs) / 1000000.0);
        series.command.push_back(record.command[static_cast<size_t>(channel)]);
        series.feedback.push_back(record.feedback[static_cast<size_t>(channel)]);
    }
    return series;
}

QPair<int, int> commonAnalysisWindow(const HelmSeries& series,
                                     const HelmAnalysisParameters& parameters)
{
    const int rawEnd = series.timeSeconds.size();
    if (rawEnd < 2) return {0, rawEnd};
    if (parameters.waveform == 3) {
        const double duration = series.timeSeconds.last() - series.timeSeconds.first();
        if (duration < 1.0) return {0, rawEnd};
        const double window = qMin(duration, qMax(duration * 0.2, 0.2));
        const double start = series.timeSeconds.last() - window;
        const auto begin = std::lower_bound(series.timeSeconds.cbegin(), series.timeSeconds.cend(), start);
        const int index = static_cast<int>(begin - series.timeSeconds.cbegin());
        return rawEnd - index >= 2 ? qMakePair(index, rawEnd) : qMakePair(0, rawEnd);
    }
    const bool singleFrequencySweep = parameters.waveform == 4 &&
        parameters.frequencyHz == parameters.maximumFrequencyHz;
    if (parameters.waveform == 0 || singleFrequencySweep) {
        const double stableStart = series.timeSeconds.first() + 2.0 / parameters.frequencyHz;
        const auto begin = std::lower_bound(series.timeSeconds.cbegin(), series.timeSeconds.cend(), stableStart);
        const int index = static_cast<int>(begin - series.timeSeconds.cbegin());
        if (index < rawEnd - 3 &&
            series.timeSeconds.last() - series.timeSeconds.at(index) >=
                3.0 / parameters.frequencyHz) {
            return {index, rawEnd};
        }
    }
    return {0, rawEnd};
}

void copyOutcome(AnalysisChannelResult* channel, const HelmWaveformOutcome& outcome)
{
    if (channel == nullptr) return;
    channel->state = outcome.state;
    channel->reasonCode = outcome.reasonCode;
    channel->message = outcome.message;
    channel->warnings = outcome.warnings;
    channel->waveformMetrics = outcome.metrics;
    for (auto it = outcome.diagnostics.cbegin(); it != outcome.diagnostics.cend(); ++it) {
        channel->diagnostics.insert(it.key(), it.value());
    }
}

void copySweepOutcome(AnalysisChannelResult* channel, const HelmSweepOutcome& outcome)
{
    if (channel == nullptr) return;
    channel->state = outcome.state;
    channel->reasonCode = outcome.reasonCode;
    channel->message = outcome.message;
    channel->warnings = outcome.warnings;
    channel->waveformMetrics = outcome.metrics;
    channel->bodePoints = outcome.bodePoints;
    for (auto it = outcome.diagnostics.cbegin(); it != outcome.diagnostics.cend(); ++it) {
        channel->diagnostics.insert(it.key(), it.value());
    }
}

} // namespace

AnalysisState reduceAnalysisState(const QVector<AnalysisChannelResult>& channels,
                                  bool infrastructureFailed, bool cancelled)
{
    if (cancelled) return AnalysisState::Cancelled;
    if (infrastructureFailed) return AnalysisState::Failed;
    bool anyEnabled = false;
    bool allCompleted = true;
    bool anyPublishable = false;
    for (const AnalysisChannelResult& channel : channels) {
        if (!channel.enabled) continue;
        anyEnabled = true;
        if (channel.state != AnalysisChannelState::Completed) allCompleted = false;
        if (channel.state == AnalysisChannelState::Completed ||
            channel.state == AnalysisChannelState::Partial) {
            anyPublishable = true;
        }
    }
    if (!anyEnabled || !anyPublishable) return AnalysisState::Unavailable;
    return allCompleted ? AnalysisState::Completed : AnalysisState::Partial;
}

AnalysisResult HelmPerformanceAnalyzer::analyze(const AnalysisInputSeal& input,
                                                const AnalysisProgressCallback& callback,
                                                const AnalysisCancelToken& cancel)
{
    AnalysisResult result;
    result.schemaVersion = input.schemaVersion.isEmpty() ? QStringLiteral("1") : input.schemaVersion;
    result.analyzerId = kAnalyzerId;
    result.analyzerVersion = kAnalyzerVersion;
    result.identity = input.identity;
    result.effectiveRunParameters = input.effectiveRunParameters;
    result.acceptedSampleCount = input.acceptedSampleCount;
    result.lateSampleCount = input.lateSampleCount;
    result.normalizedInputSha256 = input.normalizedInputSha256;
    result.generatedAtUtcUs = QDateTime::currentMSecsSinceEpoch() * 1000;
    result.reproducible = input.metadata.value(QStringLiteral("reproducible")).toBool();
    result.sourceArtifactPath = input.metadata.value(QStringLiteral("sourceArtifactPath")).toString();
    const QVariant sourceHash = input.metadata.value(QStringLiteral("sourceArtifactSha256"));
    result.sourceArtifactSha256 = sourceHash.userType() == QMetaType::QByteArray
        ? sourceHash.toByteArray()
        : QByteArray::fromHex(sourceHash.toString().toLatin1());
    result.termination = input.termination;
    progress(callback, input.identity, AnalysisState::Validating, 5, -1,
             QStringLiteral("validating"));
    if (cancel.isCancellationRequested() || input.termination.kind == AnalysisTerminationKind::Cancelled) {
        result.state = AnalysisState::Cancelled;
        result.reasonCode = QStringLiteral("cancelled");
        return result;
    }
    if (!input.valid) {
        result.state = AnalysisState::Failed;
        result.reasonCode = input.error.code.isEmpty() ? QStringLiteral("analysis_capture")
                                                       : input.error.code;
        result.message = input.error.message.isEmpty() ? input.message : input.error.message;
        return result;
    }
    HelmAnalysisParameters parameters;
    quint32 enabled = 0;
    if (!parseParameters(input.effectiveRunParameters, &parameters, &enabled)) {
        result.state = AnalysisState::Unavailable;
        result.reasonCode = QStringLiteral("invalid_parameters");
        for (int channel = 0; channel < 4; ++channel) {
            result.channels.push_back(unavailableChannel(channel, true, result.reasonCode,
                                                         QStringLiteral("Run parameters are invalid")));
        }
        return result;
    }
    if (enabled == 0u) {
        result.state = AnalysisState::Unavailable;
        result.reasonCode = QStringLiteral("no_enabled_channels");
        for (int channel = 0; channel < 4; ++channel) {
            result.channels.push_back(unavailableChannel(channel, false, {}, {}));
        }
        return result;
    }
    if (input.resourceLimitReached) {
        result.state = AnalysisState::Unavailable;
        result.reasonCode = QStringLiteral("analysis_resource_limit");
        for (int channel = 0; channel < 4; ++channel) {
            const bool channelEnabled = (enabled & (1u << channel)) != 0u;
            result.channels.push_back(unavailableChannel(channel, channelEnabled, result.reasonCode,
                                                         QStringLiteral("Analysis input resource limit was reached")));
        }
        return result;
    }
    if (input.termination.kind == AnalysisTerminationKind::Error) {
        result.state = AnalysisState::Unavailable;
        result.reasonCode = input.termination.reasonCode.isEmpty()
            ? QStringLiteral("acquisition_error") : input.termination.reasonCode;
        for (int channel = 0; channel < 4; ++channel) {
            const bool channelEnabled = (enabled & (1u << channel)) != 0u;
            result.channels.push_back(unavailableChannel(channel, channelEnabled, result.reasonCode,
                                                         QStringLiteral("Acquisition ended with an error")));
        }
        return result;
    }
    HelmCaptureData capture;
    AnalysisError captureError;
    if (!readHelmAnalysisCapture(input, &capture, &captureError)) {
        result.state = AnalysisState::Failed;
        result.reasonCode = captureError.code;
        result.message = captureError.message;
        return result;
    }
    result.acceptedSampleCount = capture.acceptedSampleCount;
    result.lateSampleCount = input.lateSampleCount;
    result.normalizedInputSha256 = capture.normalizedInputSha256;
    quint64 discontinuities = 0;
    for (int index = 1; index < capture.records.size(); ++index) {
        const HelmCaptureRecord& previous = capture.records.at(index - 1);
        const HelmCaptureRecord& current = capture.records.at(index);
        discontinuities += missingBefore(previous.productSequence, current.productSequence);
        discontinuities += missingBefore(previous.serialA, current.serialA);
        discontinuities += missingBefore(previous.serialB, current.serialB);
    }
    result.diagnostics.insert(QStringLiteral("sequenceDiscontinuities"),
                              QVariant::fromValue<qulonglong>(discontinuities));
    result.diagnostics.insert(QStringLiteral("invalidInputCount"),
                              QVariant::fromValue<qulonglong>(capture.invalidInputCount));
    if (capture.records.isEmpty()) {
        result.state = AnalysisState::Unavailable;
        result.reasonCode = QStringLiteral("no_samples");
        for (int channel = 0; channel < 4; ++channel) {
            const bool channelEnabled = (enabled & (1u << channel)) != 0u;
            result.channels.push_back(unavailableChannel(channel, channelEnabled, result.reasonCode,
                                                         QStringLiteral("No analysis samples were captured")));
        }
        return result;
    }
    progress(callback, input.identity, AnalysisState::Preprocessing, 20, -1,
             QStringLiteral("preprocessing"));
    for (int channelIndex = 0; channelIndex < 4; ++channelIndex) {
        if (cancel.isCancellationRequested()) {
            result.channels.clear();
            result.state = AnalysisState::Cancelled;
            result.reasonCode = QStringLiteral("cancelled");
            return result;
        }
        const bool channelEnabled = (enabled & (1u << channelIndex)) != 0u;
        if (!channelEnabled) {
            result.channels.push_back(unavailableChannel(channelIndex, false, {}, {}));
            continue;
        }
        progress(callback, input.identity, AnalysisState::Calculating,
                 25 + channelIndex * 17, channelIndex,
                 QStringLiteral("calculating"));
        AnalysisChannelResult channel;
        channel.channel = channelIndex;
        channel.enabled = true;
        QString invalidReason;
        if (!channelInputValid(capture, channelIndex, &invalidReason)) {
            channel = unavailableChannel(channelIndex, true, invalidReason,
                                         QStringLiteral("Input samples are invalid or report a device error"));
            result.channels.push_back(channel);
            continue;
        }
        const HelmSeries series = channelSeries(capture, channelIndex);
        const QPair<int, int> commonWindow = commonAnalysisWindow(series, parameters);
        appendHelmCommonMetrics(series, commonWindow.first, commonWindow.second,
                                &channel.commonMetrics, &channel.diagnostics);
        switch (parameters.waveform) {
        case 0:
            copyOutcome(&channel, analyzeHelmSine(series, parameters, cancel));
            break;
        case 1:
            copyOutcome(&channel, analyzeHelmSquare(series, parameters, cancel));
            break;
        case 2:
            copyOutcome(&channel, analyzeHelmTriangle(series, parameters, cancel));
            break;
        case 3:
            copyOutcome(&channel, analyzeHelmConstant(series, parameters, cancel));
            break;
        case 4:
            if (parameters.frequencyHz == parameters.maximumFrequencyHz) {
                HelmAnalysisParameters sineParameters = parameters;
                sineParameters.waveform = 0;
                copyOutcome(&channel, analyzeHelmSine(series, sineParameters, cancel));
                channel.warnings.push_back(QStringLiteral("single_frequency_sweep_fallback"));
            } else {
                copySweepOutcome(&channel, analyzeHelmSweep(series, parameters, cancel));
            }
            break;
        default:
            channel = unavailableChannel(channelIndex, true, QStringLiteral("invalid_parameters"),
                                         QStringLiteral("Unsupported waveform"));
            break;
        }
        result.channels.push_back(channel);
    }
    if (cancel.isCancellationRequested()) {
        result.channels.clear();
        result.state = AnalysisState::Cancelled;
        result.reasonCode = QStringLiteral("cancelled");
        return result;
    }
    result.state = reduceAnalysisState(result.channels);
    if (result.state == AnalysisState::Unavailable) {
        for (const AnalysisChannelResult& channel : result.channels) {
            if (channel.enabled && !channel.reasonCode.isEmpty()) {
                result.reasonCode = channel.reasonCode;
                result.message = channel.message;
                break;
            }
        }
    }
    progress(callback, input.identity, AnalysisState::Calculating, 95, -1,
             QStringLiteral("calculated"));
    return result;
}

} // namespace hwtest::algorithm::mbddf
