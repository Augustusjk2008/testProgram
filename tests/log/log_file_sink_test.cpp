#include "logging/log_file_sink.h"
#include "logging/log_service.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace hwtest::logging;

TEST(JsonLineFileSinkTest, WritesOneCompactJsonObjectPerLine)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("hwtest.jsonl"));
    JsonLineFileSink sink(path);
    ASSERT_TRUE(sink.open()) << sink.errorString().toStdString();

    LogEvent event;
    event.timestampUs = 123456;
    event.level = QStringLiteral("ERROR");
    event.source = QStringLiteral("hal");
    event.category = QStringLiteral("hal.openDevice");
    event.message = QStringLiteral("open failed");
    event.requestId = QStringLiteral("req-42");
    event.durationMs = 17;
    event.status = QStringLiteral("Timeout");
    event.adapterCode = QStringLiteral("E_TIMEOUT");
    event.context.insert(QStringLiteral("deviceId"), QStringLiteral("main_daq"));
    event.context.insert(QStringLiteral("resourceId"), QStringLiteral("AD_MAIN_0"));

    sink.append(event);
    sink.flush();

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QList<QByteArray> lines = file.readAll().trimmed().split('\n');
    ASSERT_EQ(lines.size(), 1);

    const QJsonDocument document = QJsonDocument::fromJson(lines.first());
    ASSERT_TRUE(document.isObject());
    const QJsonObject object = document.object();
    EXPECT_EQ(object.value(QStringLiteral("level")).toString(), QStringLiteral("ERROR"));
    EXPECT_EQ(object.value(QStringLiteral("source")).toString(), QStringLiteral("hal"));
    EXPECT_EQ(object.value(QStringLiteral("requestId")).toString(), QStringLiteral("req-42"));
    EXPECT_EQ(object.value(QStringLiteral("durationMs")).toInt(), 17);

    const QJsonObject context = object.value(QStringLiteral("context")).toObject();
    EXPECT_EQ(context.value(QStringLiteral("deviceId")).toString(), QStringLiteral("main_daq"));
    EXPECT_EQ(context.value(QStringLiteral("resourceId")).toString(), QStringLiteral("AD_MAIN_0"));
}

TEST(JsonLineFileSinkTest, CanBeUsedAsLogServiceSink)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("service.jsonl"));
    JsonLineFileSink sink(path);

    LogService service;
    service.addSink(&sink);

    LogEvent event;
    event.level = QStringLiteral("INFO");
    event.source = QStringLiteral("system");
    event.message = QStringLiteral("started");
    service.append(event);
    sink.flush();

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray payload = file.readAll();
    EXPECT_TRUE(payload.contains("\"message\":\"started\""));
}
