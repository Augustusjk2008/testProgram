#include "web_socket_frontend_server.h"

#include "support/websocket_test_client.h"
#include "support/mbddf_udp_test_peer.h"
#include "support/application_snapshot_test_utils.h"

#include <app/tui_shell.h>

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

namespace hwtest::app::web {
namespace {

FrontendLaunchOptions launchOptions(const QString& halConfigPath = {})
{
    return FrontendLaunchOptions{QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                 halConfigPath.isEmpty()
                                     ? QStringLiteral(HWTEST_APP_HAL_CONFIG)
                                     : halConfigPath,
                                 {},
                                 {}};
}

QString requestText(const QString& id,
                    const QString& action,
                    const QJsonObject& params = {})
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("v"), 1},
                                  {QStringLiteral("type"),
                                   QStringLiteral("request")},
                                  {QStringLiteral("id"), id},
                                  {QStringLiteral("action"), action},
                                  {QStringLiteral("params"), params}})
            .toJson(QJsonDocument::Compact));
}

void connectClient(WebSocketFrontendServer* server,
                   test::WebSocketTestClient* client)
{
    ASSERT_TRUE(server->listen());
    ASSERT_TRUE(client->connectTo(server->webSocketUrl()))
        << client->errorString().toStdString();
    ASSERT_TRUE(client->waitForMessageCount(2));
}

QJsonObject sendAndWait(test::WebSocketTestClient* client,
                        const QString& id,
                        const QString& action,
                        const QJsonObject& params = {})
{
    EXPECT_GT(client->sendText(requestText(id, action, params)), 0);
    QJsonObject reply;
    EXPECT_TRUE(client->waitForReply(id, &reply))
        << client->events().join(QStringLiteral(" | ")).toStdString();
    return reply;
}

TEST(WebSocketControllerIntegrationTest, QueuesLoadAndReturnsCachedSnapshot)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    ASSERT_GT(client.sendText(requestText(QStringLiteral("load-1"),
                                          QStringLiteral("load"))),
              0);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));

    QJsonObject loadReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("load-1"), &loadReply));
    EXPECT_TRUE(loadReply.value(QStringLiteral("ok")).toBool())
        << loadReply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));

    const QJsonObject snapshotReply = sendAndWait(
        &client, QStringLiteral("snapshot-1"), QStringLiteral("snapshot"));
    ASSERT_TRUE(snapshotReply.value(QStringLiteral("ok")).toBool());
    const QJsonObject data = snapshotReply.value(QStringLiteral("data")).toObject();
    EXPECT_GE(data.value(QStringLiteral("seq")).toInt(), 1);
    EXPECT_EQ(data.value(QStringLiteral("snapshot"))
                  .toObject()
                  .value(QStringLiteral("phase"))
                  .toString(),
              QStringLiteral("configured"));
}

TEST(WebSocketControllerIntegrationTest, MapsControlsPortsAndSelections)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QJsonObject controls = sendAndWait(
        &client, QStringLiteral("controls"), QStringLiteral("controls"));
    const QJsonArray controlArray = controls.value(QStringLiteral("data"))
                                        .toObject()
                                        .value(QStringLiteral("controls"))
                                        .toArray();
    ASSERT_GE(controlArray.size(), 2);
    EXPECT_TRUE(controlArray.first().toObject().contains(QStringLiteral("resourceId")));
    EXPECT_TRUE(controlArray.first().toObject().contains(QStringLiteral("providerId")));

    const QJsonObject ports = sendAndWait(
        &client, QStringLiteral("ports"), QStringLiteral("ports"));
    EXPECT_TRUE(ports.value(QStringLiteral("data"))
                    .toObject()
                    .value(QStringLiteral("ports"))
                    .isArray());

    const QJsonObject selected = sendAndWait(
        &client,
        QStringLiteral("select-network"),
        QStringLiteral("selectControl"),
        QJsonObject{{QStringLiteral("resourceId"),
                     QStringLiteral("CONTROL_NETWORK")}});
    EXPECT_TRUE(selected.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(controller.snapshot().providerId, QStringLiteral("qt.udp"));

    const QJsonObject missing = sendAndWait(
        &client,
        QStringLiteral("missing-port"),
        QStringLiteral("selectSerialPort"));
    EXPECT_FALSE(missing.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(missing.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));
}

