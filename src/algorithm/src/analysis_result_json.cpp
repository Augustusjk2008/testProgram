#include <algorithm/post_run_analysis.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace hwtest::algorithm::mbddf {
namespace {

constexpr quint64 kMaxJsonSafeInteger = (quint64{1} << 53) - 1;

bool safeInteger(quint64 value)
{
    return value <= kMaxJsonSafeInteger;
}

bool appendVariant(const QVariant& value, QJsonValue* target, AnalysisError* error)
{
    if (target == nullptr) return false;
    if (!value.isValid() || value.isNull()) {
        *target = QJsonValue(QJsonValue::Null);
        return true;
    }
    switch (value.userType()) {
    case QMetaType::Double:
    case QMetaType::Float: {
        const double number = value.toDouble();
        if (!std::isfinite(number)) {
            if (error != nullptr) *error = {QStringLiteral("non_finite_result"),
                                            QStringLiteral("Result contains a non-finite number")};
            return false;
        }
        *target = QJsonValue(number);
        return true;
    }
    case QMetaType::ULongLong: {
        const quint64 number = value.toULongLong();
        if (!safeInteger(number)) {
            if (error != nullptr) *error = {QStringLiteral("unsafe_integer"),
                                            QStringLiteral("Result contains an unsafe JSON integer")};
            return false;
        }
        *target = QJsonValue(static_cast<double>(number));
        return true;
    }
    case QMetaType::LongLong: {
        const qint64 number = value.toLongLong();
        if (number < -static_cast<qint64>(kMaxJsonSafeInteger) ||
            number > static_cast<qint64>(kMaxJsonSafeInteger)) {
            if (error != nullptr) *error = {QStringLiteral("unsafe_integer"),
                                            QStringLiteral("Result contains an unsafe JSON integer")};
            return false;
        }
        *target = QJsonValue(static_cast<double>(number));
        return true;
    }
    case QMetaType::QVariantMap: {
        QJsonObject object;
        const QVariantMap map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            QJsonValue child;
            if (!appendVariant(it.value(), &child, error)) return false;
            object.insert(it.key(), child);
        }
        *target = object;
        return true;
    }
    case QMetaType::QVariantList: {
        QJsonArray array;
        for (const QVariant& item : value.toList()) {
            QJsonValue child;
            if (!appendVariant(item, &child, error)) return false;
            array.push_back(child);
        }
        *target = array;
        return true;
    }
    default:
        *target = QJsonValue::fromVariant(value);
        return true;
    }
}

bool metricObject(const AnalysisMetric& metric, QJsonObject* object, AnalysisError* error)
{
    if (object == nullptr) return false;
    object->insert(QStringLiteral("key"), metric.key);
    object->insert(QStringLiteral("label"), metric.label);
    object->insert(QStringLiteral("unit"), metric.unit);
    object->insert(QStringLiteral("status"), analysisMetricStatusName(metric.status));
    object->insert(QStringLiteral("detail"), metric.detail);
    if (metric.hasValue) {
        if (!std::isfinite(metric.value)) {
            if (error != nullptr) *error = {QStringLiteral("non_finite_result"),
                                            QStringLiteral("Metric '%1' is non-finite").arg(metric.key)};
            return false;
        }
        object->insert(QStringLiteral("value"), metric.value);
    } else {
        object->insert(QStringLiteral("value"), QJsonValue(QJsonValue::Null));
    }
    return true;
}

bool metricArray(const QVector<AnalysisMetric>& metrics, QJsonArray* array, AnalysisError* error)
{
    if (array == nullptr) return false;
    for (const AnalysisMetric& metric : metrics) {
        QJsonObject object;
        if (!metricObject(metric, &object, error)) return false;
        array->push_back(object);
    }
    return true;
}

