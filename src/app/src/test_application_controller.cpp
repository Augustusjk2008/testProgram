#include <app/test_application_controller.h>

#include <algorithm/mbddf_transport.h>
#include <algorithm/system_status_executor.h>

#include <biz/biz_factory.h>
#include <biz/i_test_run_service.h>
#include <biz/test_config_manager.h>

#include <hal/hal_factory.h>
#include <hal/i_hal_device.h>
#include <hal/i_hal_service.h>

#include <logging/hal_log_bridge.h>
#include <logging/log_file_sink.h>
#include <logging/log_service.h>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QThread>
#include <QTimer>

#include <algorithm>

namespace hwtest::app {

namespace {

ActionResult failure(const QString& code, const QString& message)
{
    return ActionResult{false, code, message};
}

bool onAffinityThread(const QObject* object)
{
    return object != nullptr && QThread::currentThread() == object->thread();
}

ActionResult affinityFailure()
{
    return failure(QStringLiteral("wrong_thread"),
                   QStringLiteral("Application controller actions must run on its affinity thread"));
}

struct BoolReset {
    bool* value = nullptr;
    ~BoolReset()
    {
        if (value != nullptr) {
            *value = false;
        }
    }
};

ActionResult loadJsonMap(const QString& path, QVariantMap* output)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("hal_config_open"),
                       QStringLiteral("Cannot open '%1': %2").arg(path, file.errorString()));
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(QStringLiteral("hal_config_parse"),
                       QStringLiteral("Invalid JSON object in '%1': %2")
                           .arg(path, parseError.errorString()));
    }
    *output = document.object().toVariantMap();
    return {};
}

QString resolvedPath(const QString& configPath, const QString& value)
{
    if (value.isEmpty() || QFileInfo(value).isAbsolute()) {
        return value;
    }
    return QDir(QFileInfo(configPath).absolutePath()).absoluteFilePath(value);
}

ActionResult bizFailure(const hwtest::biz::Status& status, const QString& fallback)
{
    return failure(hwtest::biz::errorCodeToString(status.code),
                   status.error.message.isEmpty() ? fallback : status.error.message);
}

ActionResult halFailure(const hwtest::hal::HalStatus& status, const QString& fallback)
{
    return failure(hwtest::hal::toString(status.code),
                   status.error.message.isEmpty() ? fallback : status.error.message);
}

QString serialPortNameFor(const QVariantMap& halConfig, const QString& resourceId)
{
    const QVariantMap resource = halConfig.value(QStringLiteral("hardware")).toMap()
                                     .value(QStringLiteral("resources")).toMap()
                                     .value(resourceId).toMap();
    if (resource.value(QStringLiteral("providerId")).toString().trimmed() !=
        QStringLiteral("qt.serial")) {
        return {};
    }
    return resource.value(QStringLiteral("properties")).toMap()
        .value(QStringLiteral("portName")).toString().trimmed();
}

} // namespace

class TestApplicationController::Impl {
public:
    using HalServicePtr = std::unique_ptr<hwtest::hal::IHalService,
                                          void (*)(hwtest::hal::IHalService*)>;
    using TestServicePtr = std::unique_ptr<hwtest::biz::ITestRunService,
                                           void (*)(hwtest::biz::ITestRunService*)>;

    QString testConfigPath;
    QString halConfigPath;
    QVariantMap halConfig;
    QVector<ControlResource> controls;
    ApplicationSnapshot snapshot;
    int runTimeoutMs = 5000;
    HalServicePtr hal{nullptr, &hwtest::hal::destroyHalService};
    hwtest::hal::SessionId sessionId;
    hwtest::hal::IHalDevice* device = nullptr;
    std::unique_ptr<hwtest::algorithm::mbddf::SystemStatusAlgorithmExecutor> executor;
    TestServicePtr runner{nullptr, &hwtest::biz::destroyTestRunService};
    hwtest::logging::LogService logService;
    std::unique_ptr<hwtest::logging::JsonLineFileSink> fileSink;
    ActionResult latchedShutdownFailure;
    quint64 generation = 0;
    bool waitInProgress = false;
};

