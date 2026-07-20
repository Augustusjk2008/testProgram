#include "gui_main_window.h"

#include <app/tui_shell.h>

#include "support/mbddf_udp_test_peer.h"

#include <gtest/gtest.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <functional>

namespace hwtest::app {
namespace {

FrontendLaunchOptions defaultOptions()
{
    return {QStringLiteral(HWTEST_APP_TEST_CONFIG),
            QStringLiteral(HWTEST_APP_HAL_CONFIG),
            {},
            {}};
}

template <typename Widget>
Widget* requiredWidget(GuiMainWindow* window, const char* objectName)
{
    Widget* widget = window->findChild<Widget*>(QString::fromLatin1(objectName));
    EXPECT_NE(widget, nullptr) << objectName;
    return widget;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

bool prepareUdpPeer(test::MbddfUdpTestPeer* peer,
                    QTemporaryDir* directory,
                    QString* halConfigPath,
                    QString* error)
{
    return peer->bind(error) &&
        peer->writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                             directory,
                             halConfigPath,
                             error);
}

void expectEquivalentSnapshot(const ApplicationSnapshot& tui,
                              const ApplicationSnapshot& gui)
{
    EXPECT_EQ(tui.phase, gui.phase);
    EXPECT_EQ(tui.testState, gui.testState);
    EXPECT_EQ(tui.controlResourceId, gui.controlResourceId);
    EXPECT_EQ(tui.providerId, gui.providerId);
    EXPECT_EQ(tui.serialPortName, gui.serialPortName);
    EXPECT_EQ(tui.stepId, gui.stepId);
    EXPECT_EQ(tui.testItemId, gui.testItemId);
    EXPECT_EQ(tui.algorithmId, gui.algorithmId);
    EXPECT_EQ(tui.hasResult, gui.hasResult);
    EXPECT_EQ(tui.verdict, gui.verdict);
    EXPECT_EQ(tui.errorCode, gui.errorCode);
    EXPECT_EQ(tui.message, gui.message);
    EXPECT_EQ(tui.attempts, gui.attempts);
    EXPECT_TRUE(tui.rawData == gui.rawData);
}

enum class RunScenario {
    Pass,
    Timeout,
    Stop
};

struct ScenarioResult {
    bool ok = false;
    QString error;
    ApplicationSnapshot snapshot;
};

ScenarioResult runTuiScenario(RunScenario scenario)
{
    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString halConfigPath;
    QString error;
    if (!directory.isValid() ||
        !prepareUdpPeer(&peer, &directory, &halConfigPath, &error)) {
        return {false, error, {}};
    }

    TestApplicationController controller;
    TuiShell shell(&controller,
                   QStringLiteral(HWTEST_APP_TEST_CONFIG),
                   halConfigPath);
    const auto execute = [&](const QString& command, const QString& expected) {
        const TuiReply reply = shell.execute(command);
        if (reply.lines.size() != 1 || reply.lines.first() != expected) {
            error = QStringLiteral("Unexpected TUI reply for '%1': %2")
                        .arg(command, reply.lines.join(QStringLiteral(" | ")));
            return false;
        }
        return true;
    };

    if (!execute(QStringLiteral("load"), QStringLiteral("ok load")) ||
        !execute(QStringLiteral("prepare"), QStringLiteral("ok prepare")) ||
        !execute(QStringLiteral("run"), QStringLiteral("ok run")) ||
        !peer.waitForRequest(3000, &error)) {
        return {false, error, {}};
    }

    if (scenario == RunScenario::Pass && !peer.replyToLastRequest(&error)) {
        return {false, error, {}};
    }
    if (scenario == RunScenario::Stop &&
        !execute(QStringLiteral("stop 5000"), QStringLiteral("ok stop"))) {
        return {false, error, {}};
    }

    const TuiReply waited = shell.execute(QStringLiteral("wait 5000"));
    const QString expectedPhase = scenario == RunScenario::Pass
        ? QStringLiteral("finished")
        : scenario == RunScenario::Timeout
            ? QStringLiteral("error")
            : QStringLiteral("stopped");
    if (controller.snapshot().phase != expectedPhase) {
        return {false,
                QStringLiteral("TUI reached phase '%1' after wait reply '%2'")
                    .arg(controller.snapshot().phase,
                         waited.lines.join(QStringLiteral(" | "))),
                {}};
    }

    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const ApplicationSnapshot snapshot = controller.snapshot();
    const ActionResult shutdown = controller.shutdown();
    return shutdown.ok
        ? ScenarioResult{true, {}, snapshot}
        : ScenarioResult{false, shutdown.message, {}};
}

ScenarioResult runGuiScenario(RunScenario scenario)
{
    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString halConfigPath;
    QString error;
    if (!directory.isValid() ||
        !prepareUdpPeer(&peer, &directory, &halConfigPath, &error)) {
        return {false, error, {}};
    }

    TestApplicationController controller;
    FrontendLaunchOptions options = defaultOptions();
    options.halConfigPath = halConfigPath;
    GuiMainWindow window(&controller, options);
    window.loadConfigurations();
    window.prepareTest();
    window.startTest();
    if (controller.snapshot().phase != QStringLiteral("running") ||
        !peer.waitForRequest(3000, &error)) {
        return {false,
                error.isEmpty() ? window.statusBar()->currentMessage() : error,
                {}};
    }

    if (scenario == RunScenario::Pass && !peer.replyToLastRequest(&error)) {
        return {false, error, {}};
    }
    if (scenario == RunScenario::Stop) {
        window.stopTest();
    }
    const QString expectedPhase = scenario == RunScenario::Pass
        ? QStringLiteral("finished")
        : scenario == RunScenario::Timeout
            ? QStringLiteral("error")
            : QStringLiteral("stopped");
    if (!waitUntil([&] { return controller.snapshot().phase == expectedPhase; },
                   5000)) {
        return {false,
                QStringLiteral("GUI did not reach phase '%1'; current phase is '%2'")
                    .arg(expectedPhase, controller.snapshot().phase),
                {}};
    }

    const ApplicationSnapshot snapshot = controller.snapshot();
    const ActionResult shutdown = controller.shutdown();
    return shutdown.ok
        ? ScenarioResult{true, {}, snapshot}
        : ScenarioResult{false, shutdown.message, {}};
}

QString readProjectFile(const QString& relativePath)
{
    QFile file(QDir(QStringLiteral(HWTEST_PROJECT_SOURCE_DIR)).filePath(relativePath));
    if (!file.open(QIODevice::ReadOnly)) {
        ADD_FAILURE() << file.errorString().toStdString();
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

TEST(GuiMainWindowTest, StartsWithoutOpeningHardware)
{
    TestApplicationController controller;
    GuiMainWindow window(&controller, defaultOptions());

    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
    EXPECT_EQ(requiredWidget<QLabel>(&window, "phaseValue")->text(),
              QStringLiteral("empty"));
    EXPECT_TRUE(requiredWidget<QPushButton>(&window, "loadButton")->isEnabled());
    EXPECT_FALSE(requiredWidget<QPushButton>(&window, "prepareButton")->isEnabled());
    EXPECT_FALSE(requiredWidget<QPushButton>(&window, "runButton")->isEnabled());
}

TEST(GuiMainWindowTest, LoadPopulatesControlsAndConfiguredState)
{
    TestApplicationController controller;
    GuiMainWindow window(&controller, defaultOptions());

    window.loadConfigurations();

    ASSERT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
    QComboBox* controls = requiredWidget<QComboBox>(&window, "controlCombo");
    ASSERT_EQ(controls->count(), 2);
    EXPECT_GE(controls->findData(QStringLiteral("CONTROL_SERIAL")), 0);
    EXPECT_GE(controls->findData(QStringLiteral("CONTROL_NETWORK")), 0);
    EXPECT_EQ(controls->currentData().toString(), QStringLiteral("CONTROL_SERIAL"));
    EXPECT_TRUE(requiredWidget<QPushButton>(&window, "prepareButton")->isEnabled());
    EXPECT_EQ(requiredWidget<QLabel>(&window, "testStateValue")->text(),
              QStringLiteral("Uninitialized"));
}

TEST(GuiMainWindowTest, RefreshesAndSelectsSerialPort)
{
    TestApplicationController controller;
    GuiMainWindow window(&controller, defaultOptions());
    window.loadConfigurations();
    QComboBox* serial = requiredWidget<QComboBox>(&window, "serialCombo");
    ASSERT_TRUE(serial->isEditable());

    serial->setEditText(QStringLiteral("COM74"));
    window.selectSerialPort();
    window.refreshSerialPorts();

    EXPECT_EQ(controller.snapshot().serialPortName, QStringLiteral("COM74"));
    EXPECT_EQ(serial->currentText(), QStringLiteral("COM74"));
    EXPECT_TRUE(serial->isEnabled());
}

TEST(GuiMainWindowTest, ShowsConfiguredControlsWhenLaunchOverrideIsInvalid)
{
    TestApplicationController controller;
    FrontendLaunchOptions options = defaultOptions();
    options.controlResourceId = QStringLiteral("CONTROL_UNKNOWN");
    GuiMainWindow window(&controller, options);

    window.loadConfigurations();

    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
    EXPECT_EQ(requiredWidget<QComboBox>(&window, "controlCombo")->count(), 2);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(
        QStringLiteral("control_not_found")));
}

TEST(GuiMainWindowTest, ClearsSerialEditorWhenSnapshotSelectsUdp)
{
    TestApplicationController controller;
    GuiMainWindow window(&controller, defaultOptions());
    QComboBox* serial = requiredWidget<QComboBox>(&window, "serialCombo");
    serial->setEditText(QStringLiteral("COM74"));
    ApplicationSnapshot snapshot;
    snapshot.phase = QStringLiteral("configured");
    snapshot.controlResourceId = QStringLiteral("CONTROL_NETWORK");
    snapshot.providerId = QStringLiteral("qt.udp");

    window.applySnapshot(snapshot);

    EXPECT_TRUE(serial->currentText().isEmpty());
}

TEST(GuiMainWindowTest, ButtonAvailabilityFollowsSnapshot)
{
    TestApplicationController controller;
    GuiMainWindow window(&controller, defaultOptions());
    QPushButton* load = requiredWidget<QPushButton>(&window, "loadButton");
    QPushButton* prepare = requiredWidget<QPushButton>(&window, "prepareButton");
    QPushButton* run = requiredWidget<QPushButton>(&window, "runButton");
    QToolButton* pause = requiredWidget<QToolButton>(&window, "pauseButton");
    QToolButton* resume = requiredWidget<QToolButton>(&window, "resumeButton");
    QToolButton* stop = requiredWidget<QToolButton>(&window, "stopButton");
    QPushButton* disconnect = requiredWidget<QPushButton>(&window, "disconnectButton");

    ApplicationSnapshot snapshot;
    snapshot.phase = QStringLiteral("configured");
    snapshot.providerId = QStringLiteral("qt.serial");
    window.applySnapshot(snapshot);
    EXPECT_TRUE(load->isEnabled());
    EXPECT_TRUE(prepare->isEnabled());
    EXPECT_FALSE(run->isEnabled());
    EXPECT_TRUE(requiredWidget<QComboBox>(&window, "controlCombo")->isEnabled());
    EXPECT_TRUE(requiredWidget<QComboBox>(&window, "serialCombo")->isEnabled());

    snapshot.phase = QStringLiteral("ready");
    window.applySnapshot(snapshot);
    EXPECT_FALSE(load->isEnabled());
    EXPECT_FALSE(prepare->isEnabled());
    EXPECT_TRUE(run->isEnabled());
    EXPECT_TRUE(disconnect->isEnabled());

    snapshot.phase = QStringLiteral("running");
    window.applySnapshot(snapshot);
    EXPECT_TRUE(pause->isEnabled());
    EXPECT_FALSE(resume->isEnabled());
    EXPECT_TRUE(stop->isEnabled());

    snapshot.phase = QStringLiteral("paused");
    window.applySnapshot(snapshot);
    EXPECT_FALSE(pause->isEnabled());
    EXPECT_TRUE(resume->isEnabled());
    EXPECT_TRUE(stop->isEnabled());

    snapshot.phase = QStringLiteral("stopping");
    window.applySnapshot(snapshot);
    EXPECT_FALSE(run->isEnabled());
    EXPECT_FALSE(pause->isEnabled());
    EXPECT_FALSE(resume->isEnabled());
    EXPECT_FALSE(stop->isEnabled());
    EXPECT_TRUE(disconnect->isEnabled());
}

TEST(GuiMainWindowTest, DisplaysControllerErrorsWithoutChangingVerdict)
{
    TestApplicationController controller;
    GuiMainWindow window(&controller, defaultOptions());
    ApplicationSnapshot displayed;
    displayed.phase = QStringLiteral("finished");
    displayed.hasResult = true;
    displayed.verdict = QStringLiteral("Pass");
    displayed.stepId = QStringLiteral("SYSTEM_STATUS");
    window.applySnapshot(displayed);

    window.startTest();

    EXPECT_TRUE(window.statusBar()->currentMessage().contains(
        QStringLiteral("invalid_state")));
    EXPECT_EQ(requiredWidget<QLabel>(&window, "verdictValue")->text(),
              QStringLiteral("Pass"));
}

TEST(GuiMainWindowTest, StopConvergesWithoutBlockingTheGuiEventLoop)
{
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
    FrontendLaunchOptions options = defaultOptions();
    options.halConfigPath = halConfigPath;
    GuiMainWindow window(&controller, options);
    window.loadConfigurations();
    window.prepareTest();
    window.startTest();
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();

    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeatCount; });
    heartbeat.start();

