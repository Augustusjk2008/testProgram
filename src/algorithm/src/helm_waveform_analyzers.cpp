#include "helm_waveform_analyzers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace hwtest::algorithm::mbddf {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kAbsoluteExcitationFloor = 1e-6;

bool finite(double value)
{
    return std::isfinite(value);
}

double mean(const QVector<double>& values)
{
    if (values.isEmpty()) return 0.0;
    double sum = 0.0;
    for (double value : values) sum += value;
    return sum / static_cast<double>(values.size());
}

double rms(const QVector<double>& values)
{
    if (values.isEmpty()) return 0.0;
    double sum = 0.0;
    for (double value : values) sum += value * value;
    return std::sqrt(sum / static_cast<double>(values.size()));
}

double median(QVector<double> values)
{
    if (values.isEmpty()) return 0.0;
    std::sort(values.begin(), values.end());
    const int middle = values.size() / 2;
    return values.size() % 2 == 0
        ? (values.at(middle - 1) + values.at(middle)) * 0.5
        : values.at(middle);
}

double maximum(const QVector<double>& values)
{
    return values.isEmpty() ? 0.0 : *std::max_element(values.cbegin(), values.cend());
}

double minimum(const QVector<double>& values)
{
    return values.isEmpty() ? 0.0 : *std::min_element(values.cbegin(), values.cend());
}

QVector<double> valuesInRange(const QVector<double>& values, int begin, int end)
{
    QVector<double> result;
    begin = qMax(0, begin);
    end = qMin(end, values.size());
    result.reserve(qMax(0, end - begin));
    for (int index = begin; index < end; ++index) result.push_back(values.at(index));
    return result;
}

double centralMedian(const QVector<double>& values, int begin, int end)
{
    begin = qMax(0, begin);
    end = qMin(end, values.size());
    const int count = end - begin;
    if (count <= 0) return 0.0;
    const int centralBegin = begin + count / 4;
    const int centralEnd = qMax(centralBegin + 1, begin + (count * 3) / 4);
    return median(valuesInRange(values, centralBegin, centralEnd));
}

double wrapDegrees(double degrees)
{
    double wrapped = std::fmod(degrees + 180.0, 360.0);
    if (wrapped < 0.0) wrapped += 360.0;
    return wrapped - 180.0;
}

