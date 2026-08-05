#include <app/test_application_controller.h>
#include <app/tui_shell.h>

#include "support/mbddf_udp_test_peer.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <thread>

namespace hwtest::app {
namespace {

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const QByteArray& value)
        : m_name(name)
        , m_previous(qgetenv(name))
        , m_existed(qEnvironmentVariableIsSet(name))
    {
        qputenv(m_name.constData(), value);
    }

    ~ScopedEnvironmentVariable()
    {
        if (m_existed) {
            qputenv(m_name.constData(), m_previous);
        } else {
            qunsetenv(m_name.constData());
        }
    }

private:
    QByteArray m_name;
    QByteArray m_previous;
    bool m_existed = false;
};

QCoreApplication& ensureQtApplication()
{
    if (QCoreApplication* existing = QCoreApplication::instance()) {
        return *existing;
    }
    static int argc = 1;
    static char argument[] = "hwtest_app_tests";
    static char* argv[] = {argument, nullptr};
    static QCoreApplication application(argc, argv);
    return application;
}

bool selectDigitalAdapterFixture(const QString& halPath, QString* error)
{
    QFile source(halPath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = source.errorString();
        return false;
    }
    QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    source.close();
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("HAL fixture is not a JSON object");
        return false;
    }
    QJsonObject root = document.object();
    QJsonObject adapters = root.value(QStringLiteral("adapters")).toObject();
    QJsonObject ni = adapters.value(QStringLiteral("ni.daqmx")).toObject();
    ni.insert(QStringLiteral("libraryPath"),
              QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH));
    adapters.insert(QStringLiteral("ni.daqmx"), ni);
    root.insert(QStringLiteral("adapters"), adapters);
    QJsonObject hardware = root.value(QStringLiteral("hardware")).toObject();
    QJsonArray devices = hardware.value(QStringLiteral("devices")).toArray();
    for (int index = 0; index < devices.size(); ++index) {
        QJsonObject device = devices.at(index).toObject();
        if (device.value(QStringLiteral("alias")).toString() !=
            QStringLiteral("ni6259_stimulus")) {
            continue;
        }
        QJsonObject properties = device.value(QStringLiteral("properties")).toObject();
        QJsonObject vendor = properties.value(QStringLiteral("vendor")).toObject();
        QJsonObject niProperties = vendor.value(QStringLiteral("ni")).toObject();
        niProperties.insert(QStringLiteral("deviceName"), QStringLiteral("fixture_device"));
        vendor.insert(QStringLiteral("ni"), niProperties);
        properties.insert(QStringLiteral("vendor"), vendor);
        device.insert(QStringLiteral("properties"), properties);
        devices.replace(index, device);
        break;
    }
    hardware.insert(QStringLiteral("devices"), devices);
    root.insert(QStringLiteral("hardware"), hardware);
    QJsonObject safeState = root.value(QStringLiteral("safeState")).toObject();
    safeState.remove(QStringLiteral("PXI_AO_0"));
    root.insert(QStringLiteral("safeState"), safeState);

    QFile output(halPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson();
    const bool written = output.write(json) == json.size();
    if (!written && error != nullptr) *error = output.errorString();
    return written;
}

bool setDataStorageDirectory(const QString& halPath,
                             const QString& directory,
                             QString* error)
{
    QFile source(halPath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = source.errorString();
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    source.close();
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("HAL fixture is not a JSON object");
        return false;
    }

    QJsonObject root = document.object();
    root.insert(QStringLiteral("dataStorage"),
                QJsonObject{{QStringLiteral("directory"), directory}});
    QFile output(halPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson();
    const bool written = output.write(json) == json.size();
    if (!written && error != nullptr) *error = output.errorString();
    return written;
}

bool writeHalConfigWithLogPath(const QString& sourcePath,
                               const QString& outputPath,
                               const QString& logPath,
                               const QString& fileMode,
                               QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = source.errorString();
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    source.close();
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("HAL fixture is not a JSON object");
        return false;
    }

    QJsonObject root = document.object();
    QJsonObject logging{{QStringLiteral("filePath"), logPath}};
    if (!fileMode.isEmpty()) {
        logging.insert(QStringLiteral("fileMode"), fileMode);
    }
    root.insert(QStringLiteral("logging"), logging);
    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson();
    const bool written = output.write(json) == json.size();
    if (!written && error != nullptr) *error = output.errorString();
    return written;
}

bool setAnalysisConfiguration(const QString& sourcePath,
                              const QString& outputPath,
                              const QJsonObject& analysis,
                              QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = source.errorString();
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    source.close();
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("HAL fixture is not a JSON object");
        return false;
    }

    QJsonObject root = document.object();
    QJsonObject dataStorage = root.value(QStringLiteral("dataStorage")).toObject();
    dataStorage.insert(QStringLiteral("analysis"), analysis);
    root.insert(QStringLiteral("dataStorage"), dataStorage);

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson();
    const bool written = output.write(json) == json.size();
    if (!written && error != nullptr) *error = output.errorString();
    return written;
}

