#include "websocket_test_client.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QTimer>

namespace hwtest::app::test {

namespace {

bool runUntil(QEventLoop* loop, QTimer* timer, int timeoutMs)
{
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, loop, &QEventLoop::quit);
    timer->start(timeoutMs);
    loop->exec();
    return timer->isActive();
}

void settleQueuedSocketSignals()
{
    QEventLoop loop;
    QTimer::singleShot(0, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

WebSocketTestClient::WebSocketTestClient()
{
    QObject::connect(&m_socket, &QWebSocket::connected, [&] {
        m_events.push_back(QStringLiteral("connected"));
    });
    QObject::connect(&m_socket,
                     &QWebSocket::stateChanged,
                     [&](QAbstractSocket::SocketState state) {
                         m_events.push_back(QStringLiteral("state:%1").arg(state));
                     });
    QObject::connect(&m_socket,
                     &QWebSocket::textMessageReceived,
                     [this](const QString& text) {
                         const QJsonDocument document =
                             QJsonDocument::fromJson(text.toUtf8());
                         if (document.isObject()) {
                             m_messages.push_back(document.object());
                             m_events.push_back(QStringLiteral("text:%1")
                                                    .arg(document.object()
                                                             .value(QStringLiteral("type"))
                                                             .toString()));
                         }
                     });
    QObject::connect(&m_socket, &QWebSocket::disconnected, [this] {
        m_events.push_back(QStringLiteral("disconnected:%1").arg(m_socket.closeCode()));
    });
}

WebSocketTestClient::~WebSocketTestClient()
{
    QObject::disconnect(&m_socket, nullptr, nullptr, nullptr);
    m_socket.abort();
}

bool WebSocketTestClient::connectTo(const QUrl& url,
                                    const QString& origin,
                                    int timeoutMs)
{
    m_messages.clear();
    QEventLoop loop;
    QTimer timer;
    const QMetaObject::Connection connected =
        QObject::connect(&m_socket, &QWebSocket::connected, &loop, &QEventLoop::quit);
    const QMetaObject::Connection disconnected =
        QObject::connect(&m_socket, &QWebSocket::disconnected, &loop, &QEventLoop::quit);

    QNetworkRequest request(url);
    if (!origin.isNull()) {
        request.setRawHeader(QByteArrayLiteral("Origin"), origin.toUtf8());
    }
    m_socket.open(request);
    const bool beforeTimeout = runUntil(&loop, &timer, timeoutMs);

    QObject::disconnect(connected);
    QObject::disconnect(disconnected);
    return beforeTimeout && m_socket.state() == QAbstractSocket::ConnectedState;
}

bool WebSocketTestClient::waitForMessageCount(int count, int timeoutMs)
{
    if (m_messages.size() >= count) {
        return true;
    }
    QEventLoop loop;
    QTimer timer;
    const QMetaObject::Connection message = QObject::connect(
        &m_socket,
        &QWebSocket::textMessageReceived,
        &loop,
        [&](const QString&) {
            if (m_messages.size() >= count) {
                loop.quit();
            }
        });
    runUntil(&loop, &timer, timeoutMs);
    QObject::disconnect(message);
    return m_messages.size() >= count;
}

bool WebSocketTestClient::waitForReply(const QString& id,
                                       QJsonObject* reply,
                                       int timeoutMs)
{
    const auto findReply = [&]() -> QJsonObject {
        for (auto it = m_messages.crbegin(); it != m_messages.crend(); ++it) {
            if (it->value(QStringLiteral("type")).toString() ==
                    QStringLiteral("reply") &&
                it->value(QStringLiteral("id")).toString() == id) {
                return *it;
            }
        }
        return {};
    };

    QJsonObject found = findReply();
    if (found.isEmpty()) {
        QEventLoop loop;
        QTimer timer;
        const QMetaObject::Connection message = QObject::connect(
            &m_socket,
            &QWebSocket::textMessageReceived,
            &loop,
            [&](const QString&) {
                if (!findReply().isEmpty()) {
                    loop.quit();
                }
            });
        runUntil(&loop, &timer, timeoutMs);
        QObject::disconnect(message);
        found = findReply();
    }

    if (reply != nullptr) {
        *reply = found;
    }
    return !found.isEmpty();
}

bool WebSocketTestClient::waitForSnapshotPhase(const QString& phase,
                                               QJsonObject* message,
                                               int timeoutMs)
{
    const auto findSnapshot = [&]() -> QJsonObject {
        for (auto it = m_messages.crbegin(); it != m_messages.crend(); ++it) {
            if (it->value(QStringLiteral("type")).toString() ==
                    QStringLiteral("snapshot") &&
                it->value(QStringLiteral("snapshot"))
                        .toObject()
                        .value(QStringLiteral("phase"))
                        .toString() == phase) {
                return *it;
            }
        }
        return {};
    };

    QJsonObject found = findSnapshot();
    if (found.isEmpty()) {
        QEventLoop loop;
        QTimer timer;
        const QMetaObject::Connection snapshot = QObject::connect(
            &m_socket,
            &QWebSocket::textMessageReceived,
            &loop,
            [&](const QString&) {
                if (!findSnapshot().isEmpty()) {
                    loop.quit();
                }
            });
        runUntil(&loop, &timer, timeoutMs);
        QObject::disconnect(snapshot);
        found = findSnapshot();
    }

    if (message != nullptr) {
        *message = found;
    }
    return !found.isEmpty();
}

bool WebSocketTestClient::waitForDisconnected(int timeoutMs)
{
    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        settleQueuedSocketSignals();
        return true;
    }
    QEventLoop loop;
    QTimer timer;
    const QMetaObject::Connection disconnected = QObject::connect(
        &m_socket,
        &QWebSocket::disconnected,
        &loop,
        [&loop] { QTimer::singleShot(0, &loop, &QEventLoop::quit); });
    runUntil(&loop, &timer, timeoutMs);
    QObject::disconnect(disconnected);
    settleQueuedSocketSignals();
    return m_socket.state() == QAbstractSocket::UnconnectedState;
}

qint64 WebSocketTestClient::sendText(const QString& text)
{
    return m_socket.sendTextMessage(text);
}

qint64 WebSocketTestClient::sendBinary(const QByteArray& data)
{
    return m_socket.sendBinaryMessage(data);
}

void WebSocketTestClient::close()
{
    m_socket.close();
}

const QVector<QJsonObject>& WebSocketTestClient::messages() const
{
    return m_messages;
}

QAbstractSocket::SocketState WebSocketTestClient::state() const
{
    return m_socket.state();
}

QWebSocketProtocol::CloseCode WebSocketTestClient::closeCode() const
{
    return m_socket.closeCode();
}

QString WebSocketTestClient::errorString() const
{
    return m_socket.errorString();
}

QStringList WebSocketTestClient::events() const
{
    return m_events;
}

} // namespace hwtest::app::test
