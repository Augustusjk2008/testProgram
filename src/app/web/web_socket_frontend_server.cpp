#include "web_socket_frontend_server.h"

#include "../src/continuous_data_recorder.h"
#include "web_protocol.h"
#include "web_telemetry_batcher.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QFileInfo>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketCorsAuthenticator>
#include <QWebSocketProtocol>
#include <QWebSocketServer>

#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace hwtest::app::web {

namespace {

constexpr quint64 kMaxJsonSafeInteger = 9007199254740991ULL;

ActionResult protocolError(const QString& code, const QString& message)
{
    return ActionResult{false, code, message};
}

bool isAllowedOrigin(const QString& origin)
{
    const QString normalized = origin.trimmed();
    // Browsers use the opaque `null` origin for pages opened directly from file://.
    if (normalized.isEmpty() || normalized == QStringLiteral("null")) {
        return true;
    }

    const QUrl url(normalized);
    if (!url.isValid() || url.host().isEmpty()) {
        return false;
    }
    const QString scheme = url.scheme().toLower();
    const QString host = url.host().toLower();
    return (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")) &&
        (host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1"));
}

QJsonObject testConfigObject(const FrontendTestConfigOption& option)
{
    return QJsonObject{
        {QStringLiteral("configId"), option.configId},
        {QStringLiteral("title"), option.title},
        {QStringLiteral("description"), option.description},
        {QStringLiteral("algorithmId"), option.algorithmId},
    };
}

QJsonObject configurationCatalogItemObject(
    const ConfigurationCatalogItem& item)
{
    return QJsonObject{
        {QStringLiteral("documentId"), item.documentId},
        {QStringLiteral("configId"), item.configId},
        {QStringLiteral("title"), item.title},
        {QStringLiteral("description"), item.description},
        {QStringLiteral("algorithmId"), item.algorithmId},
        {QStringLiteral("enabled"), item.enabled},
        {QStringLiteral("order"), item.order},
        {QStringLiteral("valid"), item.valid},
        {QStringLiteral("message"), item.message},
    };
}

QJsonObject configurationCatalogObject(const ConfigurationCatalog& catalog)
{
    QJsonArray items;
    for (const ConfigurationCatalogItem& item : catalog.items) {
        items.push_back(configurationCatalogItemObject(item));
    }
    return QJsonObject{
        {QStringLiteral("revision"), catalog.revision},
        {QStringLiteral("items"), items},
    };
}

QJsonObject configurationDocumentObject(const ConfigurationDocument& document)
{
    return QJsonObject{
        {QStringLiteral("documentId"), document.documentId},
        {QStringLiteral("kind"), document.kind},
        {QStringLiteral("revision"), document.revision},
        {QStringLiteral("value"), QJsonObject::fromVariantMap(document.value)},
        {QStringLiteral("schema"), QJsonObject::fromVariantMap(document.schema)},
    };
}

QJsonObject hardwareOptionsObject(const HardwareOptions& options)
{
    QJsonArray devices;
    for (const HardwareOptionDevice& device : options.devices) {
        QJsonArray supportedModules;
        for (const QString& module : device.supportedModules) {
            supportedModules.push_back(module);
        }
        devices.push_back(QJsonObject{
            {QStringLiteral("deviceName"), device.deviceName},
            {QStringLiteral("deviceId"), device.deviceId},
            {QStringLiteral("model"), device.model},
            {QStringLiteral("serialNumber"), device.serialNumber},
            {QStringLiteral("supportedModules"), supportedModules},
        });
    }
    return QJsonObject{
        {QStringLiteral("state"), options.state},
        {QStringLiteral("message"), options.message},
        {QStringLiteral("allowManualEntry"), options.allowManualEntry},
        {QStringLiteral("devices"), devices},
    };
}

QString selectedTestConfigId(const FrontendLaunchOptions& options)
{
    const QString selectedPath = QFileInfo(options.testConfigPath).absoluteFilePath();
    for (const FrontendTestConfigOption& option : options.testConfigs) {
        if (QFileInfo(option.configPath).absoluteFilePath() == selectedPath) {
            return option.configId;
        }
    }
    return {};
}

ActionResult loadSelectedTest(TestApplicationController& controller,
                              const FrontendLaunchOptions& options,
                              const QString& configId,
                              QString* selectedConfigPath)
{
    const auto selected = std::find_if(
        options.testConfigs.cbegin(),
        options.testConfigs.cend(),
        [&configId](const FrontendTestConfigOption& option) {
            return option.configId == configId;
        });
    if (selected == options.testConfigs.cend()) {
        return protocolError(QStringLiteral("test_config_not_found"),
                             QStringLiteral("Unknown test configuration '%1'")
                                 .arg(configId));
    }

    const QString phase = controller.snapshot().phase;
    if (phase == QStringLiteral("running") ||
        phase == QStringLiteral("paused") ||
        phase == QStringLiteral("stopping") ||
        phase == QStringLiteral("preparing")) {
        return protocolError(QStringLiteral("invalid_state"),
                             QStringLiteral("Test configuration cannot be changed while the test is active"));
    }

    if (phase != QStringLiteral("empty") &&
        phase != QStringLiteral("configured")) {
        const ActionResult shutdown = controller.shutdown();
        if (!shutdown.ok) {
            return shutdown;
        }
    }

    FrontendLaunchOptions selectedOptions = options;
    selectedOptions.testConfigPath = selected->configPath;
    const ActionResult result = configureController(controller, selectedOptions);
    if (result.ok && selectedConfigPath != nullptr) {
        *selectedConfigPath = selected->configPath;
    }
    return result;
}

ActionResult loadCatalogSelection(TestApplicationController& controller,
                                  const FrontendLaunchOptions& options,
                                  QString* selectedConfigPath)
{
    ConfigurationCatalog catalog;
    const ActionResult listed = controller.configurationCatalog(&catalog);
    if (!listed.ok) return listed;

    const QString currentConfigId =
        controller.snapshot().descriptor.configId.trimmed();
    const QString configuredPath = QFileInfo(options.testConfigPath)
                                       .absoluteFilePath();
    const QString configurationDirectory =
        options.configurationDirectory.trimmed().isEmpty()
        ? QFileInfo(options.halConfigPath).absolutePath()
        : options.configurationDirectory;
    const auto selected = std::find_if(
        catalog.items.cbegin(), catalog.items.cend(),
        [&](const ConfigurationCatalogItem& item) {
            if (!item.enabled || !item.valid) return false;
            if (!currentConfigId.isEmpty()) {
                return item.configId == currentConfigId;
            }
            return QFileInfo(QDir(configurationDirectory)
                                 .filePath(item.documentId))
                       .absoluteFilePath() == configuredPath;
        });
    if (selected == catalog.items.cend()) {
        if (!currentConfigId.isEmpty()) {
            return protocolError(
                QStringLiteral("config_conflict"),
                QStringLiteral("The current test configuration is disabled or invalid and must be reselected"));
        }
        return {};
    }

    FrontendLaunchOptions selectedOptions = options;
    selectedOptions.testConfigPath = QDir(configurationDirectory)
                                         .absoluteFilePath(selected->documentId);
    const ActionResult loaded = configureController(controller, selectedOptions);
    if (loaded.ok && selectedConfigPath != nullptr) {
        *selectedConfigPath = selectedOptions.testConfigPath;
    }
    return loaded;
}

quint64 qtIncomingLimit(quint64 protocolLimit)
{
    if (protocolLimit == std::numeric_limits<quint64>::max()) {
        return protocolLimit;
    }
    return protocolLimit + 1;
}

int normalizedBatchSampleLimit(int value)
{
    return std::max(1, std::min(value, 64));
}

qint64 normalizedPositiveBytes(qint64 value)
{
    return std::max<qint64>(1, value);
}

int normalizedNonNegativeMilliseconds(int value)
{
    return std::max(0, value);
}

} // namespace

class WebSocketFrontendServer::Impl final {
public:
    enum class PendingOperation {
        None,
        Stop,
        Disconnect,
        Quit,
        DropCleanup,
        BackpressureCleanup,
    };

    enum class TelemetryDeliveryMode {
        Single,
        Batch,
    };

    struct QueuedOutput {
        quint64 epoch = 0;
        QString text;
        qint64 byteCount = 0;
        bool telemetry = false;
    };

    Impl(WebSocketFrontendServer* owner,
         TestApplicationController* controllerValue,
         FrontendLaunchOptions launchOptionsValue,
         WebSocketServerOptions optionsValue)
        : q(owner)
        , controller(controllerValue)
        , launchOptions(std::move(launchOptionsValue))
        , options(optionsValue)
        , server(QStringLiteral("hwtest_web"), QWebSocketServer::NonSecureMode)
    {
        snapshotFlushTimer.setSingleShot(true);
        QObject::connect(&snapshotFlushTimer,
                         &QTimer::timeout,
                         q,
                         [this] { flushDeferredRunningSnapshot(); });
        if (controller != nullptr) {
            cachedSnapshot = controller->snapshot();
            QObject::connect(controller,
                              &TestApplicationController::snapshotChanged,
                              q,
                              [this](const ApplicationSnapshot& snapshot) {
                                  handleSnapshotChanged(snapshot);
                              });
            QObject::connect(controller,
                             &TestApplicationController::stopCompleted,
                             q,
                             [this](const ActionResult& result) {
                                 handleStopCompleted(result);
                             });
            QObject::connect(controller,
                              &TestApplicationController::sampleReceived,
                              q,
                              [this](const ApplicationSample& sample) {
                                  handleSampleReceived(sample);
                              });
        }

        server.setMaxPendingConnections(2);
        server.setHandshakeTimeout(options.handshakeTimeoutMs);
        QObject::connect(&server,
                         &QWebSocketServer::originAuthenticationRequired,
                         q,
                         [](QWebSocketCorsAuthenticator* authenticator) {
                             authenticator->setAllowed(
                                 isAllowedOrigin(authenticator->origin()));
                         });
        QObject::connect(&server,
                         &QWebSocketServer::newConnection,
                         q,
                         [this] { acceptPendingConnections(); });
    }

    ~Impl()
    {
        close();
    }

    bool listen(QString* errorMessage)
    {
        if (errorMessage != nullptr) {
            errorMessage->clear();
        }
        if (server.isListening()) {
            return true;
        }
        if (server.listen(QHostAddress::LocalHost, options.port)) {
            serverClosing = false;
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = server.errorString();
        }
        return false;
    }

    void close()
    {
        serverClosing = true;
        server.close();
        if (activeClient != nullptr) {
            QWebSocket* socket = activeClient;
            suppressDisconnectCleanup = socket;
            socket->close(QWebSocketProtocol::CloseCodeNormal,
                          QStringLiteral("Server closed"));
        }
    }

    void resetActiveClientProjection()
    {
        clearActiveClientProjection();
        telemetryDeliveryMode = TelemetryDeliveryMode::Single;
        backpressureCleanupStarted = false;
        backpressureSocket.clear();
    }

    void clearActiveClientProjection()
    {
        snapshotFlushTimer.stop();
        hasDeferredRunningSnapshot = false;
        deferredRunningSnapshot = {};
        deferredRunningSnapshotSource = {};
        hasLastProjectedSnapshot = false;
        if (telemetryBatcher != nullptr) {
            telemetryBatcher->clear();
        }
        telemetryBatcher.reset();
        outputQueue.clear();
        queuedOutputBytes = 0;
        outputPausedAtHighWater = false;
        telemetryDropWarningEmitted = false;
        closeAfterDrainSocket.clear();
        closeAfterDrainEpoch = 0;
        closeAfterDrainReason.clear();
    }

    void handleSnapshotChanged(const ApplicationSnapshot& snapshot)
    {
        cachedSnapshot = snapshot;
        if (snapshotSequence >= kMaxJsonSafeInteger) {
            qWarning().noquote()
                << "Unable to project snapshot after WebSocket v1 sequence exhaustion";
            return;
        }
        ++snapshotSequence;
        if (activeClient == nullptr ||
            activeClient->state() != QAbstractSocket::ConnectedState) {
            return;
        }

        const QJsonObject message = makeSnapshot(snapshotSequence, cachedSnapshot);
        if (message.isEmpty()) {
            qWarning().noquote()
                << "Unable to project snapshot within the WebSocket v1 safe-integer range";
            return;
        }
        if (canMergeRunningSnapshot(snapshot)) {
            deferredRunningSnapshot = message;
            deferredRunningSnapshotSource = snapshot;
            hasDeferredRunningSnapshot = true;
            if (!snapshotFlushTimer.isActive()) {
                snapshotFlushTimer.start(
                    normalizedNonNegativeMilliseconds(options.snapshotIntervalMs));
            }
            return;
        }

        flushDeferredRunningSnapshot();
        flushTelemetry();
        enqueueActiveMessage(message);
        lastProjectedSnapshot = snapshot;
        hasLastProjectedSnapshot = true;
    }

    void handleSampleReceived(const ApplicationSample& sample)
    {
        if (activeClient == nullptr ||
            activeClient->state() != QAbstractSocket::ConnectedState ||
            backpressureCleanupStarted) {
            return;
        }
        if (sampleSequence >= kMaxJsonSafeInteger) {
            qWarning().noquote()
                << "Unable to project sample after WebSocket v1 sequence exhaustion";
            beginTelemetryBackpressureCleanup(activeClient);
            return;
        }

        const quint64 nextSequence = sampleSequence + 1;
        if (telemetryDeliveryMode == TelemetryDeliveryMode::Batch) {
            if (telemetryBatcher == nullptr ||
                !telemetryBatcher->enqueueSample(nextSequence, sample)) {
                qWarning().noquote()
                    << "Dropping sample outside the WebSocket v1 safe-integer range";
                return;
            }
        } else {
            const QJsonObject event = makeSample(nextSequence, sample);
            if (event.isEmpty()) {
                qWarning().noquote()
                    << "Dropping sample outside the WebSocket v1 safe-integer range";
                return;
            }
            enqueueActiveMessage(event);
        }
        sampleSequence = nextSequence;
    }

    bool canMergeRunningSnapshot(const ApplicationSnapshot& snapshot) const
    {
        if (telemetryDeliveryMode != TelemetryDeliveryMode::Batch ||
            snapshot.phase != QStringLiteral("running") ||
            !hasLastProjectedSnapshot ||
            lastProjectedSnapshot.phase != QStringLiteral("running")) {
            return false;
        }
        if (snapshot.errorCode != lastProjectedSnapshot.errorCode ||
            snapshot.dataSaveError != lastProjectedSnapshot.dataSaveError ||
            snapshot.analysis.state != lastProjectedSnapshot.analysis.state) {
            return false;
        }
        return compactJson(digitalStimulusObject(snapshot.digitalStimulus)) ==
            compactJson(digitalStimulusObject(lastProjectedSnapshot.digitalStimulus));
    }

    void flushDeferredRunningSnapshot()
    {
        if (!hasDeferredRunningSnapshot) {
            return;
        }
        snapshotFlushTimer.stop();
        const QJsonObject message = deferredRunningSnapshot;
        const ApplicationSnapshot snapshot = deferredRunningSnapshotSource;
        hasDeferredRunningSnapshot = false;
        deferredRunningSnapshot = {};
        deferredRunningSnapshotSource = {};
        enqueueActiveMessage(message);
        lastProjectedSnapshot = snapshot;
        hasLastProjectedSnapshot = true;
    }

    void flushTelemetry()
    {
        if (telemetryBatcher != nullptr) {
            telemetryBatcher->flush();
        }
    }

    void send(QWebSocket* socket, const QJsonObject& message)
    {
        if (message.isEmpty()) {
            return;
        }
        if (socket == activeClient) {
            if (message.value(QStringLiteral("type")).toString() ==
                QStringLiteral("reply")) {
                flushDeferredRunningSnapshot();
                flushTelemetry();
            }
            enqueueActiveMessage(message);
            return;
        }
        sendDirect(socket, message);
    }

    static void sendDirect(QWebSocket* socket, const QJsonObject& message)
    {
        if (socket != nullptr &&
            socket->state() == QAbstractSocket::ConnectedState &&
            !message.isEmpty()) {
            socket->sendTextMessage(compactJson(message));
        }
    }

    void enqueueActiveMessage(const QJsonObject& message)
    {
        QWebSocket* socket = activeClient;
        if (socket == nullptr ||
            socket->state() != QAbstractSocket::ConnectedState ||
            backpressureCleanupStarted || message.isEmpty()) {
            return;
        }

        const QString text = compactJson(message);
        const qint64 byteCount = text.toUtf8().size();
        const QString messageType =
            message.value(QStringLiteral("type")).toString();
        const bool telemetry = messageType == QStringLiteral("sample") ||
            messageType == QStringLiteral("sampleBatch");
        if (!makeQueuedOutputRoomByDiscardingTelemetry(socket, byteCount)) {
            beginTelemetryBackpressureCleanup(socket);
            return;
        }
        outputQueue.enqueue(
            QueuedOutput{activeClientEpoch, text, byteCount, telemetry});
        queuedOutputBytes += byteCount;
        pumpOutput();
    }

    bool makeQueuedOutputRoomByDiscardingTelemetry(const QWebSocket* socket,
                                                    qint64 additionalBytes)
    {
        qint64 droppedMessages = 0;
        qint64 droppedBytes = 0;
        while (wouldExceedQueuedOutputLimit(socket, additionalBytes)) {
            const auto telemetry = std::find_if(
                outputQueue.begin(),
                outputQueue.end(),
                [](const QueuedOutput& output) { return output.telemetry; });
            if (telemetry == outputQueue.end()) {
                break;
            }
            droppedBytes += telemetry->byteCount;
            queuedOutputBytes -= telemetry->byteCount;
            outputQueue.erase(telemetry);
            ++droppedMessages;
        }
        if (droppedMessages > 0 && !telemetryDropWarningEmitted) {
            telemetryDropWarningEmitted = true;
            qWarning().noquote()
                << "telemetry_backpressure: discarded"
                << droppedMessages << "queued telemetry message(s),"
                << droppedBytes << "byte(s), while preserving the active run";
        }
        return !wouldExceedQueuedOutputLimit(socket, additionalBytes);
    }

    bool wouldExceedQueuedOutputLimit(const QWebSocket* socket,
                                       qint64 additionalBytes) const
    {
        const qint64 hardLimit =
            normalizedPositiveBytes(options.maxQueuedOutputBytes);
        const qint64 socketBytes =
            socket == nullptr ? 0 : std::max<qint64>(0, socket->bytesToWrite());
        if (socketBytes >= hardLimit || queuedOutputBytes > hardLimit - socketBytes) {
            return true;
        }
        return additionalBytes > hardLimit - socketBytes - queuedOutputBytes;
    }

    void pumpOutput()
    {
        QWebSocket* socket = activeClient;
        if (socket == nullptr ||
            socket->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        const qint64 highWater =
            normalizedPositiveBytes(options.socketHighWaterBytes);
        const qint64 lowWater = std::min(
            std::max<qint64>(0, options.socketLowWaterBytes), highWater);
        const qint64 socketBytes = std::max<qint64>(0, socket->bytesToWrite());
        if (outputPausedAtHighWater) {
            if (socketBytes > lowWater) {
                return;
            }
            outputPausedAtHighWater = false;
        }

        while (!outputQueue.isEmpty()) {
            if (outputQueue.head().epoch != activeClientEpoch) {
                queuedOutputBytes -= outputQueue.dequeue().byteCount;
                continue;
            }
            if (socket->bytesToWrite() >= highWater) {
                outputPausedAtHighWater = true;
                break;
            }
            const QueuedOutput output = outputQueue.dequeue();
            queuedOutputBytes -= output.byteCount;
            socket->sendTextMessage(output.text);
            if (socket->bytesToWrite() >= highWater) {
                outputPausedAtHighWater = true;
                break;
            }
        }
        maybeCloseAfterDrain();
    }

    void resumeOutput(QWebSocket* socket)
    {
        if (socket == activeClient) {
            pumpOutput();
        }
    }

    void queueCloseAfterDrain(QWebSocket* socket,
                              QWebSocketProtocol::CloseCode code,
                              const QString& reason)
    {
        if (socket == nullptr || socket != activeClient) {
            return;
        }
        closeAfterDrainSocket = socket;
        closeAfterDrainEpoch = activeClientEpoch;
        closeAfterDrainCode = code;
        closeAfterDrainReason = reason;
        pumpOutput();
    }

    void detachNonStoppableClient(QWebSocket* socket, const QString& requestId)
    {
        if (socket == nullptr || socket != activeClient) {
            return;
        }

        // Detach the UI from the running device task immediately. In
        // particular, do not keep feeding telemetry into the old socket while
        // waiting for its network buffers to drain: that would prevent a new
        // observer from being accepted for the lifetime of the burst.
        clearActiveClientProjection();
        activeClient.clear();
        sendDirect(socket, makeReply(requestId, ActionResult{}));
        socket->close(QWebSocketProtocol::CloseCodeNormal,
                      QStringLiteral("detach"));
    }

    void maybeCloseAfterDrain()
    {
        QWebSocket* socket = closeAfterDrainSocket;
        if (socket == nullptr) {
            return;
        }
        if (socket != activeClient || closeAfterDrainEpoch != activeClientEpoch) {
            closeAfterDrainSocket.clear();
            return;
        }
        if (!outputQueue.isEmpty() || socket->bytesToWrite() > 0) {
            return;
        }
        const QWebSocketProtocol::CloseCode code = closeAfterDrainCode;
        const QString reason = closeAfterDrainReason;
        closeAfterDrainSocket.clear();
        closeAfterDrainReason.clear();
        suppressDisconnectCleanup = socket;
        socket->close(code, reason);
    }

    void beginTelemetryBackpressureCleanup(QWebSocket* socket)
    {
        if (backpressureCleanupStarted) {
            return;
        }
        backpressureCleanupStarted = true;
        backpressureSocket = socket;
        qWarning().noquote()
            << "telemetry_backpressure: WebSocket output exceeded its configured hard limit";
        snapshotFlushTimer.stop();
        hasDeferredRunningSnapshot = false;
        deferredRunningSnapshot = {};
        if (telemetryBatcher != nullptr) {
            telemetryBatcher->clear();
        }
        outputQueue.clear();
        queuedOutputBytes = 0;
        outputPausedAtHighWater = false;
        if (isActiveNonStoppable(cachedSnapshot)) {
            backpressureCleanupStarted = false;
            backpressureSocket.clear();
            suppressDisconnectCleanup = socket;
            socket->close(QWebSocketProtocol::CloseCodeBadOperation,
                          QStringLiteral("telemetry_backpressure"));
            return;
        }
        if (pendingOperation == PendingOperation::None) {
            beginCleanup(PendingOperation::BackpressureCleanup, QString(), socket);
        }
    }

    void closeRejectedConnection(QWebSocket* socket,
                                 const QByteArray& pingPayload,
                                 const QString& reason)
    {
        const auto waitingForPong = std::make_shared<bool>(true);
        QObject::connect(socket,
                         &QWebSocket::pong,
                         socket,
                         [socket, waitingForPong, pingPayload, reason](
                             quint64,
                             const QByteArray& payload) {
                             if (!*waitingForPong || payload != pingPayload) {
                                 return;
                             }
                             *waitingForPong = false;
                             socket->close(
                                 QWebSocketProtocol::CloseCodePolicyViolated,
                                 reason);
                         });
        QTimer::singleShot(options.handshakeTimeoutMs,
                           socket,
                           [socket, waitingForPong, reason] {
            if (*waitingForPong) {
                *waitingForPong = false;
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                              reason);
            }
        });
        socket->ping(pingPayload);
    }

    void acceptPendingConnections()
    {
        while (server.hasPendingConnections()) {
            QWebSocket* socket = server.nextPendingConnection();
            if (socket == nullptr) {
                continue;
            }
            QObject::connect(socket,
                             &QWebSocket::disconnected,
                             socket,
                             &QObject::deleteLater);

            if (socket->requestUrl().path() != QStringLiteral("/ws")) {
                closeRejectedConnection(socket,
                                        QByteArrayLiteral("hwtest-path-close"),
                                        QStringLiteral("Only /ws is available"));
                continue;
            }
            if ((activeClient != nullptr &&
                 activeClient->state() != QAbstractSocket::UnconnectedState) ||
                pendingOperation != PendingOperation::None) {
                send(socket,
                     makeReply(QString(),
                               protocolError(
                                   QStringLiteral("server_busy"),
                                   QStringLiteral(
                                       "Another client is already active"))));
                closeRejectedConnection(socket,
                                        QByteArrayLiteral("hwtest-close"),
                                        QStringLiteral("server_busy"));
                continue;
            }
            activate(socket);
        }
    }

    void activate(QWebSocket* socket)
    {
        activeClient = socket;
        ++activeClientEpoch;
        resetActiveClientProjection();
        WebTelemetryBatcherOptions batcherOptions;
        batcherOptions.maxSamples = normalizedBatchSampleLimit(options.maxBatchSamples);
        batcherOptions.maxBytes = normalizedPositiveBytes(options.maxBatchBytes);
        batcherOptions.maxLatencyMs =
            normalizedNonNegativeMilliseconds(options.maxBatchLatencyMs);
        telemetryBatcher = std::make_unique<WebTelemetryBatcher>(batcherOptions);
        const quint64 epoch = activeClientEpoch;
        telemetryBatcher->setFlushCallback([this, epoch](const QJsonObject& batch) {
            if (epoch == activeClientEpoch) {
                enqueueActiveMessage(batch);
            }
        });
        const quint64 hardLimit = qtIncomingLimit(options.maxIncomingMessageBytes);
        socket->setMaxAllowedIncomingFrameSize(hardLimit);
        socket->setMaxAllowedIncomingMessageSize(hardLimit);

        QObject::connect(socket,
                         &QWebSocket::textMessageReceived,
                         q,
                         [this, socket](const QString& text) {
                             if (socket == activeClient) {
                                 receiveText(socket, text);
                             }
                         });
        QObject::connect(socket,
                         &QWebSocket::binaryMessageReceived,
                         q,
                         [this, socket](const QByteArray&) {
                              if (socket != activeClient) {
                                  return;
                              }
                             send(socket,
                                  makeReply(
                                      QString(),
                                      protocolError(
                                          QStringLiteral("invalid_envelope"),
                                          QStringLiteral(
                                              "Binary messages are not supported"))));
                             socket->close(
                                 QWebSocketProtocol::CloseCodeDatatypeNotSupported,
                                 QStringLiteral("Binary messages are not supported"));
                          });
        QObject::connect(socket,
                         &QWebSocket::bytesWritten,
                         q,
                         [this, socket](qint64) {
                             if (socket == activeClient) {
                                 resumeOutput(socket);
                             }
                         });
        QObject::connect(socket,
                         &QWebSocket::disconnected,
                         q,
                         [this, socket] {
                             if (socket == activeClient) {
                                 activeClient.clear();
                                 clearActiveClientProjection();
                                 if (socket == suppressDisconnectCleanup) {
                                     suppressDisconnectCleanup.clear();
                                     return;
                                 }
                                 if (!serverClosing) {
                                     handleActiveClientDropped();
                                 }
                             }
                          });

        send(socket,
             makeHello(normalizedBatchSampleLimit(options.maxBatchSamples),
                       normalizedPositiveBytes(options.maxBatchBytes),
                       normalizedNonNegativeMilliseconds(options.maxBatchLatencyMs),
                       normalizedNonNegativeMilliseconds(options.snapshotIntervalMs)));
        const QJsonObject initialSnapshot = makeSnapshot(snapshotSequence, cachedSnapshot);
        send(socket, initialSnapshot);
        if (!initialSnapshot.isEmpty()) {
            lastProjectedSnapshot = cachedSnapshot;
            hasLastProjectedSnapshot = true;
        }
    }

    void receiveText(QWebSocket* socket, const QString& text)
    {
        if (static_cast<quint64>(text.toUtf8().size()) >
            options.maxIncomingMessageBytes) {
            send(socket,
                 makeReply(QString(),
                           protocolError(QStringLiteral("message_too_large"),
                                         QStringLiteral(
                                             "Message exceeds the 16 KiB limit"))));
            socket->close(QWebSocketProtocol::CloseCodeTooMuchData,
                          QStringLiteral("message_too_large"));
            return;
        }

        const ProtocolParseResult parsed = parseRequest(text);
        if (!parsed.ok) {
            send(socket,
                 makeReply(parsed.request.id,
                           protocolError(parsed.code, parsed.message)));
            return;
        }

        const WebRequest& request = parsed.request;
        if (request.action == QStringLiteral("snapshot")) {
            const QJsonObject snapshot = makeSnapshot(snapshotSequence,
                                                      cachedSnapshot);
            const QJsonObject data{
                {QStringLiteral("seq"), snapshot.value(QStringLiteral("seq"))},
                {QStringLiteral("snapshot"),
                 snapshot.value(QStringLiteral("snapshot"))},
            };
            send(socket, makeReply(request.id, ActionResult{}, data));
            return;
        }

        if (request.action == QStringLiteral("setTelemetryDelivery")) {
            ActionResult validation;
            if (!validateAction(request, &validation)) {
                send(socket, makeReply(request.id, validation));
                return;
            }
            setTelemetryDelivery(request, socket);
            return;
        }

        if (pendingOperation != PendingOperation::None &&
            !isReadAction(request.action)) {
            send(socket,
                 makeReply(request.id,
                           protocolError(QStringLiteral("command_in_progress"),
                                         QStringLiteral(
                                             "Another command is still in progress"))));
            return;
        }
        if (analysisBlocksWrites(cachedSnapshot.analysis.state) &&
            !isReadAction(request.action) &&
            request.action != QStringLiteral("stop") &&
            request.action != QStringLiteral("disconnect") &&
            request.action != QStringLiteral("quit")) {
            send(socket,
                 makeReply(request.id,
                           protocolError(
                               QStringLiteral("command_in_progress"),
                               QStringLiteral("Post-run analysis is still running"))));
            return;
        }

        ActionResult validation;
        if (!validateAction(request, &validation)) {
            send(socket, makeReply(request.id, validation));
            return;
        }

        if (isActiveNonStoppable(cachedSnapshot)) {
            if (request.action == QStringLiteral("stop") ||
                request.action == QStringLiteral("pause") ||
                request.action == QStringLiteral("resume")) {
                send(socket,
                     makeReply(
                         request.id,
                         protocolError(
                             QStringLiteral("CapabilityUnsupported"),
                             QStringLiteral("This finite device stream must complete naturally"))));
                return;
            }
            if (request.action == QStringLiteral("quit")) {
                send(socket,
                     makeReply(
                         request.id,
                         protocolError(
                             QStringLiteral("invalid_state"),
                             QStringLiteral("The server cannot quit while a non-stoppable device stream is active"))));
                return;
            }
            if (request.action == QStringLiteral("disconnect")) {
                detachNonStoppableClient(socket, request.id);
                return;
            }
        }

        if (request.action == QStringLiteral("stop")) {
            pendingOperation = PendingOperation::Stop;
            pendingRequestId = request.id;
            pendingSocket = socket;
            startStop(request.id, socket);
            return;
        }

        if (request.action == QStringLiteral("disconnect")) {
            beginCleanup(PendingOperation::Disconnect, request.id, socket);
            return;
        }
        if (request.action == QStringLiteral("quit")) {
            beginCleanup(PendingOperation::Quit, request.id, socket);
            return;
        }

        dispatchControllerAction(request, socket);
    }

    static bool isReadAction(const QString& action)
    {
        return action == QStringLiteral("snapshot") ||
            action == QStringLiteral("analysisResult") ||
            action == QStringLiteral("testConfigs") ||
            action == QStringLiteral("configCatalog") ||
            action == QStringLiteral("configDocument") ||
            action == QStringLiteral("hardwareOptions") ||
            action == QStringLiteral("controls") ||
            action == QStringLiteral("ports");
    }

    static bool analysisBlocksWrites(const QString& state)
    {
        return state == QStringLiteral("queued") ||
            state == QStringLiteral("validating") ||
            state == QStringLiteral("preprocessing") ||
            state == QStringLiteral("calculating") ||
            state == QStringLiteral("persisting");
    }

    static bool isActiveNonStoppable(const ApplicationSnapshot& snapshot)
    {
        return !snapshot.descriptor.stoppable &&
            (snapshot.phase == QStringLiteral("running") ||
             snapshot.phase == QStringLiteral("paused"));
    }

    static bool validateRequiredString(const WebRequest& request,
                                       const QString& field,
                                       ActionResult* error)
    {
        if (!request.params.contains(field) ||
            (request.params.value(field).isString() &&
             request.params.value(field).toString().trimmed().isEmpty())) {
            if (error != nullptr) {
                *error = protocolError(
                    QStringLiteral("missing_field"),
                    QStringLiteral("Parameter '%1' is required").arg(field));
            }
            return false;
        }
        if (!request.params.value(field).isString()) {
            if (error != nullptr) {
                *error = protocolError(
                    QStringLiteral("invalid_envelope"),
                    QStringLiteral("Parameter '%1' must be a string").arg(field));
            }
            return false;
        }
        return true;
    }

    static bool parseStartOptions(const WebRequest& request,
                                  TestRunOptions* options,
                                  ActionResult* error)
    {
        const QSet<QString> allowed{
            QStringLiteral("mode"),
            QStringLiteral("intervalMs"),
            QStringLiteral("maxCycles"),
            QStringLiteral("saveData"),
            QStringLiteral("dataDirectory"),
            QStringLiteral("dataFileName"),
            QStringLiteral("algorithmParameters"),
        };
        for (auto iterator = request.params.constBegin();
             iterator != request.params.constEnd();
             ++iterator) {
            if (!allowed.contains(iterator.key())) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Unknown start parameter '%1'")
                            .arg(iterator.key()));
                }
                return false;
            }
        }