bool seriesIsUsable(const HelmSeries& series)
{
    if (series.timeSeconds.size() < 2 || series.command.size() != series.timeSeconds.size() ||
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

struct SineFit {
    bool valid = false;
    double sine = 0.0;
    double cosine = 0.0;
    double offset = 0.0;
    double amplitude = 0.0;
    double phaseRad = 0.0;
};

SineFit fitSine(const HelmSeries& series, const QVector<double>& values,
                int begin, int end, double frequencyHz)
{
    SineFit fit;
    if (frequencyHz <= 0.0 || begin < 0 || end > values.size() || end - begin < 3) return fit;
    double normal[3][3]{};
    double rhs[3]{};
    for (int index = begin; index < end; ++index) {
        const double t = series.timeSeconds.at(index);
        const double previous = index == begin ? t : series.timeSeconds.at(index - 1);
        const double next = index + 1 == end ? t : series.timeSeconds.at(index + 1);
        const double weight = qMax(1e-12, (next - previous) * 0.5);
        const double angle = 2.0 * kPi * frequencyHz * t;
        const std::array<double, 3> basis{{std::sin(angle), std::cos(angle), 1.0}};
        for (int row = 0; row < 3; ++row) {
            rhs[row] += weight * basis[static_cast<size_t>(row)] * values.at(index);
            for (int column = 0; column < 3; ++column) {
                normal[row][column] += weight * basis[static_cast<size_t>(row)] *
                                       basis[static_cast<size_t>(column)];
            }
        }
    }
    double augmented[3][4]{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) augmented[row][column] = normal[row][column];
        augmented[row][3] = rhs[row];
    }
    for (int pivot = 0; pivot < 3; ++pivot) {
        int selected = pivot;
        for (int row = pivot + 1; row < 3; ++row) {
            if (std::abs(augmented[row][pivot]) > std::abs(augmented[selected][pivot])) {
                selected = row;
            }
        }
        if (std::abs(augmented[selected][pivot]) < 1e-18) return fit;
        if (selected != pivot) {
            for (int column = pivot; column < 4; ++column) {
                std::swap(augmented[selected][column], augmented[pivot][column]);
            }
        }
        const double divisor = augmented[pivot][pivot];
        for (int column = pivot; column < 4; ++column) augmented[pivot][column] /= divisor;
        for (int row = 0; row < 3; ++row) {
            if (row == pivot) continue;
            const double multiplier = augmented[row][pivot];
            for (int column = pivot; column < 4; ++column) {
                augmented[row][column] -= multiplier * augmented[pivot][column];
            }
        }
    }
    fit.sine = augmented[0][3];
    fit.cosine = augmented[1][3];
    fit.offset = augmented[2][3];
    fit.amplitude = std::hypot(fit.sine, fit.cosine);
    fit.phaseRad = std::atan2(fit.cosine, fit.sine);
    fit.valid = finite(fit.amplitude) && finite(fit.phaseRad);
    return fit;
}

double interpolateCrossing(double t0, double y0, double t1, double y1, double threshold)
{
    const double delta = y1 - y0;
    if (std::abs(delta) < 1e-18) return t1;
    const double fraction = qBound(0.0, (threshold - y0) / delta, 1.0);
    return t0 + (t1 - t0) * fraction;
}

bool directedCrosses(double a, double b, double threshold, bool rising)
{
    return rising ? (a <= threshold && b >= threshold) : (a >= threshold && b <= threshold);
}

bool firstCrossing(const HelmSeries& series, int begin, int end, double threshold,
                   bool rising, const AnalysisCancelToken& cancel, double* time)
{
    if (time == nullptr || begin <= 0 || end > series.feedback.size()) return false;
    for (int index = begin; index < end; ++index) {
        if ((index & 1023) == 0 && cancel.isCancellationRequested()) return false;
        const double previous = series.feedback.at(index - 1);
        const double current = series.feedback.at(index);
        if (directedCrosses(previous, current, threshold, rising)) {
            *time = interpolateCrossing(series.timeSeconds.at(index - 1), previous,
                                        series.timeSeconds.at(index), current, threshold);
            return true;
        }
    }
    return false;
}

double weightedLinearSlope(const QVector<double>& x, const QVector<double>& y)
{
    if (x.size() < 2 || y.size() != x.size()) return std::numeric_limits<double>::quiet_NaN();
    QVector<double> weights;
    weights.reserve(x.size());
    double totalWeight = 0.0;
    double xMean = 0.0;
    double yMean = 0.0;
    for (int index = 0; index < x.size(); ++index) {
        const double previous = index == 0 ? x.at(index) : x.at(index - 1);
        const double next = index + 1 == x.size() ? x.at(index) : x.at(index + 1);
        const double weight = qMax(1e-12, (next - previous) * 0.5);
        weights.push_back(weight);
        totalWeight += weight;
        xMean += weight * x.at(index);
        yMean += weight * y.at(index);
    }
    if (totalWeight <= 1e-18) return std::numeric_limits<double>::quiet_NaN();
    xMean /= totalWeight;
    yMean /= totalWeight;
    double numerator = 0.0;
    double denominator = 0.0;
    for (int index = 0; index < x.size(); ++index) {
        const double dx = x.at(index) - xMean;
        numerator += weights.at(index) * dx * (y.at(index) - yMean);
        denominator += weights.at(index) * dx * dx;
    }
    return denominator > 1e-18 ? numerator / denominator
                               : std::numeric_limits<double>::quiet_NaN();
}

struct EdgeMetrics {
    QVector<double> delays;
    QVector<double> riseTimes;
    QVector<double> overshoots;
    QVector<double> settlingTimes;
    QVector<double> steadyErrors;
    int notSettledCount = 0;
};

void appendEdgeMetrics(const QString& prefix, const EdgeMetrics& values,
                       QVector<AnalysisMetric>* metrics)
{
    if (metrics == nullptr) return;
    metrics->push_back(helmMetric(prefix + QStringLiteral("_edge_count"),
                                  prefix + QStringLiteral(" edge count"), QString{},
                                  static_cast<double>(values.delays.size())));
    metrics->push_back(helmMetric(prefix + QStringLiteral("_not_settled_edge_count"),
                                  prefix + QStringLiteral(" not settled edge count"), QString{},
                                  static_cast<double>(values.notSettledCount)));
    const auto appendAggregate = [&](const QString& suffix, const QString& unit,
                                     const QVector<double>& source,
                                     AnalysisMetricStatus unavailableStatus =
                                         AnalysisMetricStatus::NotCovered) {
        if (source.isEmpty()) {
            metrics->push_back(helmUnavailableMetric(prefix + suffix, prefix + suffix, unit,
                                                     unavailableStatus));
            metrics->push_back(helmUnavailableMetric(prefix + suffix + QStringLiteral("_worst"),
                                                     prefix + suffix + QStringLiteral(" worst"), unit,
                                                     unavailableStatus));
            return;
        }
        metrics->push_back(helmMetric(prefix + suffix, prefix + suffix, unit, mean(source)));
        metrics->push_back(helmMetric(prefix + suffix + QStringLiteral("_worst"),
                                      prefix + suffix + QStringLiteral(" worst"), unit,
                                      maximum(source)));
    };
    appendAggregate(QStringLiteral("_delay_ms_mean"), QStringLiteral("ms"), values.delays);
    appendAggregate(QStringLiteral("_rise_time_ms_mean"), QStringLiteral("ms"), values.riseTimes);
    appendAggregate(QStringLiteral("_overshoot_percent_mean"), QStringLiteral("%"), values.overshoots);
    appendAggregate(QStringLiteral("_settling_time_ms_mean"), QStringLiteral("ms"), values.settlingTimes,
                    values.notSettledCount > 0 ? AnalysisMetricStatus::NotSettled
                                               : AnalysisMetricStatus::NotCovered);
    if (values.steadyErrors.isEmpty()) {
        metrics->push_back(helmUnavailableMetric(prefix + QStringLiteral("_steady_state_error_mean"),
                                                 prefix + QStringLiteral(" steady state error"),
                                                 QStringLiteral("degree"),
                                                 AnalysisMetricStatus::NotCovered));
    } else {
        metrics->push_back(helmMetric(prefix + QStringLiteral("_steady_state_error_mean"),
                                      prefix + QStringLiteral(" steady state error"),
                                      QStringLiteral("degree"), mean(values.steadyErrors)));
    }
}

} // namespace

AnalysisMetric helmMetric(const QString& key, const QString& label, const QString& unit,
                          double value, AnalysisMetricStatus status, const QString& detail)
{
    AnalysisMetric metric;
    metric.key = key;
    metric.label = label;
    metric.unit = unit;
    metric.status = status;
    metric.hasValue = finite(value);
    metric.value = metric.hasValue ? value : 0.0;
    metric.detail = detail;
    if (!metric.hasValue) metric.status = AnalysisMetricStatus::Unavailable;
    return metric;
}

AnalysisMetric helmUnavailableMetric(const QString& key, const QString& label,
                                     const QString& unit, AnalysisMetricStatus status,
                                     const QString& detail)
{
    AnalysisMetric metric;
    metric.key = key;
    metric.label = label;
    metric.unit = unit;
    metric.status = status;
    metric.detail = detail;
    return metric;
}

