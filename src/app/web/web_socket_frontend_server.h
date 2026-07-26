#pragma once

#include <app/frontend_launch_options.h>
#include <app/test_application_controller.h>

#include <QHostAddress>
#include <QObject>
#include <QUrl>

#include <memory>

namespace hwtest::app::web {

struct WebSocketServerOptions {
    quint16 port = 8765;
    int handshakeTimeoutMs = 5000;
    quint64 maxIncomingMessageBytes = 16384;
};

class WebSocketFrontendServer final : public QObject {
    Q_OBJECT

public:
    explicit WebSocketFrontendServer(TestApplicationController* controller,
                                     FrontendLaunchOptions launchOptions,
                                     WebSocketServerOptions options = {},
                                     QObject* parent = nullptr);
    ~WebSocketFrontendServer() override;

    bool listen(QString* errorMessage = nullptr);
    void close();
    bool isListening() const;
    quint16 serverPort() const;
    QHostAddress serverAddress() const;
    QUrl webSocketUrl() const;

signals:
    void quitRequested();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace hwtest::app::web
