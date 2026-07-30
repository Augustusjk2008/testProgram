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
    const QJsonObject telemetryBatch = hello.value(QStringLiteral("capabilities"))
                                           .toObject()
                                           .value(QStringLiteral("telemetryBatch"))
                                           .toObject();
    EXPECT_EQ(telemetryBatch.value(QStringLiteral("version")).toInt(), 1);
    EXPECT_EQ(telemetryBatch.value(QStringLiteral("maxSamples")).toInt(), 64);
    EXPECT_EQ(telemetryBatch.value(QStringLiteral("maxBytes")).toInt(), 32768);
    EXPECT_EQ(telemetryBatch.value(QStringLiteral("maxLatencyMs")).toInt(), 20);
    EXPECT_EQ(telemetryBatch.value(QStringLiteral("snapshotIntervalMs")).toInt(),
              100);

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
    snapshot.rawData.insert(
        QStringLiteral("boardTest"),
        QVariantMap{
            {QStringLiteral("schema"),
             QStringLiteral("hwtest.mbddf-board-test-result")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("kind"), QStringLiteral("helm_board_test")},
            {QStringLiteral("mode"), QStringLiteral("automatic")},
            {QStringLiteral("completedPoints"), 1},
            {QStringLiteral("totalPoints"), 85},
            {QStringLiteral("pwmPoints"),
             QVariantList{QVariantMap{
                 {QStringLiteral("valid"), false},
                 {QStringLiteral("errorPercentagePoints"), QVariant()},
             }}},
        });
    snapshot.runMode = QStringLiteral("pc_periodic");
    snapshot.intervalMs = 250;
    snapshot.maxCycles = 0;
    snapshot.cycleIndex = 17;
    snapshot.sampleCount = 42;
    snapshot.dataSaveEnabled = true;
    snapshot.dataFilePath = QStringLiteral("C:/data/SystemStatus_data.txt");
    snapshot.dataSaveError.clear();
    snapshot.effectiveRunParameters.insert(QStringLiteral("waveform"), 4);
    snapshot.effectiveRunParameters.insert(QStringLiteral("ampl"), 250.0);
    snapshot.digitalStimulus.available = true;
    snapshot.digitalStimulus.configured = true;
    snapshot.digitalStimulus.appliedMask = 0x81u;
    snapshot.digitalStimulus.revision = 7;
    snapshot.digitalStimulus.lastWriteTimestampUs = 123456;
    snapshot.digitalStimulus.settlingMs = 20;
    snapshot.digitalStimulus.switches = {
        DigitalSwitchDescriptor{QStringLiteral("di0"), 0, QStringLiteral("DI0"), QStringLiteral("High")},
        DigitalSwitchDescriptor{QStringLiteral("di8"), 8, QStringLiteral("DI8"), QStringLiteral("Low")},
    };
    snapshot.descriptor.configId = QStringLiteral("mbddf-system-status");
    snapshot.descriptor.title = QStringLiteral("系统状态");
    snapshot.descriptor.supportedRunModes = {
        QStringLiteral("single"), QStringLiteral("pc_periodic")};
    snapshot.descriptor.stoppable = false;
    snapshot.descriptor.measurements = {
        TestMeasurementDescriptor{QStringLiteral("cpu_usage"),
                                  QStringLiteral("CPU 占用率"),
                                  QStringLiteral("%"),
                                  true}};
    snapshot.descriptor.runParameterSchemaVersion = QStringLiteral("1");
    TestRunParameterDescriptor frequency;
    frequency.id = QStringLiteral("freq");
    frequency.label = QStringLiteral("频率");
    frequency.description = QStringLiteral("扫频起始频率");
    frequency.kind = QStringLiteral("number");
    frequency.unit = QStringLiteral("Hz");
    frequency.minimum = 0.0;
    frequency.minimumExclusive = true;
    frequency.visibleWhenParameter = QStringLiteral("waveform");
    frequency.visibleWhenEquals = 4;
    frequency.persistValues = false;
    snapshot.descriptor.runParameters = {frequency};
    snapshot.descriptor.runParameterDefaults.insert(QStringLiteral("freq"), 1.0);

    const QJsonObject envelope = makeSnapshot(12, snapshot);
    EXPECT_EQ(envelope.value(QStringLiteral("v")).toInt(), 1);
    EXPECT_EQ(envelope.value(QStringLiteral("type")).toString(),
              QStringLiteral("snapshot"));
    EXPECT_EQ(envelope.value(QStringLiteral("seq")).toInt(), 12);

    const QJsonObject json = envelope.value(QStringLiteral("snapshot")).toObject();
    EXPECT_EQ(json.size(), 29);
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
    EXPECT_EQ(json.value(QStringLiteral("runMode")).toString(), snapshot.runMode);
    EXPECT_EQ(json.value(QStringLiteral("intervalMs")).toInt(), snapshot.intervalMs);
    EXPECT_EQ(json.value(QStringLiteral("maxCycles")).toDouble(),
              static_cast<double>(snapshot.maxCycles));
    EXPECT_EQ(json.value(QStringLiteral("cycleIndex")).toDouble(),
              static_cast<double>(snapshot.cycleIndex));
    EXPECT_EQ(json.value(QStringLiteral("sampleCount")).toDouble(),
              static_cast<double>(snapshot.sampleCount));
    EXPECT_TRUE(json.value(QStringLiteral("dataSaveEnabled")).toBool());
    EXPECT_EQ(json.value(QStringLiteral("dataFilePath")).toString(),
              snapshot.dataFilePath);
    EXPECT_TRUE(json.value(QStringLiteral("dataSaveError")).toString().isEmpty());
    const QJsonObject analysis = json.value(QStringLiteral("analysis")).toObject();
    EXPECT_FALSE(analysis.value(QStringLiteral("supported")).toBool());
    EXPECT_EQ(analysis.value(QStringLiteral("state")).toString(),
              QStringLiteral("none"));
    EXPECT_EQ(analysis.value(QStringLiteral("analysisGeneration")).toDouble(),
              0.0);
    const QJsonObject effectiveRunParameters =
        json.value(QStringLiteral("effectiveRunParameters")).toObject();
    EXPECT_EQ(effectiveRunParameters.value(QStringLiteral("waveform")).toInt(), 4);
    EXPECT_DOUBLE_EQ(effectiveRunParameters.value(QStringLiteral("ampl")).toDouble(),
                     250.0);
    const QJsonObject stimulus = json.value(QStringLiteral("digitalStimulus")).toObject();
    EXPECT_TRUE(stimulus.value(QStringLiteral("available")).toBool());
    EXPECT_TRUE(stimulus.value(QStringLiteral("configured")).toBool());
    EXPECT_EQ(stimulus.value(QStringLiteral("appliedMask")).toDouble(), 0x81);
    EXPECT_EQ(stimulus.value(QStringLiteral("revision")).toDouble(), 7);
    EXPECT_EQ(stimulus.value(QStringLiteral("lastWriteTimestampUs")).toDouble(), 123456);
    EXPECT_EQ(stimulus.value(QStringLiteral("settlingMs")).toInt(), 20);
    const QJsonArray switches = stimulus.value(QStringLiteral("switches")).toArray();
    ASSERT_EQ(switches.size(), 2);
    EXPECT_EQ(switches.at(1).toObject().value(QStringLiteral("switchId")).toString(),
              QStringLiteral("di8"));
    EXPECT_FALSE(switches.at(1).toObject().contains(QStringLiteral("resourceId")));
    const QJsonObject descriptor = json.value(QStringLiteral("descriptor")).toObject();
    const QJsonObject analysisCapability =
        descriptor.value(QStringLiteral("postRunAnalysis")).toObject();
    EXPECT_TRUE(descriptor.contains(QStringLiteral("postRunAnalysis")));
    EXPECT_FALSE(analysisCapability.value(QStringLiteral("supported")).toBool());
    EXPECT_EQ(descriptor.value(QStringLiteral("configId")).toString(),
              snapshot.descriptor.configId);
    EXPECT_EQ(descriptor.value(QStringLiteral("title")).toString(),
              snapshot.descriptor.title);
    EXPECT_TRUE(descriptor.contains(QStringLiteral("stoppable")));
    EXPECT_FALSE(descriptor.value(QStringLiteral("stoppable")).toBool());
    ASSERT_EQ(descriptor.value(QStringLiteral("supportedRunModes"))
                  .toArray()
                  .size(),
              2);
    ASSERT_EQ(descriptor.value(QStringLiteral("measurements"))
                  .toArray()
                  .size(),
              1);
    EXPECT_EQ(descriptor.value(QStringLiteral("measurements"))
                  .toArray()
                  .first()
                  .toObject()
                  .value(QStringLiteral("unit"))
                  .toString(),
              QStringLiteral("%"));
    EXPECT_EQ(descriptor.value(QStringLiteral("runParameterSchemaVersion")).toString(),
              QStringLiteral("1"));
    const QJsonArray runParameters =
        descriptor.value(QStringLiteral("runParameters")).toArray();
    ASSERT_EQ(runParameters.size(), 1);
    const QJsonObject frequencyParameter = runParameters.first().toObject();
    EXPECT_EQ(frequencyParameter.value(QStringLiteral("id")).toString(),
              QStringLiteral("freq"));
    EXPECT_EQ(frequencyParameter.value(QStringLiteral("kind")).toString(),
              QStringLiteral("number"));
    EXPECT_DOUBLE_EQ(frequencyParameter.value(QStringLiteral("minimum")).toDouble(), 0.0);
    EXPECT_TRUE(frequencyParameter.value(QStringLiteral("minimumExclusive")).toBool());
    EXPECT_FALSE(frequencyParameter.value(QStringLiteral("persistValues")).toBool(true));
    const QJsonObject visibility =
        frequencyParameter.value(QStringLiteral("visibleWhen")).toObject();
    EXPECT_EQ(visibility.value(QStringLiteral("parameter")).toString(),
              QStringLiteral("waveform"));
    EXPECT_EQ(visibility.value(QStringLiteral("equals")).toInt(), 4);
    EXPECT_DOUBLE_EQ(descriptor.value(QStringLiteral("runParameterDefaults"))
                         .toObject()
                         .value(QStringLiteral("freq"))
                         .toDouble(),
                     1.0);
    const QJsonObject rawData = json.value(QStringLiteral("rawData")).toObject();
    EXPECT_DOUBLE_EQ(rawData.value(QStringLiteral("cpu_usage")).toDouble(), 12.5);
    ASSERT_TRUE(rawData.value(QStringLiteral("flags")).isArray());
    EXPECT_TRUE(rawData.value(QStringLiteral("flags")).toArray().at(0).toBool());
    EXPECT_EQ(rawData.value(QStringLiteral("flags")).toArray().at(1).toString(),
              QStringLiteral("ok"));
    const QJsonObject boardTest = rawData.value(QStringLiteral("boardTest")).toObject();
    EXPECT_EQ(boardTest.value(QStringLiteral("schema")).toString(),
              QStringLiteral("hwtest.mbddf-board-test-result"));
    const QJsonObject pwmPoint = boardTest.value(QStringLiteral("pwmPoints"))
                                     .toArray().at(0).toObject();
    EXPECT_FALSE(pwmPoint.value(QStringLiteral("valid")).toBool(true));
    EXPECT_TRUE(pwmPoint.value(QStringLiteral("errorPercentagePoints")).isNull());
}

