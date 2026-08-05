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

TEST(TestApplicationControllerTest, KeepsFixedLogFileForMissingOrBlankMode)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QStringList modes{QString{}, QStringLiteral("   ")};
    for (int index = 0; index < modes.size(); ++index) {
        const QString caseDirectory = directory.filePath(
            QStringLiteral("case_%1").arg(index));
        ASSERT_TRUE(QDir().mkpath(caseDirectory));
        const QString halConfigPath = QDir(caseDirectory).filePath(
            QStringLiteral("hal.json"));
        QString error;
        ASSERT_TRUE(writeHalConfigWithLogPath(
            QStringLiteral(HWTEST_APP_HAL_CONFIG),
            halConfigPath,
            QStringLiteral("logs/backend.jsonl"),
            modes.at(index),
            &error)) << error.toStdString();

        TestApplicationController controller;
        ASSERT_TRUE(controller.loadConfigurations(
            QStringLiteral(HWTEST_APP_TEST_CONFIG), halConfigPath).ok);
        ASSERT_TRUE(controller.prepare().ok);
        ASSERT_TRUE(controller.shutdown().ok);

        const QDir logDirectory(QDir(caseDirectory).filePath(
            QStringLiteral("logs")));
        EXPECT_TRUE(QFileInfo(logDirectory.filePath(
            QStringLiteral("backend.jsonl"))).isFile());
        EXPECT_TRUE(logDirectory.entryList(
            {QStringLiteral("backend_*.jsonl")}, QDir::Files).isEmpty());
    }
}

TEST(TestApplicationControllerTest, RejectsUnknownLogFileMode)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString halConfigPath = directory.filePath(QStringLiteral("hal.json"));
    QString error;
    ASSERT_TRUE(writeHalConfigWithLogPath(
        QStringLiteral(HWTEST_APP_HAL_CONFIG),
        halConfigPath,
        {},
        QStringLiteral("per_test"),
        &error)) << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_TEST_CONFIG), halConfigPath).ok);

    const ActionResult prepared = controller.prepare();

    EXPECT_FALSE(prepared.ok);
    EXPECT_EQ(prepared.code, QStringLiteral("logging"));
    if (prepared.ok) {
        controller.shutdown();
    }
}

TEST(TestApplicationControllerTest, RejectsOutOfRangePostRunAnalysisResources)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString halConfigPath = directory.filePath(QStringLiteral("hal.json"));
    QString error;
    ASSERT_TRUE(setAnalysisConfiguration(
        QStringLiteral(HWTEST_APP_HAL_CONFIG),
        halConfigPath,
        QJsonObject{{QStringLiteral("maxProjectedPoints"), 257}},
        &error)) << error.toStdString();

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_TEST_CONFIG), halConfigPath);

    EXPECT_FALSE(loaded.ok);
    EXPECT_EQ(loaded.code, QStringLiteral("analysis_config"));
}

TEST(TestApplicationControllerTest, RejectsNonNumericPostRunAnalysisResources)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString halConfigPath = directory.filePath(QStringLiteral("hal.json"));
    QString error;
    ASSERT_TRUE(setAnalysisConfiguration(
        QStringLiteral(HWTEST_APP_HAL_CONFIG),
        halConfigPath,
        QJsonObject{{QStringLiteral("maxProjectedPoints"),
                     QStringLiteral("256")}},
        &error)) << error.toStdString();

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_TEST_CONFIG), halConfigPath);

    EXPECT_FALSE(loaded.ok);
    EXPECT_EQ(loaded.code, QStringLiteral("analysis_config"));
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

TEST(TestApplicationControllerTest, RejectsNonBooleanStoppableCapability)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString testConfigPath = directory.filePath(
        QStringLiteral("invalid-stoppable.testcfg.json"));
    ASSERT_TRUE(writeTestConfigWithStoppable(
        QStringLiteral(HWTEST_APP_TEST_CONFIG), testConfigPath,
        QJsonValue(QStringLiteral("false")), &error)) << error.toStdString();

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        testConfigPath, QStringLiteral(HWTEST_APP_HAL_CONFIG));

    EXPECT_FALSE(loaded.ok);
    EXPECT_EQ(loaded.code, QStringLiteral("test_config"));
    EXPECT_TRUE(loaded.message.contains(QStringLiteral("stoppable")));
}

TEST(TestApplicationControllerTest, RejectsNonStoppableSingleConfiguration)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString testConfigPath = directory.filePath(
        QStringLiteral("non-stoppable-single.testcfg.json"));
    ASSERT_TRUE(writeTestConfigWithStoppable(
        QStringLiteral(HWTEST_APP_TEST_CONFIG), testConfigPath,
        QJsonValue(false), &error)) << error.toStdString();

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        testConfigPath, QStringLiteral(HWTEST_APP_HAL_CONFIG));

    EXPECT_FALSE(loaded.ok);
    EXPECT_EQ(loaded.code, QStringLiteral("test_config"));
    EXPECT_TRUE(loaded.message.contains(QStringLiteral("device_stream")));
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
    EXPECT_EQ(stimulus.switches.at(1).switchId, QStringLiteral("di1"));
    EXPECT_EQ(stimulus.switches.at(2).switchId, QStringLiteral("di2"));

    const ActionResult rejected = controller.setDigitalStimulus(
        QStringLiteral("di3"), true, 0);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.code, QStringLiteral("invalid_state"));
}

