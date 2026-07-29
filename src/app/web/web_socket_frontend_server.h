#pragma once

#include <app/frontend_launch_options.h>
#include <app/test_application_controller.h>

#include <QHostAddress>
#include <QObject>
#include <QUrl>
#include <QtGlobal>

#include <memory>

namespace hwtest::app::web {

struct WebSocketServerOptions {
    quint16 port = 18765;
    int handshakeTimeoutMs = 5000;
    quint64 maxIncomingMessageBytes = 16384;
    int maxBatchSamples = 64;
    qint64 maxBatchBytes = 32768;
    int maxBatchLatencyMs = 20;
    int snapshotIntervalMs = 100;
    qint64 socketHighWaterBytes = 1024 * 1024;
    qint64 socketLowWaterBytes = 256 * 1024;
    qint64 maxQueuedOutputBytes = 4 * 1024 * 1024;
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
