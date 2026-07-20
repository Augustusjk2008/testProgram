#include "gui_main_window.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace hwtest::app {

namespace {

QString displayValue(const QString& value)
{
    return value.isEmpty() ? QStringLiteral("-") : value;
}

void configureIconButton(QToolButton* button,
                         const QIcon& icon,
                         const QString& tooltip,
                         const QString& objectName)
{
    button->setObjectName(objectName);
    button->setIcon(icon);
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setFixedSize(34, 34);
    button->setIconSize(QSize(18, 18));
}

} // namespace

GuiMainWindow::GuiMainWindow(TestApplicationController* controller,
                             FrontendLaunchOptions launchOptions,
                             QWidget* parent)
    : QMainWindow(parent)
    , m_controller(controller)
    , m_launchOptions(std::move(launchOptions))
{
    buildUi();
    if (m_controller != nullptr) {
        connect(m_controller,
                &TestApplicationController::snapshotChanged,
                this,
                &GuiMainWindow::applySnapshot);
        connect(m_controller,
                &TestApplicationController::stopCompleted,
                this,
                &GuiMainWindow::handleStopCompleted);
        applySnapshot(m_controller->snapshot());
    } else {
        applySnapshot({});
        statusBar()->showMessage(QStringLiteral("应用控制器不可用"));
    }
}

