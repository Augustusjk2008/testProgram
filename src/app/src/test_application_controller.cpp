#include <app/test_application_controller.h>

#include "continuous_data_recorder.h"
#include "mbddf_algorithm_registry.h"
#include "post_run_analysis_config.h"
#include "post_run_analysis_coordinator.h"
#include "run_mode_capabilities.h"

#include <algorithm/elec_health_status_executor.h>
#include <algorithm/di_stimulus_controller.h>
#include <algorithm/mbddf_exchange_executor.h>
#include <algorithm/mbddf_transport.h>
#include <algorithm/run_parameter_schema.h>
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
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <thread>

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

ActionResult analysisCommandInProgressFailure()
{
    return failure(QStringLiteral("command_in_progress"),
                   QStringLiteral("Post-run analysis must finish or be cancelled before another write action"));
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

QString canonicalDigitalLevel(const QVariant& value)
{
    const QString text = value.toString().trimmed();
    if (text.compare(QStringLiteral("High"), Qt::CaseInsensitive) == 0 ||
        text == QStringLiteral("1")) {
        return QStringLiteral("High");
    }
    if (text.compare(QStringLiteral("Low"), Qt::CaseInsensitive) == 0 ||
        text == QStringLiteral("0")) {
        return QStringLiteral("Low");
    }
    return {};
}

ActionResult validateDigitalStimulusSafeState(const QVariantMap& executionConfig,
                                              const QVariantMap& halConfig)
{
    const QVariantMap stimulus = executionConfig
                                     .value(QStringLiteral("digitalStimulus"))
                                     .toMap();
    const QString deviceId = stimulus.value(QStringLiteral("deviceId"))
                                 .toString().trimmed();
    const QVariantList channels = stimulus.value(QStringLiteral("channels")).toList();
    const QVariantMap resources = halConfig.value(QStringLiteral("hardware")).toMap()
                                      .value(QStringLiteral("resources")).toMap();
    const QVariantMap safeState = halConfig.value(QStringLiteral("safeState")).toMap();
    if (deviceId.isEmpty() || channels.isEmpty()) {
        return failure(QStringLiteral("stimulus_safe_state_mismatch"),
                       QStringLiteral("DI digitalStimulus requires a deviceId and channels"));
    }
    for (const QVariant& value : channels) {
        const QVariantMap channel = value.toMap();
        const QString resourceId = channel.value(QStringLiteral("resourceId"))
                                       .toString().trimmed();
        const QVariantMap resource = resources.value(resourceId).toMap();
        const QString activeLevel = canonicalDigitalLevel(
            channel.value(QStringLiteral("activeLevel")));
        const QString inactiveLevel = activeLevel == QStringLiteral("High")
            ? QStringLiteral("Low")
            : (activeLevel == QStringLiteral("Low")
                   ? QStringLiteral("High")
                   : QString{});
        const QString configuredSafe = canonicalDigitalLevel(
            safeState.value(resourceId));
        if (resourceId.isEmpty() || resource.isEmpty() ||
            resource.value(QStringLiteral("device")).toString().trimmed() != deviceId ||
            resource.value(QStringLiteral("module")).toString().trimmed() !=
                QStringLiteral("digital") ||
            resource.value(QStringLiteral("direction")).toString().trimmed() !=
                QStringLiteral("output") ||
            inactiveLevel.isEmpty() || configuredSafe != inactiveLevel) {
            return failure(
                QStringLiteral("stimulus_safe_state_mismatch"),
                QStringLiteral(
                    "Digital stimulus '%1' must map to an output on '%2' whose HAL safeState equals its inactive level '%3'")
                    .arg(resourceId, deviceId, inactiveLevel));
        }
    }
    return {};
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

QString runParameterKindName(
    hwtest::algorithm::mbddf::RunParameterKind kind)
{
    using Kind = hwtest::algorithm::mbddf::RunParameterKind;
    switch (kind) {
    case Kind::Integer: return QStringLiteral("integer");
    case Kind::Number: return QStringLiteral("number");
    case Kind::Boolean: return QStringLiteral("boolean");
    case Kind::Choice: return QStringLiteral("choice");
    }
    return {};
}

QVariantMap configuredRunParameterDefaults(const hwtest::biz::TestStep& step)
{
    return step.parameters.value(QStringLiteral("protocol")).toMap()
        .value(QStringLiteral("requestValues")).toMap();
}

TestDescriptor makeTestDescriptor(const hwtest::biz::TestConfig& config,
                                  const hwtest::biz::TestStep& step,
                                  const QVector<QString>& supportedRunModes,
                                  const QVariantMap& runParameterDefaults)
{
    TestDescriptor descriptor;
    descriptor.configId = config.configId;
    descriptor.productModel = config.productModel;
    descriptor.productName = config.productName;
    descriptor.configVersion = config.configVersion;
    descriptor.stepId = step.stepId;
    descriptor.testItemId = step.testItemId;
    descriptor.algorithmId = step.algorithmId;

    const QVariantMap presentation = config.reportFields;
    descriptor.title = presentation.value(QStringLiteral("title")).toString().trimmed();
    if (descriptor.title.isEmpty()) {
        descriptor.title = step.name.trimmed();
    }
    if (descriptor.title.isEmpty()) {
        descriptor.title = step.testItemId;
    }
    descriptor.description =
        presentation.value(QStringLiteral("description")).toString().trimmed();

    descriptor.supportedRunModes = supportedRunModes;
    if (const MbdDfAlgorithmRegistration* registration =
            findMbdDfAlgorithm(step.algorithmId);
        registration != nullptr && !registration->postRunAnalyzerId.isEmpty()) {
        descriptor.postRunAnalysis.supported = true;
        descriptor.postRunAnalysis.analyzerId = registration->postRunAnalyzerId;
        descriptor.postRunAnalysis.schemaVersion =
            registration->postRunAnalysisSchemaVersion;
    }

    QSet<QString> seenMeasurements;
    for (const QVariant& measurementValue :
         presentation.value(QStringLiteral("measurements")).toList()) {
        const QVariantMap map = measurementValue.toMap();
        const QString id = map.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty() || seenMeasurements.contains(id)) {
            continue;
        }
        QString label = map.value(QStringLiteral("label")).toString().trimmed();
        if (label.isEmpty()) {
            label = id;
        }
        descriptor.measurements.push_back(TestMeasurementDescriptor{
            id,
            label,
            map.value(QStringLiteral("unit")).toString().trimmed(),
            map.value(QStringLiteral("primary")).toBool(),
        });
        seenMeasurements.insert(id);
    }
    if (descriptor.measurements.isEmpty()) {
        for (const hwtest::biz::Criterion& criterion : step.criteria) {
            const QString id = criterion.metric.trimmed();
            if (!id.isEmpty() && !seenMeasurements.contains(id)) {
                descriptor.measurements.push_back(
                    TestMeasurementDescriptor{id, id, {}, true});
                seenMeasurements.insert(id);
            }
        }
    }

    const auto* schema =
        hwtest::algorithm::mbddf::findRunParameterSchema(step.algorithmId);
    if (schema != nullptr) {
        descriptor.runParameterSchemaVersion = schema->version;
        descriptor.runParameterDefaults = runParameterDefaults;
        for (const auto& parameter : schema->parameters) {
            TestRunParameterDescriptor projected;
            projected.id = parameter.id;
            projected.label = parameter.label;
            projected.description = parameter.description;
            projected.kind = runParameterKindName(parameter.kind);
            projected.unit = parameter.unit;
            projected.required = parameter.required;
            projected.minimum = parameter.minimum;
            projected.maximum = parameter.maximum;
            projected.minimumExclusive = parameter.minimumExclusive;
            projected.maximumExclusive = parameter.maximumExclusive;
            projected.visibleWhenParameter = parameter.visibleWhenParameter;
            projected.visibleWhenEquals = parameter.visibleWhenEquals;
            for (const auto& choice : parameter.choices) {
                projected.choices.push_back(
                    TestRunParameterChoice{choice.value, choice.label});
            }
            descriptor.runParameters.push_back(std::move(projected));
        }
    }
    return descriptor;
}

DigitalStimulusSnapshot digitalStimulusDescriptor(const QVariantMap& executionConfig)
{
    DigitalStimulusSnapshot snapshot;
    const QVariantMap stimulus = executionConfig.value(QStringLiteral("digitalStimulus")).toMap();
    const QVariantList channels = stimulus.value(QStringLiteral("channels")).toList();
    if (channels.isEmpty()) return snapshot;
    snapshot.available = true;
    snapshot.settlingMs = stimulus.value(QStringLiteral("settlingMs"), 0).toInt();
    for (const QVariant& value : channels) {
        const QVariantMap channel = value.toMap();
        snapshot.switches.push_back(DigitalSwitchDescriptor{
            channel.value(QStringLiteral("switchId")).toString().trimmed(),
            channel.value(QStringLiteral("dutBit"), -1).toInt(),
            channel.value(QStringLiteral("label")).toString().trimmed(),
            channel.value(QStringLiteral("activeLevel")).toString().trimmed(),
        });
    }
    return snapshot;
}

DigitalStimulusSnapshot digitalStimulusSnapshot(
    const hwtest::algorithm::mbddf::DiStimulusState& state)
{
    DigitalStimulusSnapshot snapshot;
    snapshot.available = true;
    snapshot.configured = state.configured;
    snapshot.appliedMask = state.appliedMask;
    snapshot.revision = state.revision;
    snapshot.lastWriteTimestampUs = state.lastWriteTimestampUs;
    snapshot.settlingMs = state.settlingMs;
    snapshot.errorCode = state.lastError.ok()
        ? QString{}
        : hwtest::hal::toString(state.lastError.code);
    snapshot.message = state.lastError.error.message;
    for (const auto& channel : state.channels) {
        snapshot.switches.push_back(DigitalSwitchDescriptor{
            channel.switchId,
            channel.dutBit,
            channel.label,
            hwtest::hal::toString(channel.activeLevel),
        });
    }
    return snapshot;
}

bool stimulusActionPhase(const QString& phase)
{
    return phase == QStringLiteral("ready") ||
        phase == QStringLiteral("running") ||
        phase == QStringLiteral("paused") ||
        phase == QStringLiteral("finished") ||
        phase == QStringLiteral("stopped");
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
    QString dataStorageDirectory;
    PostRunAnalysisConfig analysisConfig;
    QVariantMap halConfig;
    QVariantMap executionConfig;
    QVariantMap runParameterDefaults;
    QVector<ControlResource> controls;
    ApplicationSnapshot snapshot;
    int runTimeoutMs = 5000;
    HalServicePtr hal{nullptr, &hwtest::hal::destroyHalService};
    hwtest::hal::SessionId dutSessionId;
    hwtest::hal::SessionId stimulusSessionId;
    hwtest::hal::IHalDevice* device = nullptr;
    hwtest::hal::IHalDevice* stimulusDevice = nullptr;
    std::unique_ptr<hwtest::algorithm::mbddf::DiStimulusController> stimulusController;
    std::unique_ptr<hwtest::biz::IAlgorithmExecutor> executor;
    QString selectedAlgorithmId;
    TestDescriptor descriptor;
    TestServicePtr runner{nullptr, &hwtest::biz::destroyTestRunService};
    hwtest::logging::LogService logService;
    std::unique_ptr<hwtest::logging::JsonLineFileSink> fileSink;
    ContinuousDataRecorder dataRecorder;
    PostRunAnalysisCoordinator analysisCoordinator;
    ActionResult latchedShutdownFailure;
    QString suppressedResultTaskId;
    quint64 generation = 0;
    bool waitInProgress = false;
    bool asyncStopInProgress = false;
    std::thread asyncStopThread;
};

TestApplicationController::TestApplicationController(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    qRegisterMetaType<ActionResult>();
    qRegisterMetaType<ApplicationSample>();
    qRegisterMetaType<ApplicationSnapshot>();
    qRegisterMetaType<SerialPortInfo>();
    qRegisterMetaType<TestDescriptor>();
    qRegisterMetaType<TestMeasurementDescriptor>();
    qRegisterMetaType<TestRunParameterChoice>();
    qRegisterMetaType<TestRunParameterDescriptor>();
    qRegisterMetaType<PostRunAnalysisCapability>();
    qRegisterMetaType<AnalysisMetric>();
    qRegisterMetaType<AnalysisChannelSummary>();
    qRegisterMetaType<PostRunAnalysisSnapshot>();
    qRegisterMetaType<AnalysisResultQuery>();
    qRegisterMetaType<AnalysisNullableNumber>();
    qRegisterMetaType<AnalysisChannelProjection>();
    qRegisterMetaType<TestRunOptions>();
    qRegisterMetaType<DigitalSwitchDescriptor>();
    qRegisterMetaType<DigitalStimulusSnapshot>();
    qRegisterMetaType<QVector<SerialPortInfo>>();
    m_impl->analysisCoordinator.setUpdateCallback([this] {
        m_impl->snapshot.analysis = m_impl->analysisCoordinator.snapshot();
        emit snapshotChanged(m_impl->snapshot);
    });
}

TestApplicationController::~TestApplicationController()
{
    QObject::disconnect(this, nullptr, nullptr, nullptr);
    if (m_impl->asyncStopThread.joinable()) {
        m_impl->asyncStopThread.join();
    }
    m_impl->asyncStopInProgress = false;
    shutdown();
}

ActionResult TestApplicationController::loadConfigurations(const QString& testConfigPath,
                                                            const QString& halConfigPath)
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
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

    int enabledSteps = 0;
    QString selectedAlgorithmId;
    const hwtest::biz::TestStep* selectedStep = nullptr;
    for (const hwtest::biz::TestStep& step : testConfig.value.steps) {
        if (!step.enabled) {
            continue;
        }
        if (!isSupportedMbdDfAlgorithm(step.algorithmId)) {
            return failure(QStringLiteral("unsupported_algorithm"),
                           QStringLiteral("Unsupported MB_DDF algorithm '%1'")
                               .arg(step.algorithmId));
        }
        ++enabledSteps;
        selectedAlgorithmId = step.algorithmId;
        selectedStep = &step;
    }
    if (enabledSteps != 1) {
        return failure(QStringLiteral("test_config"),
                       QStringLiteral("Exactly one enabled MB_DDF step is required"));
    }

    QVector<QString> supportedRunModes;
    const QString runModeError = parseSupportedRunModes(
        testConfig.value.reportFields, &supportedRunModes);
    if (!runModeError.isEmpty()) {
        return failure(QStringLiteral("test_config"), runModeError);
    }

    QVariantMap runParameterDefaults;
    if (hwtest::algorithm::mbddf::findRunParameterSchema(
            selectedAlgorithmId) != nullptr) {
        const auto normalized =
            hwtest::algorithm::mbddf::normalizeRunParameters(
                selectedAlgorithmId,
                configuredRunParameterDefaults(*selectedStep),
                {});
        if (!normalized.ok()) {
            return failure(QStringLiteral("test_config"),
                           normalized.status.error.message);
        }
        runParameterDefaults = normalized.value;
    }

    QVariantMap halConfig;
    const ActionResult loadedHal = loadJsonMap(absoluteHalPath, &halConfig);
    if (!loadedHal.ok) {
        return loadedHal;
    }
    QString dataStorageDirectory = halConfig.value(QStringLiteral("dataStorage"))
                                       .toMap()
                                       .value(QStringLiteral("directory"))
                                       .toString()
                                       .trimmed();
    if (dataStorageDirectory.isEmpty()) {
        dataStorageDirectory = QStringLiteral("../data");
    }
    dataStorageDirectory = resolvedPath(absoluteHalPath, dataStorageDirectory);
    PostRunAnalysisConfig analysisConfig;
    QString analysisConfigError;
    if (!parsePostRunAnalysisConfig(halConfig, &analysisConfig,
                                    &analysisConfigError)) {
        return failure(QStringLiteral("analysis_config"), analysisConfigError);
    }
    if (selectedAlgorithmId == QStringLiteral("mbddf.di_read")) {
        const ActionResult safeState = validateDigitalStimulusSafeState(
            testConfig.value.executionConfig, halConfig);
        if (!safeState.ok) return safeState;
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
    m_impl->dataStorageDirectory = dataStorageDirectory;
    m_impl->analysisConfig = analysisConfig;
    m_impl->selectedAlgorithmId = selectedAlgorithmId;
    m_impl->descriptor = makeTestDescriptor(
        testConfig.value, *selectedStep, supportedRunModes,
        runParameterDefaults);
    m_impl->analysisCoordinator.configureCapability(
        m_impl->descriptor.postRunAnalysis);
    m_impl->halConfig = halConfig;
    m_impl->executionConfig = testConfig.value.executionConfig;
    m_impl->runParameterDefaults = runParameterDefaults;
    m_impl->controls = controls;
    m_impl->runTimeoutMs = timeoutMs;
    m_impl->snapshot = {};
    m_impl->suppressedResultTaskId.clear();
    m_impl->snapshot.phase = QStringLiteral("configured");
    m_impl->snapshot.descriptor = m_impl->descriptor;
    m_impl->snapshot.analysis = m_impl->analysisCoordinator.snapshot();
    m_impl->snapshot.digitalStimulus = digitalStimulusDescriptor(m_impl->executionConfig);
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
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
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
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
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
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
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

    if (m_impl->selectedAlgorithmId == QStringLiteral("mbddf.di_read")) {
        const QString stimulusDeviceId = m_impl->executionConfig
                                             .value(QStringLiteral("digitalStimulus")).toMap()
                                             .value(QStringLiteral("deviceId")).toString().trimmed();
        if (stimulusDeviceId.isEmpty()) {
            const ActionResult result = failure(
                QStringLiteral("hal_config"),
                QStringLiteral("digitalStimulus.deviceId is required for mbddf.di_read"));
            shutdown();
            return result;
        }
        const auto openedStimulus = m_impl->hal->openDevice(
            stimulusDeviceId, hwtest::hal::OperationOptions{});
        if (!openedStimulus.ok()) {
            const ActionResult result = halFailure(
                openedStimulus.status, QStringLiteral("Unable to open digital stimulus device"));
            shutdown();
            return result;
        }
        m_impl->stimulusSessionId = openedStimulus.value;
        const auto stimulusDevice = m_impl->hal->device(m_impl->stimulusSessionId);
        if (!stimulusDevice.ok() || stimulusDevice.value == nullptr) {
            const ActionResult result = stimulusDevice.ok()
                ? failure(QStringLiteral("hal_device"),
                          QStringLiteral("HAL returned a null stimulus device"))
                : halFailure(stimulusDevice.status,
                             QStringLiteral("Unable to get digital stimulus device"));
            shutdown();
            return result;
        }
        m_impl->stimulusDevice = stimulusDevice.value;
        m_impl->stimulusController = std::make_unique<
            hwtest::algorithm::mbddf::DiStimulusController>(m_impl->stimulusDevice);
        hwtest::hal::HalStatus stimulusStatus =
            m_impl->stimulusController->configure(m_impl->executionConfig);
        if (stimulusStatus.ok()) {
            stimulusStatus = m_impl->stimulusController->resetDigitalStimulus();
        }
        if (!stimulusStatus.ok()) {
            const ActionResult result = halFailure(
                stimulusStatus, QStringLiteral("Unable to enter digital stimulus safe state"));
            shutdown();
            return result;
        }
        m_impl->snapshot.digitalStimulus = digitalStimulusSnapshot(
            m_impl->stimulusController->state());
    }

    const QVariantMap control = m_impl->halConfig.value(QStringLiteral("control")).toMap();
    const QString deviceId = control.value(QStringLiteral("deviceId")).toString().trimmed();
    const auto opened = m_impl->hal->openDevice(deviceId, hwtest::hal::OperationOptions{});
    if (!opened.ok()) {
        const ActionResult result = halFailure(opened.status, QStringLiteral("Unable to open HAL device"));
        shutdown();
        return result;
    }
    m_impl->dutSessionId = opened.value;

    const auto device = m_impl->hal->device(m_impl->dutSessionId);
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
    m_impl->executor = createMbdDfExecutor(m_impl->selectedAlgorithmId,
                                           std::move(transport));
    if (!m_impl->executor) {
        const ActionResult result = failure(QStringLiteral("unsupported_algorithm"),
                                            QStringLiteral("Unsupported MB_DDF algorithm '%1'")
                                                .arg(m_impl->selectedAlgorithmId));
        shutdown();
        return result;
    }
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
                     &hwtest::biz::ITestRunService::cycleStarted,
                     this,
                     [this, generation](const hwtest::biz::TaskId& taskId,
                                        quint64 cycleIndex) {
                         if (generation != m_impl->generation ||
                             taskId == m_impl->suppressedResultTaskId) {
                             return;
                         }
                         m_impl->snapshot.taskId = taskId;
                         m_impl->snapshot.cycleIndex = cycleIndex;
                         emit snapshotChanged(m_impl->snapshot);
                     });
    QObject::connect(m_impl->runner.get(),
                     &hwtest::biz::ITestRunService::sampleProduced,
                     this,
                     [this, generation](const hwtest::biz::TaskId& taskId,
                                        const hwtest::biz::StepId& stepId,
                                        const hwtest::biz::RawSample& rawSample) {
                         if (generation != m_impl->generation) {
                             return;
                         }
                         ApplicationSample sample;
                         sample.taskId = taskId;
                         sample.stepId = stepId;
                         sample.channelId = rawSample.channelId;
                         sample.timestampUs = rawSample.timestampUs;
                         sample.cycleIndex = rawSample.cycleIndex;
                         sample.values = rawSample.values;
                         sample.tags = rawSample.tags;
                         sample.streamElapsedUs = rawSample.streamElapsedUs;
                         if (m_impl->dataRecorder.active()) {
                             const ActionResult recorded =
                                 m_impl->dataRecorder.append(sample);
                             if (!recorded.ok &&
                                 m_impl->snapshot.dataSaveError != recorded.message) {
                                 m_impl->snapshot.dataSaveError = recorded.message;
                                 m_impl->snapshot.dataFilePath =
                                     m_impl->dataRecorder.outputPath();
                                 emit snapshotChanged(m_impl->snapshot);
                             }
                         }
                         m_impl->analysisCoordinator.append(sample);
                         if (taskId == m_impl->suppressedResultTaskId) {
                             return;
                         }
                         m_impl->snapshot.cycleIndex = rawSample.cycleIndex;
                         ++m_impl->snapshot.sampleCount;
                         emit sampleReceived(sample);
                     });
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
                         const bool terminal =
                             m_impl->snapshot.phase == QStringLiteral("finished") ||
                             m_impl->snapshot.phase == QStringLiteral("stopped") ||
                             m_impl->snapshot.phase == QStringLiteral("error");
                         if (terminal && m_impl->dataRecorder.active()) {
                             QString finalStatus = QStringLiteral("已完成");
                             if (m_impl->snapshot.phase == QStringLiteral("stopped")) {
                                 finalStatus = QStringLiteral("用户停止");
                             } else if (m_impl->snapshot.phase == QStringLiteral("error")) {
                                 finalStatus = QStringLiteral("错误");
                             }
                             QString finalDetail = m_impl->snapshot.message;
                             if (finalDetail.isEmpty()) {
                                 finalDetail = m_impl->snapshot.phase == QStringLiteral("finished")
                                     ? QStringLiteral("连续测试已完成")
                                     : (m_impl->snapshot.phase == QStringLiteral("stopped")
                                            ? QStringLiteral("连续测试已停止")
                                            : QStringLiteral("连续测试异常结束"));
                             }
                             const ActionResult saved = m_impl->dataRecorder.finish(
                                 finalStatus, finalDetail);
                             m_impl->snapshot.dataFilePath =
                                 m_impl->dataRecorder.outputPath();
                             if (!saved.ok) {
                                 m_impl->snapshot.dataSaveError = saved.message;
                             }
                         }
                         if (terminal &&
                             m_impl->descriptor.postRunAnalysis.supported &&
                             m_impl->snapshot.analysis.taskId == taskId) {
                             hwtest::algorithm::mbddf::AnalysisTermination termination;
                             if (m_impl->snapshot.phase == QStringLiteral("finished")) {
                                 termination.kind = hwtest::algorithm::mbddf::
                                     AnalysisTerminationKind::Finished;
                             } else if (m_impl->snapshot.phase ==
                                        QStringLiteral("error")) {
                                 termination.kind = hwtest::algorithm::mbddf::
                                     AnalysisTerminationKind::Error;
                             } else {
                                 termination.kind = hwtest::algorithm::mbddf::
                                     AnalysisTerminationKind::Stopped;
                             }
                             termination.reasonCode = m_impl->snapshot.errorCode;
                             termination.message = m_impl->snapshot.message;
                             const QString sourceArtifact =
                                 m_impl->snapshot.dataSaveError.isEmpty()
                                 ? m_impl->snapshot.dataFilePath
                                 : QString{};
                             m_impl->analysisCoordinator.requestTerminal(
                                 termination,
                                 m_impl->snapshot.phase == QStringLiteral("stopped"),
                                 sourceArtifact);
                         }
                         emit snapshotChanged(m_impl->snapshot);
                     });
    QObject::connect(m_impl->runner.get(),
                     &hwtest::biz::ITestRunService::resultProduced,
                     this,
                     [this, generation](const hwtest::biz::TaskId& taskId,
                                        const hwtest::biz::TestResult& result) {
                         if (generation != m_impl->generation ||
                             taskId == m_impl->suppressedResultTaskId) {
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
                         m_impl->snapshot.cycleIndex = result.cycleIndex;
                         emit snapshotChanged(m_impl->snapshot);
                     });
    QObject::connect(m_impl->runner.get(),
                     &hwtest::biz::ITestRunService::hardwareError,
                     this,
                     [this, generation](const hwtest::biz::TaskId& taskId,
                                        const hwtest::biz::TestItemId&,
                                        hwtest::biz::ErrorCode code,
                                        const QString& description) {
                         if (generation != m_impl->generation ||
                             taskId == m_impl->suppressedResultTaskId) {
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
    return start(TestRunOptions{});
}

ActionResult TestApplicationController::start(const TestRunOptions& options)
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
    }
    if (!m_impl->runner ||
        (m_impl->snapshot.phase != QStringLiteral("ready") &&
         m_impl->snapshot.phase != QStringLiteral("finished") &&
         m_impl->snapshot.phase != QStringLiteral("stopped"))) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Run is only available after preparation or a finished run"));
    }

    hwtest::biz::RunMode mode = hwtest::biz::RunMode::Single;
    if (!hwtest::biz::runModeFromString(options.mode, &mode)) {
        return failure(QStringLiteral("invalid_run_mode"),
                       QStringLiteral("Unknown run mode '%1'").arg(options.mode));
    }
    const QString runMode = hwtest::biz::runModeToString(mode);
    if (!m_impl->descriptor.supportedRunModes.contains(runMode)) {
        return failure(
            QStringLiteral("CapabilityUnsupported"),
            QStringLiteral("Test configuration '%1' does not support run mode '%2'")
                .arg(m_impl->descriptor.configId, runMode));
    }
    const int effectiveIntervalMs = mode == hwtest::biz::RunMode::PcPeriodic
        ? options.intervalMs
        : 1000;
    const quint64 effectiveMaxCycles = mode == hwtest::biz::RunMode::PcPeriodic
        ? options.maxCycles
        : 1;
    hwtest::biz::RunOptions runOptions;
    runOptions.mode = mode;
    runOptions.intervalMs = effectiveIntervalMs;
    runOptions.maxCycles = effectiveMaxCycles;
    if (mode == hwtest::biz::RunMode::PcPeriodic &&
        (options.intervalMs < 10 || options.intervalMs > 60 * 60 * 1000)) {
        return failure(QStringLiteral("ParameterRangeError"),
                       QStringLiteral("PC periodic interval must be in the range 10..3600000 ms"));
    }
    if (mode == hwtest::biz::RunMode::PcPeriodic &&
        options.maxCycles > 1000000000ULL) {
        return failure(QStringLiteral("ParameterRangeError"),
                       QStringLiteral("PC periodic maxCycles must be 0 or at most 1000000000"));
    }

    const auto normalizedParameters =
        hwtest::algorithm::mbddf::normalizeRunParameters(
            m_impl->selectedAlgorithmId,
            m_impl->runParameterDefaults,
            options.algorithmParameters);
    if (!normalizedParameters.ok()) {
        return bizFailure(normalizedParameters.status,
                          QStringLiteral("Invalid run parameters"));
    }
    runOptions.parameters = normalizedParameters.value;

    if (m_impl->descriptor.postRunAnalysis.supported) {
        PostRunAnalysisStartSpec analysisSpec;
        analysisSpec.algorithmId = m_impl->selectedAlgorithmId;
        analysisSpec.configId = m_impl->descriptor.configId;
        analysisSpec.sourceStepId = m_impl->descriptor.stepId;
        analysisSpec.dataStorageDirectory = m_impl->dataStorageDirectory;
        analysisSpec.effectiveRunParameters = normalizedParameters.value;
        analysisSpec.metadata.insert(QStringLiteral("runMode"), runMode);
        analysisSpec.resources = m_impl->analysisConfig;
        const ActionResult analysisPrepared =
            m_impl->analysisCoordinator.preparePending(analysisSpec);
        if (!analysisPrepared.ok) return analysisPrepared;
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
    m_impl->snapshot.runMode = runMode;
    m_impl->snapshot.intervalMs = effectiveIntervalMs;
    m_impl->snapshot.maxCycles = effectiveMaxCycles;
    m_impl->snapshot.cycleIndex = 0;
    m_impl->snapshot.sampleCount = 0;
    m_impl->snapshot.dataSaveEnabled = false;
    m_impl->snapshot.dataFilePath.clear();
    m_impl->snapshot.dataSaveError.clear();
    m_impl->snapshot.effectiveRunParameters = normalizedParameters.value;
    m_impl->suppressedResultTaskId.clear();
    m_impl->dataRecorder.cancel();
    const bool saveContinuousData = options.saveData &&
        (mode == hwtest::biz::RunMode::PcPeriodic ||
         mode == hwtest::biz::RunMode::DeviceStream);
    if (saveContinuousData) {
        const ActionResult recording = m_impl->dataRecorder.begin(
            m_impl->dataStorageDirectory,
            m_impl->descriptor,
            runMode,
            effectiveIntervalMs,
            effectiveMaxCycles);
        if (!recording.ok) {
            m_impl->analysisCoordinator.discardPrepared();
            m_impl->snapshot.dataSaveError = recording.message;
            emit snapshotChanged(m_impl->snapshot);
            return recording;
        }
        m_impl->snapshot.dataSaveEnabled = true;
        m_impl->snapshot.dataFilePath = m_impl->dataRecorder.outputPath();
    }
    const auto started = m_impl->runner->startTestWithOptions(runOptions);
    if (!started.ok()) {
        m_impl->analysisCoordinator.discardPrepared();
        m_impl->dataRecorder.cancel();
        m_impl->snapshot.dataSaveEnabled = false;
        m_impl->snapshot.dataFilePath.clear();
        return bizFailure(started.status, QStringLiteral("Unable to start test"));
    }
    m_impl->dataRecorder.setTaskId(started.value);
    m_impl->snapshot.taskId = started.value;
    if (m_impl->descriptor.postRunAnalysis.supported) {
        const ActionResult bound =
            m_impl->analysisCoordinator.bindSuccessfulTask(started.value);
        m_impl->snapshot.analysis = m_impl->analysisCoordinator.snapshot();
        if (!bound.ok) {
            m_impl->snapshot.analysis.state = QStringLiteral("unavailable");
            m_impl->snapshot.analysis.reasonCode = bound.code;
            m_impl->snapshot.analysis.message = bound.message;
        }
    }
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
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
    }
    if (m_impl->asyncStopInProgress) {
        return failure(QStringLiteral("stop_in_progress"),
                       QStringLiteral("An asynchronous stop is already active"));
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
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
    }
    if (m_impl->asyncStopInProgress) {
        return failure(QStringLiteral("stop_in_progress"),
                       QStringLiteral("An asynchronous stop is already active"));
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
    if (m_impl->asyncStopInProgress) {
        return failure(QStringLiteral("stop_in_progress"),
                       QStringLiteral("An asynchronous stop is already active"));
    }
    if (!m_impl->runner) {
        return failure(QStringLiteral("invalid_state"), QStringLiteral("Application is not prepared"));
    }
    const QString stoppedTaskId = m_impl->snapshot.taskId;
    m_impl->suppressedResultTaskId = stoppedTaskId;
    QElapsedTimer stopTimer;
    stopTimer.start();
    const hwtest::biz::Status status = m_impl->runner->stopTest(timeoutMs);
    if (!status.ok() && m_impl->suppressedResultTaskId == stoppedTaskId) {
        m_impl->suppressedResultTaskId.clear();
    }
    hwtest::hal::HalStatus safe;
    if (m_impl->stimulusController) {
        safe = m_impl->stimulusController->resetDigitalStimulus();
        m_impl->snapshot.digitalStimulus = digitalStimulusSnapshot(
            m_impl->stimulusController->state());
        emit snapshotChanged(m_impl->snapshot);
    }
    if (!status.ok()) {
        return bizFailure(status, QStringLiteral("Unable to stop test"));
    }
    ActionResult terminal;
    if (m_impl->snapshot.phase != QStringLiteral("stopped") &&
        m_impl->snapshot.phase != QStringLiteral("finished") &&
        m_impl->snapshot.phase != QStringLiteral("error")) {
        const qint64 remaining = static_cast<qint64>(timeoutMs) - stopTimer.elapsed();
        terminal = waitForTerminal(static_cast<int>(std::max<qint64>(1, remaining)));
    }
    if (!terminal.ok) {
        return terminal;
    }
    if (!safe.ok()) {
        return halFailure(
            safe,
            QStringLiteral("Test stopped but digital stimulus could not return to safe state"));
    }
    m_impl->analysisCoordinator.notifyStopCompleted();
    return {};
}

ActionResult TestApplicationController::stopAsync(int timeoutMs)
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (timeoutMs < 0) {
        return failure(QStringLiteral("invalid_timeout"),
                       QStringLiteral("Stop timeout must not be negative"));
    }
    if (!m_impl->runner ||
        (m_impl->snapshot.phase != QStringLiteral("running") &&
         m_impl->snapshot.phase != QStringLiteral("paused"))) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("An active test is required for asynchronous stop"));
    }
    if (m_impl->asyncStopInProgress) {
        return failure(QStringLiteral("stop_in_progress"),
                       QStringLiteral("An asynchronous stop is already active"));
    }
    if (m_impl->asyncStopThread.joinable()) {
        m_impl->asyncStopThread.join();
    }

    hwtest::biz::ITestRunService* const runner = m_impl->runner.get();
    const quint64 generation = m_impl->generation;
    const QString stoppedTaskId = m_impl->snapshot.taskId;
    m_impl->suppressedResultTaskId = stoppedTaskId;
    m_impl->asyncStopInProgress = true;
    try {
        m_impl->asyncStopThread = std::thread(
            [this, runner, timeoutMs, generation, stoppedTaskId] {
                const hwtest::biz::Status status = runner->stopTest(timeoutMs);
                const ActionResult result = status.ok()
                    ? ActionResult{}
                    : bizFailure(status, QStringLiteral("Unable to stop test"));
                QMetaObject::invokeMethod(
                    this,
                    [this, generation, result, stoppedTaskId] {
                        if (m_impl->asyncStopThread.joinable()) {
                            m_impl->asyncStopThread.join();
                        }
                        m_impl->asyncStopInProgress = false;
                        if (generation == m_impl->generation) {
                            ActionResult completion = result;
                            if (!completion.ok &&
                                m_impl->suppressedResultTaskId == stoppedTaskId) {
                                m_impl->suppressedResultTaskId.clear();
                            }
                            if (m_impl->stimulusController) {
                                const hwtest::hal::HalStatus safe =
                                    m_impl->stimulusController->resetDigitalStimulus();
                                m_impl->snapshot.digitalStimulus =
                                    digitalStimulusSnapshot(
                                        m_impl->stimulusController->state());
                                emit snapshotChanged(m_impl->snapshot);
                                if (!safe.ok() && completion.ok) {
                                    completion = halFailure(
                                        safe,
                                        QStringLiteral(
                                            "Test stopped but digital stimulus could not return to safe state"));
                                }
                            }
                            emit stopCompleted(completion);
                            if (completion.ok) {
                                m_impl->analysisCoordinator.notifyStopCompleted();
                            }
                        }
                    },
                    Qt::QueuedConnection);
            });
    } catch (const std::exception& exception) {
        m_impl->asyncStopInProgress = false;
        if (m_impl->suppressedResultTaskId == stoppedTaskId) {
            m_impl->suppressedResultTaskId.clear();
        }
        return failure(QStringLiteral("stop_thread"),
                       QStringLiteral("Unable to start asynchronous stop: %1")
                           .arg(QString::fromLocal8Bit(exception.what())));
    }
    return {};
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