double helmExcitationFloor(double configuredAmplitude)
{
    return qMax(kAbsoluteExcitationFloor, std::abs(configuredAmplitude) * 0.01);
}

void appendHelmCommonMetrics(const HelmSeries& series, int analysisBegin, int analysisEnd,
                             QVector<AnalysisMetric>* metrics, QVariantMap* diagnostics)
{
    if (metrics == nullptr || !seriesIsUsable(series)) return;
    const int rawCount = series.timeSeconds.size();
    const double rawDuration = series.timeSeconds.last() - series.timeSeconds.first();
    analysisBegin = qBound(0, analysisBegin, rawCount);
    analysisEnd = qBound(analysisBegin, analysisEnd, rawCount);
    // A waveform-specific stable window may be absent for a partial result.
    // In that case the raw capture is the only honest common-analysis window.
    if (analysisEnd - analysisBegin < 2) {
        analysisBegin = 0;
        analysisEnd = rawCount;
    }
    const int count = analysisEnd - analysisBegin;
    const double duration = series.timeSeconds.at(analysisEnd - 1) -
                            series.timeSeconds.at(analysisBegin);
    QVector<double> error;
    QVector<double> commandValues;
    QVector<double> feedbackValues;
    error.reserve(count);
    commandValues.reserve(count);
    feedbackValues.reserve(count);
    for (int index = analysisBegin; index < analysisEnd; ++index) {
        error.push_back(series.feedback.at(index) - series.command.at(index));
        commandValues.push_back(series.command.at(index));
        feedbackValues.push_back(series.feedback.at(index));
    }
    const double commandMean = mean(commandValues);
    const double feedbackMean = mean(feedbackValues);
    double covariance = 0.0;
    double commandVariance = 0.0;
    double feedbackVariance = 0.0;
    for (int index = 0; index < count; ++index) {
        const double commandDelta = commandValues.at(index) - commandMean;
        const double feedbackDelta = feedbackValues.at(index) - feedbackMean;
        covariance += commandDelta * feedbackDelta;
        commandVariance += commandDelta * commandDelta;
        feedbackVariance += feedbackDelta * feedbackDelta;
    }
    const double correlation = commandVariance > 0.0 && feedbackVariance > 0.0
        ? covariance / std::sqrt(commandVariance * feedbackVariance)
        : 0.0;
    double maxCommand = 0.0;
    double maxFeedback = 0.0;
    double maxError = 0.0;
    double mae = 0.0;
    for (int index = 0; index < count; ++index) {
        maxCommand = qMax(maxCommand, std::abs(commandValues.at(index)));
        maxFeedback = qMax(maxFeedback, std::abs(feedbackValues.at(index)));
        const double absoluteError = std::abs(error.at(index));
        maxError = qMax(maxError, absoluteError);
        mae += absoluteError;
    }
    mae /= static_cast<double>(count);
    metrics->push_back(helmMetric(QStringLiteral("raw_sample_count"), QStringLiteral("Raw sample count"),
                                  QStringLiteral("sample"), rawCount));
    metrics->push_back(helmMetric(QStringLiteral("raw_duration_s"), QStringLiteral("Raw duration"),
                                  QStringLiteral("s"), rawDuration));
    metrics->push_back(helmMetric(QStringLiteral("analysis_sample_count"),
                                  QStringLiteral("Analysis sample count"), QStringLiteral("sample"), count));
    metrics->push_back(helmMetric(QStringLiteral("analysis_duration_s"),
                                  QStringLiteral("Analysis duration"), QStringLiteral("s"), duration));
    metrics->push_back(helmMetric(QStringLiteral("sampling_frequency_hz"),
                                  QStringLiteral("Sampling frequency"), QStringLiteral("Hz"),
                                  rawDuration > 0.0
                                      ? static_cast<double>(rawCount - 1) / rawDuration
                                      : 0.0));
    metrics->push_back(helmMetric(QStringLiteral("command_peak"), QStringLiteral("Command peak"),
                                  QStringLiteral("degree"), maxCommand));
    metrics->push_back(helmMetric(QStringLiteral("command_range"), QStringLiteral("Command range"),
                                  QStringLiteral("degree"), maximum(commandValues) - minimum(commandValues)));
    metrics->push_back(helmMetric(QStringLiteral("command_rms"), QStringLiteral("Command RMS"),
                                  QStringLiteral("degree"), rms(commandValues)));
    metrics->push_back(helmMetric(QStringLiteral("feedback_peak"), QStringLiteral("Feedback peak"),
                                  QStringLiteral("degree"), maxFeedback));
    metrics->push_back(helmMetric(QStringLiteral("feedback_range"), QStringLiteral("Feedback range"),
                                  QStringLiteral("degree"), maximum(feedbackValues) - minimum(feedbackValues)));
    metrics->push_back(helmMetric(QStringLiteral("feedback_rms"), QStringLiteral("Feedback RMS"),
                                  QStringLiteral("degree"), rms(feedbackValues)));
    metrics->push_back(helmMetric(QStringLiteral("mean_error"), QStringLiteral("Mean error"),
                                  QStringLiteral("degree"), mean(error)));
    metrics->push_back(helmMetric(QStringLiteral("mae"), QStringLiteral("Mean absolute error"),
                                  QStringLiteral("degree"), mae));
    metrics->push_back(helmMetric(QStringLiteral("rmse"), QStringLiteral("RMSE"),
                                  QStringLiteral("degree"), rms(error)));
    metrics->push_back(helmMetric(QStringLiteral("max_abs_error"), QStringLiteral("Maximum absolute error"),
                                  QStringLiteral("degree"), maxError));
    metrics->push_back(helmMetric(QStringLiteral("correlation_coefficient"),
                                  QStringLiteral("Correlation coefficient"), QString{}, correlation));
    if (diagnostics != nullptr) {
        diagnostics->insert(QStringLiteral("analysisStartUs"),
                            qRound64(series.timeSeconds.at(analysisBegin) * 1000000.0));
        diagnostics->insert(QStringLiteral("analysisEndUs"),
                            qRound64(series.timeSeconds.at(analysisEnd - 1) * 1000000.0));
        diagnostics->insert(QStringLiteral("analysisSampleCount"), count);
    }
}

