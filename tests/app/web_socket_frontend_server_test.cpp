#include "web_socket_frontend_server.h"

#include "support/websocket_test_client.h"

#include <gtest/gtest.h>

#include <QHostAddress>
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
    EXPECT_TRUE(accepted.waitForMessageCount(2));
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