TestApplicationController::TestApplicationController(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    qRegisterMetaType<ApplicationSnapshot>();
    qRegisterMetaType<SerialPortInfo>();
    qRegisterMetaType<QVector<SerialPortInfo>>();
}

TestApplicationController::~TestApplicationController()
{
    QObject::disconnect(this, nullptr, nullptr, nullptr);
    shutdown();
}

ActionResult TestApplicationController::loadConfigurations(const QString& testConfigPath,
                                                            const QString& halConfigPath)
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (m_impl->snapshot.phase != QStringLiteral("empty") &&
        m_impl->snapshot.phase != QStringLiteral("configured")) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Configurations can only be loaded while disconnected"));
    }

    const QString absoluteTestPath = QFileInfo(testConfigPath).absoluteFilePath();
    const QString absoluteHalPath = QFileInfo(halConfigPath).absoluteFilePath();
    hwtest::biz::TestConfigManager configManager;
    const auto testConfig = configManager.load(absoluteTestPath);
    if (!testConfig.ok()) {
        return failure(QStringLiteral("test_config"), testConfig.status.error.message);
    }

    int enabledSystemStatusSteps = 0;
    for (const hwtest::biz::TestStep& step : testConfig.value.steps) {
        if (!step.enabled) {
            continue;
        }
        if (step.algorithmId != QStringLiteral("mbddf.system_status")) {
            return failure(QStringLiteral("unsupported_algorithm"),
                           QStringLiteral("This application only supports mbddf.system_status"));
        }
        ++enabledSystemStatusSteps;
    }
    if (enabledSystemStatusSteps != 1) {
        return failure(QStringLiteral("test_config"),
                       QStringLiteral("Exactly one enabled SYSTEM_STATUS step is required"));
    }

    QVariantMap halConfig;
    const ActionResult loadedHal = loadJsonMap(absoluteHalPath, &halConfig);
    if (!loadedHal.ok) {
        return loadedHal;
    }

    const QVariantMap control = halConfig.value(QStringLiteral("control")).toMap();
    const QString selectedResource = control.value(QStringLiteral("resourceId")).toString().trimmed();
    const QString deviceId = control.value(QStringLiteral("deviceId")).toString().trimmed();
    bool timeoutOk = false;
    const int timeoutMs = control.value(QStringLiteral("runTimeoutMs")).toInt(&timeoutOk);
    if (selectedResource.isEmpty() || deviceId.isEmpty() || !timeoutOk || timeoutMs <= 0) {
        return failure(QStringLiteral("hal_config"),
                       QStringLiteral("control.deviceId, control.resourceId and a positive control.runTimeoutMs are required"));
    }

    QVector<ControlResource> controls;
    const QVariantMap resources = halConfig.value(QStringLiteral("hardware")).toMap()
                                      .value(QStringLiteral("resources")).toMap();
    for (auto iterator = resources.cbegin(); iterator != resources.cend(); ++iterator) {
        const QVariantMap resource = iterator.value().toMap();
        if (resource.value(QStringLiteral("module")).toString() != QStringLiteral("control")) {
            continue;
        }
        const QString providerId = resource.value(QStringLiteral("providerId")).toString().trimmed();
        if (!providerId.isEmpty()) {
            controls.push_back(ControlResource{iterator.key(), providerId});
        }
    }

    const auto selected = std::find_if(controls.cbegin(), controls.cend(), [&](const ControlResource& item) {
        return item.resourceId == selectedResource;
    });
    if (selected == controls.cend()) {
        return failure(QStringLiteral("control_not_found"),
                       QStringLiteral("Selected control resource is not configured"));
    }

    m_impl->testConfigPath = absoluteTestPath;
    m_impl->halConfigPath = absoluteHalPath;
    m_impl->halConfig = halConfig;
    m_impl->controls = controls;
    m_impl->runTimeoutMs = timeoutMs;
    m_impl->snapshot = {};
    m_impl->snapshot.phase = QStringLiteral("configured");
    m_impl->snapshot.controlResourceId = selected->resourceId;
    m_impl->snapshot.providerId = selected->providerId;
    m_impl->snapshot.serialPortName = serialPortNameFor(halConfig, selected->resourceId);
    emit snapshotChanged(m_impl->snapshot);
    return {};
}