void GuiMainWindow::buildUi()
{
    setObjectName(QStringLiteral("guiMainWindow"));
    setWindowTitle(QStringLiteral("hwtest 硬件测试"));
    setMinimumSize(820, 640);
    resize(980, 760);

    auto* central = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(central);
    pageLayout->setContentsMargins(18, 16, 18, 12);
    pageLayout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("MB_DDF SYSTEM_STATUS"), central);
    title->setObjectName(QStringLiteral("titleLabel"));
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);
    pageLayout->addWidget(title);

    auto* configurationGroup = new QGroupBox(QStringLiteral("配置"), central);
    auto* configurationLayout = new QGridLayout(configurationGroup);
    configurationLayout->setColumnStretch(1, 1);

    m_testConfigEdit = new QLineEdit(m_launchOptions.testConfigPath, configurationGroup);
    m_testConfigEdit->setObjectName(QStringLiteral("testConfigEdit"));
    m_testConfigEdit->setClearButtonEnabled(true);
    m_halConfigEdit = new QLineEdit(m_launchOptions.halConfigPath, configurationGroup);
    m_halConfigEdit->setObjectName(QStringLiteral("halConfigEdit"));
    m_halConfigEdit->setClearButtonEnabled(true);

    m_browseTestButton = new QToolButton(configurationGroup);
    configureIconButton(m_browseTestButton,
                        style()->standardIcon(QStyle::SP_DialogOpenButton),
                        QStringLiteral("选择测试配置"),
                        QStringLiteral("browseTestButton"));
    m_browseHalButton = new QToolButton(configurationGroup);
    configureIconButton(m_browseHalButton,
                        style()->standardIcon(QStyle::SP_DialogOpenButton),
                        QStringLiteral("选择 HAL 配置"),
                        QStringLiteral("browseHalButton"));

    m_loadButton = new QPushButton(
        style()->standardIcon(QStyle::SP_DialogOpenButton),
        QStringLiteral("加载配置"),
        configurationGroup);
    m_loadButton->setObjectName(QStringLiteral("loadButton"));
    m_loadButton->setMinimumHeight(34);

    configurationLayout->addWidget(new QLabel(QStringLiteral("测试配置"), configurationGroup),
                                   0, 0);
    configurationLayout->addWidget(m_testConfigEdit, 0, 1);
    configurationLayout->addWidget(m_browseTestButton, 0, 2);
    configurationLayout->addWidget(new QLabel(QStringLiteral("HAL 配置"), configurationGroup),
                                   1, 0);
    configurationLayout->addWidget(m_halConfigEdit, 1, 1);
    configurationLayout->addWidget(m_browseHalButton, 1, 2);
    configurationLayout->addWidget(m_loadButton, 0, 3, 2, 1);
    pageLayout->addWidget(configurationGroup);

    auto* connectionGroup = new QGroupBox(QStringLiteral("连接"), central);
    auto* connectionLayout = new QGridLayout(connectionGroup);
    connectionLayout->setColumnStretch(1, 1);
    connectionLayout->setColumnStretch(3, 1);

    m_controlCombo = new QComboBox(connectionGroup);
    m_controlCombo->setObjectName(QStringLiteral("controlCombo"));
    m_controlCombo->setMinimumContentsLength(22);
    m_serialCombo = new QComboBox(connectionGroup);
    m_serialCombo->setObjectName(QStringLiteral("serialCombo"));
    m_serialCombo->setEditable(true);
    m_serialCombo->setInsertPolicy(QComboBox::NoInsert);
    m_serialCombo->setMinimumContentsLength(20);
    m_refreshPortsButton = new QToolButton(connectionGroup);
    configureIconButton(m_refreshPortsButton,
                        style()->standardIcon(QStyle::SP_BrowserReload),
                        QStringLiteral("刷新串口"),
                        QStringLiteral("refreshPortsButton"));

    connectionLayout->addWidget(new QLabel(QStringLiteral("控制资源"), connectionGroup),
                                0, 0);
    connectionLayout->addWidget(m_controlCombo, 0, 1);
    connectionLayout->addWidget(new QLabel(QStringLiteral("串口"), connectionGroup), 0, 2);
    connectionLayout->addWidget(m_serialCombo, 0, 3);
    connectionLayout->addWidget(m_refreshPortsButton, 0, 4);
    pageLayout->addWidget(connectionGroup);

    auto* commandLayout = new QHBoxLayout;
    commandLayout->setSpacing(8);
    m_prepareButton = new QPushButton(
        style()->standardIcon(QStyle::SP_DialogApplyButton),
        QStringLiteral("准备"), central);
    m_prepareButton->setObjectName(QStringLiteral("prepareButton"));
    m_runButton = new QPushButton(
        style()->standardIcon(QStyle::SP_MediaPlay),
        QStringLiteral("运行"), central);
    m_runButton->setObjectName(QStringLiteral("runButton"));
    m_pauseButton = new QToolButton(central);
    configureIconButton(m_pauseButton,
                        style()->standardIcon(QStyle::SP_MediaPause),
                        QStringLiteral("暂停"),
                        QStringLiteral("pauseButton"));
    m_resumeButton = new QToolButton(central);
    configureIconButton(m_resumeButton,
                        style()->standardIcon(QStyle::SP_MediaPlay),
                        QStringLiteral("继续"),
                        QStringLiteral("resumeButton"));
    m_stopButton = new QToolButton(central);
    configureIconButton(m_stopButton,
                        style()->standardIcon(QStyle::SP_MediaStop),
                        QStringLiteral("停止"),
                        QStringLiteral("stopButton"));
    m_disconnectButton = new QPushButton(
        style()->standardIcon(QStyle::SP_DialogCloseButton),
        QStringLiteral("断开"), central);
    m_disconnectButton->setObjectName(QStringLiteral("disconnectButton"));

    for (QPushButton* button : {m_prepareButton, m_runButton, m_disconnectButton}) {
        button->setMinimumHeight(34);
    }
    commandLayout->addWidget(m_prepareButton);
    commandLayout->addWidget(m_runButton);
    commandLayout->addWidget(m_pauseButton);
    commandLayout->addWidget(m_resumeButton);
    commandLayout->addWidget(m_stopButton);
    commandLayout->addStretch(1);
    commandLayout->addWidget(m_disconnectButton);
    pageLayout->addLayout(commandLayout);

    auto* statusGroup = new QGroupBox(QStringLiteral("状态"), central);
    auto* statusLayout = new QGridLayout(statusGroup);
    statusLayout->setColumnStretch(1, 1);
    statusLayout->setColumnStretch(3, 2);
    m_phaseValue = new QLabel(statusGroup);
    m_phaseValue->setObjectName(QStringLiteral("phaseValue"));
    m_testStateValue = new QLabel(statusGroup);
    m_testStateValue->setObjectName(QStringLiteral("testStateValue"));
    m_connectionValue = new QLabel(statusGroup);
    m_connectionValue->setObjectName(QStringLiteral("connectionValue"));
    m_connectionValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_progressBar = new QProgressBar(statusGroup);
    m_progressBar->setObjectName(QStringLiteral("progressBar"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setMinimumHeight(22);
    m_errorValue = new QLabel(statusGroup);
    m_errorValue->setObjectName(QStringLiteral("errorValue"));
    m_errorValue->setWordWrap(true);
    m_errorValue->setTextInteractionFlags(Qt::TextSelectableByMouse);

    statusLayout->addWidget(new QLabel(QStringLiteral("阶段"), statusGroup), 0, 0);
    statusLayout->addWidget(m_phaseValue, 0, 1);
    statusLayout->addWidget(new QLabel(QStringLiteral("测试状态"), statusGroup), 0, 2);
    statusLayout->addWidget(m_testStateValue, 0, 3);
    statusLayout->addWidget(new QLabel(QStringLiteral("连接"), statusGroup), 1, 0);
    statusLayout->addWidget(m_connectionValue, 1, 1, 1, 3);
    statusLayout->addWidget(new QLabel(QStringLiteral("进度"), statusGroup), 2, 0);
    statusLayout->addWidget(m_progressBar, 2, 1, 1, 3);
    statusLayout->addWidget(new QLabel(QStringLiteral("错误"), statusGroup), 3, 0);
    statusLayout->addWidget(m_errorValue, 3, 1, 1, 3);
    pageLayout->addWidget(statusGroup);

    m_resultGroup = new QGroupBox(QStringLiteral("结果"), central);
    m_resultGroup->setObjectName(QStringLiteral("resultGroup"));
    auto* resultLayout = new QGridLayout(m_resultGroup);
    resultLayout->setColumnStretch(1, 1);
    resultLayout->setColumnStretch(3, 1);
    m_resultIdentityValue = new QLabel(m_resultGroup);
    m_resultIdentityValue->setObjectName(QStringLiteral("resultIdentityValue"));
    m_resultIdentityValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_verdictValue = new QLabel(m_resultGroup);
    m_verdictValue->setObjectName(QStringLiteral("verdictValue"));
    m_attemptsValue = new QLabel(m_resultGroup);
    m_attemptsValue->setObjectName(QStringLiteral("attemptsValue"));
    m_resultMessageValue = new QLabel(m_resultGroup);
    m_resultMessageValue->setObjectName(QStringLiteral("resultMessageValue"));
    m_resultMessageValue->setWordWrap(true);
    m_resultMessageValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_rawDataEdit = new QTextEdit(m_resultGroup);
    m_rawDataEdit->setObjectName(QStringLiteral("rawDataEdit"));
    m_rawDataEdit->setReadOnly(true);
    m_rawDataEdit->setLineWrapMode(QTextEdit::NoWrap);
    m_rawDataEdit->setMinimumHeight(120);

    resultLayout->addWidget(new QLabel(QStringLiteral("步骤"), m_resultGroup), 0, 0);
    resultLayout->addWidget(m_resultIdentityValue, 0, 1, 1, 3);
    resultLayout->addWidget(new QLabel(QStringLiteral("判定"), m_resultGroup), 1, 0);
    resultLayout->addWidget(m_verdictValue, 1, 1);
    resultLayout->addWidget(new QLabel(QStringLiteral("尝试次数"), m_resultGroup), 1, 2);
    resultLayout->addWidget(m_attemptsValue, 1, 3);
    resultLayout->addWidget(new QLabel(QStringLiteral("消息"), m_resultGroup), 2, 0);
    resultLayout->addWidget(m_resultMessageValue, 2, 1, 1, 3);
    resultLayout->addWidget(new QLabel(QStringLiteral("原始数据"), m_resultGroup), 3, 0,
                            Qt::AlignTop);
    resultLayout->addWidget(m_rawDataEdit, 3, 1, 1, 3);
    pageLayout->addWidget(m_resultGroup, 1);

    setCentralWidget(central);
    statusBar()->setSizeGripEnabled(false);

    connect(m_browseTestButton, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择测试配置"), m_testConfigEdit->text(),
            QStringLiteral("JSON 文件 (*.json);;所有文件 (*)"));
        if (!path.isEmpty()) {
            m_testConfigEdit->setText(path);
        }
    });
    connect(m_browseHalButton, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择 HAL 配置"), m_halConfigEdit->text(),
            QStringLiteral("JSON 文件 (*.json);;所有文件 (*)"));
        if (!path.isEmpty()) {
            m_halConfigEdit->setText(path);
        }
    });
    connect(m_loadButton, &QPushButton::clicked,
            this, &GuiMainWindow::loadConfigurations);
    connect(m_refreshPortsButton, &QToolButton::clicked,
            this, &GuiMainWindow::refreshSerialPorts);
    connect(m_controlCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &GuiMainWindow::selectControl);
    connect(m_serialCombo,
            QOverload<int>::of(&QComboBox::activated),
            this,
            [this](int) { selectSerialPort(); });
    connect(m_serialCombo->lineEdit(), &QLineEdit::editingFinished,
            this, &GuiMainWindow::selectSerialPort);
    connect(m_prepareButton, &QPushButton::clicked,
            this, &GuiMainWindow::prepareTest);
    connect(m_runButton, &QPushButton::clicked,
            this, &GuiMainWindow::startTest);
    connect(m_pauseButton, &QToolButton::clicked,
            this, &GuiMainWindow::pauseTest);
    connect(m_resumeButton, &QToolButton::clicked,
            this, &GuiMainWindow::resumeTest);
    connect(m_stopButton, &QToolButton::clicked,
            this, &GuiMainWindow::stopTest);
    connect(m_disconnectButton, &QPushButton::clicked,
            this, &GuiMainWindow::disconnectTest);
}