HelmWaveformOutcome analyzeHelmConstant(const HelmSeries& series,
                                        const HelmAnalysisParameters&,
                                        const AnalysisCancelToken& cancel)
{
    HelmWaveformOutcome outcome;
    if (cancel.isCancellationRequested()) {
        outcome.reasonCode = QStringLiteral("cancelled");
        return outcome;
    }
    if (!seriesIsUsable(series)) {
        outcome.reasonCode = QStringLiteral("invalid_input");
        return outcome;
    }
    const double duration = series.timeSeconds.last() - series.timeSeconds.first();
    if (duration < 1.0) {
        outcome.reasonCode = QStringLiteral("insufficient_duration");
        outcome.message = QStringLiteral("Constant analysis requires at least one second");
        return outcome;
    }
    const double window = qMin(duration, qMax(duration * 0.2, 0.2));
    const double start = series.timeSeconds.last() - window;
    int begin = 0;
    while (begin < series.timeSeconds.size() && series.timeSeconds.at(begin) < start) ++begin;
    const QVector<double> command = valuesInRange(series.command, begin, series.command.size());
    const QVector<double> feedback = valuesInRange(series.feedback, begin, series.feedback.size());
    if (command.isEmpty() || feedback.isEmpty()) {
        outcome.reasonCode = QStringLiteral("insufficient_duration");
        return outcome;
    }
    const double target = median(command);
    const double feedbackMean = mean(feedback);
    QVector<double> deviations;
    deviations.reserve(feedback.size());
    double maxAbsError = 0.0;
    for (double value : feedback) {
        deviations.push_back(value - feedbackMean);
        maxAbsError = qMax(maxAbsError, std::abs(value - target));
    }
    outcome.metrics.push_back(helmMetric(QStringLiteral("steady_feedback_mean"),
                                         QStringLiteral("Steady feedback mean"), QStringLiteral("degree"),
                                         feedbackMean));
    outcome.metrics.push_back(helmMetric(QStringLiteral("steady_mean_error"),
                                         QStringLiteral("Steady mean error"), QStringLiteral("degree"),
                                         feedbackMean - target));
    outcome.metrics.push_back(helmMetric(QStringLiteral("steady_stddev"),
                                         QStringLiteral("Steady standard deviation"), QStringLiteral("degree"),
                                         rms(deviations)));
    outcome.metrics.push_back(helmMetric(QStringLiteral("steady_peak_to_peak"),
                                         QStringLiteral("Steady peak to peak"), QStringLiteral("degree"),
                                         maximum(feedback) - minimum(feedback)));
    outcome.metrics.push_back(helmMetric(QStringLiteral("steady_max_abs_error"),
                                         QStringLiteral("Steady maximum absolute error"),
                                         QStringLiteral("degree"), maxAbsError));
    outcome.diagnostics.insert(QStringLiteral("analysisStartUs"),
                               qRound64(series.timeSeconds.at(begin) * 1000000.0));
    outcome.diagnostics.insert(QStringLiteral("analysisEndUs"),
                               qRound64(series.timeSeconds.last() * 1000000.0));
    outcome.diagnostics.insert(QStringLiteral("analysisSampleCount"), feedback.size());
    outcome.state = AnalysisChannelState::Completed;
    return outcome;
}