TEST(WebProtocolTest, RejectsAnalysisGenerationOutsideJsonSafeIntegerRange)
{
    ApplicationSnapshot snapshot;
    snapshot.analysis.supported = true;
    snapshot.analysis.analysisGeneration = 9007199254740992ULL;

    EXPECT_TRUE(makeSnapshot(1, snapshot).isEmpty());
}

TEST(WebProtocolTest, ProjectsPerChannelAnalysisReason)
{
    ApplicationSnapshot snapshot;
    snapshot.analysis.supported = true;
    snapshot.analysis.state = QStringLiteral("partial");
    AnalysisChannelSummary summary;
    summary.channel = 2;
    summary.enabled = true;
    summary.status = QStringLiteral("unavailable");
    summary.reasonCode = QStringLiteral("weak_excitation");
    summary.message = QStringLiteral("Command amplitude is below the excitation floor");
    snapshot.analysis.channelSummaries.push_back(summary);

    const QJsonObject channel = makeSnapshot(1, snapshot)
                                    .value(QStringLiteral("snapshot"))
                                    .toObject()
                                    .value(QStringLiteral("analysis"))
                                    .toObject()
                                    .value(QStringLiteral("channelSummaries"))
                                    .toArray()
                                    .first()
                                    .toObject();
    EXPECT_EQ(channel.value(QStringLiteral("reasonCode")).toString(),
              summary.reasonCode);
    EXPECT_EQ(channel.value(QStringLiteral("message")).toString(),
              summary.message);
}