TEST(WebSocketControllerIntegrationTest, ReturnsControllerErrorsAndRemainsUsable)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject prepare = sendAndWait(
        &client, QStringLiteral("prepare"), QStringLiteral("prepare"));
    EXPECT_FALSE(prepare.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(prepare.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));

    const QJsonObject stop = sendAndWait(
        &client, QStringLiteral("stop"), QStringLiteral("stop"));
    EXPECT_FALSE(stop.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(stop.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));

    const QJsonObject load = sendAndWait(
        &client, QStringLiteral("load"), QStringLiteral("load"));
    EXPECT_TRUE(load.value(QStringLiteral("ok")).toBool());
}

TEST(WebSocketControllerIntegrationTest, RejectsMistypedContinuousRunParametersAtBoundary)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject reply = sendAndWait(
        &client,
        QStringLiteral("bad-run-options"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), QStringLiteral("fast")},
                    {QStringLiteral("maxCycles"), 2}});

    EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(WebSocketControllerIntegrationTest, RejectsClientSuppliedConfigurationPaths)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject reply = sendAndWait(
        &client,
        QStringLiteral("load-path"),
        QStringLiteral("load"),
        QJsonObject{{QStringLiteral("testConfigPath"),
                     QStringLiteral("C:/untrusted.json")}});
    EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(WebSocketControllerIntegrationTest, ValidatesDigitalStimulusActionWhitelist)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject leaked = sendAndWait(
        &client,
        QStringLiteral("stimulus-leak"),
        QStringLiteral("setDigitalStimulus"),
        QJsonObject{{QStringLiteral("switchId"), QStringLiteral("di0")},
                    {QStringLiteral("active"), true},
                    {QStringLiteral("expectedRevision"), 0},
                    {QStringLiteral("resourceId"), QStringLiteral("DUT_DI0_STIM")}});
    EXPECT_FALSE(leaked.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(leaked.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));

    const QJsonObject wrongType = sendAndWait(
        &client,
        QStringLiteral("stimulus-type"),
        QStringLiteral("setDigitalStimulus"),
        QJsonObject{{QStringLiteral("switchId"), QStringLiteral("di0")},
                    {QStringLiteral("active"), QStringLiteral("true")},
                    {QStringLiteral("expectedRevision"), 0}});
    EXPECT_FALSE(wrongType.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(wrongType.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));

    const QJsonObject resetWithParams = sendAndWait(
        &client,
        QStringLiteral("stimulus-reset"),
        QStringLiteral("resetDigitalStimulus"),
        QJsonObject{{QStringLiteral("adapterId"), QStringLiteral("ni.daqmx")}});
    EXPECT_FALSE(resetWithParams.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resetWithParams.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));

    const QJsonObject unavailable = sendAndWait(
        &client,
        QStringLiteral("stimulus-unavailable"),
        QStringLiteral("setDigitalStimulus"),
        QJsonObject{{QStringLiteral("switchId"), QStringLiteral("di0")},
                    {QStringLiteral("active"), true},
                    {QStringLiteral("expectedRevision"), 0}});
    EXPECT_FALSE(unavailable.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(unavailable.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));
    const QJsonObject authoritative = unavailable
        .value(QStringLiteral("data"))
        .toObject()
        .value(QStringLiteral("digitalStimulus"))
        .toObject();
    EXPECT_FALSE(authoritative.isEmpty());
    EXPECT_FALSE(authoritative.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(authoritative.value(QStringLiteral("revision")).toDouble(), 0.0);
}

