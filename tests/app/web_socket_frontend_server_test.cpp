#include "web_socket_frontend_server.h"

#include "support/websocket_test_client.h"

#include <gtest/gtest.h>

#include <QHostAddress>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

namespace hwtest::app::web {
namespace {

WebSocketServerOptions testOptions(quint16 port = 0)
{
    WebSocketServerOptions options;
    options.port = port;
    options.handshakeTimeoutMs = 1000;
    options.maxIncomingMessageBytes = 16384;
    return options;
}

QJsonObject request(const QString& id,
                    const QString& action,
                    const QJsonObject& params = {})
{
    return QJsonObject{{QStringLiteral("v"), 1},
                       {QStringLiteral("type"), QStringLiteral("request")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("action"), action},
                       {QStringLiteral("params"), params}};
}

QString compact(const QJsonObject& object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

struct DynamicCatalogFixture {
    QTemporaryDir directory;
    FrontendLaunchOptions launchOptions;
    QString error;

    bool initialize()
    {
        if (!directory.isValid()) {
            error = QStringLiteral("Cannot create temporary configuration directory");
            return false;
        }

        const QDir sourceDirectory(
            QDir(QStringLiteral(HWTEST_PROJECT_SOURCE_DIR)).filePath(QStringLiteral("configs")));
        const QStringList files{
            QStringLiteral("mbddf_system_status.testcfg.json"),
            QStringLiteral("mbddf_elec_health.testcfg.json"),
            QStringLiteral("mbddf_pc_hal.json"),
        };
        for (const QString& file : files) {
            if (!QFile::copy(sourceDirectory.filePath(file), directory.filePath(file))) {
                error = QStringLiteral("Cannot copy fixture '%1'").arg(file);
                return false;
            }
        }

        QFile catalogFile(directory.filePath(QStringLiteral("test-config-catalog.json")));
        if (!catalogFile.open(QIODevice::WriteOnly)) {
            error = catalogFile.errorString();
            return false;
        }
        const QByteArray catalog = QJsonDocument(QJsonObject{
            {QStringLiteral("schemaVersion"), QStringLiteral("1")},
            {QStringLiteral("entries"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("documentId"),
                              QStringLiteral("mbddf_system_status.testcfg.json")},
                             {QStringLiteral("enabled"), true},
                             {QStringLiteral("order"), 0}},
                 QJsonObject{{QStringLiteral("documentId"),
                              QStringLiteral("mbddf_elec_health.testcfg.json")},
                             {QStringLiteral("enabled"), true},
                             {QStringLiteral("order"), 1}},
             }},
        }).toJson(QJsonDocument::Indented);
        if (catalogFile.write(catalog) != catalog.size()) {
            error = catalogFile.errorString();
            return false;
        }

        launchOptions.testConfigPath = directory.filePath(
            QStringLiteral("mbddf_system_status.testcfg.json"));
        launchOptions.halConfigPath = directory.filePath(QStringLiteral("mbddf_pc_hal.json"));
        launchOptions.configurationDirectory = directory.path();
        return true;
    }
};

bool sendAndWait(test::WebSocketTestClient* client,
                 const QString& id,
                 const QString& action,
                 const QJsonObject& params,
                 QJsonObject* reply)
{
    return client->sendText(compact(request(id, action, params))) > 0 &&
        client->waitForReply(id, reply);
}

TEST(WebSocketFrontendServerTest, ListensOnlyOnLoopbackAndSendsHelloThenSnapshot)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    QString error;
    ASSERT_TRUE(server.listen(&error)) << error.toStdString();
    EXPECT_TRUE(server.isListening());
    EXPECT_EQ(server.serverAddress(), QHostAddress(QHostAddress::LocalHost));
    EXPECT_NE(server.serverPort(), 0);
    EXPECT_EQ(server.webSocketUrl().path(), QStringLiteral("/ws"));

    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()))
        << client.errorString().toStdString();
    ASSERT_TRUE(client.waitForMessageCount(2));
    ASSERT_EQ(client.messages().size(), 2);
    EXPECT_EQ(client.messages().at(0).value(QStringLiteral("type")).toString(),
              QStringLiteral("hello"));
    EXPECT_EQ(client.messages().at(1).value(QStringLiteral("type")).toString(),
              QStringLiteral("snapshot"));
    EXPECT_EQ(client.messages().at(1).value(QStringLiteral("seq")).toInt(), 0);
    EXPECT_EQ(client.messages()
                  .at(1)
                  .value(QStringLiteral("snapshot"))
                  .toObject()
                  .value(QStringLiteral("phase"))
                  .toString(),
              QStringLiteral("empty"));
}