TEST(WebProtocolTest, HidesDigitalStimulusOutsideVersionOneBitRange)
{
    DigitalStimulusSnapshot stimulus;
    stimulus.available = true;
    stimulus.configured = true;
    stimulus.switches = {
        DigitalSwitchDescriptor{QStringLiteral("di63"),
                                63,
                                QStringLiteral("DI63"),
                                QStringLiteral("High")},
    };
    stimulus.appliedMask = quint64{1} << 63;
    stimulus.revision = 9;

    const QJsonObject json = digitalStimulusObject(stimulus);

    EXPECT_FALSE(json.value(QStringLiteral("available")).toBool());
    EXPECT_FALSE(json.value(QStringLiteral("configured")).toBool());
    EXPECT_TRUE(json.value(QStringLiteral("switches")).toArray().isEmpty());
    EXPECT_EQ(json.value(QStringLiteral("appliedMask")).toDouble(), 0.0);
    EXPECT_EQ(json.value(QStringLiteral("revision")).toDouble(), 0.0);
    EXPECT_EQ(json.value(QStringLiteral("errorCode")).toString(),
              QStringLiteral("CapabilityUnsupported"));
}

TEST(WebProtocolTest, RejectsAnalysisProjectionAbovePointLimit)
{
    AnalysisChannelProjection projection;
    projection.channelSummary.channel = 0;
    projection.channelSummary.enabled = true;
    projection.channelSummary.status = QStringLiteral("completed");
    projection.channelSummary.bodeAvailable = true;
    projection.channelSummary.bodePointCount = 257;
    for (int index = 0; index < 257; ++index) {
        projection.frequencyHz.push_back(1.0 + index);
        projection.magnitudeDb.push_back(AnalysisNullableNumber{true, 0.0});
        projection.phaseDeg.push_back(AnalysisNullableNumber{});
        projection.pointStatus.push_back(QStringLiteral("valid"));
    }

    EXPECT_TRUE(analysisResultObject(projection).isEmpty());
}

