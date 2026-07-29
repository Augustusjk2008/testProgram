#include "web_protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

#include <cmath>

namespace hwtest::app::web {

namespace {

constexpr qint64 kMaxJsonSafeInteger = 9007199254740991LL;

bool isJsonSafeNonNegativeInteger(qint64 value)
{
    return value >= 0 && value <= kMaxJsonSafeInteger;
}

bool isJsonSafeNonNegativeInteger(quint64 value)
{
    return value <= static_cast<quint64>(kMaxJsonSafeInteger);
}

QJsonObject analysisMetricObject(const AnalysisMetric& metric)
{
    QJsonValue value = QJsonValue::Null;
    if (metric.hasValue && std::isfinite(metric.value)) {
        value = metric.value;
    }
    return QJsonObject{
        {QStringLiteral("key"), metric.key},
        {QStringLiteral("label"), metric.label},
        {QStringLiteral("unit"), metric.unit},
        {QStringLiteral("status"), metric.status},
        {QStringLiteral("value"), value},
        {QStringLiteral("detail"), metric.detail},
    };
}

QJsonObject analysisChannelSummaryObject(const AnalysisChannelSummary& summary)
{
    QJsonArray warnings;
    for (const QString& warning : summary.warnings) warnings.push_back(warning);
    QJsonArray commonMetrics;
    for (const AnalysisMetric& metric : summary.commonMetrics) {
        commonMetrics.push_back(analysisMetricObject(metric));
    }
    QJsonArray waveformMetrics;
    for (const AnalysisMetric& metric : summary.waveformMetrics) {
        waveformMetrics.push_back(analysisMetricObject(metric));
    }
    return QJsonObject{
        {QStringLiteral("channel"), summary.channel},
        {QStringLiteral("enabled"), summary.enabled},
        {QStringLiteral("status"), summary.status},
        {QStringLiteral("warnings"), warnings},
        {QStringLiteral("omittedWarningCount"), summary.omittedWarningCount},
        {QStringLiteral("commonMetrics"), commonMetrics},
        {QStringLiteral("waveformMetrics"), waveformMetrics},
        {QStringLiteral("bodeAvailable"), summary.bodeAvailable},
        {QStringLiteral("bodePointCount"), summary.bodePointCount},
        {QStringLiteral("reasonCode"), summary.reasonCode},
        {QStringLiteral("message"), summary.message},
    };
}

QJsonObject analysisSnapshotObject(const PostRunAnalysisSnapshot& analysis)
{
    QJsonArray channelSummaries;
    for (const AnalysisChannelSummary& summary : analysis.channelSummaries) {
        channelSummaries.push_back(analysisChannelSummaryObject(summary));
    }
    return QJsonObject{
        {QStringLiteral("supported"), analysis.supported},
        {QStringLiteral("analyzerId"), analysis.analyzerId},
        {QStringLiteral("schemaVersion"), analysis.schemaVersion},
        {QStringLiteral("taskId"), analysis.taskId},
        {QStringLiteral("analysisGeneration"),
         static_cast<double>(analysis.analysisGeneration)},
        {QStringLiteral("state"), analysis.state},
        {QStringLiteral("progress"), analysis.progress},
        {QStringLiteral("stage"), analysis.stage},
        {QStringLiteral("message"), analysis.message},
        {QStringLiteral("reasonCode"), analysis.reasonCode},
        {QStringLiteral("resultFilePath"), analysis.resultFilePath},
        {QStringLiteral("diagnosticInputFilePath"),
         analysis.diagnosticInputFilePath},
        {QStringLiteral("sourceSummary"),
         QJsonObject::fromVariantMap(analysis.sourceSummary)},
        {QStringLiteral("channelSummaries"), channelSummaries},
    };
}

ProtocolParseResult failure(const QString& code, const QString& message)
{
    ProtocolParseResult result;
    result.code = code;
    result.message = message;
    return result;
}

bool isKnownAction(const QString& action)
{
    static const QSet<QString> actions{
        QStringLiteral("load"),
        QStringLiteral("testConfigs"),
        QStringLiteral("selectTest"),
        QStringLiteral("snapshot"),
        QStringLiteral("analysisResult"),
        QStringLiteral("controls"),
        QStringLiteral("ports"),
        QStringLiteral("selectControl"),
        QStringLiteral("selectSerialPort"),
        QStringLiteral("prepare"),
        QStringLiteral("start"),
        QStringLiteral("pause"),
        QStringLiteral("resume"),
        QStringLiteral("setDigitalStimulus"),
        QStringLiteral("resetDigitalStimulus"),
        QStringLiteral("setTelemetryDelivery"),
        QStringLiteral("stop"),
        QStringLiteral("disconnect"),
        QStringLiteral("quit"),
    };
    return actions.contains(action);
}

QJsonObject descriptorObject(const TestDescriptor& descriptor)
{
    QJsonArray supportedRunModes;
    for (const QString& mode : descriptor.supportedRunModes) {
        supportedRunModes.push_back(mode);
    }
    QJsonArray measurements;
    for (const TestMeasurementDescriptor& measurement : descriptor.measurements) {
        measurements.push_back(QJsonObject{
            {QStringLiteral("id"), measurement.id},
            {QStringLiteral("label"), measurement.label},
            {QStringLiteral("unit"), measurement.unit},
            {QStringLiteral("primary"), measurement.primary},
        });
    }
    QJsonArray runParameters;
    for (const TestRunParameterDescriptor& parameter : descriptor.runParameters) {
        QJsonArray choices;
        for (const TestRunParameterChoice& choice : parameter.choices) {
            choices.push_back(QJsonObject{
                {QStringLiteral("value"), QJsonValue::fromVariant(choice.value)},
                {QStringLiteral("label"), choice.label},
            });
        }
        QJsonObject projected{
            {QStringLiteral("id"), parameter.id},
            {QStringLiteral("label"), parameter.label},
            {QStringLiteral("description"), parameter.description},
            {QStringLiteral("kind"), parameter.kind},
            {QStringLiteral("unit"), parameter.unit},
            {QStringLiteral("required"), parameter.required},
            {QStringLiteral("minimumExclusive"), parameter.minimumExclusive},
            {QStringLiteral("maximumExclusive"), parameter.maximumExclusive},
            {QStringLiteral("choices"), choices},
        };
        if (parameter.minimum.isValid()) {
            projected.insert(QStringLiteral("minimum"),
                             QJsonValue::fromVariant(parameter.minimum));
        }
        if (parameter.maximum.isValid()) {
            projected.insert(QStringLiteral("maximum"),
                             QJsonValue::fromVariant(parameter.maximum));
        }
        if (!parameter.visibleWhenParameter.isEmpty()) {
            projected.insert(
                QStringLiteral("visibleWhen"),
                QJsonObject{
                    {QStringLiteral("parameter"), parameter.visibleWhenParameter},
                    {QStringLiteral("equals"),
                     QJsonValue::fromVariant(parameter.visibleWhenEquals)},
                });
        }
        runParameters.push_back(projected);
    }
    const QJsonObject postRunAnalysis{
        {QStringLiteral("supported"), descriptor.postRunAnalysis.supported},
        {QStringLiteral("analyzerId"), descriptor.postRunAnalysis.analyzerId},
        {QStringLiteral("schemaVersion"), descriptor.postRunAnalysis.schemaVersion},
    };
    return QJsonObject{
        {QStringLiteral("configId"), descriptor.configId},
        {QStringLiteral("productModel"), descriptor.productModel},
        {QStringLiteral("productName"), descriptor.productName},
        {QStringLiteral("configVersion"), descriptor.configVersion},
        {QStringLiteral("stepId"), descriptor.stepId},
        {QStringLiteral("testItemId"), descriptor.testItemId},
        {QStringLiteral("algorithmId"), descriptor.algorithmId},
        {QStringLiteral("title"), descriptor.title},
        {QStringLiteral("description"), descriptor.description},
        {QStringLiteral("supportedRunModes"), supportedRunModes},
        {QStringLiteral("measurements"), measurements},
        {QStringLiteral("runParameterSchemaVersion"),
         descriptor.runParameterSchemaVersion},
        {QStringLiteral("runParameters"), runParameters},
        {QStringLiteral("runParameterDefaults"),
         QJsonObject::fromVariantMap(descriptor.runParameterDefaults)},
        {QStringLiteral("postRunAnalysis"), postRunAnalysis},
    };
}

QJsonObject snapshotObject(const ApplicationSnapshot& snapshot)
{
    QJsonObject object;
    object.insert(QStringLiteral("phase"), snapshot.phase);
    object.insert(QStringLiteral("testState"), snapshot.testState);
    object.insert(QStringLiteral("controlResourceId"), snapshot.controlResourceId);
    object.insert(QStringLiteral("providerId"), snapshot.providerId);
    object.insert(QStringLiteral("serialPortName"), snapshot.serialPortName);
    object.insert(QStringLiteral("taskId"), snapshot.taskId);
    object.insert(QStringLiteral("stepId"), snapshot.stepId);
    object.insert(QStringLiteral("testItemId"), snapshot.testItemId);
    object.insert(QStringLiteral("algorithmId"), snapshot.algorithmId);
    object.insert(QStringLiteral("progress"), snapshot.progress);
    object.insert(QStringLiteral("progressStep"), snapshot.progressStep);
    object.insert(QStringLiteral("hasResult"), snapshot.hasResult);
    object.insert(QStringLiteral("verdict"), snapshot.verdict);
    object.insert(QStringLiteral("errorCode"), snapshot.errorCode);
    object.insert(QStringLiteral("message"), snapshot.message);
    object.insert(QStringLiteral("attempts"), snapshot.attempts);
    object.insert(QStringLiteral("rawData"),
                  QJsonObject::fromVariantMap(snapshot.rawData));
    object.insert(QStringLiteral("effectiveRunParameters"),
                  QJsonObject::fromVariantMap(snapshot.effectiveRunParameters));
    object.insert(QStringLiteral("runMode"), snapshot.runMode);
    object.insert(QStringLiteral("intervalMs"), snapshot.intervalMs);
    object.insert(QStringLiteral("maxCycles"),
                  static_cast<double>(snapshot.maxCycles));
    object.insert(QStringLiteral("cycleIndex"),
                  static_cast<double>(snapshot.cycleIndex));
    object.insert(QStringLiteral("sampleCount"),
                  static_cast<double>(snapshot.sampleCount));
    object.insert(QStringLiteral("dataSaveEnabled"), snapshot.dataSaveEnabled);
    object.insert(QStringLiteral("dataFilePath"), snapshot.dataFilePath);
    object.insert(QStringLiteral("dataSaveError"), snapshot.dataSaveError);
    object.insert(QStringLiteral("descriptor"),
                  descriptorObject(snapshot.descriptor));
    object.insert(QStringLiteral("digitalStimulus"),
                  digitalStimulusObject(snapshot.digitalStimulus));
    object.insert(QStringLiteral("analysis"),
                  analysisSnapshotObject(snapshot.analysis));
    return object;
}

QJsonObject sampleObject(const ApplicationSample& sample)
{
    if (!isJsonSafeNonNegativeInteger(sample.timestampUs) ||
        sample.streamElapsedUs > kMaxJsonSafeInteger) {
        return {};
    }
    QJsonObject object{
        {QStringLiteral("taskId"), sample.taskId},
        {QStringLiteral("stepId"), sample.stepId},
        {QStringLiteral("channelId"), sample.channelId},
        {QStringLiteral("timestampUs"), static_cast<double>(sample.timestampUs)},
        {QStringLiteral("cycleIndex"), static_cast<double>(sample.cycleIndex)},
        {QStringLiteral("values"), QJsonObject::fromVariantMap(sample.values)},
        {QStringLiteral("tags"), QJsonObject::fromVariantMap(sample.tags)},
    };
    if (sample.streamElapsedUs >= 0) {
        object.insert(QStringLiteral("streamElapsedUs"),
                      static_cast<double>(sample.streamElapsedUs));
    }
    return object;
}

} // namespace

QJsonObject digitalStimulusObject(const DigitalStimulusSnapshot& stimulus)
{
    if (!digitalStimulusSupportedByVersionOne(stimulus)) {
        return QJsonObject{
            {QStringLiteral("available"), false},
            {QStringLiteral("configured"), false},
            {QStringLiteral("switches"), QJsonArray{}},
            {QStringLiteral("appliedMask"), 0.0},
            {QStringLiteral("revision"), 0.0},
            {QStringLiteral("lastWriteTimestampUs"), 0.0},
            {QStringLiteral("settlingMs"), stimulus.settlingMs},
            {QStringLiteral("errorCode"), QStringLiteral("CapabilityUnsupported")},
            {QStringLiteral("message"),
             QStringLiteral("WebSocket v1 supports digital stimulus bits 0..15 only")},
        };
    }
    QJsonArray switches;
    for (const DigitalSwitchDescriptor& descriptor : stimulus.switches) {
        switches.push_back(QJsonObject{
            {QStringLiteral("switchId"), descriptor.switchId},
            {QStringLiteral("dutBit"), descriptor.dutBit},
            {QStringLiteral("label"), descriptor.label},
            {QStringLiteral("activeLevel"), descriptor.activeLevel},
        });
    }
    return QJsonObject{
        {QStringLiteral("available"), stimulus.available},
        {QStringLiteral("configured"), stimulus.configured},
        {QStringLiteral("switches"), switches},
        {QStringLiteral("appliedMask"), static_cast<double>(stimulus.appliedMask)},
        {QStringLiteral("revision"), static_cast<double>(stimulus.revision)},
        {QStringLiteral("lastWriteTimestampUs"),
         static_cast<double>(stimulus.lastWriteTimestampUs)},
        {QStringLiteral("settlingMs"), stimulus.settlingMs},
        {QStringLiteral("errorCode"), stimulus.errorCode},
        {QStringLiteral("message"), stimulus.message},
    };
}

bool digitalStimulusSupportedByVersionOne(const DigitalStimulusSnapshot& stimulus)
{
    constexpr quint64 mask16 = 0xFFFFu;
    constexpr quint64 maxSafeInteger = 9007199254740991ULL;
    if (stimulus.switches.size() > 16 || (stimulus.appliedMask & ~mask16) != 0 ||
        stimulus.revision > maxSafeInteger) {
        return false;
    }
    for (const DigitalSwitchDescriptor& descriptor : stimulus.switches) {
        if (descriptor.dutBit < 0 || descriptor.dutBit > 15) {
            return false;
        }
    }
    return true;
}

ProtocolParseResult parseRequest(const QString& text)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(QStringLiteral("invalid_json"),
                       QStringLiteral("Message must be a JSON object"));
    }

    const QJsonObject object = document.object();
    QString requestId;
    const QJsonValue possibleId = object.value(QStringLiteral("id"));
    if (possibleId.isString() && !possibleId.toString().trimmed().isEmpty()) {
        requestId = possibleId.toString();
    }
    const auto envelopeFailure = [&requestId](const QString& code,
                                              const QString& message) {
        ProtocolParseResult result = failure(code, message);
        result.request.id = requestId;
        return result;
    };
    if (!object.contains(QStringLiteral("v"))) {
        return envelopeFailure(QStringLiteral("missing_field"),
                               QStringLiteral("Field 'v' is required"));
    }
    const QJsonValue version = object.value(QStringLiteral("v"));
    if (!version.isDouble() || version.toDouble() != 1.0) {
        return envelopeFailure(QStringLiteral("unsupported_version"),
                               QStringLiteral("Only protocol version 1 is supported"));
    }

    if (!object.contains(QStringLiteral("type"))) {
        return envelopeFailure(QStringLiteral("missing_field"),
                               QStringLiteral("Field 'type' is required"));
    }
    const QJsonValue type = object.value(QStringLiteral("type"));
    if (!type.isString() || type.toString() != QStringLiteral("request")) {
        return envelopeFailure(QStringLiteral("invalid_envelope"),
                               QStringLiteral("Field 'type' must be 'request'"));
    }

    if (!object.contains(QStringLiteral("id"))) {
        return envelopeFailure(QStringLiteral("missing_field"),
                               QStringLiteral("Field 'id' is required"));
    }
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (!id.isString()) {
        return envelopeFailure(QStringLiteral("invalid_envelope"),
                               QStringLiteral("Field 'id' must be a string"));
    }
    if (id.toString().trimmed().isEmpty()) {
        return envelopeFailure(QStringLiteral("missing_field"),
                               QStringLiteral("Field 'id' must not be empty"));
    }

    if (!object.contains(QStringLiteral("action"))) {
        return envelopeFailure(QStringLiteral("missing_field"),
                               QStringLiteral("Field 'action' is required"));
    }
    const QJsonValue action = object.value(QStringLiteral("action"));
    if (!action.isString()) {
        return envelopeFailure(QStringLiteral("invalid_envelope"),
                               QStringLiteral("Field 'action' must be a string"));
    }
    if (action.toString().trimmed().isEmpty()) {
        return envelopeFailure(QStringLiteral("missing_field"),
                               QStringLiteral("Field 'action' must not be empty"));
    }
    if (!isKnownAction(action.toString())) {
        return envelopeFailure(QStringLiteral("unknown_action"),
                               QStringLiteral("Unknown action: %1")
                                   .arg(action.toString()));
    }

    if (!object.contains(QStringLiteral("params"))) {
        return envelopeFailure(QStringLiteral("missing_field"),
                               QStringLiteral("Field 'params' is required"));
    }
    const QJsonValue params = object.value(QStringLiteral("params"));
    if (!params.isObject()) {
        return envelopeFailure(QStringLiteral("invalid_envelope"),
                               QStringLiteral("Field 'params' must be an object"));
    }

    ProtocolParseResult result;
    result.ok = true;
    result.request.id = id.toString();
    result.request.action = action.toString();
    result.request.params = params.toObject();
    return result;
}