    QElapsedTimer callTimer;
    callTimer.start();
    window.stopTest();
    const qint64 callDurationMs = callTimer.elapsed();

    EXPECT_LT(callDurationMs, 250);
    ASSERT_TRUE(waitUntil([&] {
        return controller.snapshot().phase == QStringLiteral("stopped");
    }, 5000));
    EXPECT_GE(heartbeatCount, 2);
    EXPECT_TRUE(requiredWidget<QPushButton>(&window, "runButton")->isEnabled());
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(GuiMainWindowTest, RunsSystemStatusThroughUdpAndDisplaysResult)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString halConfigPath;
    QString error;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(prepareUdpPeer(&peer, &directory, &halConfigPath, &error))
        << error.toStdString();

    TestApplicationController controller;
    FrontendLaunchOptions options = defaultOptions();
    options.halConfigPath = halConfigPath;
    GuiMainWindow window(&controller, options);
    window.loadConfigurations();
    window.prepareTest();
    window.startTest();
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    ASSERT_TRUE(peer.replyToLastRequest(&error)) << error.toStdString();
    ASSERT_TRUE(waitUntil([&] {
        return controller.snapshot().phase == QStringLiteral("finished");
    }, 5000));

    EXPECT_EQ(requiredWidget<QLabel>(&window, "phaseValue")->text(),
              QStringLiteral("finished"));
    EXPECT_EQ(requiredWidget<QLabel>(&window, "verdictValue")->text(),
              QStringLiteral("Pass"));
    EXPECT_TRUE(requiredWidget<QGroupBox>(&window, "resultGroup")->isEnabled());
    const QString rawData = requiredWidget<QTextEdit>(&window, "rawDataEdit")->toPlainText();
    EXPECT_TRUE(rawData.contains(QStringLiteral("requestFrameHex")));
    EXPECT_TRUE(rawData.contains(QStringLiteral("responseValues")));
    EXPECT_TRUE(controller.shutdown().ok);
}

TEST(GuiMainWindowTest, CloseRequestsStopThenShutdown)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString halConfigPath;
    QString error;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(prepareUdpPeer(&peer, &directory, &halConfigPath, &error))
        << error.toStdString();