TEST(TestApplicationControllerTest, RejectsDiSafeStateThatDisagreesWithInactiveLevel)
{
    QFile source(QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    ASSERT_TRUE(document.isObject());
    QJsonObject root = document.object();
    QJsonObject safeState = root.value(QStringLiteral("safeState")).toObject();
    safeState.insert(QStringLiteral("DUT_DI3_STIM"), QStringLiteral("High"));
    root.insert(QStringLiteral("safeState"), safeState);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString halPath = directory.filePath(QStringLiteral("unsafe-di-hal.json"));
    QFile output(halPath);
    ASSERT_TRUE(output.open(QIODevice::WriteOnly));
    const QByteArray json = QJsonDocument(root).toJson();
    ASSERT_EQ(output.write(json), json.size());
    output.close();

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_DI_CONFIG), halPath);

    EXPECT_FALSE(loaded.ok);
    EXPECT_EQ(loaded.code, QStringLiteral("stimulus_safe_state_mismatch"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
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

TEST(TestApplicationControllerTest, SynchronousStopReturnsDiStimulusToSafeState)
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
    ASSERT_TRUE(controller.start().ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();

    const ActionResult stopped = controller.stop(5000);

    ASSERT_TRUE(stopped.ok) << stopped.message.toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("stopped"));
    EXPECT_EQ(controller.snapshot().digitalStimulus.appliedMask, 0u);
    EXPECT_EQ(controller.snapshot().digitalStimulus.revision, revision + 2);
    const ActionResult shutdown = controller.shutdown();
    EXPECT_TRUE(shutdown.ok) << shutdown.code.toStdString() << ": "
                             << shutdown.message.toStdString();
}

TEST(TestApplicationControllerTest, RejectsActionsFromOutsideTheControllerAffinityThread)
{
    TestApplicationController controller;
    ActionResult result;

    std::thread caller([&] {
        result = controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                               QStringLiteral(HWTEST_APP_HAL_CONFIG));
    });
    caller.join();

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("wrong_thread"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
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

TEST(TestApplicationControllerTest, ProjectsDescriptorFromElectricalHealthConfiguration)
{
    ensureQtApplication();
    const QString assets = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_ELEC_HEALTH_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG)).ok);

    const TestDescriptor& descriptor = controller.snapshot().descriptor;
    EXPECT_EQ(descriptor.configId, QStringLiteral("mbddf-elec-health"));
    EXPECT_EQ(descriptor.productModel, QStringLiteral("MB_DDF_v2"));
    EXPECT_EQ(descriptor.algorithmId, QStringLiteral("mbddf.elec_health_status"));
    EXPECT_EQ(descriptor.title, QStringLiteral("电气健康"));
    EXPECT_EQ(descriptor.description, QStringLiteral("读取电源与辅助电压健康量。"));
    const QVector<QString> expectedModes{
        QStringLiteral("single"), QStringLiteral("pc_periodic")};
    EXPECT_EQ(descriptor.supportedRunModes, expectedModes);
    ASSERT_EQ(descriptor.measurements.size(), 13);
    EXPECT_EQ(descriptor.measurements.first().id, QStringLiteral("status"));
    EXPECT_EQ(descriptor.measurements.first().label, QStringLiteral("设备状态"));
    EXPECT_EQ(descriptor.measurements.at(2).id, QStringLiteral("c_volt"));
    EXPECT_EQ(descriptor.measurements.at(2).unit, QStringLiteral("V"));
    EXPECT_TRUE(descriptor.measurements.at(2).primary);
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

