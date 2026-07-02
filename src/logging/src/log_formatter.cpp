#include "logging/log_formatter.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonValue>

namespace hwtest::logging {

QJsonObject LogFormatter::toJsonObject(const LogEvent& event)
{
    QJsonObject object;
    object.insert(QStringLiteral("timestampUs"), QJsonValue::fromVariant(event.timestampUs));
    object.insert(QStringLiteral("level"), event.level);
    object.insert(QStringLiteral("source"), event.source);
    object.insert(QStringLiteral("category"), event.category);
    object.insert(QStringLiteral("message"), event.message);
    object.insert(QStringLiteral("requestId"), event.requestId);
    object.insert(QStringLiteral("durationMs"), QJsonValue::fromVariant(event.durationMs));
    object.insert(QStringLiteral("status"), event.status);
    object.insert(QStringLiteral("adapterCode"), event.adapterCode);
    object.insert(QStringLiteral("context"), QJsonObject::fromVariantMap(event.context));
    return object;
}

QByteArray LogFormatter::toJsonLine(const LogEvent& event)
{
    QByteArray line = QJsonDocument(toJsonObject(event)).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

QString LogFormatter::toTextLine(const LogEvent& event)
{
    const QString timestamp = QDateTime::fromMSecsSinceEpoch(event.timestampUs / 1000, Qt::UTC)
                                  .toString(Qt::ISODateWithMs);
    QString category = event.source;
    if (!event.category.isEmpty()) {
        if (!category.isEmpty()) {
            category.append(QStringLiteral("."));
        }
        category.append(event.category);
    }

    return QStringLiteral("[%1] %2 %3 %4")
        .arg(timestamp, event.level, category, event.message);
}

} // namespace hwtest::logging
