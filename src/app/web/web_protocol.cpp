#include "web_protocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

namespace hwtest::app::web {

namespace {

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
        QStringLiteral("snapshot"),
        QStringLiteral("controls"),
        QStringLiteral("ports"),
        QStringLiteral("selectControl"),
        QStringLiteral("selectSerialPort"),
        QStringLiteral("prepare"),
        QStringLiteral("start"),
        QStringLiteral("pause"),
        QStringLiteral("resume"),
        QStringLiteral("stop"),
        QStringLiteral("disconnect"),
        QStringLiteral("quit"),
    };
    return actions.contains(action);
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
    return object;
}

} // namespace

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

QJsonObject makeHello()
{
    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QStringLiteral("hello")},
        {QStringLiteral("server"), QStringLiteral("hwtest_web")},
        {QStringLiteral("protocolVersion"), 1},
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

QJsonObject makeSnapshot(quint64 sequence,
                         const ApplicationSnapshot& snapshot)
{
    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QStringLiteral("snapshot")},
        {QStringLiteral("seq"), static_cast<double>(sequence)},
        {QStringLiteral("snapshot"), snapshotObject(snapshot)},
    };
}

QString compactJson(const QJsonObject& object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace hwtest::app::web