TEST(TestApplicationControllerTest, ProjectsUnboundedHelmRuntimeParameters)
{
    ensureQtApplication();
    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_HELM_STREAM_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(loaded.ok) << loaded.message.toStdString();

    const ApplicationSnapshot loadedSnapshot = controller.snapshot();
    const TestDescriptor& descriptor = loadedSnapshot.descriptor;
    EXPECT_EQ(descriptor.algorithmId, QStringLiteral("mbddf.helm_stream"));
    EXPECT_EQ(descriptor.supportedRunModes,
              QVector<QString>{QStringLiteral("device_stream")});
    EXPECT_EQ(descriptor.runParameterSchemaVersion, QStringLiteral("1"));
    EXPECT_TRUE(descriptor.postRunAnalysis.supported);
    EXPECT_EQ(descriptor.postRunAnalysis.analyzerId,
              QStringLiteral("mbddf.helm.performance"));
    EXPECT_EQ(descriptor.postRunAnalysis.schemaVersion, QStringLiteral("1"));
    EXPECT_TRUE(loadedSnapshot.analysis.supported);
    EXPECT_EQ(loadedSnapshot.analysis.state, QStringLiteral("none"));
    ASSERT_EQ(descriptor.runParameters.size(), 8);
    const auto amplitude = std::find_if(
        descriptor.runParameters.cbegin(), descriptor.runParameters.cend(),
        [](const TestRunParameterDescriptor& parameter) {
            return parameter.id == QStringLiteral("ampl");
        });
    ASSERT_NE(amplitude, descriptor.runParameters.cend());
    EXPECT_FALSE(amplitude->minimum.isValid());
    EXPECT_FALSE(amplitude->maximum.isValid());
    const auto duration = std::find_if(
        descriptor.runParameters.cbegin(), descriptor.runParameters.cend(),
        [](const TestRunParameterDescriptor& parameter) {
            return parameter.id == QStringLiteral("sweep_duration_s");
        });
    ASSERT_NE(duration, descriptor.runParameters.cend());
    EXPECT_TRUE(duration->minimumExclusive);
    EXPECT_EQ(duration->visibleWhenParameter, QStringLiteral("waveform"));
    EXPECT_EQ(duration->visibleWhenEquals.toInt(), 4);
    EXPECT_DOUBLE_EQ(descriptor.runParameterDefaults.value(
                         QStringLiteral("sweep_duration_s")).toDouble(),
                     25.0);
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, HelmAnalysisStorageFailurePreventsBizStart)
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
    const QString blockedPath = directory.filePath(QStringLiteral("not-a-directory"));
    QFile blocked(blockedPath);
    ASSERT_TRUE(blocked.open(QIODevice::WriteOnly));
    ASSERT_EQ(blocked.write("x"), 1);
    blocked.close();
    ASSERT_TRUE(setDataStorageDirectory(halConfigPath, blockedPath, &error))
        << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_HELM_STREAM_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    TestRunOptions options;
    options.mode = QStringLiteral("device_stream");

    const ActionResult started = controller.start(options);

    EXPECT_FALSE(started.ok);
    EXPECT_EQ(started.code, QStringLiteral("analysis_storage"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    if (started.ok) {
        (void)controller.stop(5000);
    }
    (void)controller.shutdown();
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

TEST(TestApplicationControllerTest, HelmStopCompletesSidecarWithoutChangingRunVerdict)
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
    const QString verdictBeforeStop = controller.snapshot().verdict;
    const QString errorBeforeStop = controller.snapshot().errorCode;

    ASSERT_TRUE(controller.stop(5000).ok);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    const QMetaObject::Connection changed = QObject::connect(
        &controller,
        &TestApplicationController::snapshotChanged,
        &loop,
        [&](const ApplicationSnapshot& snapshot) {
            const QString state = snapshot.analysis.state;
            if (state == QStringLiteral("completed") ||
                state == QStringLiteral("partial") ||
                state == QStringLiteral("unavailable") ||
                state == QStringLiteral("failed") ||
                state == QStringLiteral("cancelled")) {
                loop.quit();
            }
        });
    timeout.start(5000);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(changed);

    const ApplicationSnapshot completed = controller.snapshot();
    EXPECT_NE(completed.analysis.state, QStringLiteral("capturing"));
    EXPECT_NE(completed.analysis.state, QStringLiteral("queued"));
    EXPECT_EQ(completed.phase, QStringLiteral("stopped"));
    EXPECT_EQ(completed.verdict, verdictBeforeStop);
    EXPECT_EQ(completed.errorCode, errorBeforeStop);
    (void)controller.shutdown();
}

TEST(TestApplicationControllerTest, HelmSamplesFlowThroughCaptureAnalysisAndQuery)
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
    options.saveData = false;
    ASSERT_TRUE(controller.start(options).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(
        QStringLiteral("helm_start_response"),
        {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}},
        &error)) << error.toStdString();

    for (int index = 0; index < 12; ++index) {
        constexpr double pi = 3.14159265358979323846;
        const double timeSeconds = static_cast<double>(index) * 0.1;
        const double command = 1.8 * std::sin(2.0 * pi * timeSeconds);
        const double feedback = 1.7 * std::sin(2.0 * pi * timeSeconds - 0.1);
        ASSERT_TRUE(peer.sendToLastRequester(
            QStringLiteral("helm_feedback_response"),
            static_cast<quint16>(0x8000 + index),
            helmOneSampleFeedback(static_cast<quint64>(index) * 100000ULL,
                                   static_cast<quint16>(100 + index),
                                   static_cast<quint16>(200 + index),
                                   command,
                                   feedback),
            &error)) << error.toStdString();
    }
    QEventLoop sampleLoop;
    QTimer sampleGuard;
    sampleGuard.setSingleShot(true);
    QObject::connect(&sampleGuard, &QTimer::timeout,
                     &sampleLoop, &QEventLoop::quit);
    const QMetaObject::Connection samples = QObject::connect(
        &controller,
        &TestApplicationController::sampleReceived,
        &sampleLoop,
        [&](const ApplicationSample&) {
            if (controller.snapshot().sampleCount >= 12) sampleLoop.quit();
        });
    sampleGuard.start(5000);
    sampleLoop.exec();
    QObject::disconnect(samples);
    ASSERT_EQ(controller.snapshot().sampleCount, 12u);

    QEventLoop stopLoop;
    QTimer stopGuard;
    stopGuard.setSingleShot(true);
    ActionResult stopResult{false, QStringLiteral("not_completed"), {}};
    QObject::connect(&stopGuard, &QTimer::timeout, &stopLoop, &QEventLoop::quit);
    const QMetaObject::Connection stopped = QObject::connect(
        &controller,
        &TestApplicationController::stopCompleted,
        &stopLoop,
        [&](const ActionResult& result) {
            stopResult = result;
            stopLoop.quit();
        });
    ASSERT_TRUE(controller.stopAsync(5000).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(
        QStringLiteral("helm_stop_response"),
        {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}},
        &error)) << error.toStdString();
    stopGuard.start(5000);
    stopLoop.exec();
    QObject::disconnect(stopped);
    ASSERT_TRUE(stopResult.ok) << stopResult.message.toStdString();

    QEventLoop analysisLoop;
    QTimer analysisGuard;
    analysisGuard.setSingleShot(true);
    QObject::connect(&analysisGuard, &QTimer::timeout,
                     &analysisLoop, &QEventLoop::quit);
    const QMetaObject::Connection analyzed = QObject::connect(
        &controller,
        &TestApplicationController::snapshotChanged,
        &analysisLoop,
        [&](const ApplicationSnapshot& snapshot) {
            if (snapshot.analysis.state == QStringLiteral("completed") ||
                snapshot.analysis.state == QStringLiteral("partial") ||
                snapshot.analysis.state == QStringLiteral("unavailable") ||
                snapshot.analysis.state == QStringLiteral("failed")) {
                analysisLoop.quit();
            }
        });
    if (controller.snapshot().analysis.state == QStringLiteral("capturing") ||
        controller.snapshot().analysis.state == QStringLiteral("queued") ||
        controller.snapshot().analysis.state == QStringLiteral("validating") ||
        controller.snapshot().analysis.state == QStringLiteral("preprocessing") ||
        controller.snapshot().analysis.state == QStringLiteral("calculating") ||
        controller.snapshot().analysis.state == QStringLiteral("persisting")) {
        analysisGuard.start(10000);
        analysisLoop.exec();
    }
    QObject::disconnect(analyzed);

    const ApplicationSnapshot snapshot = controller.snapshot();
    ASSERT_TRUE(snapshot.analysis.state == QStringLiteral("completed") ||
                snapshot.analysis.state == QStringLiteral("partial"))
        << snapshot.analysis.reasonCode.toStdString() << ": "
        << snapshot.analysis.message.toStdString();
    EXPECT_FALSE(snapshot.dataSaveEnabled);
    EXPECT_TRUE(snapshot.dataFilePath.isEmpty());
    ASSERT_TRUE(QFileInfo::exists(snapshot.analysis.resultFilePath));
    AnalysisChannelProjection channel;
    const ActionResult queried = controller.analysisResult(
        AnalysisResultQuery{snapshot.analysis.taskId,
                            snapshot.analysis.analysisGeneration,
                            0},
        &channel);
    ASSERT_TRUE(queried.ok) << queried.message.toStdString();
    EXPECT_TRUE(channel.channelSummary.status == QStringLiteral("completed") ||
                channel.channelSummary.status == QStringLiteral("partial"));
    EXPECT_FALSE(channel.channelSummary.commonMetrics.isEmpty());
    EXPECT_TRUE(channel.frequencyHz.isEmpty());
    EXPECT_TRUE(controller.shutdown().ok);
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