bool writeTestConfigWithSupportedRunModes(const QString& sourcePath,
                                          const QString& outputPath,
                                          const QStringList& modes,
                                          bool removeField,
                                          QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = source.errorString();
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    source.close();
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("Test fixture is not a JSON object");
        return false;
    }

    QJsonObject root = document.object();
    QJsonObject reportFields = root.value(QStringLiteral("reportFields")).toObject();
    if (removeField) {
        reportFields.remove(QStringLiteral("supportedRunModes"));
    } else {
        QJsonArray runModes;
        for (const QString& mode : modes) {
            runModes.push_back(mode);
        }
        reportFields.insert(QStringLiteral("supportedRunModes"), runModes);
    }
    root.insert(QStringLiteral("reportFields"), reportFields);

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson();
    const bool written = output.write(json) == json.size();
    if (!written && error != nullptr) *error = output.errorString();
    return written;
}

bool writeTestConfigWithStoppable(const QString& sourcePath,
                                  const QString& outputPath,
                                  const QJsonValue& stoppable,
                                  QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = source.errorString();
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    source.close();
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("Test fixture is not a JSON object");
        return false;
    }

    QJsonObject root = document.object();
    QJsonObject reportFields = root.value(QStringLiteral("reportFields")).toObject();
    reportFields.insert(QStringLiteral("stoppable"), stoppable);
    root.insert(QStringLiteral("reportFields"), reportFields);
    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson();
    const bool written = output.write(json) == json.size();
    if (!written && error != nullptr) *error = output.errorString();
    return written;
}

QVariantMap helmOneSampleFeedback(quint64 timestampUs,
                                  quint16 serialA,
                                  quint16 serialB,
                                  double command,
                                  double feedback)
{
    QVariantMap values{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("sample_count"), 1},
        {QStringLiteral("first_timestamp_us_low"),
         static_cast<quint32>(timestampUs & 0xFFFFFFFFULL)},
        {QStringLiteral("first_timestamp_us_high"),
         static_cast<quint32>(timestampUs >> 32)},
        {QStringLiteral("sample[0].delta_us"), 0},
        {QStringLiteral("sample[0].serial_b"), serialB},
        {QStringLiteral("sample[0].version"), 0x4000},
        {QStringLiteral("sample[0].self_check"), 0},
        {QStringLiteral("sample[0].self_check_1"), 0},
        {QStringLiteral("sample[0].self_check_2"), 0},
        {QStringLiteral("sample[0].self_check_3"), 0},
        {QStringLiteral("sample[0].self_check_4"), 0},
        {QStringLiteral("sample[0].self_check_combined"), 0},
        {QStringLiteral("sample[0].timeout"), 0},
        {QStringLiteral("sample[0].serial_a"), serialA},
    };
    for (int channel = 0; channel < 4; ++channel) {
        values.insert(QStringLiteral("sample[0].fdb[%1]").arg(channel),
                      feedback + channel * 0.01);
        values.insert(QStringLiteral("sample[0].ins[%1]").arg(channel),
                      command + channel * 0.01);
    }
    return values;
}