TEST(WebSocketFrontendServerTest, NegotiatesBatchDeliveryAndFlushesTailBeforeTerminalSnapshot)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("batch"),
                  QStringLiteral("setTelemetryDelivery"),
                  QJsonObject{{QStringLiteral("mode"), QStringLiteral("batch")}}))),
              0);
    QJsonObject reply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("batch"), &reply));
    ASSERT_TRUE(reply.value(QStringLiteral("ok")).toBool())
        << reply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(reply.value(QStringLiteral("data"))
                  .toObject()
                  .value(QStringLiteral("mode"))
                  .toString(),
              QStringLiteral("batch"));

    ApplicationSample first;
    first.taskId = QStringLiteral("task-1");
    first.stepId = QStringLiteral("SYSTEM_STATUS");
    first.channelId = QStringLiteral("SYSTEM_STATUS");
    first.timestampUs = 1785000000123456LL;
    first.values.insert(QStringLiteral("cpu_usage"), 12.5);
    ApplicationSample second = first;
    second.timestampUs += 1000;
    second.cycleIndex = 2;
    second.values.insert(QStringLiteral("cpu_usage"), 13.5);
    controller.sampleReceived(first);
    controller.sampleReceived(second);

    ApplicationSnapshot terminal;
    terminal.phase = QStringLiteral("finished");
    controller.snapshotChanged(terminal);
    QJsonObject terminalMessage;
    ASSERT_TRUE(client.waitForSnapshotPhase(QStringLiteral("finished"),
                                            &terminalMessage));

    int batchIndex = -1;
    int terminalIndex = -1;
    for (int index = 0; index < client.messages().size(); ++index) {
        const QJsonObject& message = client.messages().at(index);
        if (message.value(QStringLiteral("type")).toString() ==
            QStringLiteral("sampleBatch")) {
            batchIndex = index;
        }
        if (message == terminalMessage) {
            terminalIndex = index;
        }
    }
    ASSERT_GE(batchIndex, 0);
    ASSERT_GE(terminalIndex, 0);
    EXPECT_LT(batchIndex, terminalIndex);
    const QJsonObject batch = client.messages().at(batchIndex);
    EXPECT_EQ(batch.value(QStringLiteral("firstSeq")).toInt(), 1);
    EXPECT_EQ(batch.value(QStringLiteral("lastSeq")).toInt(), 2);
    EXPECT_EQ(batch.value(QStringLiteral("samples")).toArray().size(), 2);

    ASSERT_GT(client.sendText(compact(request(QStringLiteral("disconnect"),
                                               QStringLiteral("disconnect")))),
              0);
    QJsonObject disconnectReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("disconnect"), &disconnectReply));
    ASSERT_TRUE(disconnectReply.value(QStringLiteral("ok")).toBool());
    int replyIndex = -1;
    for (int index = 0; index < client.messages().size(); ++index) {
        const QJsonObject& message = client.messages().at(index);
        if (message.value(QStringLiteral("type")).toString() ==
                QStringLiteral("reply") &&
            message.value(QStringLiteral("id")).toString() ==
                QStringLiteral("disconnect")) {
            replyIndex = index;
        }
    }
    ASSERT_GE(replyIndex, 0);
    EXPECT_LT(terminalIndex, replyIndex);
    EXPECT_TRUE(client.waitForDisconnected());
}

TEST(WebSocketFrontendServerTest, KeepsLegacySingleDeliveryUntilBatchIsNegotiated)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ApplicationSample sample;
    sample.taskId = QStringLiteral("task-legacy");
    sample.stepId = QStringLiteral("SYSTEM_STATUS");
    sample.channelId = QStringLiteral("SYSTEM_STATUS");
    sample.timestampUs = 1785000000123456LL;
    controller.sampleReceived(sample);

    ASSERT_TRUE(client.waitForMessageCount(3));
    EXPECT_EQ(client.messages().at(2).value(QStringLiteral("type")).toString(),
              QStringLiteral("sample"));
    EXPECT_EQ(client.messages().at(2).value(QStringLiteral("seq")).toInt(), 1);
}