TEST(TestApplicationControllerTest, ElectricalHealthPcPeriodicForwardsOneSamplePerCycle)
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

    TestRunOptions options;
    options.mode = QStringLiteral("pc_periodic");
    options.intervalMs = 10;
    options.maxCycles = 2;
    ASSERT_TRUE(controller.start(options).ok);
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
        ASSERT_TRUE(peer.replyToLastRequest(
                        QStringLiteral("elec_health_status_response"),
                        {{QStringLiteral("status"), 0},
                         {QStringLiteral("err_code"), 0},
                         {QStringLiteral("c_volt"), 28.51 + cycle * 0.01},
                         {QStringLiteral("external_vol"), 3.30},
                         {QStringLiteral("value_YX"), 5.045}},
                        &peerError))
            << peerError.toStdString();
    }

    ASSERT_TRUE(controller.waitForTerminal(3000).ok);
    ASSERT_EQ(samples.size(), 2);
    EXPECT_EQ(samples.at(0).cycleIndex, 1u);
    EXPECT_EQ(samples.at(1).cycleIndex, 2u);
    EXPECT_EQ(samples.at(0).stepId, QStringLiteral("ELEC_HEALTH_STATUS"));
    EXPECT_EQ(samples.at(0).channelId, QStringLiteral("ELEC_HEALTH_STATUS"));
    EXPECT_EQ(controller.snapshot().runMode, QStringLiteral("pc_periodic"));
    EXPECT_EQ(controller.snapshot().cycleIndex, 2u);
    EXPECT_EQ(controller.snapshot().sampleCount, 2u);
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, StoppedPcPeriodicSavesCompleteElectricalHealthTextData)
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

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_ELEC_HEALTH_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);

    TestRunOptions options;
    options.mode = QStringLiteral("pc_periodic");
    options.intervalMs = 0;
    options.maxCycles = 0;
    options.saveData = true;
    ASSERT_TRUE(controller.start(options).ok);
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
        ASSERT_TRUE(peer.replyToLastRequest(
                        QStringLiteral("elec_health_status_response"),
                        {{QStringLiteral("status"), 0},
                         {QStringLiteral("err_code"), 0},
                         {QStringLiteral("c_volt"), 11.1 + cycle * 10.0},
                         {QStringLiteral("b_volt"), 12.2 + cycle * 10.0},
                         {QStringLiteral("external_vol"), 3.3},
                         {QStringLiteral("core_vol"), 1.0},
                         {QStringLiteral("assist_vol"), 1.8},
                         {QStringLiteral("v28_5"), 28.5},
                         {QStringLiteral("js_5V"), 5.0},
                         {QStringLiteral("dyt_5V"), 5.1},
                         {QStringLiteral("power_24V"), 24.0},
                         {QStringLiteral("value_YX"), 4.9},
                         {QStringLiteral("activate_bits"), cycle}},
                        &error))
            << error.toStdString();
    }

    QElapsedTimer samplesTimer;
    samplesTimer.start();
    while (controller.snapshot().sampleCount < 2 && samplesTimer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(1);
    }
    ASSERT_EQ(controller.snapshot().sampleCount, 2u);
    const ActionResult stopped = controller.stop(5000);
    ASSERT_TRUE(stopped.ok) << stopped.message.toStdString();
    const ApplicationSnapshot snapshot = controller.snapshot();
    EXPECT_EQ(snapshot.phase, QStringLiteral("stopped"));
    EXPECT_TRUE(snapshot.dataSaveEnabled);
    EXPECT_TRUE(snapshot.dataSaveError.isEmpty())
        << snapshot.dataSaveError.toStdString();
    ASSERT_FALSE(snapshot.dataFilePath.isEmpty());
    const QFileInfo savedInfo(snapshot.dataFilePath);
    EXPECT_EQ(savedInfo.absolutePath(), QFileInfo(dataDirectory).absoluteFilePath());
    EXPECT_TRUE(savedInfo.fileName().startsWith(QStringLiteral("电气健康_")));
    EXPECT_EQ(savedInfo.suffix(), QStringLiteral("txt"));
    ASSERT_TRUE(savedInfo.isFile());

    QFile saved(snapshot.dataFilePath);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray bytes = saved.readAll();
    ASSERT_TRUE(bytes.startsWith(QByteArray::fromHex("EFBBBF")));
    const QString text = QString::fromUtf8(bytes.mid(3));
    EXPECT_TRUE(text.contains(QStringLiteral("# 电气健康连续采集数据\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("# final_status=用户停止\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("# sample_count=2\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("# repeat_delay_ms=0\n")));
    EXPECT_TRUE(text.contains(QStringLiteral(
        "report_index\tsample_time_us\tseq\tresponse_status\terr_code\t"
        "c_volt_V\tb_volt_V\texternal_vol_V\tcore_vol_V\tassist_vol_V\t"
        "v28_5_V\tjs_5V_V\tdyt_5V_V\tpower_24V_V\tvalue_YX_V\t"
        "activate_bits\tbc_activate_good\n")));

    QStringList dataLines;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            dataLines.push_back(line);
        }
    }
    ASSERT_EQ(dataLines.size(), 3);
    bool firstSampleTimeOk = false;
    const qint64 firstSampleTimeUs =
        dataLines.at(1).section(QLatin1Char('\t'), 1, 1).toLongLong(&firstSampleTimeOk);
    EXPECT_TRUE(firstSampleTimeOk);
    EXPECT_GE(firstSampleTimeUs, 0);
    EXPECT_TRUE(dataLines.at(1).contains(QStringLiteral("\t4660\t0\t0x0000\t")));
    EXPECT_TRUE(dataLines.at(1).contains(QStringLiteral("\t11.1\t12.2\t")));
    EXPECT_TRUE(dataLines.at(2).contains(QStringLiteral("\t4661\t0\t0x0000\t")));
    EXPECT_TRUE(dataLines.at(2).contains(QStringLiteral("\t21.1\t22.2\t")));
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest,
     StoppedPcPeriodicUsesExplicitDataDirectoryAndBaseName)
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
    const QString configuredDirectory =
        directory.filePath(QStringLiteral("configured-data"));
    const QString requestedDirectory =
        QDir(directory.filePath(QStringLiteral("requested-data"))).absolutePath();
    ASSERT_TRUE(setDataStorageDirectory(halConfigPath, configuredDirectory, &error))
        << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_ELEC_HEALTH_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);

    TestRunOptions options;
    options.mode = QStringLiteral("pc_periodic");
    options.intervalMs = 0;
    options.maxCycles = 0;
    options.saveData = true;
    options.dataDirectory = requestedDirectory;
    options.dataFileName = QStringLiteral("operator-capture");
    ASSERT_TRUE(controller.start(options).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(
                    QStringLiteral("elec_health_status_response"),
                    {{QStringLiteral("status"), 0},
                     {QStringLiteral("err_code"), 0},
                     {QStringLiteral("c_volt"), 11.1},
                     {QStringLiteral("b_volt"), 12.2},
                     {QStringLiteral("external_vol"), 3.3},
                     {QStringLiteral("core_vol"), 1.0},
                     {QStringLiteral("assist_vol"), 1.8},
                     {QStringLiteral("v28_5"), 28.5},
                     {QStringLiteral("js_5V"), 5.0},
                     {QStringLiteral("dyt_5V"), 5.1},
                     {QStringLiteral("power_24V"), 24.0},
                     {QStringLiteral("value_YX"), 4.9},
                     {QStringLiteral("activate_bits"), 0}},
                    &error))
        << error.toStdString();
    ASSERT_TRUE(controller.stop(5000).ok);

    const QFileInfo savedInfo(controller.snapshot().dataFilePath);
    EXPECT_EQ(savedInfo.absolutePath(), QFileInfo(requestedDirectory).absoluteFilePath());
    EXPECT_EQ(savedInfo.fileName(), QStringLiteral("operator-capture.txt"));
    EXPECT_TRUE(savedInfo.isFile());
    EXPECT_TRUE(QDir(configuredDirectory)
                    .entryList(QStringList{QStringLiteral("*.txt")}, QDir::Files)
                    .isEmpty());
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest,
     PcPeriodicRejectsRelativeDirectoryAndPathLikeDataFileNameBeforeRun)
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

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_ELEC_HEALTH_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);

    TestRunOptions options;
    options.mode = QStringLiteral("pc_periodic");
    options.intervalMs = 0;
    options.maxCycles = 1;
    options.saveData = true;
    options.dataDirectory = QStringLiteral("relative-output");
    ActionResult rejected = controller.start(options);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));

