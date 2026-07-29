#include "helm_sweep_estimator.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace hwtest::algorithm::mbddf {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kMaxDelaySeconds = 0.1;
constexpr int kFrequencyPointCount = 96;
constexpr double kCorrelationTieTolerance = 1e-6;
constexpr double kOverlapTieToleranceSeconds = 1e-9;

bool finite(double value)
{
    return std::isfinite(value);
}

double median(QVector<double> values)
{
    if (values.isEmpty()) return 0.0;
    std::sort(values.begin(), values.end());
    const int middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values.at(middle - 1) + values.at(middle)) * 0.5
                                    : values.at(middle);
}

bool validSeries(const HelmSeries& series)
{
    if (series.timeSeconds.size() < 8 || series.command.size() != series.timeSeconds.size() ||
        series.feedback.size() != series.timeSeconds.size()) {
        return false;
    }
    for (int index = 0; index < series.timeSeconds.size(); ++index) {
        if (!finite(series.timeSeconds.at(index)) || !finite(series.command.at(index)) ||
            !finite(series.feedback.at(index)) ||
            (index > 0 && series.timeSeconds.at(index) <= series.timeSeconds.at(index - 1))) {
            return false;
        }
    }
    return true;
}

double sweepPhase(double tau, const HelmAnalysisParameters& parameters)
{
    const double f0 = parameters.frequencyHz;
    const double f1 = parameters.maximumFrequencyHz;
    const double duration = parameters.sweepDurationSeconds;
    if (f0 == f1) return 2.0 * kPi * f0 * tau + parameters.startPhaseRad;
    const double denominator = f1 * duration - tau * (f1 - f0);
    if (denominator <= 0.0) return std::numeric_limits<double>::quiet_NaN();
    return (2.0 * kPi * f0 * f1 * duration / (f1 - f0)) *
               std::log((f1 * duration) / denominator) + parameters.startPhaseRad;
}

double tauAtFrequency(double frequency, const HelmAnalysisParameters& parameters)
{
    const double f0 = parameters.frequencyHz;
    const double f1 = parameters.maximumFrequencyHz;
    const double duration = parameters.sweepDurationSeconds;
    if (frequency <= 0.0 || f0 == f1) return std::numeric_limits<double>::quiet_NaN();
    return f1 * duration * (1.0 - f0 / frequency) / (f1 - f0);
}

double tauAtPhase(double targetPhase, const HelmAnalysisParameters& parameters)
{
    const double firstPhase = sweepPhase(0.0, parameters);
    const double lastPhase = sweepPhase(parameters.sweepDurationSeconds, parameters);
    if (!finite(targetPhase) || !finite(firstPhase) || !finite(lastPhase) ||
        targetPhase < firstPhase || targetPhase > lastPhase) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double lower = 0.0;
    double upper = parameters.sweepDurationSeconds;
    for (int iteration = 0; iteration < 56; ++iteration) {
        const double middle = (lower + upper) * 0.5;
        if (sweepPhase(middle, parameters) < targetPhase) {
            lower = middle;
        } else {
            upper = middle;
        }
    }
    return (lower + upper) * 0.5;
}

double correlation(const QVector<double>& first, const QVector<double>& second)
{
    if (first.size() < 3 || second.size() != first.size()) return -1.0;
    double firstMean = 0.0;
    double secondMean = 0.0;
    for (int index = 0; index < first.size(); ++index) {
        firstMean += first.at(index);
        secondMean += second.at(index);
    }
    firstMean /= first.size();
    secondMean /= second.size();
    double product = 0.0;
    double firstSquared = 0.0;
    double secondSquared = 0.0;
    for (int index = 0; index < first.size(); ++index) {
        const double a = first.at(index) - firstMean;
        const double b = second.at(index) - secondMean;
        product += a * b;
        firstSquared += a * a;
        secondSquared += b * b;
    }
    return firstSquared > 1e-20 && secondSquared > 1e-20
        ? product / std::sqrt(firstSquared * secondSquared)
        : -1.0;
}

struct CommandStartFit {
    bool valid = false;
    bool ambiguous = false;
    double startSeconds = 0.0;
    double bestCorrelation = -1.0;
    double secondCorrelation = -1.0;
    int overlapCount = 0;
    double overlapDurationSeconds = 0.0;
    double secondOverlapDurationSeconds = 0.0;
    double secondStartSeconds = 0.0;
    double candidateResolutionSeconds = 0.0;
    double minimumStableOverlapSeconds = 0.0;
    double observedExcitationStartSeconds = std::numeric_limits<double>::quiet_NaN();
    double observedExcitationEndSeconds = std::numeric_limits<double>::quiet_NaN();
    double minimumCandidate = 0.0;
    double maximumCandidate = 0.0;
};

struct CandidateScore {
    double correlation = -1.0;
    int overlapCount = 0;
    double overlapDurationSeconds = 0.0;
};

