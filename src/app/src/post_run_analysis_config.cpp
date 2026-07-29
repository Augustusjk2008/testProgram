#include "post_run_analysis_config.h"

#include <QMetaType>
#include <QSet>

#include <cmath>
#include <limits>

namespace hwtest::app {
namespace {

bool parseInteger(const QVariantMap& values,
                  const QString& key,
                  quint64 minimum,
                  quint64 maximum,
                  quint64* output,
                  QString* error)
{
    if (!values.contains(key)) return true;
    const QVariant value = values.value(key);
    const int type = value.userType();
    const bool numeric = type == QMetaType::Int || type == QMetaType::UInt ||
        type == QMetaType::LongLong || type == QMetaType::ULongLong ||
        type == QMetaType::Double || type == QMetaType::Float;
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!numeric || !ok || !std::isfinite(number) ||
        std::floor(number) != number ||
        number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum)) {
        if (error != nullptr) {
            *error = QStringLiteral("dataStorage.analysis.%1 must be an integer in %2..%3")
                         .arg(key)
                         .arg(minimum)
                         .arg(maximum);
        }
        return false;
    }
    if (output != nullptr) *output = static_cast<quint64>(number);
    return true;
}

bool parseInt(const QVariantMap& values,
              const QString& key,
              int minimum,
              int maximum,
              int* output,
              QString* error)
{
    quint64 parsed = output == nullptr ? 0 : static_cast<quint64>(*output);
    if (!parseInteger(values, key, static_cast<quint64>(minimum),
                      static_cast<quint64>(maximum), &parsed, error)) {
        return false;
    }
    if (values.contains(key) && output != nullptr) {
        *output = static_cast<int>(parsed);
    }
    return true;
}

} // namespace

bool parsePostRunAnalysisConfig(const QVariantMap& halConfig,
                                PostRunAnalysisConfig* output,
                                QString* error)
{
    if (output == nullptr) {
        if (error != nullptr) *error = QStringLiteral("analysis output is null");
        return false;
    }
    if (error != nullptr) error->clear();

    PostRunAnalysisConfig parsed;
    const QVariantMap values = halConfig.value(QStringLiteral("dataStorage")).toMap()
                                   .value(QStringLiteral("analysis")).toMap();
    static const QSet<QString> allowed{
        QStringLiteral("maxCaptureBytes"),
        QStringLiteral("maxInputSamples"),
        QStringLiteral("maxAnalysisDurationS"),
        QStringLiteral("minFreeBytes"),
        QStringLiteral("analysisTimeoutMs"),
        QStringLiteral("analysisShutdownTimeoutMs"),
        QStringLiteral("maxResultBytes"),
        QStringLiteral("maxProjectedPoints"),
        QStringLiteral("maxProjectedBytes"),
        QStringLiteral("maxAnalysisSummaryBytes"),
        QStringLiteral("diagnosticRetentionDays"),
    };
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!allowed.contains(it.key())) {
            if (error != nullptr) {
                *error = QStringLiteral("Unknown dataStorage.analysis field '%1'")
                             .arg(it.key());
            }
            return false;
        }
    }

    constexpr quint64 maxExactInteger = 9007199254740991ULL;
    if (!parseInteger(values, QStringLiteral("maxCaptureBytes"), 1,
                      maxExactInteger, &parsed.maxCaptureBytes, error) ||
        !parseInteger(values, QStringLiteral("maxInputSamples"), 1,
                      maxExactInteger, &parsed.maxInputSamples, error) ||
        !parseInteger(values, QStringLiteral("maxAnalysisDurationS"), 1,
                      maxExactInteger, &parsed.maxAnalysisDurationS, error) ||
        !parseInteger(values, QStringLiteral("minFreeBytes"), 0,
                      maxExactInteger, &parsed.minFreeBytes, error) ||
        !parseInt(values, QStringLiteral("analysisTimeoutMs"), 1,
                  std::numeric_limits<int>::max(), &parsed.analysisTimeoutMs,
                  error) ||
        !parseInt(values, QStringLiteral("analysisShutdownTimeoutMs"), 1,
                  std::numeric_limits<int>::max(),
                  &parsed.analysisShutdownTimeoutMs, error) ||
        !parseInteger(values, QStringLiteral("maxResultBytes"), 1,
                      maxExactInteger, &parsed.maxResultBytes, error) ||
        !parseInt(values, QStringLiteral("maxProjectedPoints"), 1, 256,
                  &parsed.maxProjectedPoints, error) ||
        !parseInt(values, QStringLiteral("maxProjectedBytes"), 1, 16384,
                  &parsed.maxProjectedBytes, error) ||
        !parseInt(values, QStringLiteral("maxAnalysisSummaryBytes"), 1, 8192,
                  &parsed.maxAnalysisSummaryBytes, error) ||
        !parseInt(values, QStringLiteral("diagnosticRetentionDays"), 0, 36500,
                  &parsed.diagnosticRetentionDays, error)) {
        return false;
    }

    *output = parsed;
    return true;
}

} // namespace hwtest::app