TEST(WebProtocolTest, RejectsAnalysisProjectionWhoseSummaryCountDoesNotMatch)
{
    AnalysisChannelProjection projection;
    projection.channelSummary.channel = 0;
    projection.channelSummary.enabled = true;
    projection.channelSummary.status = QStringLiteral("completed");
    projection.channelSummary.bodeAvailable = true;
    projection.channelSummary.bodePointCount = 2;
    projection.frequencyHz = {1.0};
    projection.magnitudeDb = {AnalysisNullableNumber{true, 0.0}};
    projection.phaseDeg = {AnalysisNullableNumber{true, -10.0}};
    projection.pointStatus = {QStringLiteral("valid")};

    EXPECT_TRUE(analysisResultObject(projection).isEmpty());
}

TEST(WebProtocolTest, RejectsAnalysisProjectionAboveCompactByteLimit)
{
    AnalysisChannelProjection projection;
    projection.channelSummary.channel = 0;
    projection.channelSummary.enabled = true;
    projection.channelSummary.status = QStringLiteral("completed");
    projection.channelSummary.bodeAvailable = true;
    projection.channelSummary.bodePointCount = 256;
    const QString oversizedStatus(128, QLatin1Char('x'));
    for (int index = 0; index < 256; ++index) {
        projection.frequencyHz.push_back(1.0 + index);
        projection.magnitudeDb.push_back(AnalysisNullableNumber{true, 0.0});
        projection.phaseDeg.push_back(AnalysisNullableNumber{true, 0.0});
        projection.pointStatus.push_back(oversizedStatus);
    }

    EXPECT_TRUE(analysisResultObject(projection).isEmpty());
}