TEST(WebSocketControllerIntegrationTest, DisconnectShutsDownSessionAndKeepsListening)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient first;
    connectClient(&server, &first);
    ASSERT_TRUE(sendAndWait(&first,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    ASSERT_GT(first.sendText(requestText(QStringLiteral("disconnect"),
                                         QStringLiteral("disconnect"))),
              0);
    QJsonObject reply;
    ASSERT_TRUE(first.waitForReply(QStringLiteral("disconnect"), &reply));
    EXPECT_TRUE(reply.value(QStringLiteral("ok")).toBool())
        << reply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_TRUE(first.waitForDisconnected());
    EXPECT_TRUE(server.isListening());
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));

    test::WebSocketTestClient second;
    ASSERT_TRUE(second.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(second.waitForMessageCount(2));
    EXPECT_EQ(second.messages()
                  .at(1)
                  .value(QStringLiteral("snapshot"))
                  .toObject()
                  .value(QStringLiteral("phase"))
                  .toString(),
              QStringLiteral("configured"));
    EXPECT_TRUE(sendAndWait(&second,
                            QStringLiteral("reload"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());
}

TEST(WebSocketControllerIntegrationTest, QuitRepliesThenStopsListeningAndRequestsExit)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    bool quitRequested = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&server,
                     &WebSocketFrontendServer::quitRequested,
                     &loop,
                     [&] {
                         quitRequested = true;
                         loop.quit();
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    ASSERT_GT(client.sendText(requestText(QStringLiteral("quit"),
                                          QStringLiteral("quit"))),
              0);
    QJsonObject reply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("quit"), &reply));
    EXPECT_TRUE(reply.value(QStringLiteral("ok")).toBool())
        << reply.value(QStringLiteral("message")).toString().toStdString();
    if (!quitRequested) {
        timeout.start(3000);
        loop.exec();
    }
    EXPECT_TRUE(quitRequested);
    EXPECT_FALSE(server.isListening());
    EXPECT_TRUE(client.waitForDisconnected());
}

TEST(WebSocketControllerIntegrationTest, QuitStillCompletesAfterClientDrops)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    bool quitRequested = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&server,
                     &WebSocketFrontendServer::quitRequested,
                     &loop,
                     [&] {
                         quitRequested = true;
                         loop.quit();
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    bool closedDuringShutdown = false;
    QObject::connect(&controller,
                     &TestApplicationController::snapshotChanged,
                     &server,
                     [&](const ApplicationSnapshot& snapshot) {
                         if (!closedDuringShutdown &&
                             snapshot.phase == QStringLiteral("configured")) {
                             closedDuringShutdown = true;
                             client.close();
                         }
                     });

    ASSERT_GT(client.sendText(requestText(QStringLiteral("quit-drop"),
                                          QStringLiteral("quit"))),
              0);
    ASSERT_TRUE(client.waitForDisconnected());
    timeout.start(3000);
    loop.exec();

    EXPECT_TRUE(quitRequested);
    EXPECT_TRUE(closedDuringShutdown);
    EXPECT_FALSE(server.isListening());
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
}

TEST(WebSocketControllerIntegrationTest, DroppedClientStillShutsDownBeforeReaccepting)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient first;
    connectClient(&server, &first);
    ASSERT_TRUE(sendAndWait(&first,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    bool shutdownSnapshotSeen = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&controller,
                     &TestApplicationController::snapshotChanged,
                     &loop,
                     [&](const ApplicationSnapshot& snapshot) {
                         if (snapshot.phase == QStringLiteral("configured")) {
                             shutdownSnapshotSeen = true;
                             loop.quit();
                         }
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    first.close();
    ASSERT_TRUE(first.waitForDisconnected());
    timeout.start(3000);
    loop.exec();
    ASSERT_TRUE(shutdownSnapshotSeen);

    test::WebSocketTestClient second;
    ASSERT_TRUE(second.connectTo(server.webSocketUrl()));
    EXPECT_TRUE(second.waitForMessageCount(2));
}

class WebSocketUdpIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
            GTEST_SKIP() << "MB_DDF protocol assets are not available";
        }
        ASSERT_TRUE(directory.isValid());
        ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
        ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                        &directory,
                                        &halConfigPath,
                                        &error))
            << error.toStdString();

        WebSocketServerOptions options;
        options.port = 0;
        server = std::make_unique<WebSocketFrontendServer>(
            &controller, launchOptions(halConfigPath), options);
        client = std::make_unique<test::WebSocketTestClient>();
        connectClient(server.get(), client.get());
        ASSERT_TRUE(sendAndWait(client.get(),
                                QStringLiteral("load"),
                                QStringLiteral("load"))
                        .value(QStringLiteral("ok"))
                        .toBool());
        ASSERT_TRUE(sendAndWait(client.get(),
                                QStringLiteral("prepare"),
                                QStringLiteral("prepare"))
                        .value(QStringLiteral("ok"))
                        .toBool());
        ASSERT_TRUE(sendAndWait(client.get(),
                                QStringLiteral("start"),
                                QStringLiteral("start"))
                        .value(QStringLiteral("ok"))
                        .toBool());
        ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    }

    void disconnectIfNeeded()
    {
        if (client == nullptr ||
            client->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        client->sendText(requestText(QStringLiteral("cleanup"),
                                     QStringLiteral("disconnect")));
        QJsonObject reply;
        client->waitForReply(QStringLiteral("cleanup"), &reply, 5000);
        client->waitForDisconnected(5000);
    }

    TestApplicationController controller;
    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString error;
    QString halConfigPath;
    std::unique_ptr<WebSocketFrontendServer> server;
    std::unique_ptr<test::WebSocketTestClient> client;
};

