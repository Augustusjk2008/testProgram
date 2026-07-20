#include <app/test_application_controller.h>
#include <app/tui_shell.h>

#include "support/mbddf_udp_test_peer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
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