    TestApplicationController controller;
    FrontendLaunchOptions options = defaultOptions();
    options.halConfigPath = halConfigPath;
    GuiMainWindow window(&controller, options);
    window.show();
    QCoreApplication::processEvents();
    window.loadConfigurations();
    window.prepareTest();
    window.startTest();
    ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();

    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeatCount; });
    heartbeat.start();
    QElapsedTimer closeTimer;
    closeTimer.start();
    EXPECT_FALSE(window.close());
    EXPECT_LT(closeTimer.elapsed(), 250);
    EXPECT_TRUE(window.isVisible());

    ASSERT_TRUE(waitUntil([&] { return !window.isVisible(); }, 5000));
    EXPECT_GE(heartbeatCount, 2);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
}

TEST(FrontendEquivalenceTest, TuiAndGuiProduceEquivalentConfiguredSnapshot)
{
    TestApplicationController tuiController;
    TuiShell shell(&tuiController,
                   QStringLiteral(HWTEST_APP_TEST_CONFIG),
                   QStringLiteral(HWTEST_APP_HAL_CONFIG));
    const TuiReply loaded = shell.execute(QStringLiteral("load"));
    ASSERT_EQ(loaded.lines.size(), 1);
    ASSERT_EQ(loaded.lines.first(), QStringLiteral("ok load"));

    TestApplicationController guiController;
    GuiMainWindow window(&guiController, defaultOptions());
    window.loadConfigurations();

    expectEquivalentSnapshot(tuiController.snapshot(), guiController.snapshot());
    EXPECT_TRUE(tuiController.shutdown().ok);
    EXPECT_TRUE(guiController.shutdown().ok);
}