QVector<ControlResource> TestApplicationController::availableControls() const
{
    Q_ASSERT_X(onAffinityThread(this),
               "TestApplicationController::availableControls",
               "must run on the controller affinity thread");
    if (!onAffinityThread(this)) {
        return {};
    }
    return m_impl->controls;
}

QVector<SerialPortInfo> TestApplicationController::availableSerialPorts() const
{
    Q_ASSERT_X(onAffinityThread(this),
               "TestApplicationController::availableSerialPorts",
               "must run on the controller affinity thread");
    if (!onAffinityThread(this)) {
        return {};
    }

    QVector<SerialPortInfo> result;
    const QVector<hwtest::hal::SerialPortDescriptor> ports =
        hwtest::hal::availableSerialPorts();
    result.reserve(ports.size());
    for (const hwtest::hal::SerialPortDescriptor& port : ports) {
        result.push_back(SerialPortInfo{port.portName,
                                        port.description,
                                        port.manufacturer,
                                        port.serialNumber,
                                        port.systemLocation});
    }
    return result;
}

ActionResult TestApplicationController::selectControl(const QString& resourceId)
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (m_impl->snapshot.phase != QStringLiteral("configured")) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Control resource can only be selected while configured and disconnected"));
    }
    const QString normalized = resourceId.trimmed();
    const auto selected = std::find_if(m_impl->controls.cbegin(), m_impl->controls.cend(),
                                       [&](const ControlResource& item) {
                                           return item.resourceId == normalized;
                                       });
    if (selected == m_impl->controls.cend()) {
        return failure(QStringLiteral("control_not_found"),
                       QStringLiteral("Unknown control resource '%1'").arg(normalized));
    }

    QVariantMap control = m_impl->halConfig.value(QStringLiteral("control")).toMap();
    control.insert(QStringLiteral("resourceId"), selected->resourceId);
    m_impl->halConfig.insert(QStringLiteral("control"), control);
    m_impl->snapshot.controlResourceId = selected->resourceId;
    m_impl->snapshot.providerId = selected->providerId;
    m_impl->snapshot.serialPortName = serialPortNameFor(m_impl->halConfig,
                                                        selected->resourceId);
    emit snapshotChanged(m_impl->snapshot);
    return {};
}

ActionResult TestApplicationController::selectSerialPort(const QString& portName)
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (m_impl->snapshot.phase != QStringLiteral("configured")) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Serial port can only be selected while configured and disconnected"));
    }

    const QString normalized = portName.trimmed();
    if (normalized.isEmpty()) {
        return failure(QStringLiteral("serial_port_required"),
                       QStringLiteral("Serial port name must not be empty"));
    }

    QVariantMap hardware = m_impl->halConfig.value(QStringLiteral("hardware")).toMap();
    QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    const QString resourceId = m_impl->snapshot.controlResourceId;
    QVariantMap resource = resources.value(resourceId).toMap();
    if (resource.value(QStringLiteral("providerId")).toString().trimmed() !=
        QStringLiteral("qt.serial")) {
        return failure(QStringLiteral("control_not_serial"),
                       QStringLiteral("The selected control resource is not a serial provider"));
    }

    QVariantMap properties = resource.value(QStringLiteral("properties")).toMap();
    properties.insert(QStringLiteral("portName"), normalized);
    resource.insert(QStringLiteral("properties"), properties);
    resources.insert(resourceId, resource);
    hardware.insert(QStringLiteral("resources"), resources);
    m_impl->halConfig.insert(QStringLiteral("hardware"), hardware);
    m_impl->snapshot.serialPortName = normalized;
    emit snapshotChanged(m_impl->snapshot);
    return {};
}