void GuiMainWindow::loadConfigurations()
{
    if (m_controller == nullptr) {
        handleAction(QStringLiteral("加载配置"),
                     {false, QStringLiteral("controller"),
                      QStringLiteral("应用控制器不可用")});
        return;
    }

    FrontendLaunchOptions options = m_launchOptions;
    options.testConfigPath = m_testConfigEdit->text().trimmed();
    options.halConfigPath = m_halConfigEdit->text().trimmed();
    const ActionResult result = configureController(*m_controller, options);
    if (m_controller->snapshot().phase == QStringLiteral("configured")) {
        m_launchOptions.testConfigPath = options.testConfigPath;
        m_launchOptions.halConfigPath = options.halConfigPath;
        populateControls();
        refreshSerialPorts();
        applySnapshot(m_controller->snapshot());
    }
    handleAction(QStringLiteral("加载配置"), result);
}

void GuiMainWindow::populateControls()
{
    if (m_controller == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_controlCombo);
    m_controlCombo->clear();
    for (const ControlResource& control : m_controller->availableControls()) {
        m_controlCombo->addItem(
            QStringLiteral("%1  [%2]").arg(control.resourceId, control.providerId),
            control.resourceId);
    }
    const QString selected = m_controller->snapshot().controlResourceId;
    const int selectedIndex = m_controlCombo->findData(selected);
    if (selectedIndex >= 0) {
        m_controlCombo->setCurrentIndex(selectedIndex);
    }
}