QJsonObject makeHello(int maxBatchSamples,
                      qint64 maxBatchBytes,
                      int maxBatchLatencyMs,
                      int snapshotIntervalMs)
{
    const QJsonObject telemetryBatch{
        {QStringLiteral("version"), 1},
        {QStringLiteral("maxSamples"), maxBatchSamples},
        {QStringLiteral("maxBytes"), static_cast<double>(maxBatchBytes)},
        {QStringLiteral("maxLatencyMs"), maxBatchLatencyMs},
        {QStringLiteral("snapshotIntervalMs"), snapshotIntervalMs},
    };
    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QStringLiteral("hello")},
        {QStringLiteral("server"), QStringLiteral("hwtest_web")},
        {QStringLiteral("protocolVersion"), 1},
        {QStringLiteral("capabilities"),
         QJsonObject{{QStringLiteral("telemetryBatch"), telemetryBatch}}},
    };
}

QJsonObject makeReply(const QString& id,
                      const ActionResult& result,
                      const QJsonObject& data)
{
    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QStringLiteral("reply")},
        {QStringLiteral("id"), id},
        {QStringLiteral("ok"), result.ok},
        {QStringLiteral("code"), result.code},
        {QStringLiteral("message"), result.message},
        {QStringLiteral("data"), data},
    };
}