ActionResult TestApplicationController::prepare()
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (m_impl->snapshot.phase != QStringLiteral("configured")) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Prepare is only available after configurations are loaded and while disconnected"));
    }

    m_impl->snapshot.phase = QStringLiteral("preparing");
    m_impl->snapshot.message.clear();
    emit snapshotChanged(m_impl->snapshot);

    m_impl->hal.reset(hwtest::hal::createHalService());
    if (!m_impl->hal) {
        m_impl->snapshot.phase = QStringLiteral("configured");
        return failure(QStringLiteral("hal_create"), QStringLiteral("Unable to create HAL service"));
    }

    const hwtest::hal::HalStatus initialized = m_impl->hal->initialize(m_impl->halConfig);
    if (!initialized.ok()) {
        const ActionResult result = halFailure(initialized, QStringLiteral("Unable to initialize HAL"));
        shutdown();
        return result;
    }

    const QVariantMap control = m_impl->halConfig.value(QStringLiteral("control")).toMap();
    const QString deviceId = control.value(QStringLiteral("deviceId")).toString().trimmed();
    const auto opened = m_impl->hal->openDevice(deviceId, hwtest::hal::OperationOptions{});
    if (!opened.ok()) {
        const ActionResult result = halFailure(opened.status, QStringLiteral("Unable to open HAL device"));
        shutdown();
        return result;
    }
    m_impl->sessionId = opened.value;

    const auto device = m_impl->hal->device(m_impl->sessionId);
    if (!device.ok() || device.value == nullptr) {
        const ActionResult result = device.ok()
            ? failure(QStringLiteral("hal_device"), QStringLiteral("HAL returned a null device"))
            : halFailure(device.status, QStringLiteral("Unable to get HAL device"));
        shutdown();
        return result;
    }
    m_impl->device = device.value;

    auto transport = std::make_unique<hwtest::algorithm::mbddf::HalControlTransport>(
        m_impl->device, m_impl->snapshot.controlResourceId);
    m_impl->executor = std::make_unique<hwtest::algorithm::mbddf::SystemStatusAlgorithmExecutor>(
        std::move(transport));
    m_impl->runner.reset(hwtest::biz::createTestRunService(m_impl->executor.get()));
    if (!m_impl->runner) {
        const ActionResult result = failure(QStringLiteral("biz_create"),
                                            QStringLiteral("Unable to create BIZ service"));
        shutdown();
        return result;
    }

    const quint64 generation = ++m_impl->generation;

    const QString configuredLogPath = m_impl->halConfig.value(QStringLiteral("logging")).toMap()
                                          .value(QStringLiteral("filePath")).toString().trimmed();
    if (!configuredLogPath.isEmpty()) {
        const QString logPath = resolvedPath(m_impl->halConfigPath, configuredLogPath);
        QDir().mkpath(QFileInfo(logPath).absolutePath());
        m_impl->fileSink = std::make_unique<hwtest::logging::JsonLineFileSink>(logPath);
        if (!m_impl->fileSink->open()) {
            const ActionResult result = failure(QStringLiteral("logging"),
                                                m_impl->fileSink->errorString());
            shutdown();
            return result;
        }
        m_impl->logService.addSink(m_impl->fileSink.get());
    }

    hwtest::logging::connectHalLogs(m_impl->hal.get(),
                                    &m_impl->logService,
                                    Qt::DirectConnection);
    QObject::connect(m_impl->runner.get(),
                     &hwtest::biz::ITestRunService::logProduced,
                     &m_impl->logService,
                     &hwtest::logging::LogService::append,
                     Qt::DirectConnection);
    QObject::connect(m_impl->runner.get(),
                     &hwtest::biz::ITestRunService::testProgress,
                     this,
                     [this, generation](const hwtest::biz::TaskId& taskId,
                                        const hwtest::biz::TestItemId&,
                                        int progress,
                                        const QString& step) {
                         if (generation != m_impl->generation) {
                             return;
                         }
                         m_impl->snapshot.taskId = taskId;
                         m_impl->snapshot.progress = progress;
                         m_impl->snapshot.progressStep = step;
                         emit snapshotChanged(m_impl->snapshot);
                     });
    QObject::connect(m_impl->runner.get(),
                     &hwtest::biz::ITestRunService::stateChanged,
                     this,
                     [this, generation](const hwtest::biz::TaskId& taskId,
                                        hwtest::biz::TestState state) {
                         if (generation != m_impl->generation) {
                             return;
                         }
                         const QString previousPhase = m_impl->snapshot.phase;
                         const bool previousTerminal =
                             previousPhase == QStringLiteral("stopped") ||
                             previousPhase == QStringLiteral("finished") ||
                             previousPhase == QStringLiteral("error");
                         if (previousTerminal && taskId == m_impl->snapshot.taskId) {
                             return;
                         }
                         m_impl->snapshot.taskId = taskId;
                         m_impl->snapshot.testState = hwtest::biz::testStateToString(state);
                         switch (state) {
                         case hwtest::biz::TestState::Uninitialized:
                             m_impl->snapshot.phase = QStringLiteral("configured");
                             break;
                         case hwtest::biz::TestState::Idle:
                             if (previousPhase == QStringLiteral("running") ||
                                 previousPhase == QStringLiteral("paused") ||
                                 previousPhase == QStringLiteral("stopping")) {
                                 m_impl->snapshot.phase = QStringLiteral("stopped");
                             } else {
                                 m_impl->snapshot.phase = QStringLiteral("ready");
                             }
                             break;
                         case hwtest::biz::TestState::Running:
                             m_impl->snapshot.phase = QStringLiteral("running");
                             break;
                         case hwtest::biz::TestState::Paused:
                             m_impl->snapshot.phase = QStringLiteral("paused");
                             break;
                         case hwtest::biz::TestState::Stopping:
                             m_impl->snapshot.phase = QStringLiteral("stopping");
                             break;
                         case hwtest::biz::TestState::Finished:
                             m_impl->snapshot.phase = QStringLiteral("finished");
                             break;
                         case hwtest::biz::TestState::Error:
                             m_impl->snapshot.phase = QStringLiteral("error");
                             break;
                         }
                         emit snapshotChanged(m_impl->snapshot);
                     });
    QObject::connect(m_impl->runner.get(),
                     &hwtest::biz::ITestRunService::resultProduced,
                     this,
                     [this, generation](const hwtest::biz::TaskId& taskId,
                                        const hwtest::biz::TestResult& result) {
                         if (generation != m_impl->generation) {
                             return;
                         }
                         m_impl->snapshot.taskId = taskId;
                         m_impl->snapshot.hasResult = true;
                         m_impl->snapshot.stepId = result.stepId;
                         m_impl->snapshot.testItemId = result.testItemId;
                         m_impl->snapshot.algorithmId = result.algorithmId;
                         m_impl->snapshot.verdict = hwtest::biz::testVerdictToString(result.verdict);
                         m_impl->snapshot.errorCode = hwtest::biz::errorCodeToString(result.errorCode);
                         m_impl->snapshot.message = result.message;
                         m_impl->snapshot.attempts = result.attempts;
                         m_impl->snapshot.rawData = result.rawData;
                         emit snapshotChanged(m_impl->snapshot);
                     });
    QObject::connect(m_impl->runner.get(),
                     &hwtest::biz::ITestRunService::hardwareError,
                     this,
                     [this, generation](const hwtest::biz::TaskId& taskId,
                                        const hwtest::biz::TestItemId&,
                                        hwtest::biz::ErrorCode code,
                                        const QString& description) {
                         if (generation != m_impl->generation) {
                             return;
                         }
                         m_impl->snapshot.taskId = taskId;
                         m_impl->snapshot.errorCode = hwtest::biz::errorCodeToString(code);
                         m_impl->snapshot.message = description;
                         emit snapshotChanged(m_impl->snapshot);
                     });

    const hwtest::biz::Status bizInitialized = m_impl->runner->initialize();
    if (!bizInitialized.ok()) {
        const ActionResult result = bizFailure(bizInitialized,
                                               QStringLiteral("Unable to initialize BIZ service"));
        shutdown();
        return result;
    }
    const hwtest::biz::Status loaded = m_impl->runner->loadConfiguration(m_impl->testConfigPath);
    if (!loaded.ok()) {
        const ActionResult result = bizFailure(loaded, QStringLiteral("Unable to load BIZ configuration"));
        shutdown();
        return result;
    }

    m_impl->snapshot.phase = QStringLiteral("ready");
    m_impl->snapshot.testState = QStringLiteral("Idle");
    emit snapshotChanged(m_impl->snapshot);
    return {};
}