#ifdef Q_OS_WIN
    options.dataDirectory = QStringLiteral("//server");
    rejected = controller.start(options);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
#endif

    options.dataDirectory = QDir(directory.filePath(QStringLiteral("requested-data"))).absolutePath();
    options.dataFileName = QStringLiteral("nested/capture.txt");
    rejected = controller.start(options);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));

    options.dataFileName = QStringLiteral("nested\\capture.txt");
    rejected = controller.start(options);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
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

TEST(TestApplicationControllerTest, ImuDeviceStreamSendsStartAndStopAndSavesFixedColumns)
{
    ensureQtApplication();
    ScopedEnvironmentVariable protocolAssets(
        "MB_DDF_PROTOCOL_CSV_DIR",
        QByteArray("H:/WorkSpace/QtWorkspace/testProgram/dut/docs/design/product_protocol_csv"));
    ASSERT_TRUE(QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir());

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

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_IMU_STREAM_CONFIG), halConfigPath).ok);
    EXPECT_EQ(controller.snapshot().descriptor.supportedRunModes,
              QVector<QString>{QStringLiteral("device_stream")});
    ASSERT_TRUE(controller.prepare().ok);

    TestRunOptions options;
    options.mode = QStringLiteral("device_stream");
    options.saveData = true;
    ASSERT_TRUE(controller.start(options).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(
        QStringLiteral("imu_stream_start_response"),
        {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}},
        &error)) << error.toStdString();

    const auto sendFeedback = [&](quint16 productSequence,
                                  quint16 sourceSequence,
                                  double base) {
        return peer.sendToLastRequester(
            QStringLiteral("imu_stream_feedback_response"),
            productSequence,
            {{QStringLiteral("status"), 0},
             {QStringLiteral("err_code"), 0},
             {QStringLiteral("source_seq"), sourceSequence},
             {QStringLiteral("delta_angle_x"), base + 0.1},
             {QStringLiteral("delta_angle_y"), base + 0.2},
             {QStringLiteral("delta_angle_z"), base + 0.3},
             {QStringLiteral("delta_velocity_x"), base + 1.1},
             {QStringLiteral("delta_velocity_y"), base + 1.2},
             {QStringLiteral("delta_velocity_z"), base + 1.3},
             {QStringLiteral("angular_rate_x"), base + 2.1},
             {QStringLiteral("angular_rate_y"), base + 2.2},
             {QStringLiteral("angular_rate_z"), base + 2.3},
             {QStringLiteral("acceleration_x"), base + 3.1},
             {QStringLiteral("acceleration_y"), base + 3.2},
             {QStringLiteral("acceleration_z"), base + 3.3},
             {QStringLiteral("temperature"), 25.5 + base},
             {QStringLiteral("self_test_status"), 0x1234},
             {QStringLiteral("work_status"), 0x56},
             {QStringLiteral("software_version"), 0x789A},
             {QStringLiteral("source_reserved"), 0xBCDE}},
            &error);
    };
    ASSERT_TRUE(sendFeedback(0x9000, 100, 10.0)) << error.toStdString();
    ASSERT_TRUE(sendFeedback(0x9001, 5, 20.0)) << error.toStdString();

    QElapsedTimer sampleTimer;
    sampleTimer.start();
    while (controller.snapshot().sampleCount < 2 && sampleTimer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(1);
    }
    ASSERT_EQ(controller.snapshot().sampleCount, 2u);
    ASSERT_TRUE(controller.stopAsync(5000).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(
        QStringLiteral("imu_stream_stop_response"),
        {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}},
        &error)) << error.toStdString();

    QElapsedTimer stopTimer;
    stopTimer.start();
    while (controller.snapshot().phase != QStringLiteral("stopped") &&
           stopTimer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(1);
    }
    const ApplicationSnapshot snapshot = controller.snapshot();
    ASSERT_EQ(snapshot.phase, QStringLiteral("stopped"));
    EXPECT_TRUE(snapshot.dataSaveEnabled);
    EXPECT_TRUE(snapshot.dataSaveError.isEmpty())
        << snapshot.dataSaveError.toStdString();
    ASSERT_EQ(snapshot.sampleCount, 2u);
    ASSERT_TRUE(QFileInfo::exists(snapshot.dataFilePath));

    QFile saved(snapshot.dataFilePath);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray bytes = saved.readAll();
    ASSERT_TRUE(bytes.startsWith(QByteArray::fromHex("EFBBBF")));
    const QString text = QString::fromUtf8(bytes.mid(3));
    EXPECT_TRUE(text.contains(QStringLiteral("# run_mode=device_stream\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("# sample_count=2\n")));
    const QString expectedHeader = QStringLiteral(
        "report_index\tsample_time_us\tseq\tresponse_status\terr_code\t"
        "source_seq\tdelta_angle_x\tdelta_angle_y\tdelta_angle_z\t"
        "delta_velocity_x\tdelta_velocity_y\tdelta_velocity_z\t"
        "angular_rate_x\tangular_rate_y\tangular_rate_z\t"
        "acceleration_x\tacceleration_y\tacceleration_z\t"
        "temperature_°C\tself_test_status\twork_status\t"
        "software_version\tsource_reserved");
    EXPECT_TRUE(text.contains(expectedHeader + QLatin1Char('\n')));
    QStringList dataLines;
    bool headerSeen = false;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (line == expectedHeader) {
            headerSeen = true;
        } else if (headerSeen && !line.isEmpty()) {
            dataLines.push_back(line);
        }
    }
    ASSERT_EQ(dataLines.size(), 2);
    const QStringList firstColumns =
        dataLines.at(0).split(QLatin1Char('\t'), Qt::KeepEmptyParts);
    const QStringList secondColumns =
        dataLines.at(1).split(QLatin1Char('\t'), Qt::KeepEmptyParts);
    ASSERT_EQ(firstColumns.size(), 23);
    ASSERT_EQ(secondColumns.size(), 23);
    EXPECT_EQ(firstColumns.at(1), QStringLiteral("0"));
    EXPECT_EQ(secondColumns.at(1), QStringLiteral("2500"));
    EXPECT_EQ(firstColumns.at(6), QStringLiteral("10.1"));
    EXPECT_EQ(secondColumns.at(6), QStringLiteral("20.1"));
    EXPECT_TRUE(text.contains(QStringLiteral("\t100\t")));
    EXPECT_TRUE(text.contains(QStringLiteral("\t5\t")));
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

TEST(TestApplicationControllerTest,
     DhIgniteWritesTxtOnlyWhenSaveDataIsExplicitlyEnabled)
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
    ASSERT_TRUE(controller.prepare().ok);
    TestRunOptions options;
    options.mode = QStringLiteral("device_stream");
    options.saveData = true;
    options.algorithmParameters = {
        {QStringLiteral("channel_enabled[0]"), true},
        {QStringLiteral("report_count"), 2},
        {QStringLiteral("interval_us"), 2500},
        {QStringLiteral("delay_frames"), 0},
    };
    ASSERT_TRUE(controller.start(options).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
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
    ASSERT_TRUE(controller.waitForTerminal(3000).ok);

    const ApplicationSnapshot snapshot = controller.snapshot();
    ASSERT_EQ(snapshot.phase, QStringLiteral("finished"));
    ASSERT_TRUE(snapshot.dataSaveEnabled);
    ASSERT_TRUE(snapshot.dataSaveError.isEmpty())
        << snapshot.dataSaveError.toStdString();
    ASSERT_TRUE(QFileInfo::exists(snapshot.dataFilePath));
    QFile saved(snapshot.dataFilePath);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray bytes = saved.readAll();
    ASSERT_TRUE(bytes.startsWith(QByteArray::fromHex("EFBBBF")));
    const QString text = QString::fromUtf8(bytes.mid(3));
    QString header;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral(
                "report_index\tsample_time_us\tseq\tresponse_status\terr_code"))) {
            header = line;
            break;
        }
    }
    ASSERT_FALSE(header.isEmpty());
    const QStringList columns = header.split(QLatin1Char('\t'));
    EXPECT_EQ(columns.count(QStringLiteral("seq")), 1);
    EXPECT_TRUE(text.contains(QStringLiteral("\n1\t0\t4660\t0\t0x0000\t")));
    EXPECT_TRUE(text.contains(QStringLiteral("\n2\t2500\t4661\t0\t0x0000\t")));
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, SingleRunNeverSavesDataWhenRequested)
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

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_ELEC_HEALTH_CONFIG), halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    TestRunOptions options;
    options.mode = QStringLiteral("single");
    options.saveData = true;
    ASSERT_TRUE(controller.start(options).ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(
                    QStringLiteral("elec_health_status_response"),
                    {{QStringLiteral("status"), 0},
                     {QStringLiteral("err_code"), 0},
                     {QStringLiteral("c_volt"), 11.1}},
                    &error))
        << error.toStdString();
    ASSERT_TRUE(controller.waitForTerminal(3000).ok);

    const ApplicationSnapshot snapshot = controller.snapshot();
    EXPECT_FALSE(snapshot.dataSaveEnabled);
    EXPECT_TRUE(snapshot.dataFilePath.isEmpty());
    EXPECT_TRUE(snapshot.dataSaveError.isEmpty());
    EXPECT_TRUE(QDir(dataDirectory)
                    .entryList(QStringList{QStringLiteral("*.txt")}, QDir::Files)
                    .isEmpty());
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