TEST(WebSocketFrontendServerTest, ValidatesTelemetryDeliveryAndRejectsActiveOrPendingSwitches)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    struct InvalidRequest {
        QJsonObject params;
        QString code;
    };
    const QVector<InvalidRequest> invalid{
        InvalidRequest{QJsonObject{}, QStringLiteral("missing_field")},
        InvalidRequest{QJsonObject{{QStringLiteral("mode"), 1}},
                       QStringLiteral("invalid_envelope")},
        InvalidRequest{QJsonObject{{QStringLiteral("mode"),
                                    QStringLiteral("stream")}},
                       QStringLiteral("invalid_envelope")},
        InvalidRequest{QJsonObject{{QStringLiteral("mode"),
                                    QStringLiteral("batch")},
                                   {QStringLiteral("extra"), true}},
                       QStringLiteral("invalid_envelope")},
    };
    for (int index = 0; index < invalid.size(); ++index) {
        const QString id = QStringLiteral("invalid-delivery-%1").arg(index);
        ASSERT_GT(client.sendText(compact(request(
                      id,
                      QStringLiteral("setTelemetryDelivery"),
                      invalid.at(index).params))),
                  0);
        QJsonObject reply;
        ASSERT_TRUE(client.waitForReply(id, &reply));
        EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
                  invalid.at(index).code);
    }

    ApplicationSnapshot running;
    running.phase = QStringLiteral("running");
    controller.snapshotChanged(running);
    ASSERT_TRUE(client.waitForSnapshotPhase(QStringLiteral("running"), nullptr));
    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("active"),
                  QStringLiteral("setTelemetryDelivery"),
                  QJsonObject{{QStringLiteral("mode"), QStringLiteral("batch")}}))),
              0);
    QJsonObject activeReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("active"), &activeReply));
    EXPECT_FALSE(activeReply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(activeReply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));

    ApplicationSnapshot finished;
    finished.phase = QStringLiteral("finished");
    finished.analysis.state = QStringLiteral("queued");
    controller.snapshotChanged(finished);
    ASSERT_TRUE(client.waitForSnapshotPhase(QStringLiteral("finished"), nullptr));
    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("batch"),
                  QStringLiteral("setTelemetryDelivery"),
                  QJsonObject{{QStringLiteral("mode"), QStringLiteral("batch")}}))),
              0);
    QJsonObject batchReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("batch"), &batchReply));
    ASSERT_TRUE(batchReply.value(QStringLiteral("ok")).toBool());

    ApplicationSample sample;
    sample.taskId = QStringLiteral("task-pending");
    sample.timestampUs = 1785000000123456LL;
    controller.sampleReceived(sample);
    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("pending"),
                  QStringLiteral("setTelemetryDelivery"),
                  QJsonObject{{QStringLiteral("mode"), QStringLiteral("single")}}))),
              0);
    QJsonObject pendingReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("pending"), &pendingReply));
    EXPECT_FALSE(pendingReply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(pendingReply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));
}

TEST(WebSocketFrontendServerTest, MergesOrdinaryRunningSnapshotsInBatchMode)
{
    WebSocketServerOptions options = testOptions();
    options.snapshotIntervalMs = 1000;
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, options);
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));
    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("batch"),
                  QStringLiteral("setTelemetryDelivery"),
                  QJsonObject{{QStringLiteral("mode"), QStringLiteral("batch")}}))),
              0);
    QJsonObject reply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("batch"), &reply));
    ASSERT_TRUE(reply.value(QStringLiteral("ok")).toBool());

    ApplicationSnapshot running;
    running.phase = QStringLiteral("running");
    running.progress = 1;
    controller.snapshotChanged(running);
    ASSERT_TRUE(client.waitForSnapshotPhase(QStringLiteral("running"), nullptr));
    running.progress = 2;
    controller.snapshotChanged(running);
    running.progress = 3;
    controller.snapshotChanged(running);

    QEventLoop settleLoop;
    QTimer::singleShot(30, &settleLoop, &QEventLoop::quit);
    settleLoop.exec();
    EXPECT_EQ(client.messages().size(), 4);

    ApplicationSnapshot finished = running;
    finished.phase = QStringLiteral("finished");
    controller.snapshotChanged(finished);
    ASSERT_TRUE(client.waitForMessageCount(6));
    const QJsonObject merged = client.messages().at(4);
    const QJsonObject terminal = client.messages().at(5);
    EXPECT_EQ(merged.value(QStringLiteral("type")).toString(),
              QStringLiteral("snapshot"));
    EXPECT_EQ(merged.value(QStringLiteral("seq")).toInt(), 3);
    EXPECT_EQ(merged.value(QStringLiteral("snapshot"))
                  .toObject()
                  .value(QStringLiteral("progress"))
                  .toInt(),
              3);
    EXPECT_EQ(terminal.value(QStringLiteral("snapshot"))
                  .toObject()
                  .value(QStringLiteral("phase"))
                  .toString(),
              QStringLiteral("finished"));
}

