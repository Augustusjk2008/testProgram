#include "web_protocol.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantList>

namespace hwtest::app::web {
namespace {

TEST(WebProtocolTest, ParsesValidRequestEnvelope)
{
    const ProtocolParseResult parsed = parseRequest(QStringLiteral(
        R"({"v":1,"type":"request","id":" req-1 ","action":"selectControl","params":{"resourceId":"CONTROL_NETWORK"}})"));

    ASSERT_TRUE(parsed.ok) << parsed.message.toStdString();
    EXPECT_EQ(parsed.request.id, QStringLiteral(" req-1 "));
    EXPECT_EQ(parsed.request.action, QStringLiteral("selectControl"));
    EXPECT_EQ(parsed.request.params.value(QStringLiteral("resourceId")).toString(),
              QStringLiteral("CONTROL_NETWORK"));
    EXPECT_TRUE(parsed.code.isEmpty());
}

TEST(WebProtocolTest, AcceptsEveryVersionOneAction)
{
    const QStringList actions{
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

    for (const QString& action : actions) {
        const QString json = QStringLiteral(
                                 R"({"v":1,"type":"request","id":"id","action":"%1","params":{}})")
                                 .arg(action);
        const ProtocolParseResult parsed = parseRequest(json);
        EXPECT_TRUE(parsed.ok) << action.toStdString() << ": "
                               << parsed.message.toStdString();
    }
}

TEST(WebProtocolTest, RejectsMalformedJsonAndNonObjectRoot)
{
    const ProtocolParseResult malformed = parseRequest(QStringLiteral("{"));
    EXPECT_FALSE(malformed.ok);
    EXPECT_EQ(malformed.code, QStringLiteral("invalid_json"));

    const ProtocolParseResult array = parseRequest(QStringLiteral("[]"));
    EXPECT_FALSE(array.ok);
    EXPECT_EQ(array.code, QStringLiteral("invalid_json"));
}

TEST(WebProtocolTest, RejectsMissingAndMistypedEnvelopeFields)
{
    struct Case {
        const char* json;
        const char* code;
    };
    const Case cases[] = {
        {R"({"type":"request","id":"id","action":"start","params":{}})",
         "missing_field"},
        {R"({"v":"1","type":"request","id":"id","action":"start","params":{}})",
         "unsupported_version"},
        {R"({"v":1,"id":"id","action":"start","params":{}})",
         "missing_field"},
        {R"({"v":1,"type":"event","id":"id","action":"start","params":{}})",
         "invalid_envelope"},
        {R"({"v":1,"type":"request","action":"start","params":{}})",
         "missing_field"},
        {R"({"v":1,"type":"request","id":7,"action":"start","params":{}})",
         "invalid_envelope"},
        {R"({"v":1,"type":"request","id":"   ","action":"start","params":{}})",
         "missing_field"},
        {R"({"v":1,"type":"request","id":"id","params":{}})",
         "missing_field"},
        {R"({"v":1,"type":"request","id":"id","action":7,"params":{}})",
         "invalid_envelope"},
        {R"({"v":1,"type":"request","id":"id","action":"start"})",
         "missing_field"},
        {R"({"v":1,"type":"request","id":"id","action":"start","params":[]})",
         "invalid_envelope"},
    };

    for (const Case& value : cases) {
        const ProtocolParseResult parsed = parseRequest(QString::fromUtf8(value.json));
        EXPECT_FALSE(parsed.ok) << value.json;
        EXPECT_EQ(parsed.code, QString::fromLatin1(value.code)) << value.json;
    }
}

TEST(WebProtocolTest, RejectsUnsupportedVersionAndUnknownAction)
{
    const ProtocolParseResult version = parseRequest(QStringLiteral(
        R"({"v":2,"type":"request","id":"id","action":"start","params":{}})"));
    EXPECT_FALSE(version.ok);
    EXPECT_EQ(version.code, QStringLiteral("unsupported_version"));

    const ProtocolParseResult action = parseRequest(QStringLiteral(
        R"({"v":1,"type":"request","id":"id","action":"launch","params":{}})"));
    EXPECT_FALSE(action.ok);
    EXPECT_EQ(action.code, QStringLiteral("unknown_action"));
}

TEST(WebProtocolTest, BuildsHelloAndReplyEnvelopes)
{
    const QJsonObject hello = makeHello();
    EXPECT_EQ(hello.value(QStringLiteral("v")).toInt(), 1);
    EXPECT_EQ(hello.value(QStringLiteral("type")).toString(), QStringLiteral("hello"));
    EXPECT_EQ(hello.value(QStringLiteral("server")).toString(),
              QStringLiteral("hwtest_web"));
    EXPECT_EQ(hello.value(QStringLiteral("protocolVersion")).toInt(), 1);

    const QJsonObject data{{QStringLiteral("selected"), QStringLiteral("CONTROL_NETWORK")}};
    const QJsonObject success = makeReply(QStringLiteral("req-1"), ActionResult{}, data);
    EXPECT_EQ(success.value(QStringLiteral("v")).toInt(), 1);
    EXPECT_EQ(success.value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    EXPECT_EQ(success.value(QStringLiteral("id")).toString(), QStringLiteral("req-1"));
    EXPECT_TRUE(success.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(success.value(QStringLiteral("code")).toString().isEmpty());
    EXPECT_EQ(success.value(QStringLiteral("data")).toObject(), data);

    const ActionResult error{false,
                             QStringLiteral("invalid_state"),
                             QStringLiteral("Cannot start")};
    const QJsonObject failure = makeReply(QStringLiteral("req-2"), error);
    EXPECT_FALSE(failure.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(failure.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));
    EXPECT_EQ(failure.value(QStringLiteral("message")).toString(),
              QStringLiteral("Cannot start"));
    EXPECT_TRUE(failure.value(QStringLiteral("data")).toObject().isEmpty());
}

TEST(WebProtocolTest, ProjectsEverySnapshotFieldAndRawData)
{
    ApplicationSnapshot snapshot;
    snapshot.phase = QStringLiteral("running");
    snapshot.testState = QStringLiteral("Running");
    snapshot.controlResourceId = QStringLiteral("CONTROL_NETWORK");
    snapshot.providerId = QStringLiteral("qt.udp");
    snapshot.serialPortName = QStringLiteral("COM7");
    snapshot.taskId = QStringLiteral("task-1");
    snapshot.stepId = QStringLiteral("step-1");
    snapshot.testItemId = QStringLiteral("item-1");
    snapshot.algorithmId = QStringLiteral("mbddf.system_status");
    snapshot.progress = 25;
    snapshot.progressStep = QStringLiteral("transact");
    snapshot.hasResult = true;
    snapshot.verdict = QStringLiteral("PASS");
    snapshot.errorCode = QStringLiteral("none");
    snapshot.message = QStringLiteral("complete");
    snapshot.attempts = 2;
    snapshot.rawData.insert(QStringLiteral("cpu_usage"), 12.5);
    snapshot.rawData.insert(QStringLiteral("flags"),
                            QVariantList{true, QStringLiteral("ok")});

    const QJsonObject envelope = makeSnapshot(12, snapshot);
    EXPECT_EQ(envelope.value(QStringLiteral("v")).toInt(), 1);
    EXPECT_EQ(envelope.value(QStringLiteral("type")).toString(),
              QStringLiteral("snapshot"));
    EXPECT_EQ(envelope.value(QStringLiteral("seq")).toInt(), 12);

    const QJsonObject json = envelope.value(QStringLiteral("snapshot")).toObject();
    EXPECT_EQ(json.size(), 17);
    EXPECT_EQ(json.value(QStringLiteral("phase")).toString(), snapshot.phase);
    EXPECT_EQ(json.value(QStringLiteral("testState")).toString(), snapshot.testState);
    EXPECT_EQ(json.value(QStringLiteral("controlResourceId")).toString(),
              snapshot.controlResourceId);
    EXPECT_EQ(json.value(QStringLiteral("providerId")).toString(), snapshot.providerId);
    EXPECT_EQ(json.value(QStringLiteral("serialPortName")).toString(),
              snapshot.serialPortName);
    EXPECT_EQ(json.value(QStringLiteral("taskId")).toString(), snapshot.taskId);
    EXPECT_EQ(json.value(QStringLiteral("stepId")).toString(), snapshot.stepId);
    EXPECT_EQ(json.value(QStringLiteral("testItemId")).toString(), snapshot.testItemId);
    EXPECT_EQ(json.value(QStringLiteral("algorithmId")).toString(), snapshot.algorithmId);
    EXPECT_EQ(json.value(QStringLiteral("progress")).toInt(), snapshot.progress);
    EXPECT_EQ(json.value(QStringLiteral("progressStep")).toString(), snapshot.progressStep);
    EXPECT_EQ(json.value(QStringLiteral("hasResult")).toBool(), snapshot.hasResult);
    EXPECT_EQ(json.value(QStringLiteral("verdict")).toString(), snapshot.verdict);
    EXPECT_EQ(json.value(QStringLiteral("errorCode")).toString(), snapshot.errorCode);
    EXPECT_EQ(json.value(QStringLiteral("message")).toString(), snapshot.message);
    EXPECT_EQ(json.value(QStringLiteral("attempts")).toInt(), snapshot.attempts);
    const QJsonObject rawData = json.value(QStringLiteral("rawData")).toObject();
    EXPECT_DOUBLE_EQ(rawData.value(QStringLiteral("cpu_usage")).toDouble(), 12.5);
    ASSERT_TRUE(rawData.value(QStringLiteral("flags")).isArray());
    EXPECT_TRUE(rawData.value(QStringLiteral("flags")).toArray().at(0).toBool());
    EXPECT_EQ(rawData.value(QStringLiteral("flags")).toArray().at(1).toString(),
              QStringLiteral("ok"));
}

TEST(WebProtocolTest, SerializesCompactJson)
{
    const QString json = compactJson(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("hello")},
                    {QStringLiteral("v"), 1}});

    EXPECT_FALSE(json.contains(QLatin1Char('\n')));
    EXPECT_FALSE(json.contains(QStringLiteral(": ")));
    const QJsonDocument reparsed = QJsonDocument::fromJson(json.toUtf8());
    ASSERT_TRUE(reparsed.isObject());
    EXPECT_EQ(reparsed.object().value(QStringLiteral("type")).toString(),
              QStringLiteral("hello"));
}

} // namespace
} // namespace hwtest::app::web