TEST(TestApplicationControllerTest, SelectsSerialPortInMemoryBeforePreparation)
{
    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                QStringLiteral(HWTEST_APP_HAL_CONFIG)).ok);
    EXPECT_EQ(controller.snapshot().serialPortName, QStringLiteral("COM_CHANGE_ME"));

    const ActionResult selected = controller.selectSerialPort(QStringLiteral("  COM42  "));

    ASSERT_TRUE(selected.ok) << selected.message.toStdString();
    EXPECT_EQ(controller.snapshot().serialPortName, QStringLiteral("COM42"));

    ASSERT_TRUE(controller.selectControl(QStringLiteral("CONTROL_NETWORK")).ok);
    EXPECT_TRUE(controller.snapshot().serialPortName.isEmpty());
    const ActionResult rejectedForUdp = controller.selectSerialPort(QStringLiteral("COM43"));
    EXPECT_FALSE(rejectedForUdp.ok);
    EXPECT_EQ(rejectedForUdp.code, QStringLiteral("control_not_serial"));

    ASSERT_TRUE(controller.selectControl(QStringLiteral("CONTROL_SERIAL")).ok);
    EXPECT_EQ(controller.snapshot().serialPortName, QStringLiteral("COM42"));
    ASSERT_TRUE(controller.prepare().ok);
    const ActionResult rejectedAfterPrepare = controller.selectSerialPort(QStringLiteral("COM43"));
    EXPECT_FALSE(rejectedAfterPrepare.ok);
    EXPECT_EQ(rejectedAfterPrepare.code, QStringLiteral("invalid_state"));
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, RejectsAnEmptySerialPortName)
{
    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                QStringLiteral(HWTEST_APP_HAL_CONFIG)).ok);

    const ActionResult selected = controller.selectSerialPort(QStringLiteral("   "));

    EXPECT_FALSE(selected.ok);
    EXPECT_EQ(selected.code, QStringLiteral("serial_port_required"));
    EXPECT_EQ(controller.snapshot().serialPortName, QStringLiteral("COM_CHANGE_ME"));
}