void GuiMainWindow::refreshSerialPorts()
{
    if (m_controller == nullptr) {
        return;
    }
    QString selected = m_serialCombo->currentText().trimmed();
    if (selected.isEmpty()) {
        selected = m_controller->snapshot().serialPortName;
    }

    const QSignalBlocker blocker(m_serialCombo);
    m_serialCombo->clear();
    for (const SerialPortInfo& port : m_controller->availableSerialPorts()) {
        m_serialCombo->addItem(port.portName);
        const int index = m_serialCombo->count() - 1;
        m_serialCombo->setItemData(
            index,
            QStringLiteral("%1\n%2\n%3")
                .arg(port.description, port.manufacturer, port.systemLocation),
            Qt::ToolTipRole);
    }
    if (!selected.isEmpty()) {
        m_serialCombo->setEditText(selected);
    }
    statusBar()->showMessage(QStringLiteral("串口列表已刷新"), 3000);
}

void GuiMainWindow::selectControl(int index)
{
    if (m_controller == nullptr || index < 0 ||
        m_snapshot.phase != QStringLiteral("configured")) {
        return;
    }
    const ActionResult result = m_controller->selectControl(
        m_controlCombo->itemData(index).toString());
    if (result.ok) {
        const QSignalBlocker blocker(m_serialCombo);
        m_serialCombo->setEditText(m_controller->snapshot().serialPortName);
    }
    handleAction(QStringLiteral("选择控制资源"), result);
}