ActionResult TestApplicationController::start()
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (!m_impl->runner ||
        (m_impl->snapshot.phase != QStringLiteral("ready") &&
         m_impl->snapshot.phase != QStringLiteral("finished") &&
         m_impl->snapshot.phase != QStringLiteral("stopped"))) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Run is only available after preparation or a finished run"));
    }

    m_impl->snapshot.progress = 0;
    m_impl->snapshot.progressStep.clear();
    m_impl->snapshot.hasResult = false;
    m_impl->snapshot.stepId.clear();
    m_impl->snapshot.testItemId.clear();
    m_impl->snapshot.algorithmId.clear();
    m_impl->snapshot.verdict.clear();
    m_impl->snapshot.errorCode.clear();
    m_impl->snapshot.message.clear();
    m_impl->snapshot.attempts = 0;
    m_impl->snapshot.rawData.clear();
    const auto started = m_impl->runner->startTest();
    if (!started.ok()) {
        return bizFailure(started.status, QStringLiteral("Unable to start test"));
    }
    m_impl->snapshot.taskId = started.value;
    m_impl->snapshot.phase = QStringLiteral("running");
    m_impl->snapshot.testState = QStringLiteral("Running");
    emit snapshotChanged(m_impl->snapshot);
    return {};
}

ActionResult TestApplicationController::pause()
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (!m_impl->runner) {
        return failure(QStringLiteral("invalid_state"), QStringLiteral("Application is not prepared"));
    }
    const hwtest::biz::Status status = m_impl->runner->pauseTest();
    return status.ok() ? ActionResult{} : bizFailure(status, QStringLiteral("Unable to pause test"));
}