TEST(TestApplicationControllerTest, RejectsPreparationBeforeConfigurationsAreLoaded)
{
    TestApplicationController controller;

    const ActionResult result = controller.prepare();

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("invalid_state"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(TestApplicationControllerTest, UsesOneProcessScopedLogFileAcrossReconnects)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString halConfigPath = directory.filePath(QStringLiteral("hal.json"));
    QString error;
    ASSERT_TRUE(writeHalConfigWithLogPath(
        QStringLiteral(HWTEST_APP_HAL_CONFIG),
        halConfigPath,
        QStringLiteral("logs/backend.jsonl"),
        QStringLiteral("per_process"),
        &error)) << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_TEST_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    ASSERT_TRUE(controller.shutdown().ok);

    const QDir logDirectory(directory.filePath(QStringLiteral("logs")));
    const QStringList firstSessionFiles = logDirectory.entryList(
        {QStringLiteral("backend_*.jsonl")}, QDir::Files, QDir::Name);
    ASSERT_EQ(firstSessionFiles.size(), 1);
    EXPECT_FALSE(QFileInfo(logDirectory.filePath(
        QStringLiteral("backend.jsonl"))).exists());
    EXPECT_TRUE(firstSessionFiles.first().contains(
        QStringLiteral("_p%1").arg(QCoreApplication::applicationPid())));
    EXPECT_TRUE(QRegularExpression(
        QStringLiteral("^backend_[0-9]{8}_[0-9]{6}_[0-9]{3}_p[0-9]+_"
                       "[0-9a-fA-F-]{36}\\.jsonl$"))
                    .match(firstSessionFiles.first()).hasMatch());

    const QString sessionPath = logDirectory.filePath(firstSessionFiles.first());
    QFile sentinel(sessionPath);
    ASSERT_TRUE(sentinel.open(QIODevice::WriteOnly | QIODevice::Append));
    ASSERT_EQ(sentinel.write("{\"sentinel\":true}\n"), 18);
    sentinel.close();

    ASSERT_TRUE(controller.prepare().ok);
    ASSERT_TRUE(controller.shutdown().ok);

    const QStringList secondSessionFiles = logDirectory.entryList(
        {QStringLiteral("backend_*.jsonl")}, QDir::Files, QDir::Name);
    ASSERT_EQ(secondSessionFiles.size(), 1);
    EXPECT_EQ(secondSessionFiles.first(), firstSessionFiles.first());

    QFile reopened(sessionPath);
    ASSERT_TRUE(reopened.open(QIODevice::ReadOnly));
    EXPECT_TRUE(reopened.readAll().contains("{\"sentinel\":true}\n"));

    TestApplicationController secondController;
    ASSERT_TRUE(secondController.loadConfigurations(
        QStringLiteral(HWTEST_APP_TEST_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(secondController.prepare().ok);
    ASSERT_TRUE(secondController.shutdown().ok);
    const QStringList sameProcessFiles = logDirectory.entryList(
        {QStringLiteral("backend_*.jsonl")}, QDir::Files, QDir::Name);
    ASSERT_EQ(sameProcessFiles.size(), 1);
    EXPECT_EQ(sameProcessFiles.first(), firstSessionFiles.first());
}

TEST(TestApplicationControllerTest, DefaultsMissingRunModeCapabilitiesToSingleOnly)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString testConfigPath = directory.filePath(
        QStringLiteral("missing-run-modes.testcfg.json"));
    ASSERT_TRUE(writeTestConfigWithSupportedRunModes(
        QStringLiteral(HWTEST_APP_TEST_CONFIG),
        testConfigPath,
        {},
        true,
        &error)) << error.toStdString();

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        testConfigPath, QStringLiteral(HWTEST_APP_HAL_CONFIG));

    ASSERT_TRUE(loaded.ok) << loaded.message.toStdString();
    EXPECT_EQ(controller.snapshot().descriptor.supportedRunModes,
              QVector<QString>{QStringLiteral("single")});
    EXPECT_TRUE(controller.snapshot().descriptor.stoppable);
}

TEST(TestApplicationControllerTest, RejectsMutuallyExclusiveContinuousCapabilities)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString testConfigPath = directory.filePath(
        QStringLiteral("conflicting-run-modes.testcfg.json"));
    ASSERT_TRUE(writeTestConfigWithSupportedRunModes(
        QStringLiteral(HWTEST_APP_TEST_CONFIG),
        testConfigPath,
        {QStringLiteral("single"),
         QStringLiteral("pc_periodic"),
         QStringLiteral("device_stream")},
        false,
        &error)) << error.toStdString();

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        testConfigPath, QStringLiteral(HWTEST_APP_HAL_CONFIG));

    EXPECT_FALSE(loaded.ok);
    EXPECT_EQ(loaded.code, QStringLiteral("test_config"));
    EXPECT_TRUE(loaded.message.contains(QStringLiteral("pc_periodic")));
    EXPECT_TRUE(loaded.message.contains(QStringLiteral("device_stream")));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(TestApplicationControllerTest, RejectsRunModeNotDeclaredByConfiguration)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString testConfigPath = directory.filePath(
        QStringLiteral("device-stream-only.testcfg.json"));
    ASSERT_TRUE(writeTestConfigWithSupportedRunModes(
        QStringLiteral(HWTEST_APP_TEST_CONFIG),
        testConfigPath,
        {QStringLiteral("single"), QStringLiteral("device_stream")},
        false,
        &error)) << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        testConfigPath, QStringLiteral(HWTEST_APP_HAL_CONFIG)).ok);
    ASSERT_TRUE(controller.prepare().ok);
    TestRunOptions options;
    options.mode = QStringLiteral("pc_periodic");
    options.intervalMs = 10;
    options.maxCycles = 1;

    const ActionResult started = controller.start(options);

    EXPECT_FALSE(started.ok);
    EXPECT_EQ(started.code, QStringLiteral("CapabilityUnsupported"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    if (started.ok) {
        controller.stop(1000);
    }
    controller.shutdown();
}

TEST(TestApplicationControllerTest, LoadsDiSwitchDescriptorsWithoutOpeningHardware)
{
    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_DI_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(loaded.ok) << loaded.message.toStdString();
    const DigitalStimulusSnapshot stimulus = controller.snapshot().digitalStimulus;
    EXPECT_TRUE(stimulus.available);
    EXPECT_FALSE(stimulus.configured);
    ASSERT_EQ(stimulus.switches.size(), 3);
    EXPECT_EQ(stimulus.switches.at(0).switchId, QStringLiteral("di3"));
    EXPECT_EQ(stimulus.switches.at(0).dutBit, 3);
    EXPECT_EQ(stimulus.switches.at(0).label,
              QStringLiteral("DI3 锁相环锁定指示"));
    EXPECT_EQ(stimulus.switches.at(1).switchId, QStringLiteral("di1"));
    EXPECT_EQ(stimulus.switches.at(1).label, QStringLiteral("DI1 引信报警"));
    EXPECT_EQ(stimulus.switches.at(2).switchId, QStringLiteral("di2"));
    EXPECT_EQ(stimulus.switches.at(2).label,
              QStringLiteral("DI2 引信起爆指令"));

    const TestDescriptor descriptor = controller.snapshot().descriptor;
    ASSERT_EQ(descriptor.measurements.size(), 4);
    EXPECT_FALSE(descriptor.measurements.at(2).taskVisible);
    EXPECT_FALSE(descriptor.measurements.at(3).taskVisible);
    ASSERT_EQ(descriptor.taskMeasurements.size(), 16);
    EXPECT_EQ(descriptor.taskMeasurements.first().id, QStringLiteral("di0"));
    EXPECT_EQ(descriptor.taskMeasurements.first().label,
              QStringLiteral("DI0 联锁、电气弹动"));
    EXPECT_EQ(descriptor.taskMeasurements.first().sourceId,
              QStringLiteral("di_state[0]"));
    EXPECT_EQ(descriptor.taskMeasurements.first().bitIndex, 0);
    EXPECT_EQ(descriptor.taskMeasurements.at(8).label,
              QStringLiteral("DI8 投放允许"));
    EXPECT_EQ(descriptor.taskMeasurements.last().bitIndex, 15);

    const ActionResult rejected = controller.setDigitalStimulus(
        QStringLiteral("di3"), true, 0);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.code, QStringLiteral("invalid_state"));
}