QJsonObject makeAnalysisResultReply(const QString& id,
                                    const ActionResult& result,
                                    const QJsonObject& data)
{
    QJsonObject reply = makeReply(id, result, data);
    if (!result.ok ||
        QJsonDocument(reply).toJson(QJsonDocument::Compact).size() <= 16384) {
        return reply;
    }
    return makeReply(
        id,
        ActionResult{false,
                     QStringLiteral("analysis_projection_too_large"),
                     QStringLiteral("The complete compact analysis reply exceeds 16384 bytes")});
}

QJsonObject makeSnapshot(quint64 sequence,
                         const ApplicationSnapshot& snapshot)
{
    if (!isJsonSafeNonNegativeInteger(sequence) ||
        snapshot.analysis.analysisGeneration >
        static_cast<quint64>(kMaxJsonSafeInteger)) {
        return {};
    }
    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QStringLiteral("snapshot")},
        {QStringLiteral("seq"), static_cast<double>(sequence)},
        {QStringLiteral("snapshot"), snapshotObject(snapshot)},
    };
}

QJsonObject makeSample(quint64 sequence,
                       const ApplicationSample& sample)
{
    if (!isJsonSafeNonNegativeInteger(sequence)) {
        return {};
    }
    const QJsonObject projectedSample = sampleObject(sample);
    if (projectedSample.isEmpty()) {
        return {};
    }
    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QStringLiteral("sample")},
        {QStringLiteral("seq"), static_cast<double>(sequence)},
        {QStringLiteral("sample"), projectedSample},
    };
}