CandidateScore candidateCorrelation(const HelmSeries& series,
                                    const HelmAnalysisParameters& parameters,
                                    double candidate, double excitationFloor,
                                    const AnalysisCancelToken& cancel)
{
    CandidateScore score;
    QVector<double> observed;
    QVector<double> model;
    double firstObservedTime = std::numeric_limits<double>::quiet_NaN();
    double lastObservedTime = std::numeric_limits<double>::quiet_NaN();
    const double firstTime = qMax(series.timeSeconds.first(), candidate);
    const double lastTime = qMin(series.timeSeconds.last(),
                                 candidate + parameters.sweepDurationSeconds);
    if (lastTime <= firstTime) return score;
    constexpr int kMaximumCorrelationSamples = 2048;
    int previousIndex = -1;
    // Candidate scoring samples evenly over DDS time rather than by vector
    // index, so an irregular delivery density cannot bias the fitted start.
    for (int ordinal = 0; ordinal < kMaximumCorrelationSamples; ++ordinal) {
        if ((ordinal & 63) == 0 && cancel.isCancellationRequested()) return score;
        const double fraction = static_cast<double>(ordinal) /
                                static_cast<double>(kMaximumCorrelationSamples - 1);
        const double target = firstTime + (lastTime - firstTime) * fraction;
        auto iterator = std::lower_bound(series.timeSeconds.cbegin(), series.timeSeconds.cend(), target);
        int index = 0;
        if (iterator == series.timeSeconds.cbegin()) {
            index = 0;
        } else if (iterator == series.timeSeconds.cend()) {
            index = series.timeSeconds.size() - 1;
        } else {
            const int upper = static_cast<int>(iterator - series.timeSeconds.cbegin());
            const int lower = upper - 1;
            index = std::abs(series.timeSeconds.at(upper) - target) <
                    std::abs(series.timeSeconds.at(lower) - target) ? upper : lower;
        }
        if (index == previousIndex) continue;
        previousIndex = index;
        const double tau = series.timeSeconds.at(index) - candidate;
        if (tau < 0.0 || tau > parameters.sweepDurationSeconds) continue;
        const double phase = sweepPhase(tau, parameters);
        if (!finite(phase)) continue;
        observed.push_back(series.command.at(index));
        model.push_back(std::sin(phase));
        if (!finite(firstObservedTime)) firstObservedTime = series.timeSeconds.at(index);
        lastObservedTime = series.timeSeconds.at(index);
    }
    score.overlapCount = observed.size();
    if (finite(firstObservedTime) && finite(lastObservedTime)) {
        score.overlapDurationSeconds = lastObservedTime - firstObservedTime;
    }
    const double floorTolerance = qMax(1e-12, excitationFloor * 2e-4);
    if (observed.size() < 8 ||
        (*std::max_element(observed.cbegin(), observed.cend()) -
         *std::min_element(observed.cbegin(), observed.cend())) * 0.5 <
            excitationFloor - floorTolerance) {
        return score;
    }
    score.correlation = correlation(observed, model);
    return score;
}