ActionResult TestApplicationController::setDigitalStimulus(const QString& switchId,
                                                            bool active,
                                                            quint64 expectedRevision)
{
    if (!onAffinityThread(this)) return affinityFailure();
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
    }
    if (!stimulusActionPhase(m_impl->snapshot.phase) ||
        !m_impl->stimulusController) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Digital stimulus is only available after DI preparation"));
    }
    const hwtest::hal::HalStatus status =
        m_impl->stimulusController->setDigitalStimulus(
            switchId.trimmed(), active, expectedRevision);
    m_impl->snapshot.digitalStimulus = digitalStimulusSnapshot(
        m_impl->stimulusController->state());
    if (!status.ok()) {
        m_impl->snapshot.digitalStimulus.errorCode = hwtest::hal::toString(status.code);
        m_impl->snapshot.digitalStimulus.message = status.error.message;
    }
    emit snapshotChanged(m_impl->snapshot);
    return status.ok() ? ActionResult{}
                       : halFailure(status, QStringLiteral("Unable to set digital stimulus"));
}

ActionResult TestApplicationController::resetDigitalStimulus()
{
    if (!onAffinityThread(this)) return affinityFailure();
    if (m_impl->analysisCoordinator.blocksWrites()) {
        return analysisCommandInProgressFailure();
    }
    if (!stimulusActionPhase(m_impl->snapshot.phase) ||
        !m_impl->stimulusController) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Digital stimulus is only available after DI preparation"));
    }
    const hwtest::hal::HalStatus status =
        m_impl->stimulusController->resetDigitalStimulus();
    m_impl->snapshot.digitalStimulus = digitalStimulusSnapshot(
        m_impl->stimulusController->state());
    if (!status.ok()) {
        m_impl->snapshot.digitalStimulus.errorCode = hwtest::hal::toString(status.code);
        m_impl->snapshot.digitalStimulus.message = status.error.message;
    }
    emit snapshotChanged(m_impl->snapshot);
    return status.ok() ? ActionResult{}
                       : halFailure(status, QStringLiteral("Unable to reset digital stimulus"));
}