ActionResult TestApplicationController::resume()
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (!m_impl->runner) {
        return failure(QStringLiteral("invalid_state"), QStringLiteral("Application is not prepared"));
    }
    const hwtest::biz::Status status = m_impl->runner->resumeTest();
    return status.ok() ? ActionResult{} : bizFailure(status, QStringLiteral("Unable to resume test"));
}

ActionResult TestApplicationController::stop(int timeoutMs)
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (!m_impl->runner) {
        return failure(QStringLiteral("invalid_state"), QStringLiteral("Application is not prepared"));
    }
    const hwtest::biz::Status status = m_impl->runner->stopTest(timeoutMs);
    return status.ok() ? ActionResult{} : bizFailure(status, QStringLiteral("Unable to stop test"));
}

ActionResult TestApplicationController::waitForTerminal(int timeoutMs)
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    const auto terminalResult = [this]() -> ActionResult {
        if (m_impl->snapshot.phase == QStringLiteral("error") &&
            !m_impl->snapshot.hasResult) {
            return failure(m_impl->snapshot.errorCode.isEmpty()
                               ? QStringLiteral("run_error")
                               : m_impl->snapshot.errorCode,
                           m_impl->snapshot.message.isEmpty()
                               ? QStringLiteral("Test entered Error without producing a result")
                               : m_impl->snapshot.message);
        }
        return {};
    };

    if (m_impl->snapshot.phase == QStringLiteral("finished") ||
        m_impl->snapshot.phase == QStringLiteral("error") ||
        m_impl->snapshot.phase == QStringLiteral("stopped")) {
        return terminalResult();
    }
    if (!m_impl->runner ||
        (m_impl->snapshot.phase != QStringLiteral("running") &&
         m_impl->snapshot.phase != QStringLiteral("paused") &&
         m_impl->snapshot.phase != QStringLiteral("stopping"))) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("There is no active test to wait for"));
    }
    if (m_impl->waitInProgress) {
        return failure(QStringLiteral("wait_in_progress"),
                       QStringLiteral("A terminal-state wait is already active"));
    }
    m_impl->waitInProgress = true;
    BoolReset waitReset{&m_impl->waitInProgress};
    const quint64 generation = m_impl->generation;

    const int effectiveTimeout = timeoutMs > 0 ? timeoutMs : m_impl->runTimeoutMs;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    const QMetaObject::Connection snapshotConnection = QObject::connect(
        this,
        &TestApplicationController::snapshotChanged,
        &loop,
        [&, generation](const ApplicationSnapshot& snapshot) {
            if (generation != m_impl->generation ||
                snapshot.phase == QStringLiteral("finished") ||
                snapshot.phase == QStringLiteral("error") ||
                snapshot.phase == QStringLiteral("stopped")) {
                loop.quit();
            }
        });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    timer.start(effectiveTimeout);
    loop.exec();
    QObject::disconnect(snapshotConnection);

    if (generation != m_impl->generation) {
        return failure(QStringLiteral("wait_interrupted"),
                       QStringLiteral("Application lifecycle changed while waiting"));
    }
    if (timedOut) {
        QString observedState = m_impl->snapshot.testState;
        if (m_impl->runner) {
            const auto currentState = m_impl->runner->getCurrentState();
            if (currentState.ok()) {
                observedState = hwtest::biz::testStateToString(currentState.value);
            }
        }
        return failure(QStringLiteral("run_timeout"),
                       QStringLiteral("Test did not reach a terminal state within %1 ms (BIZ state: %2)")
                           .arg(effectiveTimeout)
                           .arg(observedState));
    }
    return terminalResult();
}