TEST(TestApplicationControllerTest, DiPreparationOpensStimulusAndUsesRevisionedActions)
{
    QFile source(QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    ASSERT_TRUE(document.isObject());
    QJsonObject root = document.object();
    QJsonObject adapters = root.value(QStringLiteral("adapters")).toObject();
    QJsonObject ni = adapters.value(QStringLiteral("ni.daqmx")).toObject();
    ni.insert(QStringLiteral("libraryPath"),
              QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH));
    adapters.insert(QStringLiteral("ni.daqmx"), ni);
    root.insert(QStringLiteral("adapters"), adapters);
    QJsonObject hardware = root.value(QStringLiteral("hardware")).toObject();
    QJsonArray devices = hardware.value(QStringLiteral("devices")).toArray();
    for (int index = 0; index < devices.size(); ++index) {
        QJsonObject device = devices.at(index).toObject();
        if (device.value(QStringLiteral("alias")).toString() !=
            QStringLiteral("ni6259_stimulus")) {
            continue;
        }
        QJsonObject properties = device.value(QStringLiteral("properties")).toObject();
        QJsonObject vendor = properties.value(QStringLiteral("vendor")).toObject();
        QJsonObject niProperties = vendor.value(QStringLiteral("ni")).toObject();
        niProperties.insert(QStringLiteral("deviceName"), QStringLiteral("fixture_device"));
        vendor.insert(QStringLiteral("ni"), niProperties);
        properties.insert(QStringLiteral("vendor"), vendor);
        device.insert(QStringLiteral("properties"), properties);
        devices.replace(index, device);
        break;
    }
    hardware.insert(QStringLiteral("devices"), devices);
    root.insert(QStringLiteral("hardware"), hardware);
    QJsonObject safeState = root.value(QStringLiteral("safeState")).toObject();
    safeState.remove(QStringLiteral("PXI_AO_0"));
    root.insert(QStringLiteral("safeState"), safeState);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString halPath = directory.filePath(QStringLiteral("di-hal.json"));
    QFile output(halPath);
    ASSERT_TRUE(output.open(QIODevice::WriteOnly));
    ASSERT_GT(output.write(QJsonDocument(root).toJson()), 0);
    output.close();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_DI_CONFIG), halPath).ok);
    const ActionResult prepared = controller.prepare();
    ASSERT_TRUE(prepared.ok) << prepared.code.toStdString() << ": "
                             << prepared.message.toStdString();
    DigitalStimulusSnapshot stimulus = controller.snapshot().digitalStimulus;
    EXPECT_TRUE(stimulus.configured);
    EXPECT_EQ(stimulus.appliedMask, 0u);
    EXPECT_EQ(stimulus.revision, 1u);

    const ActionResult set = controller.setDigitalStimulus(
        QStringLiteral("di3"), true, stimulus.revision);
    ASSERT_TRUE(set.ok) << set.message.toStdString();
    stimulus = controller.snapshot().digitalStimulus;
    EXPECT_EQ(stimulus.appliedMask, 1u << 3);
    EXPECT_EQ(stimulus.revision, 2u);

    const ActionResult stale = controller.setDigitalStimulus(
        QStringLiteral("di1"), true, 1);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(stale.code, QStringLiteral("DataMismatch"));
    EXPECT_EQ(controller.snapshot().digitalStimulus.appliedMask, 1u << 3);

    ASSERT_TRUE(controller.resetDigitalStimulus().ok);
    EXPECT_EQ(controller.snapshot().digitalStimulus.appliedMask, 0u);
    EXPECT_EQ(controller.snapshot().digitalStimulus.revision, 3u);
    const ActionResult shutdown = controller.shutdown();
    ASSERT_TRUE(shutdown.ok) << shutdown.code.toStdString() << ": "
                             << shutdown.message.toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
}