ActionResult TestApplicationController::shutdown()
{
    if (!onAffinityThread(this)) {
        return affinityFailure();
    }
    if (m_impl->asyncStopInProgress) {
        return failure(QStringLiteral("stop_in_progress"),
                       QStringLiteral("Cannot shut down while an asynchronous stop is active"));
    }
    if (!m_impl->latchedShutdownFailure.ok && !m_impl->runner && !m_impl->hal &&
        !m_impl->executor && !m_impl->stimulusController) {
        if (m_impl->latchedShutdownFailure.code ==
            QStringLiteral("analysis_shutdown_timeout")) {
            const ActionResult retried = m_impl->analysisCoordinator.cancelAndWait(
                m_impl->analysisConfig.analysisShutdownTimeoutMs);
            if (!retried.ok) {
                m_impl->latchedShutdownFailure = retried;
                m_impl->snapshot.errorCode = retried.code;
                m_impl->snapshot.message = retried.message;
                emit snapshotChanged(m_impl->snapshot);
                return retried;
            }
            m_impl->latchedShutdownFailure = {};
            m_impl->snapshot.phase = m_impl->testConfigPath.isEmpty()
                ? QStringLiteral("empty")
                : QStringLiteral("configured");
            m_impl->snapshot.errorCode.clear();
            m_impl->snapshot.message.clear();
            m_impl->analysisCoordinator.configureCapability(
                m_impl->descriptor.postRunAnalysis);
            m_impl->snapshot.analysis = m_impl->analysisCoordinator.snapshot();
            emit snapshotChanged(m_impl->snapshot);
            return {};
        }
        return m_impl->latchedShutdownFailure;
    }
    ++m_impl->generation;
    m_impl->suppressedResultTaskId.clear();
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
    if (m_impl->dataRecorder.active()) {
        const ActionResult saved = m_impl->dataRecorder.finish(
            QStringLiteral("关闭"),
            firstFailure.ok ? QStringLiteral("应用关闭时结束连续测试")
                            : firstFailure.message);
        if (!saved.ok && firstFailure.ok) {
            firstFailure = saved;
        }
    }
    const ActionResult analysisShutdown =
        m_impl->analysisCoordinator.cancelAndWait(
            m_impl->analysisConfig.analysisShutdownTimeoutMs);
    if (!analysisShutdown.ok && firstFailure.ok) {
        firstFailure = analysisShutdown;
    }
    m_impl->runner.reset();
    m_impl->executor.reset();

    if (m_impl->hal) {
        if (m_impl->stimulusController &&
            m_impl->stimulusController->state().configured) {
            const hwtest::hal::HalStatus safe =
                m_impl->stimulusController->resetDigitalStimulus();
            if (!safe.ok() && firstFailure.ok) {
                firstFailure = halFailure(
                    safe, QStringLiteral("Unable to reset digital stimulus during shutdown"));
            }
        }
        m_impl->stimulusController.reset();
        m_impl->stimulusDevice = nullptr;
        if (!m_impl->stimulusSessionId.isEmpty()) {
            const hwtest::hal::HalStatus closed =
                m_impl->hal->closeDevice(m_impl->stimulusSessionId,
                                         hwtest::hal::OperationOptions{});
            if (!closed.ok() && firstFailure.ok) {
                firstFailure = halFailure(closed,
                                          QStringLiteral("Unable to close stimulus device"));
            }
        }
        if (!m_impl->dutSessionId.isEmpty()) {
            const hwtest::hal::HalStatus closed =
                m_impl->hal->closeDevice(m_impl->dutSessionId,
                                         hwtest::hal::OperationOptions{});
            if (!closed.ok() && firstFailure.ok) {
                firstFailure = halFailure(closed, QStringLiteral("Unable to close DUT device"));
            }
        }
        const hwtest::hal::HalStatus shutDown = m_impl->hal->shutdown();
        if (!shutDown.ok() && firstFailure.ok) {
            firstFailure = halFailure(shutDown, QStringLiteral("Unable to shut down HAL"));
        }
    }
    m_impl->logService.clearSinks();
    if (m_impl->fileSink) {
        m_impl->fileSink->flush();
    }
    m_impl->fileSink.reset();
    m_impl->hal.reset();
    m_impl->dutSessionId.clear();
    m_impl->stimulusSessionId.clear();
    m_impl->device = nullptr;

    m_impl->snapshot = {};
    if (configured) {
        m_impl->snapshot.descriptor = m_impl->descriptor;
        m_impl->analysisCoordinator.configureCapability(
            m_impl->descriptor.postRunAnalysis);
        m_impl->snapshot.analysis = m_impl->analysisCoordinator.snapshot();
        m_impl->snapshot.digitalStimulus = digitalStimulusDescriptor(
            m_impl->executionConfig);
    }
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

ActionResult TestApplicationController::analysisResult(
    const AnalysisResultQuery& query,
    AnalysisChannelProjection* output) const
{
    if (!onAffinityThread(this)) return affinityFailure();
    if (output == nullptr || query.taskId.trimmed().isEmpty() ||
        query.analysisGeneration == 0 || query.channel < 0 || query.channel > 3) {
        return failure(QStringLiteral("invalid_analysis_query"),
                       QStringLiteral("A valid analysis identity and channel are required"));
    }
    const PostRunAnalysisSnapshot& analysis = m_impl->snapshot.analysis;
    if (analysis.taskId != query.taskId ||
        analysis.analysisGeneration != query.analysisGeneration) {
        return failure(QStringLiteral("stale_analysis_result"),
                       QStringLiteral("The requested analysis identity is not current"));
    }
    if (analysis.state != QStringLiteral("completed") &&
        analysis.state != QStringLiteral("partial")) {
        return failure(QStringLiteral("analysis_not_ready"),
                       QStringLiteral("The analysis result is not ready"));
    }
    const QVector<AnalysisChannelProjection> projections =
        m_impl->analysisCoordinator.projections();
    const auto projection = std::find_if(
        projections.cbegin(),
        projections.cend(),
        [&query](const AnalysisChannelProjection& item) {
            return item.channelSummary.channel == query.channel;
        });
    if (projection == projections.cend()) {
        return failure(QStringLiteral("analysis_not_ready"),
                       QStringLiteral("The requested channel result is unavailable"));
    }
    *output = *projection;
    return {};
}

} // namespace hwtest::app