ActionResult TestApplicationController::shutdown()
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (!m_impl->latchedShutdownFailure.ok && !m_impl->runner && !m_impl->hal &&
        !m_impl->executor) {
        return m_impl->latchedShutdownFailure;
    }
    ++m_impl->generation;
    ActionResult firstFailure;
    const bool configured = !m_impl->testConfigPath.isEmpty() && !m_impl->halConfigPath.isEmpty();
    const QString selectedResource = m_impl->snapshot.controlResourceId;
    const QString selectedProvider = m_impl->snapshot.providerId;
    const QString selectedSerialPort = m_impl->snapshot.serialPortName;

    if (m_impl->runner) {
        const hwtest::biz::Status status = m_impl->runner->shutdown();
        if (!status.ok()) {
            firstFailure = bizFailure(status, QStringLiteral("Unable to shut down BIZ service"));
        }
    }
    m_impl->logService.clearSinks();
    if (m_impl->fileSink) {
        m_impl->fileSink->flush();
    }
    m_impl->fileSink.reset();
    m_impl->runner.reset();
    m_impl->executor.reset();

    if (m_impl->hal) {
        if (!m_impl->sessionId.isEmpty()) {
            const hwtest::hal::HalStatus closed =
                m_impl->hal->closeDevice(m_impl->sessionId, hwtest::hal::OperationOptions{});
            if (!closed.ok() && firstFailure.ok) {
                firstFailure = halFailure(closed, QStringLiteral("Unable to close HAL device"));
            }
        }
        const hwtest::hal::HalStatus shutDown = m_impl->hal->shutdown();
        if (!shutDown.ok() && firstFailure.ok) {
            firstFailure = halFailure(shutDown, QStringLiteral("Unable to shut down HAL"));
        }
    }
    m_impl->hal.reset();
    m_impl->sessionId.clear();
    m_impl->device = nullptr;

    m_impl->snapshot = {};
    if (!firstFailure.ok) {
        m_impl->latchedShutdownFailure = firstFailure;
        m_impl->snapshot.phase = QStringLiteral("shutdown_failed");
        m_impl->snapshot.errorCode = firstFailure.code;
        m_impl->snapshot.message = firstFailure.message;
        m_impl->snapshot.controlResourceId = selectedResource;
        m_impl->snapshot.providerId = selectedProvider;
        m_impl->snapshot.serialPortName = selectedSerialPort;
    } else if (configured) {
        m_impl->latchedShutdownFailure = {};
        m_impl->snapshot.phase = QStringLiteral("configured");
        m_impl->snapshot.controlResourceId = selectedResource;
        m_impl->snapshot.providerId = selectedProvider;
        m_impl->snapshot.serialPortName = selectedSerialPort;
    }
    emit snapshotChanged(m_impl->snapshot);
    return firstFailure;
}

ApplicationSnapshot TestApplicationController::snapshot() const
{
    Q_ASSERT_X(onAffinityThread(this),
               "TestApplicationController::snapshot",
               "must run on the controller affinity thread");
    if (!onAffinityThread(this)) {
        return {};
    }
    return m_impl->snapshot;
}

} // namespace hwtest::app