TEST(FrontendEquivalenceTest, TuiAndGuiProduceEquivalentUdpPassResult)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }
    const ScenarioResult tui = runTuiScenario(RunScenario::Pass);
    ASSERT_TRUE(tui.ok) << tui.error.toStdString();
    const ScenarioResult gui = runGuiScenario(RunScenario::Pass);
    ASSERT_TRUE(gui.ok) << gui.error.toStdString();
    expectEquivalentSnapshot(tui.snapshot, gui.snapshot);
}

TEST(FrontendEquivalenceTest, TuiAndGuiProduceEquivalentTimeoutError)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }
    const ScenarioResult tui = runTuiScenario(RunScenario::Timeout);
    ASSERT_TRUE(tui.ok) << tui.error.toStdString();
    const ScenarioResult gui = runGuiScenario(RunScenario::Timeout);
    ASSERT_TRUE(gui.ok) << gui.error.toStdString();
    expectEquivalentSnapshot(tui.snapshot, gui.snapshot);
}

TEST(FrontendEquivalenceTest, TuiAndGuiConvergeToEquivalentStoppedState)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }
    const ScenarioResult tui = runTuiScenario(RunScenario::Stop);
    ASSERT_TRUE(tui.ok) << tui.error.toStdString();
    const ScenarioResult gui = runGuiScenario(RunScenario::Stop);
    ASSERT_TRUE(gui.ok) << gui.error.toStdString();
    expectEquivalentSnapshot(tui.snapshot, gui.snapshot);
}

