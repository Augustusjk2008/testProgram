#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QWebSocket>
#include <QWebSocketProtocol>

namespace hwtest::app::test {

class WebSocketTestClient final {
public:
    WebSocketTestClient();
    ~WebSocketTestClient();

    bool connectTo(const QUrl& url,
                   const QString& origin = QString(),
                   int timeoutMs = 3000);
    bool waitForMessageCount(int count, int timeoutMs = 3000);
    bool waitForReply(const QString& id,
                      QJsonObject* reply,
                      int timeoutMs = 3000);
    bool waitForSnapshotPhase(const QString& phase,
                              QJsonObject* message,
                              int timeoutMs = 5000);
    bool waitForDisconnected(int timeoutMs = 3000);

    qint64 sendText(const QString& text);
    qint64 sendBinary(const QByteArray& data);
    void close();

    const QVector<QJsonObject>& messages() const;
    QAbstractSocket::SocketState state() const;
    QWebSocketProtocol::CloseCode closeCode() const;
    QString errorString() const;
    QStringList events() const;

private:
    QWebSocket m_socket;
    QVector<QJsonObject> m_messages;
    QStringList m_events;
};

} // namespace hwtest::app::test
