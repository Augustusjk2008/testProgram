#pragma once

#include <app/test_application_controller.h>

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace hwtest::app::web {

struct WebRequest {
    QString id;
    QString action;
    QJsonObject params;
};

struct ProtocolParseResult {
    bool ok = false;
    WebRequest request;
    QString code;
    QString message;
};

ProtocolParseResult parseRequest(const QString& text);
QJsonObject makeHello(int maxBatchSamples = 64,
                      qint64 maxBatchBytes = 32768,
                      int maxBatchLatencyMs = 20,
                      int snapshotIntervalMs = 100);
QJsonObject makeReply(const QString& id,
                      const ActionResult& result,
                      const QJsonObject& data = {});
QJsonObject makeAnalysisResultReply(const QString& id,
                                    const ActionResult& result,
                                    const QJsonObject& data = {});
bool digitalStimulusSupportedByVersionOne(const DigitalStimulusSnapshot& stimulus);
QJsonObject digitalStimulusObject(const DigitalStimulusSnapshot& stimulus);
QJsonObject makeSnapshot(quint64 sequence,
                         const ApplicationSnapshot& snapshot);
// Returns an empty object when a timestamp cannot be represented losslessly
// by the WebSocket v1 JSON number contract.
QJsonObject makeSample(quint64 sequence,
                       const ApplicationSample& sample);
QJsonObject makeSampleBatch(quint64 firstSequence,
                            const QVector<ApplicationSample>& samples);
QJsonObject analysisResultObject(const AnalysisChannelProjection& projection);
QString compactJson(const QJsonObject& object);

} // namespace hwtest::app::web