void GuiMainWindow::selectSerialPort()
{
    if (m_controller == nullptr || !m_serialCombo->isEnabled() ||
        m_snapshot.phase != QStringLiteral("configured")) {
        return;
    }
    handleAction(QStringLiteral("选择串口"),
                 m_controller->selectSerialPort(m_serialCombo->currentText()));
}

void GuiMainWindow::prepareTest()
{
    if (m_controller != nullptr) {
        handleAction(QStringLiteral("准备"), m_controller->prepare());
    }
}

void GuiMainWindow::startTest()
{
    if (m_controller != nullptr) {
        handleAction(QStringLiteral("运行"), m_controller->start());
    }
}

void GuiMainWindow::pauseTest()
{
    if (m_controller != nullptr) {
        handleAction(QStringLiteral("暂停"), m_controller->pause());
    }
}

void GuiMainWindow::resumeTest()
{
    if (m_controller != nullptr) {
        handleAction(QStringLiteral("继续"), m_controller->resume());
    }
}

void GuiMainWindow::stopTest()
{
    if (m_controller == nullptr || m_stopRequestInProgress) {
        return;
    }
    const ActionResult result = m_controller->stopAsync(5000);
    if (!result.ok) {
        handleAction(QStringLiteral("停止"), result);
        return;
    }
    m_stopRequestInProgress = true;
    statusBar()->showMessage(QStringLiteral("正在停止测试"));
    updateActionAvailability(m_snapshot);
}

void GuiMainWindow::disconnectTest()
{
    requestShutdown(false);
}

void GuiMainWindow::handleAction(const QString& action, const ActionResult& result)
{
    if (result.ok) {
        statusBar()->showMessage(QStringLiteral("%1成功").arg(action), 3000);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("%1失败: %2 - %3").arg(action, result.code, result.message));
}

void GuiMainWindow::handleStopCompleted(const ActionResult& result)
{
    m_stopRequestInProgress = false;
    handleAction(QStringLiteral("停止"), result);
    if (!result.ok) {
        m_shutdownPending = false;
        m_closePending = false;
        updateActionAvailability(m_snapshot);
        return;
    }

    updateActionAvailability(m_snapshot);
    if (m_shutdownPending) {
        const ApplicationSnapshot current = m_controller->snapshot();
        const bool terminal = current.phase == QStringLiteral("finished") ||
            current.phase == QStringLiteral("stopped") ||
            current.phase == QStringLiteral("error");
        if (terminal) {
            schedulePendingShutdown();
        }
    }
}