HelmWaveformOutcome analyzeHelmSine(const HelmSeries& series,
                                    const HelmAnalysisParameters& parameters,
                                    const AnalysisCancelToken& cancel)
{
    HelmWaveformOutcome outcome;
    if (cancel.isCancellationRequested()) {
        outcome.reasonCode = QStringLiteral("cancelled");
        return outcome;
    }
    if (!seriesIsUsable(series) || !finite(parameters.frequencyHz) || parameters.frequencyHz <= 0.0) {
        outcome.reasonCode = QStringLiteral("invalid_parameters");
        return outcome;
    }
    const double stableStart = series.timeSeconds.first() + 2.0 / parameters.frequencyHz;
    int begin = 0;
    while (begin < series.timeSeconds.size() && series.timeSeconds.at(begin) < stableStart) ++begin;
    if (begin >= series.timeSeconds.size() - 3 ||
        series.timeSeconds.last() - series.timeSeconds.at(begin) < 3.0 / parameters.frequencyHz) {
        outcome.state = AnalysisChannelState::Partial;
        outcome.reasonCode = QStringLiteral("insufficient_cycles");
        outcome.message = QStringLiteral("Sine analysis needs three stable cycles after warmup");
        return outcome;
    }
    const SineFit commandFit = fitSine(series, series.command, begin, series.command.size(),
                                       parameters.frequencyHz);
    const SineFit feedbackFit = fitSine(series, series.feedback, begin, series.feedback.size(),
                                        parameters.frequencyHz);
    if (!commandFit.valid || !feedbackFit.valid) {
        outcome.state = AnalysisChannelState::Partial;
        outcome.reasonCode = QStringLiteral("fit_failed");
        return outcome;
    }
    const double floor = helmExcitationFloor(parameters.configuredAmplitude);
    const double floorTolerance = qMax(1e-12, floor * 2e-4);
    outcome.diagnostics.insert(QStringLiteral("excitationFloor"), floor);
    if (commandFit.amplitude < floor - floorTolerance) {
        outcome.reasonCode = QStringLiteral("weak_excitation");
        return outcome;
    }
    if (std::abs(commandFit.amplitude - floor) <= floorTolerance) {
        outcome.warnings.push_back(QStringLiteral("near_excitation_floor"));
    }
    outcome.metrics.push_back(helmMetric(QStringLiteral("command_amplitude"),
                                         QStringLiteral("Command amplitude"), QStringLiteral("degree"),
                                         commandFit.amplitude));
    outcome.metrics.push_back(helmMetric(QStringLiteral("feedback_amplitude"),
                                         QStringLiteral("Feedback amplitude"), QStringLiteral("degree"),
                                         feedbackFit.amplitude));
    if (feedbackFit.amplitude < kAbsoluteExcitationFloor) {
        outcome.metrics.push_back(helmUnavailableMetric(QStringLiteral("amplitude_ratio"),
                                                         QStringLiteral("Amplitude ratio"), QString{},
                                                         AnalysisMetricStatus::UpperBound));
        outcome.metrics.push_back(helmUnavailableMetric(QStringLiteral("gain_db"),
                                                         QStringLiteral("Gain"), QStringLiteral("dB"),
                                                         AnalysisMetricStatus::UpperBound));
        outcome.metrics.push_back(helmUnavailableMetric(QStringLiteral("phase_lag_deg"),
                                                         QStringLiteral("Phase lag"), QStringLiteral("degree"),
                                                         AnalysisMetricStatus::Indeterminate));
        outcome.metrics.push_back(helmUnavailableMetric(QStringLiteral("principal_delay_ms"),
                                                         QStringLiteral("Principal delay"), QStringLiteral("ms"),
                                                         AnalysisMetricStatus::Indeterminate));
    } else {
        const double ratio = feedbackFit.amplitude / commandFit.amplitude;
        const double phaseLag = wrapDegrees((commandFit.phaseRad - feedbackFit.phaseRad) *
                                            180.0 / kPi);
        const double delayMs = phaseLag / (360.0 * parameters.frequencyHz) * 1000.0;
        outcome.metrics.push_back(helmMetric(QStringLiteral("amplitude_ratio"),
                                             QStringLiteral("Amplitude ratio"), QString{}, ratio));
        outcome.metrics.push_back(helmMetric(QStringLiteral("gain_db"), QStringLiteral("Gain"),
                                             QStringLiteral("dB"), 20.0 * std::log10(ratio)));
        outcome.metrics.push_back(helmMetric(QStringLiteral("phase_lag_deg"),
                                             QStringLiteral("Phase lag"), QStringLiteral("degree"), phaseLag));
        outcome.metrics.push_back(helmMetric(QStringLiteral("principal_delay_ms"),
                                             QStringLiteral("Principal delay"), QStringLiteral("ms"), delayMs));
    }
    QVector<double> error;
    error.reserve(series.timeSeconds.size() - begin);
    double maxError = 0.0;
    for (int index = begin; index < series.timeSeconds.size(); ++index) {
        const double value = series.feedback.at(index) - series.command.at(index);
        error.push_back(value);
        maxError = qMax(maxError, std::abs(value));
    }
    outcome.metrics.push_back(helmMetric(QStringLiteral("stable_rmse"), QStringLiteral("Stable RMSE"),
                                         QStringLiteral("degree"), rms(error)));
    outcome.metrics.push_back(helmMetric(QStringLiteral("stable_max_abs_error"),
                                         QStringLiteral("Stable maximum absolute error"),
                                         QStringLiteral("degree"), maxError));
    outcome.diagnostics.insert(QStringLiteral("analysisStartUs"),
                               qRound64(series.timeSeconds.at(begin) * 1000000.0));
    outcome.diagnostics.insert(QStringLiteral("analysisEndUs"),
                               qRound64(series.timeSeconds.last() * 1000000.0));
    outcome.diagnostics.insert(QStringLiteral("analysisSampleCount"), error.size());
    outcome.state = AnalysisChannelState::Completed;
    return outcome;
}

