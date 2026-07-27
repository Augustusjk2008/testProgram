#include <app/test_application_controller.h>
#include <app/tui_shell.h>

#include "support/mbddf_udp_test_peer.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

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

TEST(TestApplicationControllerTest, RejectsPreparationBeforeConfigurationsAreLoaded)
{
    TestApplicationController controller;

    const ActionResult result = controller.prepare();

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("invalid_state"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
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
    ASSERT_EQ(stimulus.switches.size(), 16);
    EXPECT_EQ(stimulus.switches.at(8).switchId, QStringLiteral("di8"));
    EXPECT_EQ(stimulus.switches.at(8).activeLevel, QStringLiteral("Low"));

    const ActionResult rejected = controller.setDigitalStimulus(
        QStringLiteral("di0"), true, 0);
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
    safeState.insert(QStringLiteral("DUT_DI0_STIM"), QStringLiteral("High"));
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
        QStringLiteral("di0"), true, stimulus.revision);
    ASSERT_TRUE(set.ok) << set.message.toStdString();
    stimulus = controller.snapshot().digitalStimulus;
    EXPECT_EQ(stimulus.appliedMask, 1u);
    EXPECT_EQ(stimulus.revision, 2u);

    const ActionResult stale = controller.setDigitalStimulus(
        QStringLiteral("di1"), true, 1);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(stale.code, QStringLiteral("DataMismatch"));
    EXPECT_EQ(controller.snapshot().digitalStimulus.appliedMask, 1u);

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
        QStringLiteral("di0"), true, revision).ok);
    ASSERT_EQ(controller.snapshot().digitalStimulus.appliedMask, 1u);
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
        QStringLiteral("di0"), true, revision).ok);
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
    options.intervalMs = 1000;
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
    EXPECT_TRUE(savedInfo.fileName().startsWith(QStringLiteral("ElectricalHealth_data_")));
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
    EXPECT_TRUE(text.contains(QStringLiteral("# repeat_delay_ms=1000\n")));
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
    EXPECT_TRUE(dataLines.at(1).contains(QStringLiteral("\t4660\t0\t0x0000\t")));
    EXPECT_TRUE(dataLines.at(1).contains(QStringLiteral("\t11.1\t12.2\t")));
    EXPECT_TRUE(dataLines.at(2).contains(QStringLiteral("\t4661\t0\t0x0000\t")));
    EXPECT_TRUE(dataLines.at(2).contains(QStringLiteral("\t21.1\t22.2\t")));
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
    invalidOptions.intervalMs = 9;
    invalidOptions.maxCycles = 2;
    const ActionResult rejected = controller.start(invalidOptions);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.code, QStringLiteral("ParameterRangeError"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));

    TestRunOptions options;
    options.mode = QStringLiteral("pc_periodic");
    options.intervalMs = 10;
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
    EXPECT_EQ(finished.intervalMs, 10);
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

} // namespace
} // namespace hwtest::app