void GuiMainWindow::applySnapshot(const ApplicationSnapshot& snapshot)
{
    m_snapshot = snapshot;
    m_phaseValue->setText(displayValue(snapshot.phase));
    m_testStateValue->setText(displayValue(snapshot.testState));
    m_connectionValue->setText(
        QStringLiteral("%1 / %2 / %3")
            .arg(displayValue(snapshot.controlResourceId),
                 displayValue(snapshot.providerId),
                 displayValue(snapshot.serialPortName)));
    m_progressBar->setValue(qBound(0, snapshot.progress, 100));
    m_progressBar->setFormat(snapshot.progressStep.isEmpty()
                                 ? QStringLiteral("%p%")
                                 : QStringLiteral("%p%  %1").arg(snapshot.progressStep));
    const QString error = snapshot.errorCode.isEmpty()
        ? snapshot.message
        : QStringLiteral("%1: %2").arg(snapshot.errorCode, snapshot.message);
    m_errorValue->setText(displayValue(error));

    m_resultGroup->setEnabled(snapshot.hasResult);
    m_resultIdentityValue->setText(
        QStringLiteral("%1 / %2 / %3")
            .arg(displayValue(snapshot.stepId),
                 displayValue(snapshot.testItemId),
                 displayValue(snapshot.algorithmId)));
    m_verdictValue->setText(displayValue(snapshot.verdict));
    m_attemptsValue->setText(snapshot.hasResult
                                 ? QString::number(snapshot.attempts)
                                 : QStringLiteral("-"));
    m_resultMessageValue->setText(displayValue(snapshot.message));
    m_rawDataEdit->setPlainText(snapshot.hasResult
        ? QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(snapshot.rawData))
                                .toJson(QJsonDocument::Indented))
        : QStringLiteral("{}"));

    const int controlIndex = m_controlCombo->findData(snapshot.controlResourceId);
    if (controlIndex >= 0 && controlIndex != m_controlCombo->currentIndex()) {
        const QSignalBlocker blocker(m_controlCombo);
        m_controlCombo->setCurrentIndex(controlIndex);
    }
    if (snapshot.serialPortName != m_serialCombo->currentText()) {
        const QSignalBlocker blocker(m_serialCombo);
        m_serialCombo->setEditText(snapshot.serialPortName);
    }

    updateActionAvailability(snapshot);

    const bool terminal = snapshot.phase == QStringLiteral("finished") ||
        snapshot.phase == QStringLiteral("stopped") ||
        snapshot.phase == QStringLiteral("error") ||
        snapshot.phase == QStringLiteral("ready") ||
        snapshot.phase == QStringLiteral("configured") ||
        snapshot.phase == QStringLiteral("empty");
    if (m_shutdownPending && !m_shutdownInProgress &&
        !m_stopRequestInProgress && terminal) {
        schedulePendingShutdown();
    }
}

void GuiMainWindow::updateActionAvailability(const ApplicationSnapshot& snapshot)
{
    const bool controllerAvailable = m_controller != nullptr;
    const bool empty = snapshot.phase == QStringLiteral("empty");
    const bool configured = snapshot.phase == QStringLiteral("configured");
    const bool runnable = snapshot.phase == QStringLiteral("ready") ||
        snapshot.phase == QStringLiteral("finished") ||
        snapshot.phase == QStringLiteral("stopped");
    const bool running = snapshot.phase == QStringLiteral("running");
    const bool paused = snapshot.phase == QStringLiteral("paused");
    const bool safeCleanup = !empty && snapshot.phase != QStringLiteral("preparing");
    const bool controlsEnabled = configured && !m_shutdownPending;

    m_testConfigEdit->setEnabled((empty || configured) && !m_shutdownPending);
    m_halConfigEdit->setEnabled((empty || configured) && !m_shutdownPending);
    m_browseTestButton->setEnabled((empty || configured) && !m_shutdownPending);
    m_browseHalButton->setEnabled((empty || configured) && !m_shutdownPending);
    m_loadButton->setEnabled(controllerAvailable && (empty || configured) &&
                             !m_shutdownPending);
    m_refreshPortsButton->setEnabled(controllerAvailable && (empty || configured) &&
                                     !m_shutdownPending);
    m_controlCombo->setEnabled(controlsEnabled);
    m_serialCombo->setEnabled(controlsEnabled &&
                              snapshot.providerId == QStringLiteral("qt.serial"));
    m_prepareButton->setEnabled(controllerAvailable && configured && !m_shutdownPending);
    m_runButton->setEnabled(controllerAvailable && runnable && !m_shutdownPending);
    m_pauseButton->setEnabled(controllerAvailable && running && !m_shutdownPending &&
                              !m_stopRequestInProgress);
    m_resumeButton->setEnabled(controllerAvailable && paused && !m_shutdownPending &&
                               !m_stopRequestInProgress);
    m_stopButton->setEnabled(controllerAvailable && (running || paused) &&
                             !m_shutdownPending && !m_stopRequestInProgress);
    m_disconnectButton->setEnabled(controllerAvailable && safeCleanup &&
                                   !m_shutdownInProgress);
}