HelmWaveformOutcome analyzeHelmSquare(const HelmSeries& series,
                                      const HelmAnalysisParameters& parameters,
                                      const AnalysisCancelToken& cancel)
{
    HelmWaveformOutcome outcome;
    if (cancel.isCancellationRequested()) {
        outcome.reasonCode = QStringLiteral("cancelled");
        return outcome;
    }
    if (!seriesIsUsable(series) || !finite(parameters.frequencyHz) || parameters.frequencyHz <= 0.0) {
        outcome.reasonCode = QStringLiteral("invalid_parameters");
        return outcome;
    }
    QVector<int> edges;
    const double excitationFloor = helmExcitationFloor(parameters.configuredAmplitude);
    const double excitationTolerance = qMax(1e-12, excitationFloor * 2e-4);
    const double threshold = qMax(excitationFloor, 1e-12);
    const double actualHalfStep = (maximum(series.command) - minimum(series.command)) * 0.5;
    outcome.diagnostics.insert(QStringLiteral("excitationFloor"), excitationFloor);
    outcome.diagnostics.insert(QStringLiteral("actualHalfStep"), actualHalfStep);
    if (actualHalfStep < excitationFloor - excitationTolerance) {
        outcome.reasonCode = QStringLiteral("weak_excitation");
        outcome.message = QStringLiteral("Square command is below the excitation floor");
        return outcome;
    }
    if (std::abs(actualHalfStep - excitationFloor) <= excitationTolerance) {
        outcome.warnings.push_back(QStringLiteral("near_excitation_floor"));
    }
    for (int index = 1; index < series.command.size(); ++index) {
        if ((index & 1023) == 0 && cancel.isCancellationRequested()) {
            outcome.reasonCode = QStringLiteral("cancelled");
            return outcome;
        }
        if (std::abs(series.command.at(index) - series.command.at(index - 1)) > threshold) {
            edges.push_back(index);
        }
    }
    EdgeMetrics rising;
    EdgeMetrics falling;
    const double halfPeriod = 0.5 / parameters.frequencyHz;
    for (int edgeNumber = 0; edgeNumber < edges.size(); ++edgeNumber) {
        if (cancel.isCancellationRequested()) {
            outcome.reasonCode = QStringLiteral("cancelled");
            return outcome;
        }
        const int edge = edges.at(edgeNumber);
        const int previousEdge = edgeNumber == 0 ? 0 : edges.at(edgeNumber - 1);
        const int nextEdge = edgeNumber + 1 < edges.size() ? edges.at(edgeNumber + 1)
                                                             : series.command.size();
        const int preBegin = previousEdge;
        const int preEnd = edge;
        const int postBegin = edge;
        const int postEnd = nextEdge;
        if (preEnd - preBegin < 2 || postEnd - postBegin < 2 ||
            series.timeSeconds.at(preEnd - 1) - series.timeSeconds.at(preBegin) < halfPeriod * 0.2 ||
            series.timeSeconds.at(postEnd - 1) - series.timeSeconds.at(postBegin) < halfPeriod * 0.2) {
            continue;
        }
        const double u0 = centralMedian(series.command, preBegin, preEnd);
        const double u1 = centralMedian(series.command, postBegin, postEnd);
        const double delta = u1 - u0;
        const double halfStep = std::abs(delta) * 0.5;
        if (halfStep < excitationFloor - excitationTolerance) continue;
        if (std::abs(halfStep - excitationFloor) <= excitationTolerance &&
            !outcome.warnings.contains(QStringLiteral("near_excitation_floor"))) {
            outcome.warnings.push_back(QStringLiteral("near_excitation_floor"));
        }
        const bool upward = delta > 0.0;
        const double commandTime = interpolateCrossing(series.timeSeconds.at(edge - 1),
                                                        series.command.at(edge - 1),
                                                        series.timeSeconds.at(edge),
                                                        series.command.at(edge),
                                                        u0 + delta * 0.5);
        const double y0 = centralMedian(series.feedback, preBegin, preEnd);
        const double level10 = y0 + delta * 0.1;
        const double level90 = y0 + delta * 0.9;
        double time10 = 0.0;
        double time90 = 0.0;
        if (!firstCrossing(series, postBegin, postEnd, level10, upward, cancel, &time10) ||
            !firstCrossing(series, postBegin, postEnd, level90, upward, cancel, &time90)) {
            if (cancel.isCancellationRequested()) {
                outcome.reasonCode = QStringLiteral("cancelled");
                return outcome;
            }
            continue;
        }
        EdgeMetrics& target = upward ? rising : falling;
        target.delays.push_back((time10 - commandTime) * 1000.0);
        target.riseTimes.push_back((time90 - time10) * 1000.0);
        double extreme = upward ? -std::numeric_limits<double>::infinity()
                                : std::numeric_limits<double>::infinity();
        for (int index = postBegin; index < postEnd; ++index) {
            if ((index & 1023) == 0 && cancel.isCancellationRequested()) {
                outcome.reasonCode = QStringLiteral("cancelled");
                return outcome;
            }
            if (upward) {
                extreme = qMax(extreme, series.feedback.at(index));
            } else {
                extreme = qMin(extreme, series.feedback.at(index));
            }
        }
        target.overshoots.push_back(qMax(0.0, upward ? (extreme - u1) : (u1 - extreme)) /
                                    std::abs(delta) * 100.0);
        const double tolerance = std::abs(delta) * 0.02;
        int lastOutsideTolerance = postBegin - 1;
        for (int index = postBegin; index < postEnd; ++index) {
            if ((index & 1023) == 0 && cancel.isCancellationRequested()) {
                outcome.reasonCode = QStringLiteral("cancelled");
                return outcome;
            }
            if (std::abs(series.feedback.at(index) - u1) > tolerance) {
                lastOutsideTolerance = index;
            }
        }
        const int settlingIndex = lastOutsideTolerance + 1;
        if (settlingIndex < postEnd &&
            series.timeSeconds.at(postEnd - 1) - series.timeSeconds.at(settlingIndex) >=
                halfPeriod * 0.1) {
            target.settlingTimes.push_back((series.timeSeconds.at(settlingIndex) - commandTime) * 1000.0);
        } else {
            ++target.notSettledCount;
        }
        const int steadyBegin = postBegin + (postEnd - postBegin) * 4 / 5;
        QVector<double> steady;
        for (int index = steadyBegin; index < postEnd; ++index) {
            if ((index & 1023) == 0 && cancel.isCancellationRequested()) {
                outcome.reasonCode = QStringLiteral("cancelled");
                return outcome;
            }
            steady.push_back(series.feedback.at(index) - u1);
        }
        if (!steady.isEmpty()) target.steadyErrors.push_back(mean(steady));
    }
    appendEdgeMetrics(QStringLiteral("rising"), rising, &outcome.metrics);
    appendEdgeMetrics(QStringLiteral("falling"), falling, &outcome.metrics);
    if (rising.delays.isEmpty() && falling.delays.isEmpty()) {
        outcome.state = AnalysisChannelState::Partial;
        outcome.reasonCode = QStringLiteral("insufficient_complete_edges");
    } else {
        outcome.state = AnalysisChannelState::Completed;
        if (rising.delays.isEmpty() || falling.delays.isEmpty()) {
            outcome.state = AnalysisChannelState::Partial;
            outcome.reasonCode = QStringLiteral("incomplete_edges");
        }
    }
    return outcome;
}