TEST(WebSocketFrontendServerTest, HardOutputLimitClosesClientAfterTelemetryBackpressure)
{
    WebSocketServerOptions options = testOptions();
    options.maxQueuedOutputBytes = 65536;
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, options);
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ApplicationSample oversized;
    oversized.taskId = QStringLiteral("task-overflow");
    oversized.stepId = QStringLiteral("SYSTEM_STATUS");
    oversized.channelId = QStringLiteral("SYSTEM_STATUS");
    oversized.timestampUs = 1785000000123456LL;
    oversized.values.insert(QStringLiteral("payload"),
                            QString(70000, QLatin1Char('x')));
    controller.sampleReceived(oversized);

    ASSERT_TRUE(client.waitForDisconnected(5000));
    EXPECT_EQ(client.closeCode(), QWebSocketProtocol::CloseCodeBadOperation);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(WebSocketFrontendServerTest, RejectsUnknownAnalysisResultParameters)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("analysis-invalid"),
                  QStringLiteral("analysisResult"),
                  QJsonObject{{QStringLiteral("taskId"), QStringLiteral("task")},
                              {QStringLiteral("analysisGeneration"), 1},
                              {QStringLiteral("channel"), 0},
                              {QStringLiteral("path"), QStringLiteral("forbidden")}}))),
              0);
    QJsonObject reply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("analysis-invalid"), &reply));
    EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
}

TEST(WebSocketFrontendServerTest, RejectsInvalidAnalysisResultIdentityAndChannel)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    const QVector<QJsonObject> invalid{
        QJsonObject{{QStringLiteral("taskId"), QStringLiteral(" ")},
                    {QStringLiteral("analysisGeneration"), 1},
                    {QStringLiteral("channel"), 0}},
        QJsonObject{{QStringLiteral("taskId"), QStringLiteral("task")},
                    {QStringLiteral("analysisGeneration"), 1.5},
                    {QStringLiteral("channel"), 0}},
        QJsonObject{{QStringLiteral("taskId"), QStringLiteral("task")},
                    {QStringLiteral("analysisGeneration"), 1},
                    {QStringLiteral("channel"), 4}},
    };
    for (int index = 0; index < invalid.size(); ++index) {
        const QString id = QStringLiteral("analysis-invalid-%1").arg(index);
        ASSERT_GT(client.sendText(compact(request(
                      id, QStringLiteral("analysisResult"), invalid.at(index)))),
                  0);
        QJsonObject reply;
        ASSERT_TRUE(client.waitForReply(id, &reply));
        EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
                  QStringLiteral("invalid_envelope"));
    }
}

TEST(WebSocketFrontendServerTest, ValidAnalysisResultRequestUsesReadOnlyControllerPath)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("analysis-read"),
                  QStringLiteral("analysisResult"),
                  QJsonObject{{QStringLiteral("taskId"), QStringLiteral("task")},
                              {QStringLiteral("analysisGeneration"), 1},
                              {QStringLiteral("channel"), 0}}))),
              0);
    QJsonObject reply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("analysis-read"), &reply));
    EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("stale_analysis_result"));
}