TEST(WebSocketContinuousIntegrationTest, PcPeriodicStreamsSamplesFromTwoCommandResponseCycles)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString error;
    QString halConfigPath;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halConfigPath,
                                    &error))
        << error.toStdString();

    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller,
                                   launchOptions(halConfigPath),
                                   options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client, QStringLiteral("load"), QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("prepare"),
                            QStringLiteral("prepare"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QJsonObject started = sendAndWait(
        &client,
        QStringLiteral("periodic"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 2}});
    ASSERT_TRUE(started.value(QStringLiteral("ok")).toBool())
        << started.value(QStringLiteral("message")).toString().toStdString();

    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
        ASSERT_TRUE(peer.replyToLastRequest(&error)) << error.toStdString();
    }

    QJsonObject terminal;
    ASSERT_TRUE(client.waitForSnapshotPhase(QStringLiteral("finished"),
                                            &terminal,
                                            5000));
    QVector<QJsonObject> samples;
    for (const QJsonObject& message : client.messages()) {
        if (message.value(QStringLiteral("type")).toString() ==
            QStringLiteral("sample")) {
            samples.push_back(message.value(QStringLiteral("sample")).toObject());
        }
    }
    ASSERT_EQ(samples.size(), 2);
    EXPECT_EQ(samples.at(0).value(QStringLiteral("cycleIndex")).toInt(), 1);
    EXPECT_EQ(samples.at(1).value(QStringLiteral("cycleIndex")).toInt(), 2);
    EXPECT_GT(samples.at(0).value(QStringLiteral("timestampUs")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(samples.at(0)
                         .value(QStringLiteral("values"))
                         .toObject()
                         .value(QStringLiteral("cpu_usage"))
                         .toDouble(),
                     12.5);

    const QJsonObject snapshot = terminal.value(QStringLiteral("snapshot")).toObject();
    EXPECT_EQ(snapshot.value(QStringLiteral("runMode")).toString(),
              QStringLiteral("pc_periodic"));
    EXPECT_EQ(snapshot.value(QStringLiteral("intervalMs")).toInt(), 10);
    EXPECT_EQ(snapshot.value(QStringLiteral("maxCycles")).toInt(), 2);
    EXPECT_EQ(snapshot.value(QStringLiteral("cycleIndex")).toInt(), 2);
    EXPECT_EQ(snapshot.value(QStringLiteral("sampleCount")).toInt(), 2);

    const QJsonObject disconnected = sendAndWait(&client,
                                                  QStringLiteral("cleanup"),
                                                  QStringLiteral("disconnect"));
    EXPECT_TRUE(disconnected.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(client.waitForDisconnected(5000));
}

TEST_F(WebSocketUdpIntegrationTest, PassesSystemStatusThroughHalUdp)
{
    ASSERT_TRUE(peer.replyToLastRequest(&error)) << error.toStdString();
    QJsonObject message;
    ASSERT_TRUE(client->waitForSnapshotPhase(QStringLiteral("finished"),
                                             &message,
                                             5000));
    const QJsonObject snapshot = message.value(QStringLiteral("snapshot")).toObject();
    EXPECT_TRUE(snapshot.value(QStringLiteral("hasResult")).toBool());
    EXPECT_EQ(snapshot.value(QStringLiteral("verdict")).toString(),
              QStringLiteral("Pass"));
    EXPECT_TRUE(snapshot.value(QStringLiteral("rawData"))
                    .toObject()
                    .contains(QStringLiteral("responseValues")));
    disconnectIfNeeded();
}

TEST_F(WebSocketUdpIntegrationTest, ReportsTimeoutWithoutPeerResponse)
{
    QJsonObject message;
    ASSERT_TRUE(client->waitForSnapshotPhase(QStringLiteral("error"),
                                             &message,
                                             6000));
    const QJsonObject snapshot = message.value(QStringLiteral("snapshot")).toObject();
    EXPECT_TRUE(snapshot.value(QStringLiteral("hasResult")).toBool());
    EXPECT_EQ(snapshot.value(QStringLiteral("verdict")).toString(),
              QStringLiteral("Error"));
    EXPECT_EQ(snapshot.value(QStringLiteral("errorCode")).toString(),
              QStringLiteral("BusTimeout"));
    disconnectIfNeeded();
}

TEST_F(WebSocketUdpIntegrationTest, StopIsAsyncAndRejectsWritesWhilePending)
{
    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeatCount; });
    heartbeat.start();

    ASSERT_GT(client->sendText(requestText(QStringLiteral("stop"),
                                           QStringLiteral("stop"))),
              0);
    ASSERT_GT(client->sendText(requestText(QStringLiteral("pause"),
                                           QStringLiteral("pause"))),
              0);

    QJsonObject blocked;
    ASSERT_TRUE(client->waitForReply(QStringLiteral("pause"), &blocked, 5000));
    EXPECT_FALSE(blocked.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(blocked.value(QStringLiteral("code")).toString(),
              QStringLiteral("command_in_progress"));

    QJsonObject stopped;
    ASSERT_TRUE(client->waitForReply(QStringLiteral("stop"), &stopped, 5000));
    EXPECT_TRUE(stopped.value(QStringLiteral("ok")).toBool())
        << stopped.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("stopped"));
    EXPECT_GT(heartbeatCount, 0);
    disconnectIfNeeded();
}