QJsonObject makeSampleBatch(quint64 firstSequence,
                            const QVector<ApplicationSample>& samples)
{
    constexpr int kMaxBatchSamples = 64;
    if (samples.isEmpty() || samples.size() > kMaxBatchSamples ||
        !isJsonSafeNonNegativeInteger(firstSequence)) {
        return {};
    }

    const quint64 sampleCountMinusOne =
        static_cast<quint64>(samples.size() - 1);
    const quint64 maxJsonSafeInteger =
        static_cast<quint64>(kMaxJsonSafeInteger);
    if (sampleCountMinusOne > maxJsonSafeInteger - firstSequence) {
        return {};
    }

    const QString taskId = samples.first().taskId;
    QJsonArray projectedSamples;
    for (const ApplicationSample& sample : samples) {
        if (sample.taskId != taskId) {
            return {};
        }
        const QJsonObject projectedSample = sampleObject(sample);
        if (projectedSample.isEmpty()) {
            return {};
        }
        projectedSamples.push_back(projectedSample);
    }

    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QStringLiteral("sampleBatch")},
        {QStringLiteral("firstSeq"), static_cast<double>(firstSequence)},
        {QStringLiteral("lastSeq"),
         static_cast<double>(firstSequence + sampleCountMinusOne)},
        {QStringLiteral("samples"), projectedSamples},
    };
}

