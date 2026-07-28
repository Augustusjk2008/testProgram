#include "web_socket_frontend_server.h"

#include "web_protocol.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QFileInfo>
#include <QPointer>
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

quint64 qtIncomingLimit(quint64 protocolLimit)
{
    if (protocolLimit == std::numeric_limits<quint64>::max()) {
        return protocolLimit;
    }
    return protocolLimit + 1;
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
        if (controller != nullptr) {
            cachedSnapshot = controller->snapshot();
            QObject::connect(controller,
                             &TestApplicationController::snapshotChanged,
                             q,
                             [this](const ApplicationSnapshot& snapshot) {
                                 cachedSnapshot = snapshot;
                                 ++snapshotSequence;
                                 if (activeClient != nullptr &&
                                     activeClient->state() ==
                                         QAbstractSocket::ConnectedState) {
                                     send(activeClient,
                                          makeSnapshot(snapshotSequence,
                                                       cachedSnapshot));
                                 }
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
                                 if (activeClient == nullptr ||
                                     activeClient->state() !=
                                         QAbstractSocket::ConnectedState) {
                                     return;
                                 }
                                 ++sampleSequence;
                                 send(activeClient,
                                      makeSample(sampleSequence, sample));
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
                         &QWebSocket::disconnected,
                         q,
                         [this, socket] {
                             if (socket == activeClient) {
                                 activeClient.clear();
                                 if (socket == suppressDisconnectCleanup) {
                                     suppressDisconnectCleanup.clear();
                                     return;
                                 }
                                 if (!serverClosing) {
                                     handleActiveClientDropped();
                                 }
                             }
                         });

        send(socket, makeHello());
        send(socket, makeSnapshot(snapshotSequence, cachedSnapshot));
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

        if (pendingOperation != PendingOperation::None &&
            !isReadAction(request.action)) {
            send(socket,
                 makeReply(request.id,
                           protocolError(QStringLiteral("command_in_progress"),
                                         QStringLiteral(
                                             "Another command is still in progress"))));
            return;
        }

        ActionResult validation;
        if (!validateAction(request, &validation)) {
            send(socket, makeReply(request.id, validation));
            return;
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
            action == QStringLiteral("testConfigs") ||
            action == QStringLiteral("controls") ||
            action == QStringLiteral("ports");
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
            if (value < 10.0 || value > 3600000.0) {
                if (error != nullptr) {
                    *error = protocolError(
                        QStringLiteral("ParameterRangeError"),
                        QStringLiteral("Parameter 'intervalMs' must be in the range 10..3600000"));
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
        if (request.action == QStringLiteral("start")) {
            return parseStartOptions(request, nullptr, error);
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

    void dispatchControllerAction(const WebRequest& request, QWebSocket* socket)
    {
        if (request.action == QStringLiteral("testConfigs")) {
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
        QMetaObject::invokeMethod(
            controller,
            [owner,
             controllerGuard,
             socketGuard,
             request,
             optionsCopy] {
                if (owner == nullptr || controllerGuard == nullptr) {
                    return;
                }

                ActionResult result;
                QJsonObject data;
                QString selectedConfigPath;
                if (request.action == QStringLiteral("load")) {
                    result = configureController(*controllerGuard, optionsCopy);
                } else if (request.action == QStringLiteral("selectTest")) {
                    result = loadSelectedTest(
                        *controllerGuard,
                        optionsCopy,
                        request.params.value(QStringLiteral("configId"))
                            .toString().trimmed(),
                        &selectedConfigPath);
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
                } else if (request.action == QStringLiteral("selectControl")) {
                    result = controllerGuard->selectControl(
                        request.params.value(QStringLiteral("resourceId")).toString());
                } else if (request.action == QStringLiteral("selectSerialPort")) {
                    result = controllerGuard->selectSerialPort(
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
                        send(socketGuard, makeReply(id, result, data));
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
        if (pendingOperation != PendingOperation::Stop ||
            pendingRequestId != id || result.ok) {
            return;
        }
        clearPending();
        send(socket, makeReply(id, result));
    }

    void handleStopCompleted(const ActionResult& result)
    {
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
            pendingOperation != PendingOperation::DropCleanup) {
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
        suppressDisconnectCleanup = socket;
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
        socket->close(QWebSocketProtocol::CloseCodeNormal,
                      quitting ? QStringLiteral("quit")
                               : QStringLiteral("disconnect"));
    }

    void handleActiveClientDropped()
    {
        if (pendingOperation == PendingOperation::Stop) {
            pendingOperation = PendingOperation::DropCleanup;
            pendingRequestId.clear();
            pendingSocket.clear();
            return;
        }
        if (pendingOperation == PendingOperation::Disconnect ||
            pendingOperation == PendingOperation::Quit ||
            pendingOperation == PendingOperation::DropCleanup) {
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

    static void send(QWebSocket* socket, const QJsonObject& message)
    {
        if (socket != nullptr && socket->state() == QAbstractSocket::ConnectedState) {
            const QString text = compactJson(message);
            socket->sendTextMessage(text);
        }
    }

    WebSocketFrontendServer* q = nullptr;
    QPointer<TestApplicationController> controller;
    FrontendLaunchOptions launchOptions;
    WebSocketServerOptions options;
    QWebSocketServer server;
    QPointer<QWebSocket> activeClient;
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