TEST_F(WebSocketUdpIntegrationTest, PausesAndResumesThroughController)
{
    const QJsonObject paused = sendAndWait(client.get(),
                                           QStringLiteral("pause"),
                                           QStringLiteral("pause"));
    ASSERT_TRUE(paused.value(QStringLiteral("ok")).toBool())
        << paused.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("paused"));

    const QJsonObject resumed = sendAndWait(client.get(),
                                            QStringLiteral("resume"),
                                            QStringLiteral("resume"));
    ASSERT_TRUE(resumed.value(QStringLiteral("ok")).toBool())
        << resumed.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("running"));

    const QJsonObject stopped = sendAndWait(client.get(),
                                            QStringLiteral("stop"),
                                            QStringLiteral("stop"));
    EXPECT_TRUE(stopped.value(QStringLiteral("ok")).toBool());
    disconnectIfNeeded();
}

TEST_F(WebSocketUdpIntegrationTest, DisconnectStopsBeforeShutdown)
{
    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeatCount; });
    heartbeat.start();

    ASSERT_GT(client->sendText(requestText(QStringLiteral("disconnect-run"),
                                           QStringLiteral("disconnect"))),
              0);
    ASSERT_GT(client->sendText(requestText(QStringLiteral("start-again"),
                                           QStringLiteral("start"))),
              0);
    QJsonObject blocked;
    ASSERT_TRUE(client->waitForReply(QStringLiteral("start-again"),
                                     &blocked,
                                     5000));
    EXPECT_EQ(blocked.value(QStringLiteral("code")).toString(),
              QStringLiteral("command_in_progress"));

    QJsonObject disconnected;
    ASSERT_TRUE(client->waitForReply(QStringLiteral("disconnect-run"),
                                     &disconnected,
                                     5000));
    EXPECT_TRUE(disconnected.value(QStringLiteral("ok")).toBool())
        << disconnected.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_TRUE(client->waitForDisconnected(5000));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
    EXPECT_GT(heartbeatCount, 0);
}

