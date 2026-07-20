#include <app/test_application_controller.h>
#include <app/tui_shell.h>

#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

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

QString udpHalConfig(QTemporaryDir* directory, quint16 peerPort)
{
    QFile source(QStringLiteral(HWTEST_APP_HAL_CONFIG));
    EXPECT_TRUE(source.open(QIODevice::ReadOnly));
    QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    EXPECT_TRUE(document.isObject());
    QVariantMap root = document.object().toVariantMap();
    root.remove(QStringLiteral("logging"));

    QVariantMap control = root.value(QStringLiteral("control")).toMap();
    control.insert(QStringLiteral("resourceId"), QStringLiteral("CONTROL_NETWORK"));
    root.insert(QStringLiteral("control"), control);

    QVariantMap hardware = root.value(QStringLiteral("hardware")).toMap();
    QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    QVariantMap network = resources.value(QStringLiteral("CONTROL_NETWORK")).toMap();
    QVariantMap properties = network.value(QStringLiteral("properties")).toMap();
    properties.insert(QStringLiteral("localAddress"), QStringLiteral("127.0.0.1"));
    properties.insert(QStringLiteral("localPort"), 0);
    properties.insert(QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1"));
    properties.insert(QStringLiteral("remotePort"), static_cast<int>(peerPort));
    network.insert(QStringLiteral("properties"), properties);
    resources.insert(QStringLiteral("CONTROL_NETWORK"), network);
    hardware.insert(QStringLiteral("resources"), resources);
    root.insert(QStringLiteral("hardware"), hardware);

    const QString path = directory->filePath(QStringLiteral("udp-hal.json"));
    QFile output(path);
    EXPECT_TRUE(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
    EXPECT_GT(output.write(QJsonDocument(QJsonObject::fromVariantMap(root)).toJson()), 0);
    output.close();
    return path;
}

bool makeSystemStatusResponse(const QByteArray& request,
                              QByteArray* response,
                              QString* error)
{
    hwtest::algorithm::mbddf::ProtocolCatalog catalog;
    if (!catalog.loadFromDirectory(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR"), error)) {
        return false;
    }

    hwtest::algorithm::mbddf::SystemStatusSimulator simulator(&catalog);
    simulator.setResponseValues({
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("cpu_usage"), 12.5},
        {QStringLiteral("mem_usage"), 25.0},
        {QStringLiteral("power_on_sec"), 99u},
    });
    if (!simulator.open(error)) {
        return false;
    }
    const hwtest::algorithm::mbddf::TransportResult result = simulator.transact(request, 1000);
    simulator.close();
    if (!result.ok) {
        if (error != nullptr) {
            *error = result.error;
        }
        return false;
    }
    *response = result.frame;
    return true;
}

void expectSingleLine(const TuiReply& reply, const QString& expected)
{
    ASSERT_EQ(reply.lines.size(), 1);
    EXPECT_EQ(reply.lines.first(), expected);
}

TEST(TestApplicationControllerTest, RejectsPreparationBeforeConfigurationsAreLoaded)
{
    TestApplicationController controller;

    const ActionResult result = controller.prepare();

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("invalid_state"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
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

TEST(TestApplicationControllerTest, RunsSystemStatusThroughTheSelectedUdpControlResource)
{
    ensureQtApplication();
    const QString assets = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    TestApplicationController controller;
    ASSERT_EQ(controller.thread(), QThread::currentThread());
    ASSERT_EQ(QCoreApplication::instance()->thread(), QThread::currentThread());
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                udpHalConfig(&directory, peer.localPort())).ok);
    ASSERT_TRUE(controller.prepare().ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    EXPECT_EQ(controller.snapshot().testState, QStringLiteral("Idle"));
    EXPECT_FALSE(controller.selectControl(QStringLiteral("CONTROL_SERIAL")).ok);

    const ActionResult started = controller.start();
    ASSERT_TRUE(started.ok) << started.message.toStdString();
    EXPECT_FALSE(controller.snapshot().taskId.isEmpty());

    ASSERT_TRUE(peer.waitForReadyRead(3000));
    const qint64 size = peer.pendingDatagramSize();
    ASSERT_GT(size, 0);
    QByteArray frame(static_cast<int>(size), Qt::Uninitialized);
    QHostAddress sender;
    quint16 senderPort = 0;
    ASSERT_EQ(peer.readDatagram(frame.data(), frame.size(), &sender, &senderPort), size);
    QByteArray response;
    QString responseError;
    ASSERT_TRUE(makeSystemStatusResponse(frame, &response, &responseError))
        << responseError.toStdString();
    ASSERT_NE(response, frame);
    ASSERT_EQ(peer.writeDatagram(response, sender, senderPort), response.size());

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

TEST(TuiShellTest, RunsTheStagedSystemStatusWorkflowThroughUdp)
{
    ensureQtApplication();
    const QString assets = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    TestApplicationController controller;
    TuiShell shell(&controller,
                   QStringLiteral(HWTEST_APP_TEST_CONFIG),
                   udpHalConfig(&directory, peer.localPort()));

    expectSingleLine(shell.execute(QStringLiteral("load")), QStringLiteral("ok load"));
    expectSingleLine(shell.execute(QStringLiteral("prepare")), QStringLiteral("ok prepare"));
    expectSingleLine(shell.execute(QStringLiteral("run")), QStringLiteral("ok run"));

    ASSERT_TRUE(peer.waitForReadyRead(3000));
    const qint64 size = peer.pendingDatagramSize();
    ASSERT_GT(size, 0);
    QByteArray frame(static_cast<int>(size), Qt::Uninitialized);
    QHostAddress sender;
    quint16 senderPort = 0;
    ASSERT_EQ(peer.readDatagram(frame.data(), frame.size(), &sender, &senderPort), size);
    QByteArray response;
    QString responseError;
    ASSERT_TRUE(makeSystemStatusResponse(frame, &response, &responseError))
        << responseError.toStdString();
    ASSERT_NE(response, frame);
    ASSERT_EQ(peer.writeDatagram(response, sender, senderPort), response.size());

    expectSingleLine(shell.execute(QStringLiteral("wait 3000")), QStringLiteral("ok wait"));
    const TuiReply result = shell.execute(QStringLiteral("result"));
    ASSERT_EQ(result.lines.size(), 3);
    EXPECT_TRUE(result.lines.at(0).contains(QStringLiteral("step=SYSTEM_STATUS")));
    EXPECT_TRUE(result.lines.at(0).contains(QStringLiteral("item=system_status")));
    EXPECT_TRUE(result.lines.at(1).contains(QStringLiteral("verdict=Pass")));
    EXPECT_TRUE(result.lines.at(2).startsWith(QStringLiteral("rawData={")));
    expectSingleLine(shell.execute(QStringLiteral("disconnect")),
                     QStringLiteral("ok disconnect"));
}

TEST(TuiShellTest, StopThenWaitConvergesToStoppedInsteadOfTimingOut)
{
    ensureQtApplication();
    const QString assets = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    TestApplicationController controller;
    TuiShell shell(&controller,
                   QStringLiteral(HWTEST_APP_TEST_CONFIG),
                   udpHalConfig(&directory, peer.localPort()));
    expectSingleLine(shell.execute(QStringLiteral("load")), QStringLiteral("ok load"));
    expectSingleLine(shell.execute(QStringLiteral("prepare")), QStringLiteral("ok prepare"));
    expectSingleLine(shell.execute(QStringLiteral("run")), QStringLiteral("ok run"));
    ASSERT_TRUE(peer.waitForReadyRead(3000));

    expectSingleLine(shell.execute(QStringLiteral("stop 3000")), QStringLiteral("ok stop"));
    expectSingleLine(shell.execute(QStringLiteral("wait 3000")), QStringLiteral("ok wait"));
    const TuiReply status = shell.execute(QStringLiteral("status"));
    ASSERT_EQ(status.lines.size(), 1);
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("phase=stopped")));
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("state=Idle")));
    expectSingleLine(shell.execute(QStringLiteral("disconnect")),
                     QStringLiteral("ok disconnect"));
}

TEST(TestApplicationControllerTest, AsyncPreparationFailureIsReturnedByWaitWithoutFabricatingAResult)
{
    ensureQtApplication();
    ScopedEnvironmentVariable invalidAssets(
        "MB_DDF_PROTOCOL_CSV_DIR",
        QByteArray("H:/definitely-missing-mbddf-assets"));
    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                udpHalConfig(&directory, peer.localPort())).ok);
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

    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                                udpHalConfig(&directory, peer.localPort())).ok);
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

} // namespace
} // namespace hwtest::app