TEST(TestApplicationControllerTest, RunsSystemStatusThroughTheSelectedUdpControlResource)
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
    ASSERT_EQ(controller.thread(), QThread::currentThread());
    ASSERT_EQ(QCoreApplication::instance()->thread(), QThread::currentThread());
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    EXPECT_EQ(controller.snapshot().testState, QStringLiteral("Idle"));
    EXPECT_FALSE(controller.selectControl(QStringLiteral("CONTROL_SERIAL")).ok);

    const ActionResult started = controller.start();
    ASSERT_TRUE(started.ok) << started.message.toStdString();
    EXPECT_FALSE(controller.snapshot().taskId.isEmpty());

    ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(&peerError)) << peerError.toStdString();

    const ActionResult waited = controller.waitForTerminal(3000);
    ASSERT_TRUE(waited.ok) << waited.message.toStdString()
                           << ", phase=" << controller.snapshot().phase.toStdString()
                           << ", state=" << controller.snapshot().testState.toStdString()
                           << ", error=" << controller.snapshot().errorCode.toStdString()
                           << ", message=" << controller.snapshot().message.toStdString();
    const ApplicationSnapshot finished = controller.snapshot();
    EXPECT_EQ(finished.phase, QStringLiteral("finished"));
    EXPECT_EQ(finished.testState, QStringLiteral("Finished"));
    EXPECT_TRUE(finished.hasResult);
    EXPECT_EQ(finished.stepId, QStringLiteral("SYSTEM_STATUS"));
    EXPECT_EQ(finished.testItemId, QStringLiteral("system_status"));
    EXPECT_EQ(finished.algorithmId, QStringLiteral("mbddf.system_status"));
    EXPECT_EQ(finished.verdict, QStringLiteral("Pass"));
    EXPECT_EQ(finished.errorCode, QStringLiteral("Ok"));
    EXPECT_EQ(finished.attempts, 1);
    EXPECT_EQ(finished.progress, 100);
    EXPECT_EQ(finished.progressStep, QStringLiteral("response decoded"));
    EXPECT_FALSE(finished.rawData.value(QStringLiteral("requestFrameHex")).toString().isEmpty());
    EXPECT_NEAR(finished.rawData.value(QStringLiteral("responseValues"))
                    .toMap()
                    .value(QStringLiteral("cpu_usage"))
                    .toDouble(),
                12.5,
                1e-6);

    ASSERT_TRUE(controller.shutdown().ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
}

TEST(TestApplicationControllerTest, PcPeriodicRunForwardsTimestampedSamplesFromEveryCycle)
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
                     [&](const ApplicationSample& sample) {
                         samples.push_back(sample);
                     });
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);

    TestRunOptions invalidOptions;
    invalidOptions.mode = QStringLiteral("pc_periodic");
    invalidOptions.intervalMs = -1;
    invalidOptions.maxCycles = 2;
    const ActionResult rejected = controller.start(invalidOptions);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.code, QStringLiteral("ParameterRangeError"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));

    TestRunOptions options;
    options.mode = QStringLiteral("pc_periodic");
    options.intervalMs = 0;
    options.maxCycles = 2;
    const ActionResult started = controller.start(options);
    ASSERT_TRUE(started.ok) << started.message.toStdString();

    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
        ASSERT_TRUE(peer.replyToLastRequest(&peerError)) << peerError.toStdString();
    }

    const ActionResult waited = controller.waitForTerminal(3000);
    ASSERT_TRUE(waited.ok) << waited.message.toStdString();
    ASSERT_EQ(samples.size(), 2);
    EXPECT_EQ(samples.at(0).cycleIndex, 1u);
    EXPECT_EQ(samples.at(1).cycleIndex, 2u);
    EXPECT_EQ(samples.at(0).taskId, controller.snapshot().taskId);
    EXPECT_EQ(samples.at(0).stepId, QStringLiteral("SYSTEM_STATUS"));
    EXPECT_EQ(samples.at(0).channelId, QStringLiteral("SYSTEM_STATUS"));
    EXPECT_GT(samples.at(0).timestampUs, 0);
    EXPECT_NEAR(samples.at(0).values.value(QStringLiteral("cpu_usage")).toDouble(),
                12.5,
                1e-6);

    const ApplicationSnapshot finished = controller.snapshot();
    EXPECT_EQ(finished.phase, QStringLiteral("finished"));
    EXPECT_EQ(finished.runMode, QStringLiteral("pc_periodic"));
    EXPECT_EQ(finished.intervalMs, 0);
    EXPECT_EQ(finished.maxCycles, 2u);
    EXPECT_EQ(finished.cycleIndex, 2u);
    EXPECT_EQ(finished.sampleCount, 2u);
    EXPECT_EQ(finished.rawData.value(QStringLiteral("responseValues"))
                  .toMap()
                  .value(QStringLiteral("seq"))
                  .toUInt(),
              0x1235u);

    ASSERT_TRUE(controller.shutdown().ok);
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

