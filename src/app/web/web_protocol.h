#pragma once

#include <app/test_application_controller.h>

#include <QJsonObject>
#include <QString>
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
QJsonObject makeHello();
QJsonObject makeReply(const QString& id,
                      const ActionResult& result,
                      const QJsonObject& data = {});
QJsonObject digitalStimulusObject(const DigitalStimulusSnapshot& stimulus);
QJsonObject makeSnapshot(quint64 sequence,
                         const ApplicationSnapshot& snapshot);
QJsonObject makeSample(quint64 sequence,
                       const ApplicationSample& sample);
QString compactJson(const QJsonObject& object);

} // namespace hwtest::app::web