HelmWaveformOutcome analyzeHelmTriangle(const HelmSeries& series,
                                        const HelmAnalysisParameters& parameters,
                                        const AnalysisCancelToken& cancel)
{
    HelmWaveformOutcome outcome;
    if (cancel.isCancellationRequested()) {
        outcome.reasonCode = QStringLiteral("cancelled");
        return outcome;
    }
    if (!seriesIsUsable(series) || !finite(parameters.frequencyHz) || parameters.frequencyHz <= 0.0) {
        outcome.reasonCode = QStringLiteral("invalid_parameters");
        return outcome;
    }
    const double commandMin = minimum(series.command);
    const double commandMax = maximum(series.command);
    const double commandRange = commandMax - commandMin;
    const double actualHalfPeak = commandRange * 0.5;
    const double excitationFloor = helmExcitationFloor(parameters.configuredAmplitude);
    const double excitationTolerance = qMax(1e-12, excitationFloor * 2e-4);
    outcome.diagnostics.insert(QStringLiteral("excitationFloor"), excitationFloor);
    outcome.diagnostics.insert(QStringLiteral("actualHalfPeak"), actualHalfPeak);
    if (actualHalfPeak < excitationFloor - excitationTolerance) {
        outcome.reasonCode = QStringLiteral("weak_excitation");
        outcome.message = QStringLiteral("Triangle command is below the excitation floor");
        return outcome;
    }
    if (std::abs(actualHalfPeak - excitationFloor) <= excitationTolerance) {
        outcome.warnings.push_back(QStringLiteral("near_excitation_floor"));
    }
    struct CycleBucket {
        qint64 id = 0;
        QVector<double> absoluteSlopes;
        double deadZone = 0.0;
    };
    struct SlopeSample {
        double value = 0.0;
        int cycleBucket = -1;
    };
    QVector<SlopeSample> slopes;
    slopes.reserve(series.command.size() - 1);
    QVector<CycleBucket> cycleBuckets;
    for (int index = 1; index < series.command.size(); ++index) {
        if ((index & 1023) == 0 && cancel.isCancellationRequested()) {
            outcome.reasonCode = QStringLiteral("cancelled");
            return outcome;
        }
        const double slope = (series.command.at(index) - series.command.at(index - 1)) /
                             (series.timeSeconds.at(index) - series.timeSeconds.at(index - 1));
        const double cyclePosition = (series.timeSeconds.at(index) - series.timeSeconds.first()) *
                                     parameters.frequencyHz;
        if (!finite(cyclePosition) || cyclePosition < 0.0 ||
            cyclePosition > static_cast<double>(std::numeric_limits<qint64>::max())) {
            outcome.reasonCode = QStringLiteral("invalid_cycle_index");
            return outcome;
        }
        const qint64 cycle = static_cast<qint64>(std::floor(cyclePosition));
        if (cycleBuckets.isEmpty() || cycleBuckets.last().id != cycle) {
            CycleBucket bucket;
            bucket.id = cycle;
            cycleBuckets.push_back(bucket);
        }
        const int cycleBucket = cycleBuckets.size() - 1;
        slopes.push_back({slope, cycleBucket});
        cycleBuckets[cycleBucket].absoluteSlopes.push_back(std::abs(slope));
    }
    QVector<double> cycleDeadZones;
    cycleDeadZones.reserve(cycleBuckets.size());
    for (CycleBucket& bucket : cycleBuckets) {
        bucket.deadZone = median(bucket.absoluteSlopes) * 0.05;
        cycleDeadZones.push_back(bucket.deadZone);
    }
    struct Segment { int begin = 0; int end = 0; bool rising = false; int cycleBucket = -1; };
    QVector<Segment> segments;
    int segmentBegin = -1;
    bool segmentRising = false;
    int segmentCycleBucket = -1;
    for (int index = 0; index < slopes.size(); ++index) {
        if ((index & 1023) == 0 && cancel.isCancellationRequested()) {
            outcome.reasonCode = QStringLiteral("cancelled");
            return outcome;
        }
        const SlopeSample& sample = slopes.at(index);
        const double deadZone = cycleBuckets.at(sample.cycleBucket).deadZone;
        if (std::abs(sample.value) <= deadZone) {
            if (segmentBegin >= 0) {
                segments.push_back({segmentBegin, index, segmentRising, segmentCycleBucket});
                segmentBegin = -1;
            }
            continue;
        }
        const bool rising = sample.value > 0.0;
        if (segmentBegin < 0) {
            segmentBegin = index;
            segmentRising = rising;
            segmentCycleBucket = sample.cycleBucket;
        } else if (rising != segmentRising || sample.cycleBucket != segmentCycleBucket) {
            segments.push_back({segmentBegin, index, segmentRising, segmentCycleBucket});
            segmentBegin = index;
            segmentRising = rising;
            segmentCycleBucket = sample.cycleBucket;
        }
    }
    if (segmentBegin >= 0) {
        segments.push_back({segmentBegin, slopes.size(), segmentRising, segmentCycleBucket});
    }
    QVector<double> risingRatios;
    QVector<double> fallingRatios;
    QVector<double> risingErrors;
    QVector<double> fallingErrors;
    QVector<double> risingMaxErrors;
    QVector<double> fallingMaxErrors;
    std::array<QVector<double>, 32> risingBins;
    std::array<QVector<double>, 32> fallingBins;
    const auto appendSegment = [&](const Segment& segment) -> bool {
        const int begin = segment.begin;
        const int end = qMin(series.timeSeconds.size(), segment.end + 1);
        if (end - begin < 3 || series.timeSeconds.at(end - 1) - series.timeSeconds.at(begin) <
                               0.25 / parameters.frequencyHz) {
            return true;
        }
        QVector<double> time;
        QVector<double> command;
        QVector<double> feedback;
        for (int index = begin; index < end; ++index) {
            if (((index - begin) & 1023) == 0 && cancel.isCancellationRequested()) return false;
            const double normalized = commandRange > 0.0
                ? (series.command.at(index) - commandMin) / commandRange : 0.5;
            if (normalized <= 0.1 || normalized >= 0.9) continue;
            time.push_back(series.timeSeconds.at(index));
            command.push_back(series.command.at(index));
            feedback.push_back(series.feedback.at(index));
            const int bin = qBound(0, static_cast<int>(normalized * 32.0), 31);
            (segment.rising ? risingBins : fallingBins)[static_cast<size_t>(bin)]
                .push_back(series.feedback.at(index));
        }
        const double commandSlope = weightedLinearSlope(time, command);
        const double feedbackSlope = weightedLinearSlope(time, feedback);
        if (!finite(commandSlope) || !finite(feedbackSlope) || std::abs(commandSlope) < 1e-12) {
            return true;
        }
        QVector<double> errors;
        errors.reserve(command.size());
        double maxError = 0.0;
        for (int index = 0; index < command.size(); ++index) {
            if ((index & 1023) == 0 && cancel.isCancellationRequested()) return false;
            const double error = feedback.at(index) - command.at(index);
            errors.push_back(error);
            maxError = qMax(maxError, std::abs(error));
        }
        if (segment.rising) {
            risingRatios.push_back(feedbackSlope / commandSlope);
            risingErrors.push_back(rms(errors));
            risingMaxErrors.push_back(maxError);
        } else {
            fallingRatios.push_back(feedbackSlope / commandSlope);
            fallingErrors.push_back(rms(errors));
            fallingMaxErrors.push_back(maxError);
        }
        return true;
    };
    for (const Segment& segment : segments) {
        if (!appendSegment(segment)) {
            outcome.reasonCode = QStringLiteral("cancelled");
            return outcome;
        }
    }
    const auto metricOrUnavailable = [&](const QString& key, const QString& label,
                                         const QString& unit, const QVector<double>& values) {
        return values.isEmpty()
            ? helmUnavailableMetric(key, label, unit, AnalysisMetricStatus::NotCovered)
            : helmMetric(key, label, unit, mean(values));
    };
    outcome.metrics.push_back(metricOrUnavailable(QStringLiteral("rising_slope_ratio"),
                                                  QStringLiteral("Rising slope ratio"), QString{}, risingRatios));
    outcome.metrics.push_back(metricOrUnavailable(QStringLiteral("falling_slope_ratio"),
                                                  QStringLiteral("Falling slope ratio"), QString{}, fallingRatios));
    outcome.metrics.push_back(metricOrUnavailable(QStringLiteral("rising_rmse"),
                                                  QStringLiteral("Rising RMSE"), QStringLiteral("degree"), risingErrors));
    outcome.metrics.push_back(metricOrUnavailable(QStringLiteral("falling_rmse"),
                                                  QStringLiteral("Falling RMSE"), QStringLiteral("degree"), fallingErrors));
    outcome.metrics.push_back(metricOrUnavailable(QStringLiteral("rising_max_abs_error"),
                                                  QStringLiteral("Rising maximum error"), QStringLiteral("degree"),
                                                  risingMaxErrors));
    outcome.metrics.push_back(metricOrUnavailable(QStringLiteral("falling_max_abs_error"),
                                                  QStringLiteral("Falling maximum error"), QStringLiteral("degree"),
                                                  fallingMaxErrors));
    if (!risingRatios.isEmpty() && !fallingRatios.isEmpty()) {
        const double asymmetry = std::abs(mean(risingRatios) - mean(fallingRatios)) /
            qMax(kAbsoluteExcitationFloor, (std::abs(mean(risingRatios)) +
                                             std::abs(mean(fallingRatios))) * 0.5);
        outcome.metrics.push_back(helmMetric(QStringLiteral("slope_asymmetry"),
                                             QStringLiteral("Slope asymmetry"), QString{}, asymmetry));
    } else {
        outcome.metrics.push_back(helmUnavailableMetric(QStringLiteral("slope_asymmetry"),
                                                         QStringLiteral("Slope asymmetry"), QString{},
                                                         AnalysisMetricStatus::NotCovered));
    }
    QVector<double> directionalDifferences;
    for (int bin = 0; bin < 32; ++bin) {
        if (cancel.isCancellationRequested()) {
            outcome.reasonCode = QStringLiteral("cancelled");
            return outcome;
        }
        if (!risingBins[static_cast<size_t>(bin)].isEmpty() &&
            !fallingBins[static_cast<size_t>(bin)].isEmpty()) {
            directionalDifferences.push_back(std::abs(
                median(risingBins[static_cast<size_t>(bin)]) -
                median(fallingBins[static_cast<size_t>(bin)])));
        }
    }
    outcome.metrics.push_back(directionalDifferences.isEmpty()
        ? helmUnavailableMetric(QStringLiteral("directional_tracking_difference"),
                                QStringLiteral("Directional tracking difference (including dynamic lag)"),
                                QStringLiteral("degree"), AnalysisMetricStatus::NotCovered)
        : helmMetric(QStringLiteral("directional_tracking_difference"),
                     QStringLiteral("Directional tracking difference (including dynamic lag)"),
                     QStringLiteral("degree"), mean(directionalDifferences)));
    outcome.diagnostics.insert(QStringLiteral("turningDeadZoneSlope"),
                               cycleDeadZones.isEmpty() ? 0.0 : median(cycleDeadZones));
    outcome.diagnostics.insert(QStringLiteral("directionalBinCount"), directionalDifferences.size());
    if (risingRatios.isEmpty() && fallingRatios.isEmpty()) {
        outcome.state = AnalysisChannelState::Partial;
        outcome.reasonCode = QStringLiteral("insufficient_direction_segments");
    } else if (risingRatios.isEmpty() || fallingRatios.isEmpty()) {
        outcome.state = AnalysisChannelState::Partial;
        outcome.reasonCode = QStringLiteral("incomplete_direction_segments");
    } else {
        outcome.state = AnalysisChannelState::Completed;
    }
    return outcome;
}

} // namespace hwtest::algorithm::mbddf