TEST(WebSocketFrontendServerTest, ListsAndSelectsAllowlistedTestConfigurations)
{
    const QString projectDirectory = QStringLiteral(HWTEST_PROJECT_SOURCE_DIR);
    const QString systemConfig = QDir(projectDirectory).filePath(
        QStringLiteral("configs/mbddf_system_status.testcfg.json"));
    const QString electricalConfig = QDir(projectDirectory).filePath(
        QStringLiteral("configs/mbddf_elec_health.testcfg.json"));
    FrontendLaunchOptions launchOptions{
        systemConfig,
        QDir(projectDirectory).filePath(QStringLiteral("configs/mbddf_pc_hal.json")),
        {},
        {},
        {
            FrontendTestConfigOption{QStringLiteral("mbddf-system-status"),
                                     QStringLiteral("系统状态"),
                                     QStringLiteral("读取系统状态量。"),
                                     QStringLiteral("mbddf.system_status"),
                                     systemConfig},
            FrontendTestConfigOption{QStringLiteral("mbddf-elec-health"),
                                     QStringLiteral("电气健康"),
                                     QStringLiteral("读取电气健康量。"),
                                     QStringLiteral("mbddf.elec_health_status"),
                                     electricalConfig},
        }};

    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, launchOptions, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendText(compact(request(QStringLiteral("catalog"),
                                              QStringLiteral("testConfigs")))),
              0);
    QJsonObject catalogReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("catalog"), &catalogReply));
    ASSERT_TRUE(catalogReply.value(QStringLiteral("ok")).toBool());
    const QJsonObject catalog = catalogReply.value(QStringLiteral("data")).toObject();
    EXPECT_EQ(catalog.value(QStringLiteral("selectedConfigId")).toString(),
              QStringLiteral("mbddf-system-status"));
    ASSERT_EQ(catalog.value(QStringLiteral("configs")).toArray().size(), 2);

    ASSERT_GT(client.sendText(compact(request(
                                  QStringLiteral("reject-path"),
                                  QStringLiteral("selectTest"),
                                  QJsonObject{
                                      {QStringLiteral("configId"),
                                       QStringLiteral("mbddf-elec-health")},
                                      {QStringLiteral("testConfigPath"),
                                       QStringLiteral("C:/untrusted.json")}}))),
              0);
    QJsonObject rejectedPathReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("reject-path"),
                                    &rejectedPathReply));
    EXPECT_FALSE(rejectedPathReply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(rejectedPathReply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));

    ASSERT_GT(client.sendText(compact(request(
                                  QStringLiteral("select"),
                                  QStringLiteral("selectTest"),
                                  QJsonObject{{QStringLiteral("configId"),
                                               QStringLiteral("mbddf-elec-health")}}))),
              0);
    QJsonObject selectReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("select"), &selectReply));
    ASSERT_TRUE(selectReply.value(QStringLiteral("ok")).toBool())
        << selectReply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
    EXPECT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-elec-health"));
    EXPECT_EQ(controller.snapshot().descriptor.algorithmId,
              QStringLiteral("mbddf.elec_health_status"));

    ASSERT_GT(client.sendText(compact(request(QStringLiteral("catalog-after"),
                                              QStringLiteral("testConfigs")))),
              0);
    QJsonObject updatedCatalogReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("catalog-after"),
                                    &updatedCatalogReply));
    EXPECT_EQ(updatedCatalogReply.value(QStringLiteral("data"))
                  .toObject()
                  .value(QStringLiteral("selectedConfigId"))
                  .toString(),
              QStringLiteral("mbddf-elec-health"));

    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("select-pipelined"),
                  QStringLiteral("selectTest"),
                  QJsonObject{{QStringLiteral("configId"),
                               QStringLiteral("mbddf-system-status")}}))),
              0);
    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("load-pipelined"), QStringLiteral("load")))),
              0);
    QJsonObject pipelinedSelectReply;
    QJsonObject pipelinedLoadReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("select-pipelined"),
                                    &pipelinedSelectReply));
    ASSERT_TRUE(client.waitForReply(QStringLiteral("load-pipelined"),
                                    &pipelinedLoadReply));
    ASSERT_TRUE(pipelinedSelectReply.value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(pipelinedLoadReply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-system-status"));
}

TEST(WebSocketFrontendServerTest, DynamicLoadKeepsTheCurrentCatalogSelection)
{
    const QDir configDirectory(
        QDir(QStringLiteral(HWTEST_PROJECT_SOURCE_DIR)).filePath(
            QStringLiteral("configs")));
    FrontendLaunchOptions launchOptions;
    launchOptions.testConfigPath = configDirectory.filePath(
        QStringLiteral("mbddf_system_status.testcfg.json"));
    launchOptions.halConfigPath = configDirectory.filePath(
        QStringLiteral("mbddf_pc_hal.json"));
    launchOptions.configurationDirectory = configDirectory.absolutePath();

    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, launchOptions, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("select"),
                  QStringLiteral("selectTest"),
                  QJsonObject{{QStringLiteral("configId"),
                               QStringLiteral("mbddf-elec-health")}}))),
              0);
    QJsonObject selectReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("select"), &selectReply));
    ASSERT_TRUE(selectReply.value(QStringLiteral("ok")).toBool())
        << selectReply.value(QStringLiteral("message")).toString().toStdString();
    ASSERT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-elec-health"));

    ASSERT_GT(client.sendText(compact(request(QStringLiteral("reload"),
                                              QStringLiteral("load")))),
              0);
    QJsonObject loadReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("reload"), &loadReply));
    ASSERT_TRUE(loadReply.value(QStringLiteral("ok")).toBool())
        << loadReply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-elec-health"));
}