TEST_F(WebSocketUdpIntegrationTest, DroppedRunningClientStopsAndReaccepts)
{
    bool configured = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&controller,
                     &TestApplicationController::snapshotChanged,
                     &loop,
                     [&](const ApplicationSnapshot& snapshot) {
                         if (snapshot.phase == QStringLiteral("configured")) {
                             configured = true;
                             loop.quit();
                         }
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    client->close();
    ASSERT_TRUE(client->waitForDisconnected());
    timeout.start(5000);
    loop.exec();
    ASSERT_TRUE(configured);

    test::WebSocketTestClient replacement;
    ASSERT_TRUE(replacement.connectTo(server->webSocketUrl()));
    EXPECT_TRUE(replacement.waitForMessageCount(2));
}

enum class EquivalenceScenario {
    Pass,
    Timeout,
    Stop,
};

struct EquivalenceResult {
    bool ok = false;
    QString error;
    ApplicationSnapshot snapshot;
};

EquivalenceResult runTuiEquivalenceScenario(EquivalenceScenario scenario)
{
    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString halConfigPath;
    QString error;
    if (!directory.isValid() || !peer.bind(&error) ||
        !peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                             &directory,
                             &halConfigPath,
                             &error)) {
        return {false, error, {}};
    }

    TestApplicationController controller;
    TuiShell shell(&controller,
                   QStringLiteral(HWTEST_APP_TEST_CONFIG),
                   halConfigPath);
    for (const QString& command : {QStringLiteral("load"),
                                   QStringLiteral("prepare"),
                                   QStringLiteral("run")}) {
        const TuiReply reply = shell.execute(command);
        if (reply.lines.size() != 1 ||
            !reply.lines.first().startsWith(QStringLiteral("ok "))) {
            return {false, reply.lines.join(QStringLiteral(" | ")), {}};
        }
    }
    if (!peer.waitForRequest(3000, &error)) {
        return {false, error, {}};
    }
    if (scenario == EquivalenceScenario::Pass &&
        !peer.replyToLastRequest(&error)) {
        return {false, error, {}};
    }
    if (scenario == EquivalenceScenario::Stop) {
        const TuiReply stopped = shell.execute(QStringLiteral("stop 5000"));
        if (stopped.lines.size() != 1 ||
            stopped.lines.first() != QStringLiteral("ok stop")) {
            return {false, stopped.lines.join(QStringLiteral(" | ")), {}};
        }
    }
    shell.execute(QStringLiteral("wait 5000"));

    const QString expectedPhase = scenario == EquivalenceScenario::Pass
        ? QStringLiteral("finished")
        : scenario == EquivalenceScenario::Timeout
            ? QStringLiteral("error")
            : QStringLiteral("stopped");
    if (controller.snapshot().phase != expectedPhase) {
        return {false,
                QStringLiteral("TUI phase is %1").arg(controller.snapshot().phase),
                {}};
    }
    const ApplicationSnapshot snapshot = controller.snapshot();
    const ActionResult shutdown = controller.shutdown();
    return shutdown.ok ? EquivalenceResult{true, {}, snapshot}
                       : EquivalenceResult{false, shutdown.message, {}};
}