CommandStartFit fitCommandStart(const HelmSeries& series, const HelmAnalysisParameters& parameters,
                                 const AnalysisCancelToken& cancel)
{
    CommandStartFit fit;
    fit.minimumCandidate = series.timeSeconds.first() - parameters.sweepDurationSeconds;
    fit.maximumCandidate = series.timeSeconds.last();
    const double excitationFloor = helmExcitationFloor(parameters.configuredAmplitude);
    const double floorTolerance = qMax(1e-12, excitationFloor * 2e-4);
    const double commandCenter = median(series.command);
    int firstExcited = -1;
    int lastExcited = -1;
    for (int index = 0; index < series.command.size(); ++index) {
        if (std::abs(series.command.at(index) - commandCenter) >= excitationFloor - floorTolerance) {
            if (firstExcited < 0) firstExcited = index;
            lastExcited = index;
        }
    }
    if (firstExcited >= 0 && lastExcited >= firstExcited) {
        fit.observedExcitationStartSeconds = series.timeSeconds.at(firstExcited);
        fit.observedExcitationEndSeconds = series.timeSeconds.at(lastExcited);
        fit.minimumCandidate = qMax(fit.minimumCandidate,
                                    series.timeSeconds.at(firstExcited) - parameters.sweepDurationSeconds);
        fit.maximumCandidate = qMin(fit.maximumCandidate,
                                    series.timeSeconds.at(lastExcited));
    }
    if (fit.maximumCandidate <= fit.minimumCandidate) return fit;
    const int coarseCount = 1025;
    const double coarseStep = (fit.maximumCandidate - fit.minimumCandidate) /
                              static_cast<double>(coarseCount - 1);
    double bestCandidate = fit.minimumCandidate;
    int bestOverlap = 0;
    double bestOverlapDuration = 0.0;
    const auto better = [](const CandidateScore& score, double candidate,
                           double knownCorrelation, double knownDuration,
                           double knownCandidate) {
        if (score.correlation > knownCorrelation + kCorrelationTieTolerance) return true;
        if (std::abs(score.correlation - knownCorrelation) > kCorrelationTieTolerance) return false;
        if (score.overlapDurationSeconds > knownDuration + kOverlapTieToleranceSeconds) return true;
        if (std::abs(score.overlapDurationSeconds - knownDuration) > kOverlapTieToleranceSeconds) {
            return false;
        }
        return candidate < knownCandidate;
    };
    const auto consider = [&](const CandidateScore& score, double candidate) {
        if (score.correlation < 0.0) return;
        if (better(score, candidate, fit.bestCorrelation, bestOverlapDuration, bestCandidate)) {
            fit.secondCorrelation = fit.bestCorrelation;
            fit.secondOverlapDurationSeconds = bestOverlapDuration;
            fit.secondStartSeconds = bestCandidate;
            fit.bestCorrelation = score.correlation;
            bestCandidate = candidate;
            bestOverlap = score.overlapCount;
            bestOverlapDuration = score.overlapDurationSeconds;
        } else if (std::abs(candidate - bestCandidate) > 1e-12 &&
                   better(score, candidate, fit.secondCorrelation,
                          fit.secondOverlapDurationSeconds, fit.secondStartSeconds)) {
            fit.secondCorrelation = score.correlation;
            fit.secondOverlapDurationSeconds = score.overlapDurationSeconds;
            fit.secondStartSeconds = candidate;
        }
    };
    for (int index = 0; index < coarseCount; ++index) {
        if (cancel.isCancellationRequested()) return fit;
        const double candidate = fit.minimumCandidate + coarseStep * index;
        const CandidateScore score = candidateCorrelation(series, parameters, candidate, excitationFloor,
                                                          cancel);
        consider(score, candidate);
    }
    QVector<double> intervals;
    intervals.reserve(series.timeSeconds.size() - 1);
    for (int index = 1; index < series.timeSeconds.size(); ++index) {
        intervals.push_back(series.timeSeconds.at(index) - series.timeSeconds.at(index - 1));
    }
    const double medianInterval = median(intervals);
    const double fineStep = qMax(1e-6, medianInterval * 0.25);
    const int fineRadius = qMax(4, static_cast<int>(std::ceil(coarseStep / fineStep)) + 2);
    for (int offset = -fineRadius; offset <= fineRadius; ++offset) {
        if (cancel.isCancellationRequested()) return fit;
        const double candidate = qBound(fit.minimumCandidate,
                                        bestCandidate + fineStep * offset,
                                        fit.maximumCandidate);
        const CandidateScore score = candidateCorrelation(series, parameters, candidate, excitationFloor,
                                                          cancel);
        consider(score, candidate);
    }
    fit.startSeconds = bestCandidate;
    fit.overlapCount = bestOverlap;
    fit.overlapDurationSeconds = bestOverlapDuration;
    fit.candidateResolutionSeconds = medianInterval;
    fit.minimumStableOverlapSeconds = 4.0 /
        qMax(parameters.frequencyHz, parameters.maximumFrequencyHz);
    fit.valid = bestOverlap >= 8 && fit.bestCorrelation >= 0.95;
    // Correlation ties with distinct candidate times are resolved by the
    // frozen earlier-start rule.  By contrast, an overlap shorter than the
    // four-cycle local-estimation window cannot establish a stable physical
    // command origin at all.
    fit.ambiguous = fit.valid &&
        fit.overlapDurationSeconds + kOverlapTieToleranceSeconds <
            fit.minimumStableOverlapSeconds;
    return fit;
}

int nearestIndex(const QVector<double>& times, double target)
{
    auto iterator = std::lower_bound(times.cbegin(), times.cend(), target);
    if (iterator == times.cbegin()) return 0;
    if (iterator == times.cend()) return times.size() - 1;
    const int upper = static_cast<int>(iterator - times.cbegin());
    const int lower = upper - 1;
    return std::abs(times.at(upper) - target) < std::abs(times.at(lower) - target) ? upper : lower;
}

double estimateDelay(const HelmSeries& series, double medianInterval,
                     const AnalysisCancelToken& cancel)
{
    double bestCorrelation = -1.0;
    double bestDelay = 0.0;
    const int count = qMax(1, static_cast<int>(std::floor(kMaxDelaySeconds / medianInterval)));
    const int stride = qMax(1, series.timeSeconds.size() / 2048);
    for (int step = 0; step <= count; ++step) {
        if (cancel.isCancellationRequested()) return 0.0;
        const double delay = qMin(kMaxDelaySeconds, medianInterval * step);
        QVector<double> command;
        QVector<double> feedback;
        command.reserve(2048);
        feedback.reserve(2048);
        for (int index = 0; index < series.timeSeconds.size(); index += stride) {
            if ((index / stride) % 256 == 0 && cancel.isCancellationRequested()) return 0.0;
            const double target = series.timeSeconds.at(index) + delay;
            if (target > series.timeSeconds.last()) break;
            const int responseIndex = nearestIndex(series.timeSeconds, target);
            if (std::abs(series.timeSeconds.at(responseIndex) - target) > medianInterval * 0.75) continue;
            command.push_back(series.command.at(index));
            feedback.push_back(series.feedback.at(responseIndex));
        }
        const double value = correlation(command, feedback);
        if (value > bestCorrelation) {
            bestCorrelation = value;
            bestDelay = delay;
        }
    }
    return bestDelay;
}

