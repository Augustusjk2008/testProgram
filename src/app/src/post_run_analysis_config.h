#pragma once

#include <QString>
#include <QVariantMap>
#include <QtGlobal>

namespace hwtest::app {

struct PostRunAnalysisConfig {
    quint64 maxCaptureBytes = 536870912ULL;
    quint64 maxInputSamples = 5000000ULL;
    quint64 maxAnalysisDurationS = 3600ULL;
    quint64 minFreeBytes = 1073741824ULL;
    int analysisTimeoutMs = 30000;
    int analysisShutdownTimeoutMs = 5000;
    quint64 maxResultBytes = 16777216ULL;
    int maxProjectedPoints = 256;
    int maxProjectedBytes = 16384;
    int maxAnalysisSummaryBytes = 8192;
    int diagnosticRetentionDays = 7;
};

bool parsePostRunAnalysisConfig(const QVariantMap& halConfig,
                                PostRunAnalysisConfig* output,
                                QString* error);

} // namespace hwtest::app