TEST(WebProtocolTest, BoundsTheCompleteCompactAnalysisReply)
{
    AnalysisChannelProjection projection;
    projection.channelSummary.channel = 0;
    projection.channelSummary.enabled = true;
    projection.channelSummary.status = QStringLiteral("completed");
    projection.channelSummary.bodeAvailable = true;
    projection.channelSummary.bodePointCount = 200;
    const QString status(40, QLatin1Char('x'));
    for (int index = 0; index < 200; ++index) {
        projection.frequencyHz.push_back(1.0 + index);
        projection.magnitudeDb.push_back(AnalysisNullableNumber{true, 0.0});
        projection.phaseDeg.push_back(AnalysisNullableNumber{true, 0.0});
        projection.pointStatus.push_back(status);
    }
    const QJsonObject projected = analysisResultObject(projection);
    ASSERT_FALSE(projected.isEmpty());
    const QJsonObject data{{QStringLiteral("analysisResult"), projected}};
    const QString id(8000, QLatin1Char('i'));
    ASSERT_GT(compactJson(makeReply(id, ActionResult{}, data)).toUtf8().size(),
              16384);

    const QJsonObject reply = makeAnalysisResultReply(id, ActionResult{}, data);

    EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("analysis_projection_too_large"));
    EXPECT_LE(compactJson(reply).toUtf8().size(), 16384);
}

TEST(WebProtocolTest, ProjectsApplicationSampleAsVersionedEvent)
{
    ApplicationSample sample;
    sample.taskId = QStringLiteral("task-7");
    sample.stepId = QStringLiteral("SYSTEM_STATUS");
    sample.channelId = QStringLiteral("SYSTEM_STATUS");
    sample.timestampUs = 1785000000123456LL;
    sample.streamElapsedUs = 15000;
    sample.cycleIndex = 7;
    sample.values.insert(QStringLiteral("cpu_usage"), 12.5);
    sample.values.insert(QStringLiteral("power_on_sec"), 99u);
    sample.tags.insert(QStringLiteral("requestFrameHex"), QStringLiteral("55aa"));

    const QJsonObject envelope = makeSample(31, sample);

    EXPECT_EQ(envelope.value(QStringLiteral("v")).toInt(), 1);
    EXPECT_EQ(envelope.value(QStringLiteral("type")).toString(),
              QStringLiteral("sample"));
    EXPECT_EQ(envelope.value(QStringLiteral("seq")).toInt(), 31);
    const QJsonObject json = envelope.value(QStringLiteral("sample")).toObject();
    EXPECT_EQ(json.value(QStringLiteral("taskId")).toString(), sample.taskId);
    EXPECT_EQ(json.value(QStringLiteral("stepId")).toString(), sample.stepId);
    EXPECT_EQ(json.value(QStringLiteral("channelId")).toString(), sample.channelId);
    EXPECT_EQ(json.value(QStringLiteral("timestampUs")).toDouble(),
              static_cast<double>(sample.timestampUs));
    EXPECT_EQ(json.value(QStringLiteral("streamElapsedUs")).toDouble(),
              static_cast<double>(sample.streamElapsedUs));
    EXPECT_EQ(json.value(QStringLiteral("cycleIndex")).toInt(), 7);
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("values"))
                         .toObject()
                         .value(QStringLiteral("cpu_usage"))
                         .toDouble(),
                     12.5);
    EXPECT_EQ(json.value(QStringLiteral("tags"))
                  .toObject()
                  .value(QStringLiteral("requestFrameHex"))
                  .toString(),
              QStringLiteral("55aa"));
}

TEST(WebProtocolTest, OmitsUnavailableStreamTimeButPreservesZeroOrigin)
{
    ApplicationSample sample;
    sample.timestampUs = 1785000000123456LL;
    sample.streamElapsedUs = -2;

    QJsonObject json = makeSample(1, sample)
                           .value(QStringLiteral("sample"))
                           .toObject();
    EXPECT_FALSE(json.contains(QStringLiteral("streamElapsedUs")));

    sample.streamElapsedUs = 0;
    json = makeSample(2, sample)
               .value(QStringLiteral("sample"))
               .toObject();
    ASSERT_TRUE(json.contains(QStringLiteral("streamElapsedUs")));
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("streamElapsedUs")).toDouble(), 0.0);

    constexpr qint64 maxJsonSafeInteger = 9007199254740991LL;
    sample.timestampUs = maxJsonSafeInteger;
    sample.streamElapsedUs = maxJsonSafeInteger;
    json = makeSample(3, sample)
               .value(QStringLiteral("sample"))
               .toObject();
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("timestampUs")).toDouble(),
                     static_cast<double>(maxJsonSafeInteger));
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("streamElapsedUs")).toDouble(),
                     static_cast<double>(maxJsonSafeInteger));

    sample.timestampUs = -1;
    EXPECT_TRUE(makeSample(4, sample).isEmpty());
    sample.timestampUs = maxJsonSafeInteger + 1;
    EXPECT_TRUE(makeSample(5, sample).isEmpty());
    sample.timestampUs = maxJsonSafeInteger;
    sample.streamElapsedUs = maxJsonSafeInteger + 1;
    EXPECT_TRUE(makeSample(6, sample).isEmpty());
}