TEST(TestApplicationControllerTest, AsyncStopReturnsDiStimulusToSafeState)
{
    ensureQtApplication();
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QString error;
    ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString halPath;
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halPath,
                                    &error))
        << error.toStdString();

    ASSERT_TRUE(selectDigitalAdapterFixture(halPath, &error))
        << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_DI_CONFIG), halPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    const quint64 revision = controller.snapshot().digitalStimulus.revision;
    ASSERT_TRUE(controller.setDigitalStimulus(
        QStringLiteral("di3"), true, revision).ok);
    ASSERT_EQ(controller.snapshot().digitalStimulus.appliedMask, 1u << 3);
    ASSERT_TRUE(controller.start().ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    ActionResult completion;
    bool completed = false;
    QObject::connect(&controller,
                     &TestApplicationController::stopCompleted,
                     &loop,
                     [&](const ActionResult& result) {
                         completion = result;
                         completed = true;
                         loop.quit();
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    ASSERT_TRUE(controller.stopAsync(5000).ok);
    timeout.start(5000);
    loop.exec();

    ASSERT_TRUE(completed);
    ASSERT_TRUE(completion.ok) << completion.message.toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("stopped"));
    EXPECT_EQ(controller.snapshot().digitalStimulus.appliedMask, 0u);
    EXPECT_EQ(controller.snapshot().digitalStimulus.revision, revision + 2);
    const ActionResult shutdown = controller.shutdown();
    EXPECT_TRUE(shutdown.ok) << shutdown.code.toStdString() << ": "
                             << shutdown.message.toStdString();
}

TEST(TestApplicationControllerTest, LoadsAndSelectsConfiguredControlResourcesWithoutOpeningHardware)
{
    TestApplicationController controller;

    const ActionResult loaded = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_TEST_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG));

    ASSERT_TRUE(loaded.ok) << loaded.message.toStdString();
    const QVector<ControlResource> controls = controller.availableControls();
    ASSERT_EQ(controls.size(), 2);
    EXPECT_EQ(controls.at(0).resourceId, QStringLiteral("CONTROL_NETWORK"));
    EXPECT_EQ(controls.at(0).providerId, QStringLiteral("qt.udp"));
    EXPECT_EQ(controls.at(1).resourceId, QStringLiteral("CONTROL_SERIAL"));
    EXPECT_EQ(controls.at(1).providerId, QStringLiteral("qt.serial"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
    EXPECT_EQ(controller.snapshot().controlResourceId, QStringLiteral("CONTROL_SERIAL"));

    const ActionResult selected = controller.selectControl(QStringLiteral("CONTROL_NETWORK"));

    ASSERT_TRUE(selected.ok) << selected.message.toStdString();
    EXPECT_EQ(controller.snapshot().controlResourceId, QStringLiteral("CONTROL_NETWORK"));
    EXPECT_EQ(controller.snapshot().providerId, QStringLiteral("qt.udp"));
}

TEST(TestApplicationControllerTest, LoadsAndPreparesElectricalHealthConfiguration)
{
    ensureQtApplication();
    const QString assets = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_ELEC_HEALTH_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(loaded.ok) << loaded.code.toStdString() << ": "
                           << loaded.message.toStdString();
    ASSERT_TRUE(controller.selectControl(QStringLiteral("CONTROL_NETWORK")).ok);
    ASSERT_TRUE(controller.prepare().ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, ProjectsAlgorithmOwnedDhRunParameters)
{
    ensureQtApplication();
    const QString projectDirectory = QStringLiteral(HWTEST_PROJECT_SOURCE_DIR);
    const QString dhConfig = QDir(projectDirectory).filePath(
        QStringLiteral("configs/mbddf_dh_pulse_config.testcfg.json"));

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        dhConfig, QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(loaded.ok) << loaded.message.toStdString();

    const TestDescriptor& descriptor = controller.snapshot().descriptor;
    EXPECT_EQ(descriptor.algorithmId, QStringLiteral("mbddf.dh_pulse_config"));
    EXPECT_EQ(descriptor.runParameterSchemaVersion, QStringLiteral("1"));
    ASSERT_EQ(descriptor.runParameters.size(), 24);
    EXPECT_EQ(descriptor.runParameters.first().id,
              QStringLiteral("config_enable"));
    EXPECT_EQ(descriptor.runParameters.at(1).id,
              QStringLiteral("pulse_width[0]"));
    EXPECT_EQ(descriptor.runParameters.at(1).unit, QStringLiteral("ms"));
    EXPECT_EQ(descriptor.runParameterDefaults.value(
                  QStringLiteral("pulse_width[0]")).toInt(),
              80);
    EXPECT_EQ(descriptor.runParameterDefaults.value(
                  QStringLiteral("pulse_width[22]")).toInt(),
              63);
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, RejectsUnknownRuntimeParameterBeforeStartingDh)
{
    ensureQtApplication();
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString error;
    QString halConfigPath;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halConfigPath,
                                    &error))
        << error.toStdString();

    const QString dhConfig = QDir(QStringLiteral(HWTEST_PROJECT_SOURCE_DIR))
        .filePath(QStringLiteral("configs/mbddf_dh_pulse_config.testcfg.json"));
    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(dhConfig, halConfigPath).ok);
    ASSERT_TRUE(controller.selectControl(QStringLiteral("CONTROL_NETWORK")).ok);
    ASSERT_TRUE(controller.prepare().ok);

    TestRunOptions options;
    options.algorithmParameters.insert(QStringLiteral("mechanical_limit"), 20.5);
    const ActionResult started = controller.start(options);
    EXPECT_FALSE(started.ok);
    EXPECT_EQ(started.code, QStringLiteral("ParameterRangeError"));
    EXPECT_NE(started.message.indexOf(QStringLiteral("mechanical_limit")), -1);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, HelmSuccessfulStartCreatesIndependentAnalysisIdentity)
{
    ensureQtApplication();
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString error;
    QString halConfigPath;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halConfigPath,
                                    &error))
        << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_HELM_STREAM_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    TestRunOptions options;
    options.mode = QStringLiteral("device_stream");
    ASSERT_TRUE(controller.start(options).ok);

    const ApplicationSnapshot started = controller.snapshot();
    EXPECT_FALSE(started.taskId.isEmpty());
    EXPECT_EQ(started.analysis.taskId, started.taskId);
    EXPECT_EQ(started.analysis.analysisGeneration, 1u);
    EXPECT_EQ(started.analysis.state, QStringLiteral("capturing"));
    EXPECT_EQ(started.analysis.progress, 0);

    (void)controller.stop(5000);
    (void)controller.shutdown();
}