TEST(TestApplicationControllerTest, AsyncStopGuardsLifecycleAndCompletesOnAffinityThread)
{
    ensureQtApplication();
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
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
    ASSERT_TRUE(controller.start().ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool completed = false;
    ActionResult completion;
    QThread* completionThread = nullptr;
    QObject::connect(&controller,
                     &TestApplicationController::stopCompleted,
                     &controller,
                     [&](const ActionResult& result) {
                         completed = true;
                         completion = result;
                         completionThread = QThread::currentThread();
                         loop.quit();
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    QElapsedTimer callTimer;
    callTimer.start();
    const ActionResult requested = controller.stopAsync(5000);
    EXPECT_LT(callTimer.elapsed(), 250);
    ASSERT_TRUE(requested.ok) << requested.message.toStdString();
    const ActionResult duplicate = controller.stopAsync(5000);
    EXPECT_FALSE(duplicate.ok);
    EXPECT_EQ(duplicate.code, QStringLiteral("stop_in_progress"));
    const ActionResult pauseWhileStopping = controller.pause();
    EXPECT_FALSE(pauseWhileStopping.ok);
    EXPECT_EQ(pauseWhileStopping.code, QStringLiteral("stop_in_progress"));
    const ActionResult duplicateSync = controller.stop(5000);
    EXPECT_FALSE(duplicateSync.ok);
    EXPECT_EQ(duplicateSync.code, QStringLiteral("stop_in_progress"));
    const ActionResult restartWhileStopping = controller.start();
    EXPECT_FALSE(restartWhileStopping.ok);
    EXPECT_EQ(restartWhileStopping.code, QStringLiteral("stop_in_progress"));
    const ActionResult prematureShutdown = controller.shutdown();
    EXPECT_FALSE(prematureShutdown.ok);
    EXPECT_EQ(prematureShutdown.code, QStringLiteral("stop_in_progress"));

    timeout.start(5000);
    loop.exec();
    ASSERT_TRUE(completed);
    EXPECT_TRUE(completion.ok) << completion.message.toStdString();
    EXPECT_EQ(completionThread, controller.thread());
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("stopped"));
    EXPECT_FALSE(controller.snapshot().hasResult);
    EXPECT_TRUE(controller.snapshot().errorCode.isEmpty());
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest, AsyncStopStillProjectsSampleQueuedBeforeStopRequest)
{
    ensureQtApplication();
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
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
                     [&](const ApplicationSample& sample) {
                         samples.push_back(sample);
                     });
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    ASSERT_TRUE(controller.start().ok);
    ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(&peerError)) << peerError.toStdString();

    // Keep the controller affinity thread from draining queued BIZ signals until
    // the stop suppression marker has been installed.
    QThread::msleep(200);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool completed = false;
    ActionResult completion;
    QObject::connect(&controller,
                     &TestApplicationController::stopCompleted,
                     &controller,
                     [&](const ActionResult& result) {
                         completed = true;
                         completion = result;
                         loop.quit();
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    ASSERT_TRUE(controller.stopAsync(5000).ok);
    timeout.start(5000);
    loop.exec();

    ASSERT_TRUE(completed);
    ASSERT_TRUE(completion.ok) << completion.message.toStdString();
    ASSERT_EQ(samples.size(), 1);
    EXPECT_EQ(samples.first().taskId, controller.snapshot().taskId);
    EXPECT_EQ(controller.snapshot().sampleCount, 1u);
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(TestApplicationControllerTest,
     DoesNotProjectQueuedPriorTaskEventsAfterNextTaskStarts)
{
    ensureQtApplication();
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
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
    QVector<ApplicationSample> projectedSamples;
    QObject::connect(&controller,
                     &TestApplicationController::sampleReceived,
                     &controller,
                     [&projectedSamples](const ApplicationSample& sample) {
                         projectedSamples.push_back(sample);
                     });
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                halConfigPath).ok);
    ASSERT_TRUE(controller.prepare().ok);

    TestRunOptions options;
    options.mode = QStringLiteral("pc_periodic");
    options.intervalMs = 0;
    options.maxCycles = 0;
    ASSERT_TRUE(controller.start(options).ok);
    const QString firstTaskId = controller.snapshot().taskId;
    ASSERT_FALSE(firstTaskId.isEmpty());
    ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(&peerError)) << peerError.toStdString();

    // The next request is a causal barrier: BIZ cannot begin cycle 2 until it has
    // produced cycle 1's sample and queued A's controller-facing signals.
    ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
    ASSERT_TRUE(projectedSamples.isEmpty());
    ASSERT_TRUE(controller.stop(5000).ok);
    ASSERT_EQ(controller.snapshot().phase, QStringLiteral("stopped"));
    ASSERT_TRUE(projectedSamples.isEmpty());

    TestRunOptions secondOptions = options;
    secondOptions.intervalMs = 60000;
    ASSERT_TRUE(controller.start(secondOptions).ok);
    const QString secondTaskId = controller.snapshot().taskId;
    ASSERT_NE(secondTaskId, firstTaskId);
    ASSERT_EQ(controller.snapshot().sampleCount, 0u);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool secondSampleReceived = false;
    QObject::connect(&controller,
                     &TestApplicationController::sampleReceived,
                     &loop,
                     [&](const ApplicationSample& sample) {
                         if (sample.taskId == secondTaskId) {
                             secondSampleReceived = true;
                             loop.quit();
                         }
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    ASSERT_TRUE(peer.waitForRequest(3000, &peerError)) << peerError.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(&peerError)) << peerError.toStdString();
    timeout.start(3000);
    loop.exec();

    ASSERT_TRUE(secondSampleReceived);
    EXPECT_EQ(controller.snapshot().taskId, secondTaskId);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("running"));
    ASSERT_FALSE(projectedSamples.isEmpty());
    for (const auto& sample : projectedSamples) {
        EXPECT_EQ(sample.taskId, secondTaskId);
    }
    EXPECT_EQ(controller.snapshot().sampleCount, 1u);
    EXPECT_TRUE(controller.stop(5000).ok);
    EXPECT_TRUE(controller.shutdown().ok);
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