QJsonObject analysisResultObject(const AnalysisChannelProjection& projection)
{
    const int count = projection.frequencyHz.size();
    if (count > 256 || projection.channelSummary.bodePointCount != count ||
        count != projection.magnitudeDb.size() ||
        count != projection.phaseDeg.size() ||
        count != projection.pointStatus.size()) {
        return {};
    }
    QJsonArray frequencyHz;
    QJsonArray magnitudeDb;
    QJsonArray phaseDeg;
    QJsonArray pointStatus;
    for (int index = 0; index < count; ++index) {
        const double frequency = projection.frequencyHz.at(index);
        if (!std::isfinite(frequency) || frequency <= 0.0 ||
            projection.pointStatus.at(index).isEmpty()) {
            return {};
        }
        frequencyHz.push_back(frequency);
        const AnalysisNullableNumber magnitude = projection.magnitudeDb.at(index);
        const AnalysisNullableNumber phase = projection.phaseDeg.at(index);
        if ((magnitude.hasValue && !std::isfinite(magnitude.value)) ||
            (phase.hasValue && !std::isfinite(phase.value))) {
            return {};
        }
        magnitudeDb.push_back(magnitude.hasValue
                                  ? QJsonValue(magnitude.value)
                                  : QJsonValue(QJsonValue::Null));
        phaseDeg.push_back(phase.hasValue
                              ? QJsonValue(phase.value)
                              : QJsonValue(QJsonValue::Null));
        pointStatus.push_back(projection.pointStatus.at(index));
    }
    const QJsonObject result{
        {QStringLiteral("channelSummary"),
         analysisChannelSummaryObject(projection.channelSummary)},
        {QStringLiteral("bode"),
         QJsonObject{{QStringLiteral("frequencyHz"), frequencyHz},
                     {QStringLiteral("magnitudeDb"), magnitudeDb},
                     {QStringLiteral("phaseDeg"), phaseDeg},
                     {QStringLiteral("pointStatus"), pointStatus}}},
    };
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 16384) {
        return {};
    }
    return result;
}

QString compactJson(const QJsonObject& object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace hwtest::app::web