TEST(TestApplicationControllerTest, RunsElectricalHealthThroughTheSelectedUdpControlResource)
{
    ensureQtApplication();
    const QString assets = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QString peerError;
    ASSERT_TRUE(peer.bind(&peerError)) << peerError.toStdString();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString halConfigPath;
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halConfigPath,
                                    &peerError))
        << peerError.toStdString();

    TestApplicationController controller;
    QVector<ApplicationSample> samples;
    QObject::connect(&controller,
                     &TestApplicationController::sampleReceived,
                     &controller,
                     [&](const ApplicationSample& sample) { samples.push_back(sample); });
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_ELEC_HEALTH_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    ASSERT_TRUE(controller.start().ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(
                    QStringLiteral("elec_health_status_response"),
                    {{QStringLiteral("status"), 0},
                     {QStringLiteral("err_code"), 0},
                     {QStringLiteral("c_volt"), 28.51},
                     {QStringLiteral("external_vol"), 3.30},
                     {QStringLiteral("value_YX"), 5.045}},
                    &peerError))
        << peerError.toStdString();

    const ActionResult waited = controller.waitForTerminal(3000);
    ASSERT_TRUE(waited.ok) << waited.message.toStdString();
    const ApplicationSnapshot finished = controller.snapshot();
    EXPECT_EQ(finished.phase, QStringLiteral("finished"));
    EXPECT_EQ(finished.stepId, QStringLiteral("ELEC_HEALTH_STATUS"));
    EXPECT_EQ(finished.algorithmId, QStringLiteral("mbddf.elec_health_status"));
    EXPECT_EQ(finished.verdict, QStringLiteral("Pass"));
    EXPECT_EQ(finished.errorCode, QStringLiteral("Ok"));
    ASSERT_EQ(samples.size(), 1);
    EXPECT_EQ(samples.first().channelId, QStringLiteral("ELEC_HEALTH_STATUS"));
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, DeviceStreamCanSaveWithoutPcPeriodicSettings)
{
    ensureQtApplication();
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QString error;
    ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString halConfigPath;
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halConfigPath,
                                    &error))
        << error.toStdString();
    const QString dataDirectory = directory.filePath(QStringLiteral("continuous-data"));
    ASSERT_TRUE(setDataStorageDirectory(halConfigPath, dataDirectory, &error))
        << error.toStdString();
    const QString testConfigPath = directory.filePath(
        QStringLiteral("device-stream.testcfg.json"));
    ASSERT_TRUE(writeTestConfigWithSupportedRunModes(
        QStringLiteral(HWTEST_APP_TEST_CONFIG),
        testConfigPath,
        {QStringLiteral("single"), QStringLiteral("device_stream")},
        false,
        &error)) << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(testConfigPath, halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    TestRunOptions options;
    options.mode = QStringLiteral("device_stream");
    options.intervalMs = 9;
    options.maxCycles = 1000000001ULL;
    options.saveData = true;
    const ActionResult started = controller.start(options);
    ASSERT_TRUE(started.ok) << started.code.toStdString() << ": "
                            << started.message.toStdString();

    const ActionResult terminal = controller.waitForTerminal(3000);
    EXPECT_FALSE(terminal.ok);
    const ApplicationSnapshot snapshot = controller.snapshot();
    EXPECT_EQ(snapshot.phase, QStringLiteral("error"));
    EXPECT_EQ(snapshot.runMode, QStringLiteral("device_stream"));
    EXPECT_EQ(snapshot.intervalMs, 1000);
    EXPECT_EQ(snapshot.maxCycles, 1u);
    EXPECT_TRUE(snapshot.dataSaveEnabled);
    EXPECT_TRUE(snapshot.dataSaveError.isEmpty())
        << snapshot.dataSaveError.toStdString();
    ASSERT_FALSE(snapshot.dataFilePath.isEmpty());
    ASSERT_TRUE(QFileInfo::exists(snapshot.dataFilePath));

    QFile saved(snapshot.dataFilePath);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray bytes = saved.readAll();
    ASSERT_TRUE(bytes.startsWith(QByteArray::fromHex("EFBBBF")));
    const QString text = QString::fromUtf8(bytes.mid(3));
    EXPECT_TRUE(text.contains(QStringLiteral("# run_mode=device_stream\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("# sample_count=0\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("# repeat_delay_ms=NA\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("# max_cycles=NA\n")));
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest,
     DhIgniteRejectsLifecycleInterruptionAndCompletesNaturally)
{
    ensureQtApplication();
    const QString protocolDirectory = QDir(QStringLiteral(HWTEST_PROJECT_SOURCE_DIR))
                                          .filePath(QStringLiteral(
                                              "dut/docs/design/product_protocol_csv"));
    ScopedEnvironmentVariable protocolAssets(
        "MB_DDF_PROTOCOL_CSV_DIR", protocolDirectory.toUtf8());
    ASSERT_TRUE(QFileInfo(protocolDirectory).isDir());

    test::MbddfUdpTestPeer peer;
    QString error;
    ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString halConfigPath;
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halConfigPath,
                                    &error))
        << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_DH_IGNITE_STREAM_CONFIG),
        halConfigPath).ok);
    ASSERT_FALSE(controller.snapshot().descriptor.stoppable);
    ASSERT_TRUE(controller.prepare().ok);

    TestRunOptions options;
    options.mode = QStringLiteral("device_stream");
    options.saveData = false;
    options.algorithmParameters = {
        {QStringLiteral("channel_enabled[0]"), true},
        {QStringLiteral("report_count"), 2},
        {QStringLiteral("interval_us"), 2500},
        {QStringLiteral("delay_frames"), 0},
    };
    ASSERT_TRUE(controller.start(options).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();

    const ActionResult paused = controller.pause();
    const ActionResult resumed = controller.resume();
    const ActionResult stopped = controller.stop(100);
    const ActionResult stoppedAsync = controller.stopAsync(100);
    const ActionResult shutDown = controller.shutdown();
    EXPECT_FALSE(paused.ok);
    EXPECT_FALSE(resumed.ok);
    EXPECT_FALSE(stopped.ok);
    EXPECT_FALSE(stoppedAsync.ok);
    EXPECT_FALSE(shutDown.ok);
    EXPECT_EQ(paused.code, QStringLiteral("CapabilityUnsupported"));
    EXPECT_EQ(resumed.code, QStringLiteral("CapabilityUnsupported"));
    EXPECT_EQ(stopped.code, QStringLiteral("CapabilityUnsupported"));
    EXPECT_EQ(stoppedAsync.code, QStringLiteral("CapabilityUnsupported"));
    EXPECT_EQ(shutDown.code, QStringLiteral("CapabilityUnsupported"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("running"));

    const QVariantMap responseValues{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("dh_status.ch0"), 1},
        {QStringLiteral("telemetry[0]"), 1.25},
    };
    ASSERT_TRUE(peer.sendToLastRequester(QStringLiteral("dh_control_response"),
                                         0x1234,
                                         responseValues,
                                         &error))
        << error.toStdString();
    ASSERT_TRUE(peer.sendToLastRequester(QStringLiteral("dh_control_response"),
                                         0x1235,
                                         responseValues,
                                         &error))
        << error.toStdString();

    const ActionResult terminal = controller.waitForTerminal(3000);
    ASSERT_TRUE(terminal.ok) << terminal.message.toStdString();
    const ApplicationSnapshot snapshot = controller.snapshot();
    EXPECT_EQ(snapshot.phase, QStringLiteral("finished"));
    EXPECT_EQ(snapshot.verdict, QStringLiteral("Pass"));
    EXPECT_EQ(snapshot.sampleCount, 2u);
    EXPECT_FALSE(snapshot.dataSaveEnabled);
    EXPECT_TRUE(snapshot.dataFilePath.isEmpty());
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, RejectsAnUnknownControlResource)
{
    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                QStringLiteral(HWTEST_APP_HAL_CONFIG)).ok);

    const ActionResult selected = controller.selectControl(QStringLiteral("CONTROL_UNKNOWN"));

    EXPECT_FALSE(selected.ok);
    EXPECT_EQ(selected.code, QStringLiteral("control_not_found"));
    EXPECT_EQ(controller.snapshot().controlResourceId, QStringLiteral("CONTROL_SERIAL"));
}