TEST(WebSocketFrontendServerTest,
     DynamicLoadDoesNotReviveDisabledLaunchSelectionAfterCatalogSave)
{
    DynamicCatalogFixture fixture;
    ASSERT_TRUE(fixture.initialize()) << fixture.error.toStdString();

    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, fixture.launchOptions, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    QJsonObject initialLoadReply;
    ASSERT_TRUE(sendAndWait(&client, QStringLiteral("initial-load"), QStringLiteral("load"),
                            {}, &initialLoadReply));
    ASSERT_TRUE(initialLoadReply.value(QStringLiteral("ok")).toBool())
        << initialLoadReply.value(QStringLiteral("message")).toString().toStdString();
    ASSERT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-system-status"));

    QJsonObject catalogDocumentReply;
    ASSERT_TRUE(sendAndWait(
        &client, QStringLiteral("catalog-document"), QStringLiteral("configDocument"),
        QJsonObject{{QStringLiteral("documentId"), QStringLiteral("test-config-catalog")}},
        &catalogDocumentReply));
    ASSERT_TRUE(catalogDocumentReply.value(QStringLiteral("ok")).toBool())
        << catalogDocumentReply.value(QStringLiteral("message")).toString().toStdString();
    const QJsonObject catalogDocument = catalogDocumentReply.value(QStringLiteral("data")).toObject();
    const QString revision = catalogDocument.value(QStringLiteral("revision")).toString();
    QJsonObject catalog = catalogDocument.value(QStringLiteral("value")).toObject();
    QJsonArray entries = catalog.value(QStringLiteral("entries")).toArray();
    ASSERT_FALSE(revision.isEmpty());

    bool disabledLaunchSelection = false;
    bool keptReplacementEnabled = false;
    for (int index = 0; index < entries.size(); ++index) {
        QJsonObject entry = entries.at(index).toObject();
        const QString documentId = entry.value(QStringLiteral("documentId")).toString();
        if (documentId == QStringLiteral("mbddf_system_status.testcfg.json")) {
            entry.insert(QStringLiteral("enabled"), false);
            disabledLaunchSelection = true;
        } else if (documentId == QStringLiteral("mbddf_elec_health.testcfg.json")) {
            entry.insert(QStringLiteral("enabled"), true);
            keptReplacementEnabled = true;
        }
        entries.replace(index, entry);
    }
    ASSERT_TRUE(disabledLaunchSelection);
    ASSERT_TRUE(keptReplacementEnabled);
    catalog.insert(QStringLiteral("entries"), entries);

    QJsonObject saveReply;
    ASSERT_TRUE(sendAndWait(
        &client, QStringLiteral("disable-launch-selection"), QStringLiteral("saveConfig"),
        QJsonObject{{QStringLiteral("documentId"), QStringLiteral("test-config-catalog")},
                    {QStringLiteral("expectedRevision"), revision},
                    {QStringLiteral("value"), catalog}},
        &saveReply));
    ASSERT_TRUE(saveReply.value(QStringLiteral("ok")).toBool())
        << saveReply.value(QStringLiteral("message")).toString().toStdString();
    ASSERT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-elec-health"));

    QJsonObject loadReply;
    ASSERT_TRUE(sendAndWait(&client, QStringLiteral("reload-after-catalog-save"),
                            QStringLiteral("load"), {}, &loadReply));
    ASSERT_TRUE(loadReply.value(QStringLiteral("ok")).toBool())
        << loadReply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-elec-health"));
}