void GuiMainWindow::requestShutdown(bool closeAfterShutdown)
{
    if (m_shutdownComplete && closeAfterShutdown) {
        QTimer::singleShot(0, this, &QWidget::close);
        return;
    }
    if (m_controller == nullptr) {
        if (closeAfterShutdown) {
            m_shutdownComplete = true;
            QTimer::singleShot(0, this, &QWidget::close);
        }
        return;
    }

    m_closePending = m_closePending || closeAfterShutdown;
    m_shutdownPending = true;
    updateActionAvailability(m_snapshot);

    if (m_stopRequestInProgress) {
        statusBar()->showMessage(QStringLiteral("正在等待测试停止后安全断开"));
        return;
    }

    if (m_snapshot.phase == QStringLiteral("running") ||
        m_snapshot.phase == QStringLiteral("paused")) {
        const ActionResult stopped = m_controller->stopAsync(5000);
        if (!stopped.ok) {
            m_shutdownPending = false;
            m_closePending = false;
            handleAction(QStringLiteral("停止"), stopped);
            updateActionAvailability(m_snapshot);
            return;
        }
        m_stopRequestInProgress = true;
        statusBar()->showMessage(QStringLiteral("正在停止测试并安全断开"));
        updateActionAvailability(m_snapshot);
        return;
    }
    if (m_snapshot.phase == QStringLiteral("stopping") ||
        m_snapshot.phase == QStringLiteral("preparing")) {
        statusBar()->showMessage(QStringLiteral("正在等待当前动作完成后安全断开"));
        return;
    }
    schedulePendingShutdown();
}

void GuiMainWindow::schedulePendingShutdown()
{
    if (m_shutdownScheduled) {
        return;
    }
    m_shutdownScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_shutdownScheduled = false;
        if (m_shutdownPending && !m_shutdownInProgress) {
            completePendingShutdown();
        }
    });
}

void GuiMainWindow::completePendingShutdown()
{
    if (m_controller == nullptr || !m_shutdownPending) {
        return;
    }
    m_shutdownInProgress = true;
    const ActionResult result = m_controller->shutdown();
    m_shutdownInProgress = false;
    if (!result.ok) {
        m_shutdownPending = false;
        m_closePending = false;
        handleAction(QStringLiteral("断开"), result);
        updateActionAvailability(m_snapshot);
        return;
    }

    const bool closeAfterShutdown = m_closePending;
    m_shutdownPending = false;
    m_closePending = false;
    handleAction(QStringLiteral("断开"), result);
    updateActionAvailability(m_snapshot);
    if (closeAfterShutdown) {
        m_shutdownComplete = true;
        QTimer::singleShot(0, this, &QWidget::close);
    }
}

void GuiMainWindow::closeEvent(QCloseEvent* event)
{
    if (m_shutdownComplete) {
        event->accept();
        return;
    }
    event->ignore();
    requestShutdown(true);
}

} // namespace hwtest::app