TEST(WebProtocolTest, RejectsEventSequencesOutsideJavaScriptSafeIntegerRange)
{
    constexpr quint64 maxJsonSafeInteger = 9007199254740991ULL;
    ApplicationSnapshot snapshot;
    ApplicationSample sample;
    sample.timestampUs = 1785000000123456LL;

    EXPECT_FALSE(makeSnapshot(maxJsonSafeInteger, snapshot).isEmpty());
    EXPECT_FALSE(makeSample(maxJsonSafeInteger, sample).isEmpty());
    EXPECT_TRUE(makeSnapshot(maxJsonSafeInteger + 1, snapshot).isEmpty());
    EXPECT_TRUE(makeSample(maxJsonSafeInteger + 1, sample).isEmpty());
}

TEST(WebProtocolTest, ProjectsOrderedApplicationSamplesAsVersionOneBatch)
{
    ApplicationSample first;
    first.taskId = QStringLiteral("task-7");
    first.stepId = QStringLiteral("SYSTEM_STATUS");
    first.channelId = QStringLiteral("SYSTEM_STATUS");
    first.timestampUs = 1785000000123456LL;
    first.streamElapsedUs = 15000;
    first.cycleIndex = 7;
    first.values.insert(QStringLiteral("cpu_usage"), 12.5);
    first.tags.insert(QStringLiteral("requestFrameHex"), QStringLiteral("55aa"));
    ApplicationSample second = first;
    second.timestampUs += 1000;
    second.cycleIndex = 8;
    second.values.insert(QStringLiteral("cpu_usage"), 13.5);

    const QJsonObject batch = makeSampleBatch(101, {first, second});

    EXPECT_EQ(batch.value(QStringLiteral("v")).toInt(), 1);
    EXPECT_EQ(batch.value(QStringLiteral("type")).toString(),
              QStringLiteral("sampleBatch"));
    EXPECT_EQ(batch.value(QStringLiteral("firstSeq")).toInt(), 101);
    EXPECT_EQ(batch.value(QStringLiteral("lastSeq")).toInt(), 102);
    const QJsonArray samples = batch.value(QStringLiteral("samples")).toArray();
    ASSERT_EQ(samples.size(), 2);
    EXPECT_EQ(samples.at(0).toObject(),
              makeSample(101, first).value(QStringLiteral("sample")).toObject());
    EXPECT_EQ(samples.at(1).toObject(),
              makeSample(102, second).value(QStringLiteral("sample")).toObject());
}

TEST(WebProtocolTest, RejectsInvalidApplicationSampleBatches)
{
    constexpr quint64 maxJsonSafeInteger = 9007199254740991ULL;
    ApplicationSample sample;
    sample.taskId = QStringLiteral("task-1");
    sample.timestampUs = 1785000000123456LL;

    EXPECT_TRUE(makeSampleBatch(1, {}).isEmpty());
    EXPECT_FALSE(makeSampleBatch(maxJsonSafeInteger, {sample}).isEmpty());
    EXPECT_TRUE(makeSampleBatch(maxJsonSafeInteger, {sample, sample}).isEmpty());
    EXPECT_TRUE(makeSampleBatch(maxJsonSafeInteger + 1, {sample}).isEmpty());

    QVector<ApplicationSample> tooMany(65, sample);
    EXPECT_TRUE(makeSampleBatch(1, tooMany).isEmpty());

    ApplicationSample otherTask = sample;
    otherTask.taskId = QStringLiteral("task-2");
    EXPECT_TRUE(makeSampleBatch(1, {sample, otherTask}).isEmpty());

    sample.timestampUs = -1;
    EXPECT_TRUE(makeSampleBatch(1, {sample}).isEmpty());
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
