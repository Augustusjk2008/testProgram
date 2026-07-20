#pragma once

#include <app/frontend_launch_options.h>

#include <QMainWindow>

class QCloseEvent;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTextEdit;
class QToolButton;

namespace hwtest::app {

class GuiMainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit GuiMainWindow(TestApplicationController* controller,
                           FrontendLaunchOptions launchOptions,
                           QWidget* parent = nullptr);

public slots:
    void loadConfigurations();
    void refreshSerialPorts();
    void selectControl(int index);
    void selectSerialPort();
    void prepareTest();
    void startTest();
    void pauseTest();
    void resumeTest();
    void stopTest();
    void disconnectTest();
    void applySnapshot(const hwtest::app::ApplicationSnapshot& snapshot);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void populateControls();
    void handleAction(const QString& action, const ActionResult& result);
    void handleStopCompleted(const ActionResult& result);
    void requestShutdown(bool closeAfterShutdown);
    void completePendingShutdown();
    void schedulePendingShutdown();
    void updateActionAvailability(const ApplicationSnapshot& snapshot);

    TestApplicationController* m_controller = nullptr;
    FrontendLaunchOptions m_launchOptions;
    ApplicationSnapshot m_snapshot;

    QLineEdit* m_testConfigEdit = nullptr;
    QLineEdit* m_halConfigEdit = nullptr;
    QToolButton* m_browseTestButton = nullptr;
    QToolButton* m_browseHalButton = nullptr;
    QPushButton* m_loadButton = nullptr;
    QComboBox* m_controlCombo = nullptr;
    QComboBox* m_serialCombo = nullptr;
    QToolButton* m_refreshPortsButton = nullptr;
    QPushButton* m_prepareButton = nullptr;
    QPushButton* m_runButton = nullptr;
    QToolButton* m_pauseButton = nullptr;
    QToolButton* m_resumeButton = nullptr;
    QToolButton* m_stopButton = nullptr;
    QPushButton* m_disconnectButton = nullptr;
    QLabel* m_phaseValue = nullptr;
    QLabel* m_testStateValue = nullptr;
    QLabel* m_connectionValue = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QLabel* m_errorValue = nullptr;
    QGroupBox* m_resultGroup = nullptr;
    QLabel* m_resultIdentityValue = nullptr;
    QLabel* m_verdictValue = nullptr;
    QLabel* m_attemptsValue = nullptr;
    QLabel* m_resultMessageValue = nullptr;
    QTextEdit* m_rawDataEdit = nullptr;

    bool m_closePending = false;
    bool m_shutdownComplete = false;
    bool m_shutdownPending = false;
    bool m_shutdownInProgress = false;
    bool m_shutdownScheduled = false;
    bool m_stopRequestInProgress = false;
};

} // namespace hwtest::app