EquivalenceResult runWebEquivalenceScenario(EquivalenceScenario scenario)
{
    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString halConfigPath;
    QString error;
    if (!directory.isValid() || !peer.bind(&error) ||
        !peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                             &directory,
                             &halConfigPath,
                             &error)) {
        return {false, error, {}};
    }

    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller,
                                   launchOptions(halConfigPath),
                                   options);
    test::WebSocketTestClient client;
    if (!server.listen(&error) || !client.connectTo(server.webSocketUrl()) ||
        !client.waitForMessageCount(2)) {
        return {false,
                error.isEmpty() ? client.errorString() : error,
                {}};
    }
    for (const QString& action : {QStringLiteral("load"),
                                  QStringLiteral("prepare"),
                                  QStringLiteral("start")}) {
        const QJsonObject reply = sendAndWait(&client, action, action);
        if (!reply.value(QStringLiteral("ok")).toBool()) {
            return {false,
                    reply.value(QStringLiteral("message")).toString(),
                    {}};
        }
    }
    if (!peer.waitForRequest(3000, &error)) {
        return {false, error, {}};
    }
    if (scenario == EquivalenceScenario::Pass &&
        !peer.replyToLastRequest(&error)) {
        return {false, error, {}};
    }
    if (scenario == EquivalenceScenario::Stop) {
        const QJsonObject stopped = sendAndWait(&client,
                                                QStringLiteral("stop"),
                                                QStringLiteral("stop"));
        if (!stopped.value(QStringLiteral("ok")).toBool()) {
            return {false,
                    stopped.value(QStringLiteral("message")).toString(),
                    {}};
        }
    }

    const QString expectedPhase = scenario == EquivalenceScenario::Pass
        ? QStringLiteral("finished")
        : scenario == EquivalenceScenario::Timeout
            ? QStringLiteral("error")
            : QStringLiteral("stopped");
    QJsonObject terminal;
    if (!client.waitForSnapshotPhase(expectedPhase, &terminal, 6000)) {
        return {false,
                QStringLiteral("Web phase did not reach %1").arg(expectedPhase),
                {}};
    }

    const ApplicationSnapshot snapshot = controller.snapshot();
    const QJsonObject disconnected = sendAndWait(&client,
                                                 QStringLiteral("disconnect"),
                                                 QStringLiteral("disconnect"));
    if (!disconnected.value(QStringLiteral("ok")).toBool() ||
        !client.waitForDisconnected(5000)) {
        return {false,
                disconnected.value(QStringLiteral("message")).toString(),
                {}};
    }
    return {true, {}, snapshot};
}

TEST(FrontendEquivalenceTest, TuiAndWebProduceEquivalentConfiguredSnapshot)
{
    TestApplicationController tuiController;
    TuiShell shell(&tuiController,
                   QStringLiteral(HWTEST_APP_TEST_CONFIG),
                   QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_EQ(shell.execute(QStringLiteral("load")).lines.first(),
              QStringLiteral("ok load"));

    TestApplicationController webController;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&webController, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    test::expectSemanticallyEquivalentSnapshots(tuiController.snapshot(),
                                                webController.snapshot());
    EXPECT_TRUE(tuiController.shutdown().ok);
    EXPECT_TRUE(sendAndWait(&client,
                            QStringLiteral("disconnect"),
                            QStringLiteral("disconnect"))
                    .value(QStringLiteral("ok"))
                    .toBool());
}

TEST(FrontendEquivalenceTest, TuiAndWebProduceEquivalentUdpPassResult)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }
    const EquivalenceResult tui =
        runTuiEquivalenceScenario(EquivalenceScenario::Pass);
    ASSERT_TRUE(tui.ok) << tui.error.toStdString();
    const EquivalenceResult web =
        runWebEquivalenceScenario(EquivalenceScenario::Pass);
    ASSERT_TRUE(web.ok) << web.error.toStdString();
    test::expectSemanticallyEquivalentSnapshots(tui.snapshot, web.snapshot);
}

TEST(FrontendEquivalenceTest, TuiAndWebProduceEquivalentTimeoutError)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }
    const EquivalenceResult tui =
        runTuiEquivalenceScenario(EquivalenceScenario::Timeout);
    ASSERT_TRUE(tui.ok) << tui.error.toStdString();
    const EquivalenceResult web =
        runWebEquivalenceScenario(EquivalenceScenario::Timeout);
    ASSERT_TRUE(web.ok) << web.error.toStdString();
    test::expectSemanticallyEquivalentSnapshots(tui.snapshot, web.snapshot);
}

TEST(FrontendEquivalenceTest, TuiAndWebConvergeToEquivalentStoppedState)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }
    const EquivalenceResult tui =
        runTuiEquivalenceScenario(EquivalenceScenario::Stop);
    ASSERT_TRUE(tui.ok) << tui.error.toStdString();
    const EquivalenceResult web =
        runWebEquivalenceScenario(EquivalenceScenario::Stop);
    ASSERT_TRUE(web.ok) << web.error.toStdString();
    test::expectSemanticallyEquivalentSnapshots(tui.snapshot, web.snapshot);
}

} // namespace
} // namespace hwtest::app::web
