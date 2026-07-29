#pragma once

#include "helm_waveform_analyzers.h"

#include <algorithm/post_run_analysis.h>

namespace hwtest::algorithm::mbddf {

struct HelmSweepOutcome {
    AnalysisChannelState state = AnalysisChannelState::Unavailable;
    QString reasonCode;
    QString message;
    QVector<QString> warnings;
    QVector<AnalysisMetric> metrics;
    QVector<AnalysisBodePoint> bodePoints;
    QVariantMap diagnostics;
};

HelmSweepOutcome analyzeHelmSweep(const HelmSeries& series,
                                  const HelmAnalysisParameters& parameters,
                                  const AnalysisCancelToken& cancel);

} // namespace hwtest::algorithm::mbddf