TEST(TestApplicationControllerTest, AsyncPreparationFailureIsReturnedByWaitWithoutFabricatingAResult)
{
    ensureQtApplication();
    ScopedEnvironmentVariable invalidAssets(
        "MB_DDF_PROTOCOL_CSV_DIR",
        QByteArray("H:/definitely-missing-mbddf-assets"));
    test::MbddfUdpTestPeer peer;
    QString peerError;
    ASSERT_TRUE(peer.bind(&peerError)) << peerError.toStdString();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString halConfigPath;
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halConfigPath,
                                    &peerError))
        << peerError.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    ASSERT_TRUE(controller.start().ok);

    const ActionResult waited = controller.waitForTerminal(3000);
    EXPECT_FALSE(waited.ok);
    EXPECT_NE(waited.code, QStringLiteral("missing_result"));
    EXPECT_FALSE(waited.message.isEmpty());
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("error"));
    EXPECT_FALSE(controller.snapshot().hasResult);
    EXPECT_FALSE(controller.snapshot().errorCode.isEmpty());
    TuiShell shell(&controller, QString(), QString());
    const TuiReply repeatedWait = shell.execute(QStringLiteral("wait 3000"));
    ASSERT_EQ(repeatedWait.lines.size(), 1);
    EXPECT_TRUE(repeatedWait.lines.first().startsWith(QStringLiteral("error ")));
    const TuiReply status = shell.execute(QStringLiteral("status"));
    ASSERT_EQ(status.lines.size(), 1);
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("phase=error")));
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("error=")));
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("message=")));
    ASSERT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, ShutdownDuringWaitInterruptsSafelyAndIgnoresQueuedOldRunEvents)
{
    ensureQtApplication();
    const QString assets = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QString peerError;
    ASSERT_TRUE(peer.bind(&peerError)) << peerError.toStdString();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString halConfigPath;
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory,
                                    &halConfigPath,
                                    &peerError))
        << peerError.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);

    bool shutdownTriggered = false;
    ActionResult shutdownResult;
    QObject::connect(&controller,
                     &TestApplicationController::snapshotChanged,
                     &controller,
                     [&](const ApplicationSnapshot& snapshot) {
                         if (!shutdownTriggered && snapshot.progress == 25) {
                             shutdownTriggered = true;
                             shutdownResult = controller.shutdown();
                         }
                     });

    ASSERT_TRUE(controller.start().ok);
    const ActionResult waited = controller.waitForTerminal(5000);

    EXPECT_TRUE(shutdownTriggered);
    EXPECT_TRUE(shutdownResult.ok) << shutdownResult.message.toStdString();
    EXPECT_FALSE(waited.ok);
    EXPECT_EQ(waited.code, QStringLiteral("wait_interrupted"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
    EXPECT_EQ(controller.snapshot().testState, QStringLiteral("Uninitialized"));
}

TEST(TestApplicationControllerTest, HelmBoardManualModeRunsWithoutPxiAndAutomaticFailsClosed)
{
    ensureQtApplication();
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol catalog directory is unavailable";
    }
    test::MbddfUdpTestPeer peer;
    QString peerError;
    ASSERT_TRUE(peer.bind(&peerError)) << peerError.toStdString();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString halConfigPath;
    ASSERT_TRUE(peer.writeHalConfig(
        QStringLiteral(HWTEST_PROJECT_SOURCE_DIR "/tests/app/fixtures/mbddf_udp_hal.json"),
        &directory, &halConfigPath, &peerError)) << peerError.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_HELM_BOARD_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    ASSERT_EQ(controller.snapshot().descriptor.runParameters.size(), 9);
    for (const auto& parameter : controller.snapshot().descriptor.runParameters) {
        EXPECT_FALSE(parameter.persistValues);
    }

    TestRunOptions manual;
    manual.algorithmParameters.insert(QStringLiteral("test_mode"), 1);
    for (int channel = 0; channel < 4; ++channel) {
        manual.algorithmParameters.insert(
            QStringLiteral("pwm_duty_percent[%1]").arg(channel),
            (channel + 1) * 10);
        manual.algorithmParameters.insert(
            QStringLiteral("direction[%1]").arg(channel),
            (channel % 2) != 0);
    }
    ASSERT_TRUE(controller.start(manual).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
    QVariantMap response{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("pwm_peak"), 0u},
        {QStringLiteral("pwm_enable_mask"), 0x0Fu},
        {QStringLiteral("pwm_update_enabled"), 1u},
        {QStringLiteral("ad_acquisition_enabled"), 1u},
        {QStringLiteral("ad_filter_enabled"), 1u},
    };
    for (int channel = 0; channel < 4; ++channel) {
        response.insert(QStringLiteral("pwm_duty_match[%1]").arg(channel), true);
        response.insert(QStringLiteral("direction_readback[%1]").arg(channel),
                        (channel % 2) != 0);
        response.insert(QStringLiteral("pwm_duty[%1]").arg(channel),
                        static_cast<quint32>((channel + 1) * 10));
        response.insert(QStringLiteral("helm_AD_value[%1]").arg(channel), 0.0);
    }
    ASSERT_TRUE(peer.replyToLastRequest(
        QStringLiteral("helm_board_test_response"), response, &peerError))
        << peerError.toStdString();
    ASSERT_TRUE(controller.waitForTerminal(5000).ok);
    ASSERT_TRUE(controller.snapshot().hasResult);
    EXPECT_EQ(controller.snapshot().verdict, QStringLiteral("Pass"));
    EXPECT_EQ(controller.snapshot().rawData
                  .value(QStringLiteral("boardTest")).toMap()
                  .value(QStringLiteral("mode")).toString(),
              QStringLiteral("manual"));

    TestRunOptions automatic;
    automatic.algorithmParameters.insert(QStringLiteral("test_mode"), 0);
    for (int channel = 0; channel < 4; ++channel) {
        automatic.algorithmParameters.insert(
            QStringLiteral("pwm_duty_percent[%1]").arg(channel), 0);
        automatic.algorithmParameters.insert(
            QStringLiteral("direction[%1]").arg(channel), false);
    }
    const ActionResult rejected = controller.start(automatic);
    EXPECT_FALSE(rejected.ok);
    EXPECT_NE(rejected.message.indexOf(QStringLiteral("PXI-6259")), -1)
        << rejected.message.toStdString();
    EXPECT_TRUE(controller.shutdown().ok);
}

} // namespace
} // namespace hwtest::app