TEST(WebSocketFrontendServerTest, QueuedSelectThenLoadKeepsSelectedCatalogConfiguration)
{
    DynamicCatalogFixture fixture;
    ASSERT_TRUE(fixture.initialize()) << fixture.error.toStdString();

    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, fixture.launchOptions, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    QJsonObject initialLoadReply;
    ASSERT_TRUE(sendAndWait(&client, QStringLiteral("initial-load"), QStringLiteral("load"),
                            {}, &initialLoadReply));
    ASSERT_TRUE(initialLoadReply.value(QStringLiteral("ok")).toBool())
        << initialLoadReply.value(QStringLiteral("message")).toString().toStdString();
    ASSERT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-system-status"));

    ASSERT_GT(client.sendText(compact(request(
                  QStringLiteral("select-replacement"), QStringLiteral("selectTest"),
                  QJsonObject{{QStringLiteral("configId"),
                               QStringLiteral("mbddf-elec-health")}}))),
              0);
    ASSERT_GT(client.sendText(compact(request(QStringLiteral("load-after-select"),
                                              QStringLiteral("load")))),
              0);

    QJsonObject selectReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("select-replacement"), &selectReply));
    ASSERT_TRUE(selectReply.value(QStringLiteral("ok")).toBool())
        << selectReply.value(QStringLiteral("message")).toString().toStdString();
    QJsonObject loadReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("load-after-select"), &loadReply));
    ASSERT_TRUE(loadReply.value(QStringLiteral("ok")).toBool())
        << loadReply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().descriptor.configId,
              QStringLiteral("mbddf-elec-health"));
}

TEST(WebSocketFrontendServerTest, RepliesToInvalidJsonWithoutClosingActiveClient)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendText(QStringLiteral("{")), 0);
    ASSERT_TRUE(client.waitForMessageCount(3));
    const QJsonObject reply = client.messages().at(2);
    EXPECT_EQ(reply.value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_json"));
    EXPECT_EQ(client.state(), QAbstractSocket::ConnectedState);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(WebSocketFrontendServerTest, PreservesValidRequestIdOnProtocolError)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendText(compact(request(QStringLiteral("bad-1"),
                                              QStringLiteral("unknown")))),
              0);
    ASSERT_TRUE(client.waitForMessageCount(3));
    const QJsonObject reply = client.messages().at(2);
    EXPECT_EQ(reply.value(QStringLiteral("id")).toString(), QStringLiteral("bad-1"));
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("unknown_action"));
}

TEST(WebSocketFrontendServerTest, RejectsNonWebSocketPath)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    QUrl wrong = server.webSocketUrl();
    wrong.setPath(QStringLiteral("/other"));

    test::WebSocketTestClient client;
    client.connectTo(wrong);
    ASSERT_TRUE(client.waitForDisconnected());
    EXPECT_EQ(client.closeCode(), QWebSocketProtocol::CloseCodePolicyViolated)
        << client.events().join(QStringLiteral(" | ")).toStdString();
}

TEST(WebSocketFrontendServerTest, RejectsRemoteOriginButStillAcceptsLocalOrigin)
{
    {
        TestApplicationController controller;
        WebSocketFrontendServer server(&controller, {}, testOptions());
        ASSERT_TRUE(server.listen());

        test::WebSocketTestClient rejected;
        EXPECT_FALSE(rejected.connectTo(server.webSocketUrl(),
                                        QStringLiteral("https://example.com"),
                                        1000));

        test::WebSocketTestClient accepted;
        ASSERT_TRUE(accepted.connectTo(server.webSocketUrl(),
                                       QStringLiteral("http://localhost:8080")));
        ASSERT_TRUE(accepted.waitForMessageCount(2));
    }

    // A dropped active client deliberately keeps the server busy until its
    // asynchronous cleanup finishes.  Use an independent server here so this
    // origin test does not race that separately covered cleanup contract.
    TestApplicationController standaloneController;
    WebSocketFrontendServer standaloneServer(
        &standaloneController, {}, testOptions());
    ASSERT_TRUE(standaloneServer.listen());
    test::WebSocketTestClient standalone;
    ASSERT_TRUE(standalone.connectTo(standaloneServer.webSocketUrl(),
                                     QStringLiteral("null")));
    EXPECT_TRUE(standalone.waitForMessageCount(2));
}