        TestRunOptions parsed;
        if (request.params.contains(QStringLiteral("mode"))) {
            const QJsonValue mode = request.params.value(QStringLiteral("mode"));
            if (!mode.isString()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'mode' must be a string"));
                }
                return false;
            }
            parsed.mode = mode.toString();
        }
        if (parsed.mode != QStringLiteral("single") &&
            parsed.mode != QStringLiteral("pc_periodic") &&
            parsed.mode != QStringLiteral("device_stream")) {
            if (error != nullptr) {
                *error = protocolError(
                    QStringLiteral("invalid_run_mode"),
                    QStringLiteral("Unknown run mode '%1'").arg(parsed.mode));
            }
            return false;
        }

        if (request.params.contains(QStringLiteral("intervalMs"))) {
            const QJsonValue interval =
                request.params.value(QStringLiteral("intervalMs"));
            const double value = interval.toDouble();
            if (!interval.isDouble() || !std::isfinite(value) ||
                std::floor(value) != value) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'intervalMs' must be an integer"));
                }
                return false;
            }
            if (value < 0.0 || value > 3600000.0) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("ParameterRangeError"),
                        QStringLiteral("Parameter 'intervalMs' must be in the range 0..3600000"));
                }
                return false;
            }
            parsed.intervalMs = static_cast<int>(value);
        }

        if (request.params.contains(QStringLiteral("maxCycles"))) {
            const QJsonValue cycles =
                request.params.value(QStringLiteral("maxCycles"));
            const double value = cycles.toDouble();
            if (!cycles.isDouble() || !std::isfinite(value) ||
                std::floor(value) != value) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'maxCycles' must be an integer"));
                }
                return false;
            }
            if (value < 0.0 || value > 1000000000.0) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("ParameterRangeError"),
                        QStringLiteral("Parameter 'maxCycles' must be in the range 0..1000000000"));
                }
                return false;
            }
            parsed.maxCycles = static_cast<quint64>(value);
        }

        if (request.params.contains(QStringLiteral("saveData"))) {
            const QJsonValue saveData =
                request.params.value(QStringLiteral("saveData"));
            if (!saveData.isBool()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'saveData' must be a boolean"));
                }
                return false;
            }
            parsed.saveData = saveData.toBool();
        }

        if (request.params.contains(QStringLiteral("dataDirectory"))) {
            const QJsonValue directory =
                request.params.value(QStringLiteral("dataDirectory"));
            if (!directory.isString()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'dataDirectory' must be a string"));
                }
                return false;
            }
            parsed.dataDirectory = directory.toString();
        }
        if (request.params.contains(QStringLiteral("dataFileName"))) {
            const QJsonValue fileName =
                request.params.value(QStringLiteral("dataFileName"));
            if (!fileName.isString()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'dataFileName' must be a string"));
                }
                return false;
            }
            parsed.dataFileName = fileName.toString();
        }
        const bool saveContinuousData = parsed.saveData &&
            parsed.mode != QStringLiteral("single");
        if (saveContinuousData) {
            const ActionResult destinationValidated =
                ContinuousDataRecorder::validateDestinationOverrides(
                    parsed.dataDirectory, parsed.dataFileName);
            if (!destinationValidated.ok) {
                if (error != nullptr) *error = destinationValidated;
                return false;
            }
        }

        if (request.params.contains(QStringLiteral("algorithmParameters"))) {
            const QJsonValue parameters =
                request.params.value(QStringLiteral("algorithmParameters"));
            if (!parameters.isObject()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'algorithmParameters' must be an object"));
                }
                return false;
            }
            parsed.algorithmParameters = parameters.toObject().toVariantMap();
        }

        if (options != nullptr) {
            *options = parsed;
        }
        return true;
    }

    static bool validateAction(const WebRequest& request, ActionResult* error)
    {
        if (request.action == QStringLiteral("load") && !request.params.isEmpty()) {
            if (error != nullptr) {
                *error = protocolError(
                    QStringLiteral("invalid_envelope"),
                    QStringLiteral("The load action does not accept client paths"));
            }
            return false;
        }
        if (request.action == QStringLiteral("testConfigs") &&
            !request.params.isEmpty()) {
            if (error != nullptr) {
                *error = protocolError(
                    QStringLiteral("invalid_envelope"),
                    QStringLiteral("The testConfigs action does not accept parameters"));
            }
            return false;
        }
        if (request.action == QStringLiteral("configCatalog")) {
            if (!request.params.isEmpty()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("The configCatalog action does not accept parameters"));
                }
                return false;
            }
            return true;
        }
        if (request.action == QStringLiteral("hardwareOptions")) {
            if (!request.params.isEmpty()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("The hardwareOptions action does not accept parameters"));
                }
                return false;
            }
            return true;
        }
        if (request.action == QStringLiteral("configDocument")) {
            if (!validateRequiredString(request,
                                        QStringLiteral("documentId"),
                                        error)) {
                return false;
            }
            if (request.params.size() != 1) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("The configDocument action only accepts documentId"));
                }
                return false;
            }
            return true;
        }
        if (request.action == QStringLiteral("saveConfig")) {
            if (!validateRequiredString(request,
                                        QStringLiteral("documentId"),
                                        error) ||
                !validateRequiredString(request,
                                        QStringLiteral("expectedRevision"),
                                        error)) {
                return false;
            }
            if (request.params.size() != 3 ||
                !request.params.contains(QStringLiteral("value")) ||
                !request.params.value(QStringLiteral("value")).isObject()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("The saveConfig action requires documentId, expectedRevision and an object value"));
                }
                return false;
            }
            return true;
        }
        if (request.action == QStringLiteral("selectTest")) {
            if (!validateRequiredString(request,
                                        QStringLiteral("configId"),
                                        error)) {
                return false;
            }
            if (request.params.size() != 1) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("The selectTest action only accepts configId"));
                }
                return false;
            }
            return true;
        }
        if (request.action == QStringLiteral("selectControl")) {
            return validateRequiredString(request,
                                          QStringLiteral("resourceId"),
                                          error);
        }
        if (request.action == QStringLiteral("selectSerialPort")) {
            return validateRequiredString(request,
                                          QStringLiteral("portName"),
                                          error);
        }
        if (request.action == QStringLiteral("selectAuxiliarySerialPort")) {
            return validateRequiredString(request,
                                          QStringLiteral("portName"),
                                          error);
        }
        if (request.action == QStringLiteral("start")) {
            return parseStartOptions(request, nullptr, error);
        }
        if (request.action == QStringLiteral("setTelemetryDelivery")) {
            if (request.params.size() != 1 ||
                !request.params.contains(QStringLiteral("mode"))) {
                if (error != nullptr) {
                    *error = protocolError(
                        request.params.contains(QStringLiteral("mode"))
                            ? QStringLiteral("invalid_envelope")
                            : QStringLiteral("missing_field"),
                        QStringLiteral("setTelemetryDelivery only accepts mode"));
                }
                return false;
            }
            const QJsonValue mode = request.params.value(QStringLiteral("mode"));
            if (!mode.isString() ||
                (mode.toString() != QStringLiteral("single") &&
                 mode.toString() != QStringLiteral("batch"))) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'mode' must be 'single' or 'batch'"));
                }
                return false;
            }
            return true;
        }
        if (request.action == QStringLiteral("analysisResult")) {
            static const QSet<QString> allowed{
                QStringLiteral("taskId"),
                QStringLiteral("analysisGeneration"),
                QStringLiteral("channel"),
            };
            if (request.params.size() != allowed.size()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("analysisResult only accepts taskId, analysisGeneration and channel"));
                }
                return false;
            }
            for (auto it = request.params.constBegin();
                 it != request.params.constEnd(); ++it) {
                if (!allowed.contains(it.key())) {
                    if (error != nullptr) {
                        *error = protocolError(
                            QStringLiteral("invalid_envelope"),
                            QStringLiteral("Unknown analysisResult parameter '%1'")
                                .arg(it.key()));
                    }
                    return false;
                }
            }
            const QJsonValue taskId = request.params.value(
                QStringLiteral("taskId"));
            if (!taskId.isString() || taskId.toString().trimmed().isEmpty()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'taskId' must be a non-empty string"));
                }
                return false;
            }
            const QJsonValue generation = request.params.value(
                QStringLiteral("analysisGeneration"));
            const double generationValue = generation.toDouble();
            if (!generation.isDouble() || !std::isfinite(generationValue) ||
                std::floor(generationValue) != generationValue ||
                generationValue < 1.0 ||
                generationValue > 9007199254740991.0) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'analysisGeneration' must be a positive safe integer"));
                }
                return false;
            }
            const QJsonValue channel = request.params.value(
                QStringLiteral("channel"));
            const double channelValue = channel.toDouble();
            if (!channel.isDouble() || !std::isfinite(channelValue) ||
                std::floor(channelValue) != channelValue ||
                channelValue < 0.0 || channelValue > 3.0) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'channel' must be an integer in 0..3"));
                }
                return false;
            }
            return true;
        }
        if (request.action == QStringLiteral("setDigitalStimulus")) {
            static const QSet<QString> allowed{
                QStringLiteral("switchId"),
                QStringLiteral("active"),
                QStringLiteral("expectedRevision"),
            };
            if (request.params.size() != allowed.size()) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("setDigitalStimulus only accepts switchId, active and expectedRevision"));
                }
                return false;
            }
            for (auto it = request.params.constBegin();
                 it != request.params.constEnd();
                 ++it) {
                if (!allowed.contains(it.key())) {
                    if (error != nullptr) {
                        *error = protocolError(
                            QStringLiteral("invalid_envelope"),
                            QStringLiteral("Unknown digital stimulus parameter '%1'").arg(it.key()));
                    }
                    return false;
                }
            }
            if (!validateRequiredString(request,
                                        QStringLiteral("switchId"),
                                        error)) {
                return false;
            }
            if (!request.params.value(QStringLiteral("active")).isBool()) {
                if (error != nullptr) {
                    *error = protocolError(QStringLiteral("invalid_envelope"),
                                           QStringLiteral("Parameter 'active' must be a boolean"));
                }
                return false;
            }
            const QJsonValue revision = request.params.value(
                QStringLiteral("expectedRevision"));
            const double revisionValue = revision.toDouble();
            if (!revision.isDouble() || !std::isfinite(revisionValue) ||
                std::floor(revisionValue) != revisionValue ||
                revisionValue < 0.0 || revisionValue > 9007199254740991.0) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("invalid_envelope"),
                        QStringLiteral("Parameter 'expectedRevision' must be a non-negative safe integer"));
                }
                return false;
            }
            return true;
        }
        if (request.action == QStringLiteral("resetDigitalStimulus") &&
            !request.params.isEmpty()) {
            if (error != nullptr) {
                *error = protocolError(
                    QStringLiteral("invalid_envelope"),
                    QStringLiteral("resetDigitalStimulus does not accept parameters"));
            }
            return false;
        }
        return true;
    }

    void setTelemetryDelivery(const WebRequest& request, QWebSocket* socket)
    {
        const QString mode = request.params.value(QStringLiteral("mode")).toString();
        if (!canChangeTelemetryDelivery()) {
            send(socket,
                 makeReply(
                     request.id,
                     protocolError(
                         QStringLiteral("invalid_state"),
                         QStringLiteral(
                             "Telemetry delivery can only change without an active test or pending output"))));
            return;
        }

        telemetryDeliveryMode = mode == QStringLiteral("batch")
            ? TelemetryDeliveryMode::Batch
            : TelemetryDeliveryMode::Single;
        send(socket,
             makeReply(request.id,
                       ActionResult{},
                       QJsonObject{{QStringLiteral("mode"), mode}}));
    }

    bool canChangeTelemetryDelivery() const
    {
        if (pendingOperation != PendingOperation::None ||
            backpressureCleanupStarted) {
            return false;
        }
        const QString& phase = cachedSnapshot.phase;
        if (phase == QStringLiteral("preparing") ||
            phase == QStringLiteral("running") ||
            phase == QStringLiteral("paused") ||
            phase == QStringLiteral("stopping")) {
            return false;
        }
        if ((telemetryBatcher != nullptr && telemetryBatcher->hasPendingSamples()) ||
            hasDeferredRunningSnapshot || !outputQueue.isEmpty()) {
            return false;
        }
        return activeClient == nullptr || activeClient->bytesToWrite() == 0;
    }

    void dispatchControllerAction(const WebRequest& request, QWebSocket* socket)
    {
        const bool legacyTestAllowlist =
            launchOptions.configurationDirectory.trimmed().isEmpty() &&
            !launchOptions.testConfigs.isEmpty();
        if (request.action == QStringLiteral("testConfigs") &&
            legacyTestAllowlist) {
            QJsonArray configs;
            for (const FrontendTestConfigOption& option : launchOptions.testConfigs) {
                configs.push_back(testConfigObject(option));
            }
            const QJsonObject data{
                {QStringLiteral("selectedConfigId"), selectedTestConfigId(launchOptions)},
                {QStringLiteral("configs"), configs},
            };
            send(socket, makeReply(request.id, ActionResult{}, data));
            return;
        }
        if (controller == nullptr) {
            send(socket,
                 makeReply(request.id,
                           protocolError(QStringLiteral("invalid_state"),
                                         QStringLiteral("Controller is unavailable"))));
            return;
        }

        const QPointer<WebSocketFrontendServer> owner(q);
        const QPointer<TestApplicationController> controllerGuard(controller);
        const QPointer<QWebSocket> socketGuard(socket);
        const FrontendLaunchOptions optionsCopy = launchOptions;
        const bool useLegacyTestAllowlist = legacyTestAllowlist;
        QMetaObject::invokeMethod(
            controller,
            [owner,
             controllerGuard,
             socketGuard,
             request,
             optionsCopy,
             useLegacyTestAllowlist] {
                if (owner == nullptr || controllerGuard == nullptr) {
                    return;
                }

                ActionResult result;
                QJsonObject data;
                QString selectedConfigPath;
                const bool needsConfigurationStorage =
                    request.action == QStringLiteral("load") ||
                    request.action == QStringLiteral("testConfigs") ||
                    request.action == QStringLiteral("selectTest") ||
                    request.action == QStringLiteral("configCatalog") ||
                    request.action == QStringLiteral("configDocument") ||
                    request.action == QStringLiteral("saveConfig") ||
                    request.action == QStringLiteral("hardwareOptions");
                if (needsConfigurationStorage &&
                    !optionsCopy.halConfigPath.trimmed().isEmpty()) {
                    const QString configurationDirectory =
                        optionsCopy.configurationDirectory.trimmed().isEmpty()
                        ? QFileInfo(optionsCopy.halConfigPath).absolutePath()
                        : optionsCopy.configurationDirectory;
                    result = controllerGuard->configureConfigurationStorage(
                        configurationDirectory, optionsCopy.halConfigPath);
                }

                if (!result.ok) {
                    // Preserve configuration-storage initialization failure.
                } else if (request.action == QStringLiteral("load")) {
                    const QString currentConfigId =
                        controllerGuard->snapshot().descriptor.configId.trimmed();
                    if (useLegacyTestAllowlist && !currentConfigId.isEmpty()) {
                        result = loadSelectedTest(*controllerGuard,
                                                  optionsCopy,
                                                  currentConfigId,
                                                  &selectedConfigPath);
                    } else if (optionsCopy.configurationDirectory.trimmed().isEmpty()) {
                        result = configureController(*controllerGuard, optionsCopy);
                    } else {
                        result = loadCatalogSelection(*controllerGuard,
                                                      optionsCopy,
                                                      &selectedConfigPath);
                    }
                } else if (request.action == QStringLiteral("selectTest")) {
                    const QString configId = request.params
                                                 .value(QStringLiteral("configId"))
                                                 .toString().trimmed();
                    if (useLegacyTestAllowlist) {
                        result = loadSelectedTest(*controllerGuard,
                                                  optionsCopy,
                                                  configId,
                                                  &selectedConfigPath);
                    } else {
                        result = controllerGuard->selectTestConfiguration(configId);
                        if (result.ok) {
                            ConfigurationCatalog catalog;
                            result = controllerGuard->configurationCatalog(&catalog);
                            const auto selected = std::find_if(
                                catalog.items.cbegin(), catalog.items.cend(),
                                [&configId](const ConfigurationCatalogItem& item) {
                                    return item.enabled && item.valid &&
                                        item.configId == configId;
                                });
                            if (result.ok && selected != catalog.items.cend()) {
                                const QString configurationDirectory =
                                    optionsCopy.configurationDirectory.trimmed().isEmpty()
                                    ? QFileInfo(optionsCopy.halConfigPath).absolutePath()
                                    : optionsCopy.configurationDirectory;
                                selectedConfigPath = QDir(configurationDirectory)
                                                         .absoluteFilePath(
                                                             selected->documentId);
                            }
                        }
                    }
                } else if (request.action == QStringLiteral("testConfigs")) {
                    ConfigurationCatalog catalog;
                    result = controllerGuard->configurationCatalog(&catalog);
                    if (result.ok) {
                        QJsonArray configs;
                        for (const ConfigurationCatalogItem& item : catalog.items) {
                            if (!item.enabled || !item.valid) continue;
                            configs.push_back(QJsonObject{
                                {QStringLiteral("configId"), item.configId},
                                {QStringLiteral("title"), item.title},
                                {QStringLiteral("description"), item.description},
                                {QStringLiteral("algorithmId"), item.algorithmId},
                            });
                        }
                        data.insert(
                            QStringLiteral("selectedConfigId"),
                            controllerGuard->snapshot().descriptor.configId);
                        data.insert(QStringLiteral("configs"), configs);
                    }
                } else if (request.action == QStringLiteral("configCatalog")) {
                    ConfigurationCatalog catalog;
                    result = controllerGuard->configurationCatalog(&catalog);
                    if (result.ok) data = configurationCatalogObject(catalog);
                } else if (request.action == QStringLiteral("configDocument")) {
                    ConfigurationDocument document;
                    result = controllerGuard->configurationDocument(
                        request.params.value(QStringLiteral("documentId"))
                            .toString().trimmed(),
                        &document);
                    if (result.ok) data = configurationDocumentObject(document);
                } else if (request.action == QStringLiteral("saveConfig")) {
                    ConfigurationDocument document;
                    result = controllerGuard->saveConfiguration(
                        request.params.value(QStringLiteral("documentId"))
                            .toString().trimmed(),
                        request.params.value(QStringLiteral("expectedRevision"))
                            .toString(),
                        request.params.value(QStringLiteral("value"))
                            .toObject().toVariantMap(),
                        &document);
                    if (result.ok) data = configurationDocumentObject(document);
                } else if (request.action == QStringLiteral("hardwareOptions")) {
                    data = hardwareOptionsObject(controllerGuard->hardwareOptions());
                } else if (request.action == QStringLiteral("controls")) {
                    QJsonArray controls;
                    for (const ControlResource& control :
                         controllerGuard->availableControls()) {
                        controls.push_back(QJsonObject{
                            {QStringLiteral("resourceId"), control.resourceId},
                            {QStringLiteral("providerId"), control.providerId},
                        });
                    }
                    data.insert(QStringLiteral("controls"), controls);
                } else if (request.action == QStringLiteral("ports")) {
                    QJsonArray ports;
                    for (const SerialPortInfo& port :
                         controllerGuard->availableSerialPorts()) {
                        ports.push_back(QJsonObject{
                            {QStringLiteral("portName"), port.portName},
                            {QStringLiteral("description"), port.description},
                            {QStringLiteral("manufacturer"), port.manufacturer},
                            {QStringLiteral("serialNumber"), port.serialNumber},
                            {QStringLiteral("systemLocation"), port.systemLocation},
                        });
                    }
                    data.insert(QStringLiteral("ports"), ports);
                } else if (request.action == QStringLiteral("analysisResult")) {
                    AnalysisChannelProjection projection;
                    result = controllerGuard->analysisResult(
                        AnalysisResultQuery{
                            request.params.value(QStringLiteral("taskId"))
                                .toString().trimmed(),
                            static_cast<quint64>(request.params
                                                     .value(QStringLiteral("analysisGeneration"))
                                                     .toDouble()),
                            static_cast<int>(request.params
                                                 .value(QStringLiteral("channel"))
                                                 .toDouble()),
                        },
                        &projection);
                    if (result.ok) {
                        const QJsonObject projected = analysisResultObject(projection);
                        if (projected.isEmpty()) {
                            result = protocolError(
                                QStringLiteral("analysis_projection_invalid"),
                                QStringLiteral("The stored analysis projection is invalid"));
                        } else {
                            data.insert(QStringLiteral("analysisResult"), projected);
                        }
                    }
                } else if (request.action == QStringLiteral("selectControl")) {
                    result = controllerGuard->selectControl(
                        request.params.value(QStringLiteral("resourceId")).toString());
                } else if (request.action == QStringLiteral("selectSerialPort")) {
                    result = controllerGuard->selectSerialPort(
                        request.params.value(QStringLiteral("portName")).toString());
                } else if (request.action == QStringLiteral("selectAuxiliarySerialPort")) {
                    result = controllerGuard->selectAuxiliarySerialPort(
                        request.params.value(QStringLiteral("portName")).toString());
                } else if (request.action == QStringLiteral("prepare")) {
                    result = controllerGuard->prepare();
                } else if (request.action == QStringLiteral("start")) {
                    TestRunOptions runOptions;
                    ActionResult parseError;
                    if (!parseStartOptions(request, &runOptions, &parseError)) {
                        result = parseError;
                    } else {
                        result = controllerGuard->start(runOptions);
                    }
                } else if (request.action == QStringLiteral("pause")) {
                    result = controllerGuard->pause();
                } else if (request.action == QStringLiteral("resume")) {
                    result = controllerGuard->resume();
                } else if (request.action == QStringLiteral("setDigitalStimulus")) {
                    if (!digitalStimulusSupportedByVersionOne(
                            controllerGuard->snapshot().digitalStimulus)) {
                        result = protocolError(
                            QStringLiteral("capability_unsupported"),
                            QStringLiteral("WebSocket v1 supports digital stimulus bits 0..15 only"));
                    } else {
                        result = controllerGuard->setDigitalStimulus(
                            request.params.value(QStringLiteral("switchId")).toString(),
                            request.params.value(QStringLiteral("active")).toBool(),
                            static_cast<quint64>(request.params
                                                     .value(QStringLiteral("expectedRevision"))
                                                     .toDouble()));
                    }
                } else if (request.action == QStringLiteral("resetDigitalStimulus")) {
                    if (!digitalStimulusSupportedByVersionOne(
                            controllerGuard->snapshot().digitalStimulus)) {
                        result = protocolError(
                            QStringLiteral("capability_unsupported"),
                            QStringLiteral("WebSocket v1 supports digital stimulus bits 0..15 only"));
                    } else {
                        result = controllerGuard->resetDigitalStimulus();
                    }
                } else {
                    result = protocolError(QStringLiteral("unknown_action"),
                                           QStringLiteral("Action is not implemented"));
                }

                if (request.action == QStringLiteral("setDigitalStimulus") ||
                    request.action == QStringLiteral("resetDigitalStimulus")) {
                    data.insert(
                        QStringLiteral("digitalStimulus"),
                        digitalStimulusObject(
                            controllerGuard->snapshot().digitalStimulus));
                }

                QMetaObject::invokeMethod(
                    owner.data(),
                    [owner,
                     socketGuard,
                     id = request.id,
                     action = request.action,
                     result,
                     data,
                     selectedConfigPath] {
                        if (owner == nullptr || owner->m_impl == nullptr) {
                            return;
                        }
                        if (result.ok && !selectedConfigPath.isEmpty()) {
                            owner->m_impl->launchOptions.testConfigPath =
                                selectedConfigPath;
                        }
                        if (socketGuard == nullptr) {
                            return;
                        }
                        owner->m_impl->send(
                            socketGuard,
                            action == QStringLiteral("analysisResult")
                                ? makeAnalysisResultReply(id, result, data)
                                : makeReply(id, result, data));
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    void startStop(const QString& id, QWebSocket* socket)
    {
        if (controller == nullptr) {
            clearPending();
            send(socket,
                 makeReply(id,
                           protocolError(QStringLiteral("invalid_state"),
                                         QStringLiteral("Controller is unavailable"))));
            return;
        }

        const QPointer<WebSocketFrontendServer> owner(q);
        const QPointer<TestApplicationController> controllerGuard(controller);
        const QPointer<QWebSocket> socketGuard(socket);
        QMetaObject::invokeMethod(
            controller,
            [owner, controllerGuard, socketGuard, id] {
                if (owner == nullptr || controllerGuard == nullptr) {
                    return;
                }
                const ActionResult result = controllerGuard->stopAsync(5000);
                QMetaObject::invokeMethod(
                    owner.data(),
                    [owner, socketGuard, id, result] {
                        if (owner == nullptr || owner->m_impl == nullptr) {
                            return;
                        }
                        owner->m_impl->handleStopStarted(socketGuard, id, result);
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    void handleStopStarted(QWebSocket* socket,
                           const QString& id,
                           const ActionResult& result)
    {
        if (backpressureCleanupStarted &&
            pendingOperation == PendingOperation::Stop &&
            pendingRequestId == id && !result.ok) {
            pendingOperation = PendingOperation::BackpressureCleanup;
            pendingRequestId.clear();
            pendingSocket = backpressureSocket;
            scheduleShutdown();
            return;
        }
        if (pendingOperation != PendingOperation::Stop ||
            pendingRequestId != id || result.ok) {
            return;
        }
        clearPending();
        send(socket, makeReply(id, result));
    }

    void handleStopCompleted(const ActionResult& result)
    {
        if (backpressureCleanupStarted) {
            pendingOperation = PendingOperation::BackpressureCleanup;
            pendingRequestId.clear();
            pendingSocket = backpressureSocket;
            Q_UNUSED(result);
            scheduleShutdown();
            return;
        }
        if (pendingOperation == PendingOperation::Stop) {
            const QString id = pendingRequestId;
            const QPointer<QWebSocket> socket = pendingSocket;
            clearPending();
            send(socket, makeReply(id, result));
            return;
        }
        if (pendingOperation == PendingOperation::Disconnect ||
            pendingOperation == PendingOperation::Quit) {
            if (result.ok) {
                scheduleShutdown();
            } else {
                finishCleanup(result);
            }
            return;
        }
        if (pendingOperation == PendingOperation::DropCleanup) {
            scheduleShutdown();
        }
    }

    void beginCleanup(PendingOperation operation,
                      const QString& id,
                      QWebSocket* socket)
    {
        pendingOperation = operation;
        pendingRequestId = id;
        pendingSocket = socket;
        inspectCleanupState();
    }

    void inspectCleanupState()
    {
        if (controller == nullptr) {
            finishCleanup(protocolError(QStringLiteral("invalid_state"),
                                        QStringLiteral("Controller is unavailable")));
            return;
        }

        const QPointer<WebSocketFrontendServer> owner(q);
        const QPointer<TestApplicationController> controllerGuard(controller);
        QMetaObject::invokeMethod(
            controller,
            [owner, controllerGuard] {
                if (owner == nullptr || controllerGuard == nullptr) {
                    return;
                }
                const ApplicationSnapshot snapshot = controllerGuard->snapshot();
                const bool needsStop = snapshot.phase == QStringLiteral("running") ||
                    snapshot.phase == QStringLiteral("paused");
                const ActionResult result = needsStop
                    ? controllerGuard->stopAsync(5000)
                    : ActionResult{};
                QMetaObject::invokeMethod(
                    owner.data(),
                    [owner, needsStop, result] {
                        if (owner == nullptr || owner->m_impl == nullptr) {
                            return;
                        }
                        owner->m_impl->handleCleanupInspected(needsStop, result);
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    void handleCleanupInspected(bool needsStop, const ActionResult& result)
    {
        if (pendingOperation != PendingOperation::Disconnect &&
            pendingOperation != PendingOperation::Quit &&
            pendingOperation != PendingOperation::DropCleanup &&
            pendingOperation != PendingOperation::BackpressureCleanup) {
            return;
        }
        if (backpressureCleanupStarted) {
            if (!result.ok || !needsStop) {
                scheduleShutdown();
            }
            return;
        }
        if (!result.ok) {
            if (pendingOperation == PendingOperation::DropCleanup) {
                scheduleShutdown();
            } else {
                finishCleanup(result);
            }
            return;
        }
        if (!needsStop) {
            scheduleShutdown();
        }
    }

    void scheduleShutdown()
    {
        if (controller == nullptr) {
            finishCleanup(protocolError(QStringLiteral("invalid_state"),
                                        QStringLiteral("Controller is unavailable")));
            return;
        }

        const QPointer<WebSocketFrontendServer> owner(q);
        const QPointer<TestApplicationController> controllerGuard(controller);
        QMetaObject::invokeMethod(
            controller,
            [owner, controllerGuard] {
                if (owner == nullptr || controllerGuard == nullptr) {
                    return;
                }
                const ActionResult result = controllerGuard->shutdown();
                QMetaObject::invokeMethod(
                    owner.data(),
                    [owner, result] {
                        if (owner == nullptr || owner->m_impl == nullptr) {
                            return;
                        }
                        if (!result.ok &&
                            result.code == QStringLiteral("analysis_shutdown_timeout")) {
                            QTimer::singleShot(100, owner.data(), [owner] {
                                if (owner != nullptr && owner->m_impl != nullptr &&
                                    owner->m_impl->pendingOperation !=
                                        PendingOperation::None) {
                                    owner->m_impl->scheduleShutdown();
                                }
                            });
                            return;
                        }
                        owner->m_impl->finishCleanup(result);
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    void finishCleanup(const ActionResult& result)
    {
        const PendingOperation completedOperation = pendingOperation;
        const QString id = pendingRequestId;
        const QPointer<QWebSocket> socket = pendingSocket;
        clearPending();

        if (backpressureCleanupStarted ||
            completedOperation == PendingOperation::BackpressureCleanup) {
            backpressureCleanupStarted = false;
            const QPointer<QWebSocket> backpressureTarget = backpressureSocket;
            backpressureSocket.clear();
            if (backpressureTarget != nullptr) {
                suppressDisconnectCleanup = backpressureTarget;
                backpressureTarget->close(
                    QWebSocketProtocol::CloseCodeBadOperation,
                    QStringLiteral("telemetry_backpressure"));
            }
            return;
        }

        if (completedOperation == PendingOperation::DropCleanup) {
            return;
        }
        if (completedOperation != PendingOperation::Disconnect &&
            completedOperation != PendingOperation::Quit) {
            return;
        }

        send(socket, makeReply(id, result));
        if (!result.ok) {
            return;
        }

        const bool quitting = completedOperation == PendingOperation::Quit;
        if (quitting) {
            server.close();
        }
        if (socket == nullptr) {
            if (quitting) {
                const QPointer<WebSocketFrontendServer> owner(q);
                QTimer::singleShot(0, q, [owner] {
                    if (owner != nullptr) {
                        emit owner->quitRequested();
                    }
                });
            }
            return;
        }
        if (quitting) {
            const QPointer<WebSocketFrontendServer> owner(q);
            QObject::connect(socket,
                             &QWebSocket::disconnected,
                             q,
                             [owner] {
                                 if (owner != nullptr) {
                                     emit owner->quitRequested();
                                 }
                             });
        }
        queueCloseAfterDrain(socket,
                             QWebSocketProtocol::CloseCodeNormal,
                             quitting ? QStringLiteral("quit")
                                      : QStringLiteral("disconnect"));
    }

    void handleActiveClientDropped()
    {
        if (isActiveNonStoppable(cachedSnapshot)) {
            clearPending();
            backpressureCleanupStarted = false;
            backpressureSocket.clear();
            return;
        }
        if (pendingOperation == PendingOperation::Stop) {
            pendingOperation = PendingOperation::DropCleanup;
            pendingRequestId.clear();
            pendingSocket.clear();
            return;
        }
        if (pendingOperation == PendingOperation::Disconnect ||
            pendingOperation == PendingOperation::Quit ||
            pendingOperation == PendingOperation::DropCleanup ||
            pendingOperation == PendingOperation::BackpressureCleanup) {
            pendingSocket.clear();
            return;
        }
        beginCleanup(PendingOperation::DropCleanup, QString(), nullptr);
    }

    void clearPending()
    {
        pendingOperation = PendingOperation::None;
        pendingRequestId.clear();
        pendingSocket.clear();
    }

    WebSocketFrontendServer* q = nullptr;
    QPointer<TestApplicationController> controller;
    FrontendLaunchOptions launchOptions;
    WebSocketServerOptions options;
    QWebSocketServer server;
    QPointer<QWebSocket> activeClient;
    quint64 activeClientEpoch = 0;
    TelemetryDeliveryMode telemetryDeliveryMode = TelemetryDeliveryMode::Single;
    std::unique_ptr<WebTelemetryBatcher> telemetryBatcher;
    QTimer snapshotFlushTimer;
    bool hasDeferredRunningSnapshot = false;
    QJsonObject deferredRunningSnapshot;
    ApplicationSnapshot deferredRunningSnapshotSource;
    ApplicationSnapshot lastProjectedSnapshot;
    bool hasLastProjectedSnapshot = false;
    QQueue<QueuedOutput> outputQueue;
    qint64 queuedOutputBytes = 0;
    bool outputPausedAtHighWater = false;
    bool telemetryDropWarningEmitted = false;
    QPointer<QWebSocket> closeAfterDrainSocket;
    quint64 closeAfterDrainEpoch = 0;
    QWebSocketProtocol::CloseCode closeAfterDrainCode =
        QWebSocketProtocol::CloseCodeNormal;
    QString closeAfterDrainReason;
    bool backpressureCleanupStarted = false;
    QPointer<QWebSocket> backpressureSocket;
    ApplicationSnapshot cachedSnapshot;
    quint64 snapshotSequence = 0;
    quint64 sampleSequence = 0;
    PendingOperation pendingOperation = PendingOperation::None;
    QString pendingRequestId;
    QPointer<QWebSocket> pendingSocket;
    QPointer<QWebSocket> suppressDisconnectCleanup;
    bool serverClosing = false;
};

WebSocketFrontendServer::WebSocketFrontendServer(
    TestApplicationController* controller,
    FrontendLaunchOptions launchOptions,
    WebSocketServerOptions options,
    QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>(this,
                                    controller,
                                    std::move(launchOptions),
                                    options))
{
}

WebSocketFrontendServer::~WebSocketFrontendServer() = default;

bool WebSocketFrontendServer::listen(QString* errorMessage)
{
    return m_impl->listen(errorMessage);
}

void WebSocketFrontendServer::close()
{
    m_impl->close();
}

bool WebSocketFrontendServer::isListening() const
{
    return m_impl->server.isListening();
}

quint16 WebSocketFrontendServer::serverPort() const
{
    return m_impl->server.serverPort();
}

QHostAddress WebSocketFrontendServer::serverAddress() const
{
    return m_impl->server.serverAddress();
}

QUrl WebSocketFrontendServer::webSocketUrl() const
{
    QUrl url;
    url.setScheme(QStringLiteral("ws"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(serverPort());
    url.setPath(QStringLiteral("/ws"));
    return url;
}

} // namespace hwtest::app::web