std::complex<double> coefficient(const QVector<double>& times, const QVector<double>& values,
                                 const QVector<double>& phases, const QVector<double>& weights)
{
    if (times.size() < 3 || values.size() != times.size() || phases.size() != times.size() ||
        weights.size() != times.size()) {
        return {};
    }
    double weightSum = 0.0;
    double weightedMean = 0.0;
    for (int index = 0; index < values.size(); ++index) {
        weightSum += weights.at(index);
        weightedMean += weights.at(index) * values.at(index);
    }
    if (weightSum <= 1e-18) return {};
    weightedMean /= weightSum;
    std::complex<double> sum{};
    for (int index = 0; index < values.size(); ++index) {
        sum += weights.at(index) * (values.at(index) - weightedMean) *
               std::exp(std::complex<double>(0.0, -phases.at(index)));
    }
    return sum / weightSum;
}

AnalysisBodePoint point(double frequency, AnalysisPointStatus status, const QString& detail = {})
{
    AnalysisBodePoint result;
    result.frequencyHz = frequency;
    result.status = status;
    result.detail = detail;
    return result;
}

void upsertMetric(QVector<AnalysisMetric>* metrics, const AnalysisMetric& metric)
{
    if (metrics == nullptr) return;
    for (AnalysisMetric& existing : *metrics) {
        if (existing.key == metric.key) {
            existing = metric;
            return;
        }
    }
    metrics->push_back(metric);
}

void appendSweepMetricDefaults(HelmSweepOutcome* outcome)
{
    if (outcome == nullptr) return;
    const auto unavailable = [&](const QString& key, const QString& label,
                                 const QString& unit, AnalysisMetricStatus status) {
        upsertMetric(&outcome->metrics, helmUnavailableMetric(key, label, unit, status));
    };
    unavailable(QStringLiteral("low_frequency_gain_db"), QStringLiteral("Low frequency gain"),
                QStringLiteral("dB"), AnalysisMetricStatus::Indeterminate);
    unavailable(QStringLiteral("bandwidth_minus_1db_hz"), QStringLiteral("-1 dB bandwidth"),
                QStringLiteral("Hz"), AnalysisMetricStatus::Indeterminate);
    unavailable(QStringLiteral("bandwidth_minus_3db_hz"), QStringLiteral("-3 dB bandwidth"),
                QStringLiteral("Hz"), AnalysisMetricStatus::Indeterminate);
    unavailable(QStringLiteral("phase_5hz_deg"), QStringLiteral("Phase at 5 Hz"),
                QStringLiteral("degree"), AnalysisMetricStatus::NotCovered);
    unavailable(QStringLiteral("phase_10hz_deg"), QStringLiteral("Phase at 10 Hz"),
                QStringLiteral("degree"), AnalysisMetricStatus::NotCovered);
    unavailable(QStringLiteral("phase_20hz_deg"), QStringLiteral("Phase at 20 Hz"),
                QStringLiteral("degree"), AnalysisMetricStatus::NotCovered);
    unavailable(QStringLiteral("resonance_peak_db"), QStringLiteral("Resonance peak"),
                QStringLiteral("dB"), AnalysisMetricStatus::Indeterminate);
    unavailable(QStringLiteral("resonance_frequency_hz"), QStringLiteral("Resonance frequency"),
                QStringLiteral("Hz"), AnalysisMetricStatus::Indeterminate);
}

bool interpolate(const AnalysisBodePoint& first, const AnalysisBodePoint& second,
                 double frequency, double* value, bool phase)
{
    if (value == nullptr || !first.hasMagnitude || !second.hasMagnitude ||
        (phase && (!first.hasPhase || !second.hasPhase)) ||
        first.frequencyHz > frequency || second.frequencyHz < frequency ||
        second.frequencyHz <= first.frequencyHz) {
        return false;
    }
    const double fraction = (frequency - first.frequencyHz) /
                            (second.frequencyHz - first.frequencyHz);
    const double firstValue = phase ? first.phaseDeg : first.magnitudeDb;
    const double secondValue = phase ? second.phaseDeg : second.magnitudeDb;
    *value = firstValue + (secondValue - firstValue) * fraction;
    return finite(*value);
}

} // namespace