TEST(GuiArchitectureTest, LinksOnlyAppCoreAndQtWidgets)
{
    const QString guiSources =
        readProjectFile(QStringLiteral("src/app/gui/gui_main_window.h")) +
        readProjectFile(QStringLiteral("src/app/gui/gui_main_window.cpp")) +
        readProjectFile(QStringLiteral("src/app/gui/gui_main.cpp"));
    for (const QString& forbidden : {
             QStringLiteral("<hal/"),
             QStringLiteral("<biz/"),
             QStringLiteral("<algorithm/"),
             QStringLiteral("QSerialPort"),
             QStringLiteral("QUdpSocket"),
             QStringLiteral("QTcpSocket"),
             QStringLiteral("waitForTerminal(")}) {
        EXPECT_FALSE(guiSources.contains(forbidden)) << forbidden.toStdString();
    }

    const QString cmake = readProjectFile(QStringLiteral("src/app/CMakeLists.txt"));
    const int linkStart = cmake.indexOf(
        QStringLiteral("target_link_libraries(hwtest_gui_support"));
    ASSERT_GE(linkStart, 0);
    const int linkEnd = cmake.indexOf(QLatin1Char(')'), linkStart);
    ASSERT_GT(linkEnd, linkStart);
    const QString linkBlock = cmake.mid(linkStart, linkEnd - linkStart + 1);
    EXPECT_TRUE(linkBlock.contains(QStringLiteral("hwtest_app_core")));
    EXPECT_TRUE(linkBlock.contains(QStringLiteral("${HWTEST_QT_WIDGETS_TARGET}")));
    for (const QString& forbidden : {
             QStringLiteral("hwtest_hal"),
             QStringLiteral("hwtest_biz"),
             QStringLiteral("hwtest_algorithm"),
             QStringLiteral("HWTEST_QT_NETWORK_TARGET"),
             QStringLiteral("HWTEST_QT_SERIALPORT_TARGET")}) {
        EXPECT_FALSE(linkBlock.contains(forbidden)) << forbidden.toStdString();
    }
}

} // namespace
} // namespace hwtest::app
