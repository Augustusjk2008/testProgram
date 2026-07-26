"""COM3 echo and product hardware-test user interface."""

import re
from typing import Callable, Dict, Iterable, List, Optional

from PyQt5.QtCore import QDateTime, QSize, Qt, QTimer
from PyQt5.QtGui import QColor
from PyQt5.QtSerialPort import QSerialPortInfo
from PyQt5.QtWidgets import (
    QComboBox,
    QFormLayout,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QPushButton,
    QSizePolicy,
    QStyle,
    QStyleOptionTab,
    QStylePainter,
    QTabBar,
    QTabWidget,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from .app_config import SerialPortConfig
from .colored_log import ColoredLog
from .hardware_test_pages import create_test_page
from .hardware_test_session import HardwareTestSession
from .protocol_catalog import ProtocolCatalog
from .serial_channel import SerialChannel
from .serial_protocol import ProtocolErrorCode
from .test_page_widgets import ParameterValidationError


CHANNEL_NAME = "板端 COM3"
ECHO_TIMEOUT_MS = 1000
CONTINUOUS_INTERVAL_MS = 200
MAX_LOG_BLOCKS = 5000
DH_RX_LOG_PROGRESS_INTERVAL = 100


def format_hex(data: bytes) -> str:
    return " ".join("{:02X}".format(value) for value in bytes(data))


def parse_hex_payload(text: str) -> bytes:
    compact = re.sub(r"[\s,]+", "", text)
    if not compact:
        raise ValueError("请输入十六进制有效数据")
    if len(compact) % 2 != 0 or re.fullmatch(r"[0-9A-Fa-f]+", compact) is None:
        raise ValueError("十六进制输入必须由完整字节组成")
    payload = bytes.fromhex(compact)
    if not 1 <= len(payload) <= 255:
        raise ValueError("有效数据长度必须在 1..255 字节范围内")
    return payload


class WestTabBar(QTabBar):
    """West-positioned tabs with horizontal, fully visible labels."""

    TAB_WIDTH = 112
    TAB_HEIGHT = 35

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setUsesScrollButtons(False)
        self.setExpanding(False)

    def tabSizeHint(self, index: int) -> QSize:  # noqa: N802 - Qt API
        del index
        return QSize(self.TAB_WIDTH, self.TAB_HEIGHT)

    def minimumTabSizeHint(self, index: int) -> QSize:  # noqa: N802 - Qt API
        return self.tabSizeHint(index)

    def paintEvent(self, event) -> None:  # type: ignore[no-untyped-def]
        del event
        painter = QStylePainter(self)
        for index in range(self.count()):
            option = QStyleOptionTab()
            self.initStyleOption(option, index)
            option.rect = self.tabRect(index)
            painter.drawControl(QStyle.CE_TabBarTabShape, option)
            painter.setPen(
                QColor("#006D77")
                if option.state & QStyle.State_Selected
                else QColor("#263238")
            )
            painter.drawText(
                option.rect.adjusted(12, 0, -6, 0),
                Qt.AlignLeft | Qt.AlignVCenter,
                self.tabText(index),
            )


class MainWindow(QMainWindow):
    """One COM3 channel with explicit tab-driven parser selection."""

    def __init__(
        self,
        channel: Optional[SerialChannel] = None,
        session: Optional[HardwareTestSession] = None,
        port_provider: Optional[Callable[[], Iterable[object]]] = None,
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("MB_DDF COM3 回显与硬件测试")
        self.setMinimumSize(820, 620)
        self.resize(1080, 760)

        self._port_provider = port_provider or QSerialPortInfo.availablePorts
        owned_channel = channel or SerialChannel(parent=self)
        self.channels: List[SerialChannel] = [owned_channel]
        self._channels = self.channels
        self.channel = owned_channel
        self.session = session or HardwareTestSession(owned_channel, parent=self)
        self._pending_payload: Optional[bytes] = None
        self._hardware_mode = False
        self._session_busy = False
        self._continuous_key: Optional[str] = None
        self._continuous_parameters: Dict[str, object] = {}
        self._continuous_stop_pending = False
        self._continuous_stop_status = "用户停止"
        self._continuous_stop_detail = "用户停止"
        self._dh_rx_frame_count = 0
        self._dh_rx_summary_pending = False

        self._echo_timer = QTimer(self)
        self._echo_timer.setSingleShot(True)
        self._echo_timer.setInterval(ECHO_TIMEOUT_MS)
        self._echo_timer.timeout.connect(self._on_echo_timeout)

        self._continuous_timer = QTimer(self)
        self._continuous_timer.setSingleShot(True)
        self._continuous_timer.setInterval(CONTINUOUS_INTERVAL_MS)
        self._continuous_timer.timeout.connect(self._run_continuous_iteration)

        self.pages_by_key: Dict[str, QWidget] = {}
        self.tab_index_by_key: Dict[str, int] = {}
        self._build_ui()
        self._connect_signals()
        self.refresh_ports()
        self._set_connected(self.channel.is_open)
        self.session.set_enabled(False)
        self._update_parser_label()
        self._update_action_state()

    def _build_ui(self) -> None:
        central = QWidget(self)
        root = QVBoxLayout(central)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(8)

        top_bar = QFrame(central)
        top_bar.setObjectName("topStatusBar")
        top_bar.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        top_layout = QHBoxLayout(top_bar)
        top_layout.setContentsMargins(10, 6, 10, 6)
        top_layout.setSpacing(8)

        top_layout.addWidget(QLabel("Windows COM", top_bar))
        self.current_port_label = QLabel("--", top_bar)
        self.current_port_label.setObjectName("currentPortLabel")
        self.current_port_label.setMinimumWidth(64)
        top_layout.addWidget(self.current_port_label)

        separator_one = QFrame(top_bar)
        separator_one.setFrameShape(QFrame.VLine)
        top_layout.addWidget(separator_one)

        self.status_label = QLabel("未连接", top_bar)
        self.status_label.setObjectName("connectionStatus")
        self.status_label.setAlignment(Qt.AlignCenter)
        self.status_label.setMinimumWidth(64)
        top_layout.addWidget(self.status_label)

        self.parser_mode_label = QLabel("解析：回显", top_bar)
        self.parser_mode_label.setObjectName("parserModeLabel")
        self.parser_mode_label.setMinimumWidth(104)
        top_layout.addWidget(self.parser_mode_label)
        top_layout.addStretch(1)

        self.connect_button = QPushButton("连接", top_bar)
        self.connect_button.setObjectName("connectButton")
        self.connect_button.setIcon(
            self.style().standardIcon(QStyle.SP_DialogOpenButton)
        )
        self.execute_all_button = QPushButton("执行全部", top_bar)
        self.execute_all_button.setObjectName("executeAllButton")
        self.execute_all_button.setIcon(
            self.style().standardIcon(QStyle.SP_MediaSkipForward)
        )
        self.stop_button = QPushButton("停止", top_bar)
        self.stop_button.setObjectName("stopButton")
        self.stop_button.setIcon(self.style().standardIcon(QStyle.SP_MediaStop))
        top_layout.addWidget(self.connect_button)
        top_layout.addWidget(self.execute_all_button)
        top_layout.addWidget(self.stop_button)

        self.navigation_tabs = QTabWidget(central)
        self.navigation_tabs.setObjectName("navigationTabs")
        self.navigation_tabs.setTabBar(WestTabBar(self.navigation_tabs))
        self.navigation_tabs.setTabPosition(QTabWidget.West)
        self.navigation_tabs.setDocumentMode(True)

        self.connection_tab = self._build_connection_tab(self.navigation_tabs)
        self.echo_tab = self._build_echo_tab(self.navigation_tabs)
        self.navigation_tabs.addTab(self.connection_tab, "连接与日志")
        self.navigation_tabs.addTab(self.echo_tab, "串口回显")

        protocol = getattr(self.session, "protocol", None)
        catalog = getattr(protocol, "catalog", None)
        if catalog is None:
            catalog = ProtocolCatalog.load_default()
        for spec in self.session.test_specs:
            page = create_test_page(spec, catalog, self.navigation_tabs)
            index = self.navigation_tabs.addTab(page, spec.label)
            self.pages_by_key[spec.key] = page
            self.tab_index_by_key[spec.key] = index

        self.navigation_tabs.setCurrentWidget(self.connection_tab)
        root.addWidget(top_bar)
        root.addWidget(self.navigation_tabs, 1)
        self.setCentralWidget(central)

    def _build_connection_tab(self, parent: QWidget) -> QWidget:
        page = QWidget(parent)
        page.setObjectName("connectionLogPage")
        root = QVBoxLayout(page)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(8)

        connection_group = QGroupBox(
            "板端 COM3 / Windows 串口连接", page
        )
        connection_grid = QGridLayout(connection_group)
        connection_grid.setColumnStretch(1, 1)
        connection_grid.setColumnStretch(3, 1)

        self.port_combo = QComboBox(connection_group)
        self.port_combo.setObjectName("portCombo")
        self.port_combo.setEditable(True)
        self.port_combo.setInsertPolicy(QComboBox.NoInsert)
        self.refresh_button = QPushButton("刷新", connection_group)
        self.refresh_button.setObjectName("refreshButton")
        self.refresh_button.setIcon(
            self.style().standardIcon(QStyle.SP_BrowserReload)
        )

        self.baud_combo = QComboBox(connection_group)
        self.baud_combo.setObjectName("baudCombo")
        self.baud_combo.addItems(["614400", "460800", "230400", "115200"])
        self.data_bits_combo = QComboBox(connection_group)
        self.data_bits_combo.setObjectName("dataBitsCombo")
        self.data_bits_combo.addItems(["8", "7", "6", "5"])
        self.parity_combo = QComboBox(connection_group)
        self.parity_combo.setObjectName("parityCombo")
        self.parity_combo.addItems(["Even", "None", "Odd", "Mark", "Space"])
        self.stop_bits_combo = QComboBox(connection_group)
        self.stop_bits_combo.setObjectName("stopBitsCombo")
        self.stop_bits_combo.addItems(["1", "1.5", "2"])
        self.flow_control_combo = QComboBox(connection_group)
        self.flow_control_combo.setObjectName("flowControlCombo")
        self.flow_control_combo.addItems(["None", "Hardware", "Software"])

        connection_grid.addWidget(
            QLabel("Windows 串口（COMx）", connection_group), 0, 0
        )
        connection_grid.addWidget(self.port_combo, 0, 1, 1, 2)
        connection_grid.addWidget(self.refresh_button, 0, 3)
        connection_grid.addWidget(QLabel("波特率", connection_group), 1, 0)
        connection_grid.addWidget(self.baud_combo, 1, 1)
        connection_grid.addWidget(QLabel("数据位", connection_group), 1, 2)
        connection_grid.addWidget(self.data_bits_combo, 1, 3)
        connection_grid.addWidget(QLabel("校验", connection_group), 2, 0)
        connection_grid.addWidget(self.parity_combo, 2, 1)
        connection_grid.addWidget(QLabel("停止位", connection_group), 2, 2)
        connection_grid.addWidget(self.stop_bits_combo, 2, 3)
        connection_grid.addWidget(QLabel("流控", connection_group), 3, 0)
        connection_grid.addWidget(self.flow_control_combo, 3, 1)

        log_header = QHBoxLayout()
        log_header.addWidget(QLabel("完整通信日志", page))
        log_header.addStretch(1)
        self.clear_log_button = QPushButton("清空", page)
        self.clear_log_button.setObjectName("clearLogButton")
        self.clear_log_button.setIcon(
            self.style().standardIcon(QStyle.SP_DialogResetButton)
        )
        log_header.addWidget(self.clear_log_button)
        self.log_edit = QTextEdit(page)
        self.log_edit.setObjectName("logEdit")
        self.log = ColoredLog(self.log_edit, MAX_LOG_BLOCKS)

        root.addWidget(connection_group)
        root.addLayout(log_header)
        root.addWidget(self.log_edit, 1)

        self._config_controls = [
            self.port_combo,
            self.refresh_button,
            self.baud_combo,
            self.data_bits_combo,
            self.parity_combo,
            self.stop_bits_combo,
            self.flow_control_combo,
        ]
        return page

    def _build_echo_tab(self, parent: QWidget) -> QWidget:
        page = QWidget(parent)
        page.setObjectName("echoPage")
        root = QVBoxLayout(page)
        root.setContentsMargins(12, 12, 12, 12)
        root.setSpacing(8)

        form = QFormLayout()
        self.payload_input = QLineEdit(page)
        self.payload_input.setObjectName("payloadInput")
        self.payload_input.setPlaceholderText("4D 42 31")
        self.send_button = QPushButton("发送并校验回显", page)
        self.send_button.setObjectName("sendButton")
        self.send_button.setIcon(self.style().standardIcon(QStyle.SP_ArrowForward))
        input_row = QHBoxLayout()
        input_row.addWidget(self.payload_input, 1)
        input_row.addWidget(self.send_button)
        form.addRow("十六进制有效数据", input_row)

        self.result_label = QLabel("未测试", page)
        self.result_label.setObjectName("testResult")
        self.result_label.setMinimumHeight(32)
        self.result_label.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        form.addRow("最近结果", self.result_label)
        root.addLayout(form)
        root.addStretch(1)
        return page

    def _connect_signals(self) -> None:
        self.refresh_button.clicked.connect(self.refresh_ports)
        self.connect_button.clicked.connect(self.toggle_connection)
        self.port_combo.currentTextChanged.connect(self._update_current_port)
        self.send_button.clicked.connect(self.send_echo_request)
        self.payload_input.returnPressed.connect(self.send_echo_request)
        self.clear_log_button.clicked.connect(self.log_edit.clear)
        self.execute_all_button.clicked.connect(self._execute_all)
        self.stop_button.clicked.connect(self._stop_active)
        self.navigation_tabs.currentChanged.connect(self._on_tab_changed)
        for page in self.pages_by_key.values():
            page.run_requested.connect(self._execute_page)
            page.continuous_requested.connect(self._on_continuous_requested)

        self.channel.connected.connect(lambda: self._set_connected(True))
        self.channel.disconnected.connect(lambda: self._set_connected(False))
        self.channel.frame_queued.connect(self._on_frame_queued)
        self.channel.send_completed.connect(self._on_send_completed)
        self.channel.send_failed.connect(self._on_send_failed)
        self.channel.frame_received.connect(self._on_frame_received)
        self.channel.protocol_error.connect(self._on_protocol_error)
        self.channel.io_error.connect(self._on_io_error)

        self.session.test_started.connect(self._on_test_started)
        self.session.test_status_changed.connect(self._on_test_status_changed)
        self.session.test_finished.connect(self._on_test_finished)
        self.session.response_received.connect(self._on_product_response)
        self.session.busy_changed.connect(self._on_session_busy_changed)
        self.session.log_event.connect(self._append_log)

    def refresh_ports(self) -> None:
        current = self.port_combo.currentText().strip()
        names = []
        try:
            for port in self._port_provider():
                name = port.portName() if hasattr(port, "portName") else str(port)
                if name and name not in names:
                    names.append(name)
        except Exception as exc:
            self._append_log("ERROR", "枚举 Windows 串口失败：{}".format(exc))
            return
        self.port_combo.blockSignals(True)
        self.port_combo.clear()
        self.port_combo.addItems(names)
        if current:
            self.port_combo.setCurrentText(current)
        self.port_combo.blockSignals(False)
        self._update_current_port(self.port_combo.currentText())
        self._append_log(
            "INFO",
            "Windows 串口列表已刷新：{}".format(
                ", ".join(names) if names else "未发现端口"
            ),
        )

    def _update_current_port(self, text: str) -> None:
        name = str(text).strip()
        self.current_port_label.setText(name or "未选择")

    def toggle_connection(self) -> None:
        if self.channel.is_open:
            self.channel.close()
            return
        try:
            config = SerialPortConfig(
                port_name=self.port_combo.currentText().strip(),
                baud_rate=int(self.baud_combo.currentText()),
                data_bits=int(self.data_bits_combo.currentText()),
                parity=self.parity_combo.currentText(),
                stop_bits=self.stop_bits_combo.currentText(),
                flow_control=self.flow_control_combo.currentText(),
            )
        except ValueError as exc:
            self._append_log("ERROR", "串口配置无效：{}".format(exc))
            return
        self._append_log(
            "INFO",
            "连接 Windows {}（{} {}{}{}）".format(
                config.port_name or "<未选择>",
                config.baud_rate,
                config.data_bits,
                config.parity,
                config.stop_bits,
            ),
        )
        opened = self.channel.open(config)
        if opened and self.channel.is_open:
            self._set_connected(True)
        elif not opened and not self.channel.is_open:
            self._set_connected(False)
            self._append_log("ERROR", "连接失败：串口后端未打开")

    def _on_tab_changed(self, index: int) -> None:
        widget = self.navigation_tabs.widget(index)
        if widget is self.connection_tab:
            return
        if widget is self.echo_tab:
            self._set_parser_mode(False)
            return
        if index in self.tab_index_by_key.values():
            self._set_parser_mode(True)

    def _set_parser_mode(self, hardware: bool) -> None:
        requested = bool(hardware)
        if requested and self._pending_payload is not None:
            self.navigation_tabs.setCurrentWidget(self.echo_tab)
            return
        if not requested and (
            self._session_busy or self._continuous_key is not None
        ):
            return
        self._hardware_mode = requested
        self.session.set_enabled(requested)
        self._update_parser_label()
        self._update_action_state()

    def _update_parser_label(self) -> None:
        self.parser_mode_label.setText(
            "解析：产品协议" if self._hardware_mode else "解析：回显"
        )

    def _update_action_state(self) -> None:
        echo_pending = self._pending_payload is not None
        continuous_active = self._continuous_key is not None
        can_run_hardware = (
            self._hardware_mode
            and not self._session_busy
            and not echo_pending
            and not continuous_active
        )
        self.execute_all_button.setEnabled(can_run_hardware)
        self.stop_button.setEnabled(
            self._session_busy or echo_pending or continuous_active
        )
        self.send_button.setEnabled(not self._hardware_mode and not echo_pending)
        for page in self.pages_by_key.values():
            page.set_actions_enabled(
                not self._session_busy
                and not echo_pending
                and not continuous_active
            )
        echo_index = self.navigation_tabs.indexOf(self.echo_tab)
        self.navigation_tabs.setTabEnabled(
            echo_index, not self._session_busy and not continuous_active
        )
        for index in self.tab_index_by_key.values():
            self.navigation_tabs.setTabEnabled(index, not echo_pending)
        self.navigation_tabs.setTabEnabled(
            self.navigation_tabs.indexOf(self.connection_tab), True
        )

    def _execute_page(self, key: str, parameters: object) -> None:
        page = self.pages_by_key.get(str(key))
        if page is None or not isinstance(parameters, dict):
            return
        self.session.run_test(str(key), parameters)

    def _on_continuous_requested(
        self, key: str, parameters: object, enabled: bool
    ) -> None:
        page = self.pages_by_key.get(str(key))
        if page is None:
            return
        if not enabled:
            if self._continuous_key == str(key):
                self._stop_continuous("用户停止")
            return
        if (
            self._continuous_key is not None
            or self._continuous_request_is_active()
            or not self._hardware_mode
            or not isinstance(parameters, dict)
        ):
            page.set_continuous_checked(False)
            self._update_action_state()
            return

        self._continuous_key = str(key)
        self._continuous_parameters = dict(parameters)
        self._continuous_stop_pending = False
        self._continuous_stop_status = "用户停止"
        self._continuous_stop_detail = "用户停止"
        if self._continuous_key == "electrical_health":
            page.start_continuous_capture()
        self._update_action_state()
        self._run_continuous_iteration()

    def _run_continuous_iteration(self) -> None:
        key = self._continuous_key
        if key is None:
            return
        if self._continuous_request_is_active() or not self._hardware_mode:
            self._stop_continuous(
                "连续执行环境不可用", "执行失败", wait_for_active=False
            )
            return
        if not self.session.run_test(key, dict(self._continuous_parameters)):
            self._stop_continuous(
                "连续执行未能启动", "执行失败", wait_for_active=False
            )

    def _continuous_request_is_active(self) -> bool:
        return self._session_busy or bool(getattr(self.session, "is_busy", False))

    def _stop_continuous(
        self,
        detail: str,
        final_status: str = "用户停止",
        wait_for_active: bool = True,
    ) -> None:
        key = self._continuous_key
        if key is None:
            return
        self._continuous_timer.stop()
        page = self.pages_by_key.get(key)
        if page is not None:
            page.set_continuous_checked(False)
        if wait_for_active and self._continuous_request_is_active():
            self._continuous_stop_pending = True
            self._continuous_stop_status = final_status
            self._continuous_stop_detail = detail
            self._update_action_state()
            return
        self._finish_continuous(final_status, detail)

    def _finish_continuous(self, final_status: str, final_detail: str) -> None:
        key = self._continuous_key
        if key is None:
            return
        self._continuous_timer.stop()
        self._continuous_key = None
        self._continuous_parameters = {}
        self._continuous_stop_pending = False
        self._continuous_stop_status = "用户停止"
        self._continuous_stop_detail = "用户停止"
        page = self.pages_by_key.get(key)
        if page is not None:
            page.set_continuous_checked(False)
        if key == "electrical_health" and page is not None:
            try:
                saved_path = page.finish_continuous_capture(
                    final_status, final_detail
                )
            except Exception as exc:
                self._append_log(
                    "ERROR", "电气健康连续数据保存失败：{}".format(exc)
                )
            else:
                if saved_path is not None:
                    self._append_log(
                        "COMPLETE",
                        "电气健康连续数据已保存：{}".format(saved_path),
                    )
        self._update_action_state()

    def _execute_all(self) -> None:
        overrides: Dict[str, Dict[str, object]] = {}
        for spec in self.session.test_specs:
            page = self.pages_by_key[spec.key]
            try:
                overrides[spec.key] = page.collect_parameters()
            except ParameterValidationError as exc:
                page.set_test_status("参数错误", str(exc))
                widget = getattr(exc, "widget", None)
                if widget is not None:
                    widget.setFocus(Qt.OtherFocusReason)
                self.navigation_tabs.setCurrentIndex(
                    self.tab_index_by_key[spec.key]
                )
                self._append_log(
                    "ERROR", "{}参数无效：{}".format(spec.label, exc)
                )
                return
            except (TypeError, ValueError) as exc:
                page.set_test_status("参数错误", str(exc))
                self.navigation_tabs.setCurrentIndex(
                    self.tab_index_by_key[spec.key]
                )
                self._append_log(
                    "ERROR", "{}参数无效：{}".format(spec.label, exc)
                )
                return
        self.session.run_all(overrides)

    def _stop_active(self) -> None:
        if self._pending_payload is not None:
            self._finish_request("已停止", "WARNING", "用户停止回显等待")
            return
        self._stop_continuous("用户停止")
        self.session.stop()

    def send_echo_request(self) -> None:
        if self._hardware_mode or self._pending_payload is not None:
            return
        try:
            payload = parse_hex_payload(self.payload_input.text())
        except ValueError as exc:
            self.result_label.setText("发送失败")
            self._append_log("ERROR", "发送失败：{}".format(exc))
            return
        if not self.channel.is_open:
            self.result_label.setText("发送失败")
            self._append_log(
                "ERROR", "发送失败：板端 COM3 对应的 Windows 串口未连接"
            )
            return
        self._pending_payload = payload
        self.result_label.setText("等待回显")
        self._update_action_state()
        if self.channel.send_payload(payload):
            if self._pending_payload is not None:
                self._echo_timer.start()
        elif self._pending_payload is not None:
            self._finish_request("发送失败", "ERROR", "串口未接受发送请求")

    def _set_connected(self, connected: bool) -> None:
        self.status_label.setText("已连接" if connected else "未连接")
        self.connect_button.setText("断开" if connected else "连接")
        self.connect_button.setIcon(
            self.style().standardIcon(
                QStyle.SP_DialogCloseButton
                if connected
                else QStyle.SP_DialogOpenButton
            )
        )
        for control in self._config_controls:
            control.setEnabled(not connected)
        if not connected:
            self._stop_continuous("串口断开", "通信失败")
        if not connected and self._pending_payload is not None:
            self._finish_request("发送失败", "ERROR", "等待回显时串口断开")

    def _on_frame_queued(self, frame: bytes) -> None:
        self._append_log("TX", "TX {}".format(format_hex(frame)))

    def _on_send_completed(self, frame: bytes) -> None:
        self._append_log("TX", "TX 完成（{} 字节）".format(len(frame)))

    def _on_send_failed(self, frame: bytes, message: str) -> None:
        if frame:
            self._append_log(
                "ERROR", "TX {}；发送失败：{}".format(format_hex(frame), message)
            )
        else:
            self._append_log("ERROR", "发送失败：{}".format(message))
        if not self._hardware_mode and self._pending_payload is not None:
            self._finish_request(
                "发送失败", "ERROR", message, log_result=False
            )

    def _on_frame_received(self, payload: bytes, frame: bytes) -> None:
        if self._hardware_mode and self._is_dh_response_payload(payload):
            self._dh_rx_frame_count += 1
            if self._dh_rx_frame_count == 1:
                self._append_log(
                    "RX", "DH 回告首帧 {}".format(format_hex(frame))
                )
            elif self._dh_rx_frame_count % DH_RX_LOG_PROGRESS_INTERVAL == 0:
                self._append_log(
                    "RX", "DH 回告已接收 {} 帧".format(self._dh_rx_frame_count)
                )
        else:
            self._append_log("RX", "RX {}".format(format_hex(frame)))
        if self._hardware_mode or self._pending_payload is None:
            return
        if payload == self._pending_payload:
            self._finish_request("通过", "COMPLETE", "回显内容一致")
        else:
            self._finish_request(
                "内容不符",
                "ERROR",
                "期望 {}，收到 {}".format(
                    format_hex(self._pending_payload), format_hex(payload)
                ),
            )

    def _on_protocol_error(self, code: str, message: str, frame: bytes) -> None:
        suffix = "：{}".format(format_hex(frame)) if frame else ""
        self._append_log(
            "ERROR", "协议错误 [{}] {}{}".format(code, message, suffix)
        )
        if (
            not self._hardware_mode
            and code == ProtocolErrorCode.CRC_MISMATCH.value
            and self._pending_payload is not None
        ):
            self._finish_request(
                "CRC 错误", "ERROR", message, log_result=False
            )

    def _on_io_error(self, message: str) -> None:
        self._append_log("ERROR", message)
        if not self._hardware_mode and self._pending_payload is not None:
            self._finish_request(
                "发送失败", "ERROR", message, log_result=False
            )

    def _on_echo_timeout(self) -> None:
        if self._pending_payload is not None:
            self._finish_request("超时", "ERROR", "1000 ms 内未收到有效回显")

    def _finish_request(
        self,
        result: str,
        level: str,
        detail: str,
        log_result: bool = True,
    ) -> None:
        self._echo_timer.stop()
        self._pending_payload = None
        self.result_label.setText(result)
        self._update_action_state()
        if log_result:
            self._append_log(level, "{}：{}".format(result, detail))

    def _on_test_started(self, key: str) -> None:
        page = self.pages_by_key.get(key)
        if page is not None:
            page.reset_for_run()

    def _on_test_status_changed(self, key: str, status: str, detail: str) -> None:
        if key == "dh":
            if status == "执行中":
                self._dh_rx_frame_count = 0
                self._dh_rx_summary_pending = False
            elif self._dh_rx_frame_count > 0 and not self._dh_rx_summary_pending:
                self._dh_rx_summary_pending = True
                QTimer.singleShot(0, self._finish_dh_rx_summary)
        page = self.pages_by_key.get(key)
        if page is not None:
            page.set_test_status(status, detail)

    def _on_test_finished(self, key: str, status: str, detail: str) -> None:
        if key == "dh":
            page = self.pages_by_key.get(key)
            if page is not None:
                try:
                    saved_path = page.save_reports(status, detail)
                except Exception as exc:
                    self._append_log(
                        "ERROR", "DH 回告数据保存失败：{}".format(exc)
                    )
                else:
                    if saved_path is not None:
                        self._append_log(
                            "COMPLETE", "DH 回告数据已保存：{}".format(saved_path)
                        )
        if key == self._continuous_key:
            if self._continuous_stop_pending:
                self._finish_continuous(
                    self._continuous_stop_status,
                    self._continuous_stop_detail,
                )
            else:
                self._continuous_timer.start()
                self._update_action_state()

    @staticmethod
    def _is_dh_response_payload(payload: bytes) -> bool:
        data = bytes(payload)
        return len(data) >= 3 and data[1] == 0x06 and data[2] == 0x02

    def _finish_dh_rx_summary(self) -> None:
        if not self._dh_rx_summary_pending:
            return
        self._dh_rx_summary_pending = False
        self._append_log(
            "RX", "DH 回告接收结束，共 {} 帧".format(self._dh_rx_frame_count)
        )

    def _on_product_response(self, key: str, decoded: object) -> None:
        page = self.pages_by_key.get(key)
        if page is not None:
            page.render_response(decoded)

    def _on_session_busy_changed(self, busy: bool) -> None:
        self._session_busy = bool(busy)
        if not self._session_busy and self._continuous_stop_pending:
            self._finish_continuous(
                self._continuous_stop_status,
                self._continuous_stop_detail,
            )
        self._update_action_state()

    def _append_log(self, level: str, message: str) -> None:
        category = str(level).upper()
        timestamp = QDateTime.currentDateTime().toString("HH:mm:ss.zzz")
        line = "[{}] [{}] [{}] {}".format(
            timestamp, category, CHANNEL_NAME, message
        )
        self.log.append(category, line)

    def closeEvent(self, event) -> None:  # type: ignore[no-untyped-def]
        self._echo_timer.stop()
        self._pending_payload = None
        self._stop_continuous("窗口关闭", "用户停止")
        self.session.set_enabled(False)
        for channel in self.channels:
            channel.close()
        super().closeEvent(event)


__all__ = [
    "CHANNEL_NAME",
    "CONTINUOUS_INTERVAL_MS",
    "ECHO_TIMEOUT_MS",
    "MAX_LOG_BLOCKS",
    "MainWindow",
    "WestTabBar",
    "format_hex",
    "parse_hex_payload",
]