bool resultObject(const AnalysisResult& result, QJsonObject* object, AnalysisError* error)
{
    if (object == nullptr) return false;
    if (!safeInteger(result.identity.analysisGeneration) ||
        !safeInteger(result.acceptedSampleCount) || !safeInteger(result.lateSampleCount)) {
        if (error != nullptr) *error = {QStringLiteral("unsafe_integer"),
                                        QStringLiteral("Analysis identity or sample count is not JSON-safe")};
        return false;
    }
    object->insert(QStringLiteral("schemaVersion"), result.schemaVersion);
    object->insert(QStringLiteral("analyzerId"), result.analyzerId);
    object->insert(QStringLiteral("analyzerVersion"), result.analyzerVersion);
    object->insert(QStringLiteral("state"), analysisStateName(result.state));
    object->insert(QStringLiteral("reasonCode"), result.reasonCode);
    object->insert(QStringLiteral("message"), result.message);
    object->insert(QStringLiteral("taskId"), result.identity.taskId);
    object->insert(QStringLiteral("analysisGeneration"),
                   static_cast<double>(result.identity.analysisGeneration));
    object->insert(QStringLiteral("acceptedSampleCount"),
                   static_cast<double>(result.acceptedSampleCount));
    object->insert(QStringLiteral("lateSampleCount"), static_cast<double>(result.lateSampleCount));
    object->insert(QStringLiteral("normalizedInputSha256"),
                   QString::fromLatin1(result.normalizedInputSha256.toHex()));
    if (result.generatedAtUtcUs < 0 ||
        result.generatedAtUtcUs > static_cast<qint64>(kMaxJsonSafeInteger)) {
        if (error != nullptr) *error = {QStringLiteral("unsafe_integer"),
                                        QStringLiteral("Result generation time is not JSON-safe")};
        return false;
    }
    object->insert(QStringLiteral("generatedAtUtcUs"),
                   static_cast<double>(result.generatedAtUtcUs));
    object->insert(QStringLiteral("reproducible"), result.reproducible);
    object->insert(QStringLiteral("sourceArtifact"), QJsonObject{
        {QStringLiteral("path"), result.sourceArtifactPath},
        {QStringLiteral("sha256"), QString::fromLatin1(result.sourceArtifactSha256.toHex())},
    });
    QJsonValue parameters;
    if (!appendVariant(result.effectiveRunParameters, &parameters, error)) return false;
    object->insert(QStringLiteral("effectiveRunParameters"), parameters);
    QJsonObject termination;
    termination.insert(QStringLiteral("kind"), analysisTerminationKindName(result.termination.kind));
    termination.insert(QStringLiteral("reasonCode"), result.termination.reasonCode);
    termination.insert(QStringLiteral("message"), result.termination.message);
    if (result.termination.endStreamElapsedUs >= 0 &&
        result.termination.endStreamElapsedUs <= static_cast<qint64>(kMaxJsonSafeInteger)) {
        termination.insert(QStringLiteral("endStreamElapsedUs"),
                           static_cast<double>(result.termination.endStreamElapsedUs));
    } else {
        termination.insert(QStringLiteral("endStreamElapsedUs"), QJsonValue(QJsonValue::Null));
    }
    object->insert(QStringLiteral("termination"), termination);
    QJsonArray channels;
    const bool suppressChannels = result.state == AnalysisState::Failed ||
                                  result.state == AnalysisState::Cancelled;
    for (const AnalysisChannelResult& channel : result.channels) {
        if (suppressChannels) break;
        QJsonObject channelObject;
        channelObject.insert(QStringLiteral("channel"), channel.channel);
        channelObject.insert(QStringLiteral("enabled"), channel.enabled);
        channelObject.insert(QStringLiteral("status"), analysisChannelStateName(channel.state));
        channelObject.insert(QStringLiteral("reasonCode"), channel.reasonCode);
        channelObject.insert(QStringLiteral("message"), channel.message);
        QJsonArray warnings;
        for (const QString& warning : channel.warnings) warnings.push_back(warning);
        channelObject.insert(QStringLiteral("warnings"), warnings);
        channelObject.insert(QStringLiteral("omittedWarningCount"), 0);
        QJsonArray common;
        QJsonArray waveform;
        if (!metricArray(channel.commonMetrics, &common, error) ||
            !metricArray(channel.waveformMetrics, &waveform, error)) return false;
        channelObject.insert(QStringLiteral("commonMetrics"), common);
        channelObject.insert(QStringLiteral("waveformMetrics"), waveform);
        QJsonObject bode;
        QJsonArray frequency;
        QJsonArray magnitude;
        QJsonArray phase;
        QJsonArray pointStatus;
        for (const AnalysisBodePoint& point : channel.bodePoints) {
            if (!std::isfinite(point.frequencyHz)) {
                if (error != nullptr) *error = {QStringLiteral("non_finite_result"),
                                                QStringLiteral("Bode frequency is non-finite")};
                return false;
            }
            frequency.push_back(point.frequencyHz);
            if (point.hasMagnitude) {
                if (!std::isfinite(point.magnitudeDb)) {
                    if (error != nullptr) *error = {QStringLiteral("non_finite_result"),
                                                    QStringLiteral("Bode magnitude is non-finite")};
                    return false;
                }
                magnitude.push_back(point.magnitudeDb);
            } else {
                magnitude.push_back(QJsonValue(QJsonValue::Null));
            }
            if (point.hasPhase) {
                if (!std::isfinite(point.phaseDeg)) {
                    if (error != nullptr) *error = {QStringLiteral("non_finite_result"),
                                                    QStringLiteral("Bode phase is non-finite")};
                    return false;
                }
                phase.push_back(point.phaseDeg);
            } else {
                phase.push_back(QJsonValue(QJsonValue::Null));
            }
            pointStatus.push_back(analysisPointStatusName(point.status));
        }
        bode.insert(QStringLiteral("frequencyHz"), frequency);
        bode.insert(QStringLiteral("magnitudeDb"), magnitude);
        bode.insert(QStringLiteral("phaseDeg"), phase);
        bode.insert(QStringLiteral("pointStatus"), pointStatus);
        channelObject.insert(QStringLiteral("bode"), bode);
        QJsonValue diagnostics;
        if (!appendVariant(channel.diagnostics, &diagnostics, error)) return false;
        channelObject.insert(QStringLiteral("diagnostics"), diagnostics);
        channels.push_back(channelObject);
    }
    object->insert(QStringLiteral("channels"), channels);
    QJsonValue diagnostics;
    if (!appendVariant(result.diagnostics, &diagnostics, error)) return false;
    object->insert(QStringLiteral("diagnostics"), diagnostics);
    return true;
}

} // namespace

QByteArray serializeAnalysisResultJson(const AnalysisResult& result, QString* error)
{
    AnalysisError details;
    QByteArray output;
    if (!serializeAnalysisResultJson(result, &output, &details)) {
        if (error != nullptr) *error = details.message;
        return {};
    }
    if (error != nullptr) error->clear();
    return output;
}

bool serializeAnalysisResultJson(const AnalysisResult& result, QByteArray* utf8,
                                 AnalysisError* error)
{
    if (error != nullptr) *error = {};
    if (utf8 == nullptr) {
        if (error != nullptr) *error = {QStringLiteral("invalid_output"),
                                        QStringLiteral("JSON output buffer is null")};
        return false;
    }
    QJsonObject object;
    if (!resultObject(result, &object, error)) return false;
    *utf8 = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return true;
}

} // namespace hwtest::algorithm::mbddf
