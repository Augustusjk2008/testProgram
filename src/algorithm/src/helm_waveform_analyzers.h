#pragma once

#include <algorithm/post_run_analysis.h>

#include <QVector>

namespace hwtest::algorithm::mbddf {

struct HelmSeries {
    QVector<double> timeSeconds;
    QVector<double> command;
    QVector<double> feedback;
};

struct HelmAnalysisParameters {
    int waveform = -1;
    double frequencyHz = 0.0;
    double configuredAmplitude = 0.0;
    double offset = 0.0;
    double startPhaseRad = 0.0;
    double maximumFrequencyHz = 0.0;
    double sweepDurationSeconds = 0.0;
};

struct HelmWaveformOutcome {
    AnalysisChannelState state = AnalysisChannelState::Unavailable;
    QString reasonCode;
    QString message;
    QVector<QString> warnings;
    QVector<AnalysisMetric> metrics;
    QVariantMap diagnostics;
};

void appendHelmCommonMetrics(const HelmSeries& series,
                             int analysisBegin,
                             int analysisEnd,
                             QVector<AnalysisMetric>* metrics,
                             QVariantMap* diagnostics);

HelmWaveformOutcome analyzeHelmConstant(const HelmSeries& series,
                                        const HelmAnalysisParameters& parameters,
                                        const AnalysisCancelToken& cancel);
HelmWaveformOutcome analyzeHelmSine(const HelmSeries& series,
                                    const HelmAnalysisParameters& parameters,
                                    const AnalysisCancelToken& cancel);
HelmWaveformOutcome analyzeHelmSquare(const HelmSeries& series,
                                      const HelmAnalysisParameters& parameters,
                                      const AnalysisCancelToken& cancel);
HelmWaveformOutcome analyzeHelmTriangle(const HelmSeries& series,
                                        const HelmAnalysisParameters& parameters,
                                        const AnalysisCancelToken& cancel);

AnalysisMetric helmMetric(const QString& key,
                          const QString& label,
                          const QString& unit,
                          double value,
                          AnalysisMetricStatus status = AnalysisMetricStatus::Valid,
                          const QString& detail = {});
AnalysisMetric helmUnavailableMetric(const QString& key,
                                     const QString& label,
                                     const QString& unit,
                                     AnalysisMetricStatus status,
                                     const QString& detail = {});

double helmExcitationFloor(double configuredAmplitude);

} // namespace hwtest::algorithm::mbddf
