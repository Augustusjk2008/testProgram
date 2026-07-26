#include "web_socket_frontend_server.h"

#include "support/websocket_test_client.h"

#include <gtest/gtest.h>

#include <QHostAddress>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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
    accepted.close();
    ASSERT_TRUE(accepted.waitForDisconnected());

    test::WebSocketTestClient standalone;
    ASSERT_TRUE(standalone.connectTo(server.webSocketUrl(), QStringLiteral("null")));
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