HelmSweepOutcome analyzeHelmSweep(const HelmSeries& series,
                                  const HelmAnalysisParameters& parameters,
                                  const AnalysisCancelToken& cancel)
{
    HelmSweepOutcome outcome;
    // Sweep results always publish the same metric keys.  Individual values
    // remain explicitly unavailable when the capture cannot support them.
    appendSweepMetricDefaults(&outcome);
    if (cancel.isCancellationRequested()) {
        outcome.reasonCode = QStringLiteral("cancelled");
        return outcome;
    }
    if (!validSeries(series) || !finite(parameters.frequencyHz) ||
        !finite(parameters.maximumFrequencyHz) || !finite(parameters.sweepDurationSeconds) ||
        parameters.frequencyHz <= 0.0 || parameters.maximumFrequencyHz <= 0.0 ||
        parameters.sweepDurationSeconds <= 0.0) {
        outcome.reasonCode = QStringLiteral("invalid_parameters");
        return outcome;
    }
    QVector<double> intervals;
    intervals.reserve(series.timeSeconds.size() - 1);
    for (int index = 1; index < series.timeSeconds.size(); ++index) {
        intervals.push_back(series.timeSeconds.at(index) - series.timeSeconds.at(index - 1));
    }
    const double medianInterval = median(intervals);
    const double nyquist = medianInterval > 0.0 ? 0.5 / medianInterval : 0.0;
    if (qMax(parameters.frequencyHz, parameters.maximumFrequencyHz) >= nyquist) {
        outcome.reasonCode = QStringLiteral("nyquist_violation");
        outcome.message = QStringLiteral("Sweep frequency exceeds the DDS-time Nyquist limit");
        return outcome;
    }
    const CommandStartFit startFit = fitCommandStart(series, parameters, cancel);
    outcome.diagnostics.insert(QStringLiteral("commandStartCandidateMinUs"),
                               qRound64(startFit.minimumCandidate * 1000000.0));
    outcome.diagnostics.insert(QStringLiteral("commandStartCandidateMaxUs"),
                               qRound64(startFit.maximumCandidate * 1000000.0));
    outcome.diagnostics.insert(QStringLiteral("commandCorrelation"), startFit.bestCorrelation);
    outcome.diagnostics.insert(QStringLiteral("commandSecondCorrelation"), startFit.secondCorrelation);
    outcome.diagnostics.insert(QStringLiteral("commandOverlapSamples"), startFit.overlapCount);
    outcome.diagnostics.insert(QStringLiteral("commandOverlapDurationS"),
                               startFit.overlapDurationSeconds);
    outcome.diagnostics.insert(QStringLiteral("commandSecondOverlapDurationS"),
                               startFit.secondOverlapDurationSeconds);
    outcome.diagnostics.insert(QStringLiteral("commandCandidateResolutionS"),
                               startFit.candidateResolutionSeconds);
    outcome.diagnostics.insert(QStringLiteral("minimumStableCommandOverlapS"),
                               startFit.minimumStableOverlapSeconds);
    outcome.diagnostics.insert(QStringLiteral("commandStartAmbiguous"), startFit.ambiguous);
    outcome.diagnostics.insert(QStringLiteral("commandStartSelection"),
                               QStringLiteral("correlation_then_overlap_then_earliest"));
    if (finite(startFit.observedExcitationStartSeconds)) {
        outcome.diagnostics.insert(QStringLiteral("observedExcitationStartUs"),
                                   qRound64(startFit.observedExcitationStartSeconds * 1000000.0));
        outcome.diagnostics.insert(QStringLiteral("observedExcitationEndUs"),
                                   qRound64(startFit.observedExcitationEndSeconds * 1000000.0));
    }
    if (cancel.isCancellationRequested()) {
        outcome.reasonCode = QStringLiteral("cancelled");
        return outcome;
    }
    if (startFit.ambiguous) {
        outcome.reasonCode = QStringLiteral("command_model_ambiguous");
        outcome.message = QStringLiteral("Sweep command start cannot be resolved at DDS time resolution");
        return outcome;
    }
    if (!startFit.valid) {
        outcome.reasonCode = QStringLiteral("command_model_mismatch");
        outcome.message = QStringLiteral("Observed command does not match the configured sweep model");
        return outcome;
    }
    outcome.diagnostics.insert(QStringLiteral("tCommandStartUs"),
                               qRound64(startFit.startSeconds * 1000000.0));
    const double delay = estimateDelay(series, medianInterval, cancel);
    outcome.diagnostics.insert(QStringLiteral("coarseDelayMs"), delay * 1000.0);
    outcome.diagnostics.insert(QStringLiteral("maxDelayMs"), kMaxDelaySeconds * 1000.0);
    outcome.diagnostics.insert(QStringLiteral("cyclesPerEstimate"), 4);
    outcome.diagnostics.insert(QStringLiteral("frequencyPointCount"), kFrequencyPointCount);
    const double floor = helmExcitationFloor(parameters.configuredAmplitude);
    // The weighted local estimator has a bounded finite-window normalization
    // error.  Keep the frozen equality semantics stable while remaining much
    // tighter than the 1% excitation rule itself.
    const double floorTolerance = qMax(1e-12, floor * 2e-4);
    outcome.diagnostics.insert(QStringLiteral("excitationFloor"), floor);
    const double fmin = qMin(parameters.frequencyHz, parameters.maximumFrequencyHz);
    const double fmax = qMax(parameters.frequencyHz, parameters.maximumFrequencyHz);
    QVector<bool> planned;
    planned.reserve(kFrequencyPointCount);
    QVector<int> plannedIndices;
    double plannedLow = std::numeric_limits<double>::quiet_NaN();
    double plannedHigh = std::numeric_limits<double>::quiet_NaN();
    double minimumLocalCommandAmplitude = std::numeric_limits<double>::infinity();
    double maximumLocalCommandAmplitude = 0.0;
    for (int pointIndex = 0; pointIndex < kFrequencyPointCount; ++pointIndex) {
        if (cancel.isCancellationRequested()) {
            outcome.reasonCode = QStringLiteral("cancelled");
            return outcome;
        }
        const double ratio = static_cast<double>(pointIndex) /
                             static_cast<double>(kFrequencyPointCount - 1);
        const double frequency = fmin * std::pow(fmax / fmin, ratio);
        const double tauCenter = tauAtFrequency(frequency, parameters);
        const double phaseCenter = sweepPhase(tauCenter, parameters);
        const double lowPhase = phaseCenter - 4.0 * kPi;
        const double highPhase = phaseCenter + 4.0 * kPi;
        const double lowTau = tauAtPhase(lowPhase, parameters);
        const double highTau = tauAtPhase(highPhase, parameters);
        const bool theoreticalWindowComplete = finite(lowTau) && finite(highTau) &&
            lowTau >= 0.0 && highTau <= parameters.sweepDurationSeconds && highTau > lowTau;
        planned.push_back(theoreticalWindowComplete);
        if (!theoreticalWindowComplete) {
            outcome.bodePoints.push_back(point(frequency, AnalysisPointStatus::NotCovered,
                                               QStringLiteral("window_outside_planned_sweep")));
            continue;
        }
        if (theoreticalWindowComplete) {
            plannedIndices.push_back(pointIndex);
            if (!finite(plannedLow)) plannedLow = frequency;
            plannedHigh = frequency;
        }
        const double windowStartTime = startFit.startSeconds + lowTau;
        const double windowEndTime = startFit.startSeconds + highTau;
        const auto beginIterator = std::lower_bound(series.timeSeconds.cbegin(),
                                                    series.timeSeconds.cend(), windowStartTime);
        const auto endIterator = std::upper_bound(series.timeSeconds.cbegin(),
                                                  series.timeSeconds.cend(), windowEndTime);
        const int begin = static_cast<int>(beginIterator - series.timeSeconds.cbegin());
        const int end = static_cast<int>(endIterator - series.timeSeconds.cbegin());
        const bool commandWindowComplete = end - begin >= 4 && begin < series.timeSeconds.size() &&
            sweepPhase(series.timeSeconds.at(begin) - startFit.startSeconds, parameters) <=
                lowPhase + 0.35 &&
            sweepPhase(series.timeSeconds.at(end - 1) - startFit.startSeconds, parameters) >=
                highPhase - 0.35;
        if (!commandWindowComplete) {
            outcome.bodePoints.push_back(point(frequency, AnalysisPointStatus::NotCovered,
                                               QStringLiteral("command_window_not_covered")));
            continue;
        }
        const double observedWindowEnd = series.timeSeconds.at(end - 1);
        // delay is only a local alignment estimate.  Publishability follows
        // the frozen 100 ms protection band so an underestimated delay never
        // turns an incomplete response tail into a valid Bode point.
        if (observedWindowEnd + kMaxDelaySeconds > series.timeSeconds.last()) {
            outcome.bodePoints.push_back(point(frequency, AnalysisPointStatus::NotCovered,
                                               QStringLiteral("feedback_tail_not_covered")));
            continue;
        }
        QVector<double> times;
        QVector<double> command;
        QVector<double> feedback;
        QVector<double> phases;
        QVector<double> weights;
        times.reserve(end - begin);
        command.reserve(end - begin);
        feedback.reserve(end - begin);
        phases.reserve(end - begin);
        for (int index = begin; index < end; ++index) {
            if (((index - begin) & 1023) == 0 && cancel.isCancellationRequested()) {
                outcome.reasonCode = QStringLiteral("cancelled");
                return outcome;
            }
            const double time = series.timeSeconds.at(index);
            const double responseTarget = time + delay;
            const int responseIndex = nearestIndex(series.timeSeconds, responseTarget);
            if (std::abs(series.timeSeconds.at(responseIndex) - responseTarget) > medianInterval * 0.75) {
                continue;
            }
            const double phase = sweepPhase(time - startFit.startSeconds, parameters);
            const double left = index == begin ? time : series.timeSeconds.at(index - 1);
            const double right = index + 1 == end ? time : series.timeSeconds.at(index + 1);
            const double trapezoid = qMax(1e-12, (right - left) * 0.5);
            const double normalizedPhase = qBound(-1.0, (phase - phaseCenter) / (4.0 * kPi), 1.0);
            const double hann = 0.5 * (1.0 + std::cos(kPi * normalizedPhase));
            times.push_back(time);
            command.push_back(series.command.at(index));
            feedback.push_back(series.feedback.at(responseIndex));
            phases.push_back(phase);
            weights.push_back(trapezoid * hann);
        }
        const std::complex<double> commandCoefficient = coefficient(times, command, phases, weights);
        const std::complex<double> feedbackCoefficient = coefficient(times, feedback, phases, weights);
        const double localAmplitude = 2.0 * std::abs(commandCoefficient);
        minimumLocalCommandAmplitude = qMin(minimumLocalCommandAmplitude, localAmplitude);
        maximumLocalCommandAmplitude = qMax(maximumLocalCommandAmplitude, localAmplitude);
        if (localAmplitude < floor - floorTolerance) {
            outcome.bodePoints.push_back(point(frequency, AnalysisPointStatus::WeakExcitation,
                                               QStringLiteral("weak_excitation")));
            continue;
        }
        if (std::abs(localAmplitude - floor) <= floorTolerance) {
            if (!outcome.warnings.contains(QStringLiteral("near_excitation_floor"))) {
                outcome.warnings.push_back(QStringLiteral("near_excitation_floor"));
            }
        }
        if (std::abs(commandCoefficient) < 1e-18 || !finite(std::abs(feedbackCoefficient))) {
            outcome.bodePoints.push_back(point(frequency, AnalysisPointStatus::Invalid,
                                               QStringLiteral("invalid_local_coefficient")));
            continue;
        }
        const std::complex<double> response = feedbackCoefficient / commandCoefficient *
            std::exp(std::complex<double>(0.0, -2.0 * kPi * frequency * delay));
        AnalysisBodePoint bode = point(frequency, AnalysisPointStatus::Valid);
        const double responseAmplitude = std::abs(response);
        if (responseAmplitude < 1e-6) {
            bode.status = AnalysisPointStatus::UpperBound;
            bode.detail = QStringLiteral("feedback_below_absolute_floor");
        } else {
            bode.hasMagnitude = finite(responseAmplitude) && responseAmplitude > 0.0;
            bode.magnitudeDb = bode.hasMagnitude ? 20.0 * std::log10(responseAmplitude) : 0.0;
            bode.hasPhase = bode.hasMagnitude && finite(std::arg(response));
            bode.phaseDeg = bode.hasPhase ? std::arg(response) * 180.0 / kPi : 0.0;
            if (!bode.hasMagnitude || !bode.hasPhase) {
                bode.status = AnalysisPointStatus::Invalid;
            }
        }
        outcome.bodePoints.push_back(bode);
    }
    if (plannedIndices.isEmpty()) {
        outcome.reasonCode = QStringLiteral("no_usable_sweep_band");
        return outcome;
    }
    outcome.diagnostics.insert(QStringLiteral("plannedUsableBandMinHz"), plannedLow);
    outcome.diagnostics.insert(QStringLiteral("plannedUsableBandMaxHz"), plannedHigh);
    if (finite(minimumLocalCommandAmplitude)) {
        outcome.diagnostics.insert(QStringLiteral("minimumLocalCommandAmplitude"),
                                   minimumLocalCommandAmplitude);
        outcome.diagnostics.insert(QStringLiteral("maximumLocalCommandAmplitude"),
                                   maximumLocalCommandAmplitude);
    }
    int validPoints = 0;
    bool allPlannedValid = true;
    bool previousValid = false;
    double previousPhase = 0.0;
    for (int index = 0; index < outcome.bodePoints.size(); ++index) {
        AnalysisBodePoint& bode = outcome.bodePoints[index];
        const bool valid = bode.status == AnalysisPointStatus::Valid && bode.hasMagnitude && bode.hasPhase;
        if (planned.at(index) && !valid) allPlannedValid = false;
        if (!valid) {
            previousValid = false;
            continue;
        }
        if (previousValid) {
            while (bode.phaseDeg - previousPhase >= 180.0) bode.phaseDeg -= 360.0;
            while (bode.phaseDeg - previousPhase < -180.0) bode.phaseDeg += 360.0;
        }
        previousPhase = bode.phaseDeg;
        previousValid = true;
        ++validPoints;
    }
    outcome.diagnostics.insert(QStringLiteral("validBodePointCount"), validPoints);
    double actualLow = std::numeric_limits<double>::quiet_NaN();
    double actualHigh = std::numeric_limits<double>::quiet_NaN();
    for (const AnalysisBodePoint& bode : outcome.bodePoints) {
        if (bode.status != AnalysisPointStatus::Valid || !bode.hasMagnitude || !bode.hasPhase) continue;
        if (!finite(actualLow)) actualLow = bode.frequencyHz;
        actualHigh = bode.frequencyHz;
    }
    if (finite(actualLow)) {
        outcome.diagnostics.insert(QStringLiteral("actualCoveredBandMinHz"), actualLow);
        outcome.diagnostics.insert(QStringLiteral("actualCoveredBandMaxHz"), actualHigh);
    }
    if (validPoints == 0) {
        outcome.reasonCode = QStringLiteral("no_valid_sweep_points");
        return outcome;
    }
    QVector<QPair<int, int>> segments;
    int segmentStart = -1;
    for (int index = 0; index < outcome.bodePoints.size(); ++index) {
        const bool valid = outcome.bodePoints.at(index).status == AnalysisPointStatus::Valid &&
                           outcome.bodePoints.at(index).hasMagnitude &&
                           outcome.bodePoints.at(index).hasPhase;
        if (valid && segmentStart < 0) segmentStart = index;
        if ((!valid || index + 1 == outcome.bodePoints.size()) && segmentStart >= 0) {
            const int end = valid && index + 1 == outcome.bodePoints.size() ? index : index - 1;
            segments.push_back(qMakePair(segmentStart, end));
            segmentStart = -1;
        }
    }
    QPair<int, int> referenceSegment{-1, -1};
    const int lowFrequencyAnchor = plannedIndices.first();
    for (const auto& segment : segments) {
        if (segment.first <= lowFrequencyAnchor && segment.second >= lowFrequencyAnchor) {
            referenceSegment = segment;
            break;
        }
    }

    // Phase summaries are independent of G0, but never bridge an invalid
    // Bode-point hole.  This preserves usable phase values after a weak or
    // truncated low-frequency region without inventing a continuous curve.
    for (double target : {5.0, 10.0, 20.0}) {
        double phase = 0.0;
        bool found = false;
        for (const auto& segment : segments) {
            for (int index = segment.first + 1; index <= segment.second; ++index) {
                if (interpolate(outcome.bodePoints.at(index - 1), outcome.bodePoints.at(index),
                                target, &phase, true)) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        const QString key = QStringLiteral("phase_%1hz_deg").arg(target, 0, 'f', 0);
        const QString label = QStringLiteral("Phase at %1 Hz").arg(target);
        upsertMetric(&outcome.metrics, found
            ? helmMetric(key, label, QStringLiteral("degree"), phase)
            : helmUnavailableMetric(key, label, QStringLiteral("degree"),
                                    AnalysisMetricStatus::NotCovered));
    }

    if (referenceSegment.first >= 0 && referenceSegment.second - referenceSegment.first + 1 >= 5) {
        const double low = outcome.bodePoints.at(referenceSegment.first).frequencyHz;
        const double high = outcome.bodePoints.at(referenceSegment.second).frequencyHz;
        const double limit = low * std::pow(high / low, 0.1);
        QVector<double> baseline;
        for (int index = referenceSegment.first; index <= referenceSegment.second; ++index) {
            if (outcome.bodePoints.at(index).frequencyHz > limit) break;
            baseline.push_back(outcome.bodePoints.at(index).magnitudeDb);
        }
        if (baseline.size() >= 5) {
            const double referenceGain = median(baseline);
            upsertMetric(&outcome.metrics,
                         helmMetric(QStringLiteral("low_frequency_gain_db"),
                                    QStringLiteral("Low frequency gain"), QStringLiteral("dB"),
                                    referenceGain));
            const auto bandwidth = [&](double drop, const QString& key, const QString& label) {
                for (int index = referenceSegment.first + 1; index <= referenceSegment.second; ++index) {
                    const auto& a = outcome.bodePoints.at(index - 1);
                    const auto& b = outcome.bodePoints.at(index);
                    const double threshold = referenceGain - drop;
                    if (a.magnitudeDb >= threshold && b.magnitudeDb <= threshold &&
                        b.magnitudeDb != a.magnitudeDb) {
                        const double fraction = (threshold - a.magnitudeDb) /
                                                (b.magnitudeDb - a.magnitudeDb);
                        return helmMetric(key, label, QStringLiteral("Hz"),
                                          a.frequencyHz + (b.frequencyHz - a.frequencyHz) * fraction);
                    }
                }
                bool plannedBeyondSegment = false;
                for (int index = referenceSegment.second + 1; index < planned.size(); ++index) {
                    if (planned.at(index)) {
                        plannedBeyondSegment = true;
                        break;
                    }
                }
                return helmUnavailableMetric(
                    key, label, QStringLiteral("Hz"),
                    plannedBeyondSegment ? AnalysisMetricStatus::NotCovered
                                         : AnalysisMetricStatus::AboveObservedRange);
            };
            upsertMetric(&outcome.metrics,
                         bandwidth(1.0, QStringLiteral("bandwidth_minus_1db_hz"),
                                   QStringLiteral("-1 dB bandwidth")));
            upsertMetric(&outcome.metrics,
                         bandwidth(3.0, QStringLiteral("bandwidth_minus_3db_hz"),
                                   QStringLiteral("-3 dB bandwidth")));
            int peakIndex = referenceSegment.first;
            for (int index = referenceSegment.first + 1; index <= referenceSegment.second; ++index) {
                if (outcome.bodePoints.at(index).magnitudeDb >
                    outcome.bodePoints.at(peakIndex).magnitudeDb) {
                    peakIndex = index;
                }
            }
            upsertMetric(&outcome.metrics,
                         helmMetric(QStringLiteral("resonance_peak_db"),
                                    QStringLiteral("Resonance peak"), QStringLiteral("dB"),
                                    outcome.bodePoints.at(peakIndex).magnitudeDb - referenceGain));
            upsertMetric(&outcome.metrics,
                         helmMetric(QStringLiteral("resonance_frequency_hz"),
                                    QStringLiteral("Resonance frequency"), QStringLiteral("Hz"),
                                    outcome.bodePoints.at(peakIndex).frequencyHz));
        }
    }
    outcome.state = allPlannedValid ? AnalysisChannelState::Completed
                                    : AnalysisChannelState::Partial;
    if (outcome.state == AnalysisChannelState::Partial) {
        outcome.reasonCode = QStringLiteral("partial_sweep_coverage");
    }
    return outcome;
}

} // namespace hwtest::algorithm::mbddf