TEST(WebSocketFrontendServerTest, RejectsSecondClientAsBusy)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient first;
    ASSERT_TRUE(first.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(first.waitForMessageCount(2));

    test::WebSocketTestClient second;
    second.connectTo(server.webSocketUrl());
    ASSERT_TRUE(second.waitForMessageCount(1))
        << second.events().join(QStringLiteral(" | ")).toStdString();
    EXPECT_EQ(second.messages().first().value(QStringLiteral("code")).toString(),
              QStringLiteral("server_busy"));
    ASSERT_TRUE(second.waitForDisconnected());
    EXPECT_EQ(second.closeCode(), QWebSocketProtocol::CloseCodePolicyViolated)
        << second.events().join(QStringLiteral(" | ")).toStdString();
    EXPECT_EQ(first.state(), QAbstractSocket::ConnectedState);
}

TEST(WebSocketFrontendServerTest, RejectsBinaryMessages)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendBinary(QByteArrayLiteral("binary")), 0);
    ASSERT_TRUE(client.waitForMessageCount(3))
        << client.events().join(QStringLiteral(" | ")).toStdString();
    EXPECT_EQ(client.messages().at(2).value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    ASSERT_TRUE(client.waitForDisconnected());
    EXPECT_EQ(client.closeCode(), QWebSocketProtocol::CloseCodeDatatypeNotSupported);
}

TEST(WebSocketFrontendServerTest, RejectsTextLargerThanSixteenKiB)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    ASSERT_GT(client.sendText(QString(16385, QLatin1Char('x'))), 0);
    ASSERT_TRUE(client.waitForMessageCount(3))
        << client.events().join(QStringLiteral(" | ")).toStdString();
    EXPECT_EQ(client.messages().at(2).value(QStringLiteral("code")).toString(),
              QStringLiteral("message_too_large"));
    ASSERT_TRUE(client.waitForDisconnected());
    EXPECT_EQ(client.closeCode(), QWebSocketProtocol::CloseCodeTooMuchData);
}

TEST(WebSocketFrontendServerTest, AcceptsTextExactlyAtSixteenKiB)
{
    TestApplicationController controller;
    WebSocketFrontendServer server(&controller, {}, testOptions());
    ASSERT_TRUE(server.listen());
    test::WebSocketTestClient client;
    ASSERT_TRUE(client.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(client.waitForMessageCount(2));

    QJsonObject envelope = request(QStringLiteral("boundary"),
                                   QStringLiteral("snapshot"),
                                   QJsonObject{{QStringLiteral("padding"), QString()}});
    const int baseBytes = compact(envelope).toUtf8().size();
    ASSERT_LT(baseBytes, 16384);
    envelope[QStringLiteral("params")] = QJsonObject{
        {QStringLiteral("padding"), QString(16384 - baseBytes, QLatin1Char('x'))}};
    const QString text = compact(envelope);
    ASSERT_EQ(text.toUtf8().size(), 16384);

    ASSERT_GT(client.sendText(text), 0);
    QJsonObject reply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("boundary"), &reply));
    EXPECT_TRUE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(client.state(), QAbstractSocket::ConnectedState);
}

TEST(WebSocketFrontendServerTest, ReleasesPortWhenClosed)
{
    TestApplicationController controller;
    quint16 port = 0;
    {
        WebSocketFrontendServer first(&controller, {}, testOptions());
        ASSERT_TRUE(first.listen());
        port = first.serverPort();
        first.close();
        EXPECT_FALSE(first.isListening());
    }

    WebSocketFrontendServer second(&controller, {}, testOptions(port));
    QString error;
    EXPECT_TRUE(second.listen(&error)) << error.toStdString();
}

TEST(WebSocketFrontendServerTest, ReportsListenFailureWhenPortIsOccupied)
{
    TestApplicationController firstController;
    WebSocketFrontendServer first(&firstController, {}, testOptions());
    ASSERT_TRUE(first.listen());

    TestApplicationController secondController;
    WebSocketFrontendServer second(&secondController,
                                   {},
                                   testOptions(first.serverPort()));
    QString error;
    EXPECT_FALSE(second.listen(&error));
    EXPECT_FALSE(error.isEmpty());
}

} // namespace
} // namespace hwtest::app::web
