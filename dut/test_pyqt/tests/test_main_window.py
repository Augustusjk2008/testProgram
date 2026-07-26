from itertools import combinations

import pytest

from PyQt5.QtCore import QObject, QPoint, QRect, Qt, pyqtSignal
from PyQt5.QtGui import QFontDatabase
from PyQt5.QtWidgets import (
    QApplication,
    QFormLayout,
    QGroupBox,
    QScrollArea,
    QTabWidget,
    QTextEdit,
)

from test_pyqt.main_window import (
    DH_RX_LOG_PROGRESS_INTERVAL,
    ECHO_TIMEOUT_MS,
    MAX_LOG_BLOCKS,
    MainWindow,
)
from test_pyqt import main as application_main
from test_pyqt.hardware_test_session import (
    TEST_SPECS,
    TestStatus as HardwareTestStatus,
)
from test_pyqt.product_protocol import DecodedMessage, ProductProtocol
from test_pyqt.serial_protocol import ProtocolErrorCode, encode_frame
from test_pyqt.test_page_widgets import BitGrid


EXPECTED_TAB_LABELS = (
    "连接与日志",
    "串口回显",
    "系统状态",
    "内存",
    "SPI Flash",
    "总线",
    "DI",
    "DO",
    "电气健康",
    "DH 脉宽配置",
    "DH",
    "舵控板级",
    "定时器",
)


class FakeChannel(QObject):
    connected = pyqtSignal()
    disconnected = pyqtSignal()
    frame_received = pyqtSignal(bytes, bytes)
    protocol_error = pyqtSignal(str, str, bytes)
    frame_queued = pyqtSignal(bytes)
    send_completed = pyqtSignal(bytes)
    send_failed = pyqtSignal(bytes, str)
    io_error = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self._open = False
        self.opened_configs = []
        self.sent_payloads = []
        self.send_result = True
        self.close_count = 0

    @property
    def is_open(self):
        return self._open

    def open(self, config):
        self.opened_configs.append(config)
        if not config.port_name:
            self.io_error.emit("未选择 Windows 串口")
            return False
        self._open = True
        self.connected.emit()
        return True

    def close(self):
        self.close_count += 1
        if self._open:
            self._open = False
            self.disconnected.emit()

    def send_payload(self, payload):
        self.sent_payloads.append(bytes(payload))
        frame = encode_frame(payload)
        self.frame_queued.emit(frame)
        if not self.send_result:
            self.send_failed.emit(frame, "fake send failure")
            return False
        return True


class FakeSession(QObject):
    test_started = pyqtSignal(str)
    test_status_changed = pyqtSignal(str, str, str)
    test_finished = pyqtSignal(str, str, str)
    response_received = pyqtSignal(str, object)
    dh_report_received = pyqtSignal(object)
    helm_feedback_received = pyqtSignal(object)
    busy_changed = pyqtSignal(bool)
    all_finished = pyqtSignal()
    log_event = pyqtSignal(str, str)

    def __init__(self):
        super().__init__()
        self.protocol = ProductProtocol()
        self.test_specs = TEST_SPECS
        self.enabled = []
        self.run_test_calls = []
        self.run_all_calls = []
        self.stop_calls = 0

    def set_enabled(self, value):
        self.enabled.append(bool(value))

    def status_for(self, key):
        return HardwareTestStatus.NOT_RUN

    def parameter_fields(self, key):
        for spec in self.test_specs:
            if spec.key == key:
                return spec.parameters
        return ()

    def run_test(self, key, parameters):
        self.run_test_calls.append((key, dict(parameters)))
        self.test_started.emit(key)
        return True

    def run_all(self, overrides=None):
        snapshot = {
            key: dict(parameters)
            for key, parameters in (overrides or {}).items()
        }
        self.run_all_calls.append(snapshot)
        return True

    def stop(self):
        self.stop_calls += 1


def make_window(qtbot, ports=("COM3", "COM7")):
    channel = FakeChannel()
    session = FakeSession()
    window = MainWindow(
        channel=channel, session=session, port_provider=lambda: ports
    )
    qtbot.addWidget(window)
    window.show()
    window.fake_session = session
    return window, channel


def connect_window(qtbot, window):
    qtbot.mouseClick(window.connect_button, Qt.LeftButton)
    assert window.status_label.text() == "已连接"


def select_echo_page(window):
    window.navigation_tabs.setCurrentWidget(window.echo_tab)


def decoded_response(session, name, values=None, sequence=100):
    definition = session.protocol.catalog.get(name)
    merged = {"status": 0, "err_code": 0}
    if values:
        merged.update(values)
    return DecodedMessage(
        name=name,
        sequence=sequence,
        type_group=definition.type_group,
        sub_type=definition.sub_type,
        values=merged,
        payload=b"",
    )


def widget_geometry_in_window(window, widget):
    origin = widget.mapTo(window, QPoint(0, 0))
    return QRect(origin, widget.size())


def assert_visible_widgets_do_not_overlap(window, widgets):
    visible = [widget for widget in widgets if widget.isVisible()]
    assert len(visible) == len(widgets)
    for widget in visible:
        geometry = widget_geometry_in_window(window, widget)
        assert geometry.width() > 0
        assert geometry.height() > 0
    for first, second in combinations(visible, 2):
        first_geometry = widget_geometry_in_window(window, first)
        second_geometry = widget_geometry_in_window(window, second)
        assert not first_geometry.intersects(second_geometry), (
            "{} 与 {} 的可见区域重叠".format(
                first.objectName() or type(first).__name__,
                second.objectName() or type(second).__name__,
            )
        )


def assert_widgets_fit_scroll_viewport_width(scroll_area, widgets):
    viewport = scroll_area.viewport()
    for widget in widgets:
        geometry = QRect(widget.mapTo(viewport, QPoint(0, 0)), widget.size())
        assert geometry.left() >= viewport.rect().left()
        text = getattr(widget, "text", None)
        description = widget.objectName() or (
            text() if callable(text) else type(widget).__name__
        )
        assert geometry.right() <= viewport.rect().right(), (
            "{} 超出参数区横向可见范围: {} not in {}".format(
                description,
                geometry.getRect(),
                viewport.rect().getRect(),
            )
        )


def test_default_ui_identifies_board_and_windows_ports(qtbot) -> None:
    window, channel = make_window(qtbot)

    assert (window.minimumWidth(), window.minimumHeight()) == (820, 620)
    assert window.windowTitle() == "MB_DDF COM3 回显与硬件测试"
    assert any(
        "板端 COM3" in group.title()
        for group in window.findChildren(QGroupBox)
    )
    assert window.port_combo.currentText() == "COM3"
    assert window.baud_combo.currentText() == "614400"
    assert window.data_bits_combo.currentText() == "8"
    assert window.parity_combo.currentText() == "Even"
    assert window.stop_bits_combo.currentText() == "1"
    assert window.flow_control_combo.currentText() == "None"
    assert window.status_label.text() == "未连接"
    assert "COM3" in window.current_port_label.text()
    assert "回显" in window.parser_mode_label.text()
    assert window._echo_timer.interval() == ECHO_TIMEOUT_MS
    assert isinstance(window.log_edit, QTextEdit)
    assert window.log_edit.document().maximumBlockCount() == MAX_LOG_BLOCKS


def test_refresh_preserves_manually_entered_windows_port(qtbot) -> None:
    values = [["COM3"], ["COM7"]]
    channel = FakeChannel()
    window = MainWindow(
        channel=channel,
        session=FakeSession(),
        port_provider=lambda: values.pop(0),
    )
    qtbot.addWidget(window)
    window.port_combo.setCurrentText("COM99")

    window.refresh_ports()

    assert window.port_combo.currentText() == "COM99"
    assert window.port_combo.findText("COM7") >= 0


def test_connection_uses_displayed_614400_8e1_and_locks_controls(qtbot) -> None:
    window, channel = make_window(qtbot)

    connect_window(qtbot, window)

    config = channel.opened_configs[-1]
    assert config.port_name == "COM3"
    assert (config.baud_rate, config.data_bits, config.parity, config.stop_bits) == (
        614400,
        8,
        "Even",
        "1",
    )
    assert not window.port_combo.isEnabled()
    assert not window.refresh_button.isEnabled()
    assert window.connect_button.text() == "断开"

    qtbot.mouseClick(window.connect_button, Qt.LeftButton)
    assert window.port_combo.isEnabled()
    assert window.status_label.text() == "未连接"


def test_matching_echo_passes_and_logs_complete_tx_rx_frames(qtbot) -> None:
    window, channel = make_window(qtbot)
    connect_window(qtbot, window)
    select_echo_page(window)
    window.payload_input.setText("4D 42 31")

    qtbot.mouseClick(window.send_button, Qt.LeftButton)
    assert channel.sent_payloads == [bytes.fromhex("4D 42 31")]
    assert window.result_label.text() == "等待回显"
    assert not window.send_button.isEnabled()

    frame = encode_frame(bytes.fromhex("4D 42 31"))
    channel.frame_received.emit(bytes.fromhex("4D 42 31"), frame)

    assert window.result_label.text() == "通过"
    assert window.send_button.isEnabled()
    log = window.log_edit.toPlainText()
    assert "TX 55 AA 03 4D 42 31 FC 89" in log
    assert "RX 55 AA 03 4D 42 31 FC 89" in log
    assert "[板端 COM3]" in log


def test_echo_mode_preserves_255_byte_payload_boundary(qtbot) -> None:
    window, channel = make_window(qtbot)
    connect_window(qtbot, window)
    select_echo_page(window)
    payload = bytes(range(255))
    window.payload_input.setText(payload.hex(" "))

    qtbot.mouseClick(window.send_button, Qt.LeftButton)
    channel.frame_received.emit(payload, encode_frame(payload))

    assert channel.sent_payloads == [payload]
    assert window.result_label.text() == "通过"


def test_dh_burst_log_is_sampled_instead_of_rendering_every_frame(qtbot) -> None:
    window, _channel = make_window(qtbot)
    window._hardware_mode = True
    window.log_edit.clear()
    window._on_test_status_changed("dh", "执行中", "")
    payload = bytes((0x11, 0x06, 0x02))
    frame = bytes.fromhex("55 AA 03 11 06 02 00 00")
    total = DH_RX_LOG_PROGRESS_INTERVAL * 2 + 5

    for _index in range(total):
        window._on_frame_received(payload, frame)
    window._on_test_status_changed("dh", "执行完成", "")
    qtbot.wait(1)

    log = window.log_edit.toPlainText()
    assert log.count("DH 回告首帧") == 1
    assert "DH 回告已接收 {} 帧".format(DH_RX_LOG_PROGRESS_INTERVAL) in log
    assert "DH 回告已接收 {} 帧".format(DH_RX_LOG_PROGRESS_INTERVAL * 2) in log
    assert "DH 回告接收结束，共 {} 帧".format(total) in log


def test_mismatched_echo_finishes_as_content_mismatch(qtbot) -> None:
    window, channel = make_window(qtbot)
    connect_window(qtbot, window)
    select_echo_page(window)
    window.payload_input.setText("01 02")
    qtbot.mouseClick(window.send_button, Qt.LeftButton)

    other = b"\x01\x03"
    channel.frame_received.emit(other, encode_frame(other))

    assert window.result_label.text() == "内容不符"
    assert "期望 01 02，收到 01 03" in window.log_edit.toPlainText()


def test_crc_error_finishes_pending_request_without_accepting_payload(qtbot) -> None:
    window, channel = make_window(qtbot)
    connect_window(qtbot, window)
    select_echo_page(window)
    window.payload_input.setText("AA")
    qtbot.mouseClick(window.send_button, Qt.LeftButton)
    broken = bytearray(encode_frame(b"\xAA"))
    broken[-1] ^= 1

    channel.protocol_error.emit(
        ProtocolErrorCode.CRC_MISMATCH.value, "CRC 错误", bytes(broken)
    )

    assert window.result_label.text() == "CRC 错误"
    assert "协议错误 [crc_mismatch]" in window.log_edit.toPlainText()


def test_send_failure_and_timeout_have_distinct_results(qtbot) -> None:
    window, channel = make_window(qtbot)
    connect_window(qtbot, window)
    select_echo_page(window)
    window.payload_input.setText("01")
    channel.send_result = False

    qtbot.mouseClick(window.send_button, Qt.LeftButton)
    assert window.result_label.text() == "发送失败"

    channel.send_result = True
    qtbot.mouseClick(window.send_button, Qt.LeftButton)
    window._on_echo_timeout()
    assert window.result_label.text() == "超时"
    assert "1000 ms 内未收到有效回显" in window.log_edit.toPlainText()


def test_only_one_echo_request_can_be_pending(qtbot) -> None:
    window, channel = make_window(qtbot)
    connect_window(qtbot, window)
    select_echo_page(window)
    window.payload_input.setText("01")

    window.send_echo_request()
    window.payload_input.setText("02")
    window.send_echo_request()

    assert channel.sent_payloads == [b"\x01"]


def test_invalid_or_empty_hex_is_rejected_before_channel_send(qtbot) -> None:
    window, channel = make_window(qtbot)
    connect_window(qtbot, window)
    select_echo_page(window)

    for value in ("", "0", "GG", "00 " * 256):
        window.payload_input.setText(value)
        window.send_echo_request()
        assert window.result_label.text() == "发送失败"
    assert channel.sent_payloads == []


def test_close_cancels_pending_request_and_closes_managed_channels(qtbot) -> None:
    window, channel = make_window(qtbot)
    connect_window(qtbot, window)
    select_echo_page(window)
    window.payload_input.setText("01")
    window.send_echo_request()
    assert window._echo_timer.isActive()

    window.close()

    assert not window._echo_timer.isActive()
    assert channel.close_count == 1


def test_runtime_validation_requires_pinned_python_38(monkeypatch) -> None:
    monkeypatch.setattr(application_main.sys, "executable", application_main.EXPECTED_PYTHON)
    monkeypatch.setattr(application_main.sys, "version_info", (3, 8, 18))
    assert application_main.validate_runtime() is None

    monkeypatch.setattr(application_main.sys, "executable", r"C:\Python38\python.exe")
    error = application_main.validate_runtime()
    assert error is not None
    assert "解释器路径不正确" in error


def test_runtime_validation_accepts_frozen_python_38_executable(monkeypatch) -> None:
    monkeypatch.setattr(application_main.sys, "frozen", True, raising=False)
    monkeypatch.setattr(
        application_main.sys,
        "executable",
        r"C:\Program Files\MB_DDF_HW_Test_PC\MB_DDF_HW_Test_PC.exe",
    )
    monkeypatch.setattr(application_main.sys, "version_info", (3, 8, 18))

    assert application_main.validate_runtime() is None


def test_runtime_validation_rejects_wrong_frozen_python_version(monkeypatch) -> None:
    monkeypatch.setattr(application_main.sys, "frozen", True, raising=False)
    monkeypatch.setattr(application_main.sys, "version_info", (3, 9, 0))

    error = application_main.validate_runtime()

    assert error is not None
    assert "Python 版本不正确" in error


def test_global_application_identity_and_font_are_configured(qapp) -> None:
    application_main.configure_application(qapp)

    assert qapp.applicationName() == "MB_DDF COM3 硬件测试"
    assert qapp.font().family() == "Microsoft YaHei"
    assert "#006D77" in qapp.styleSheet()
    assert "Microsoft YaHei" in QFontDatabase().families()


def test_navigation_has_shared_echo_and_eleven_hardware_tabs(qtbot) -> None:
    window, _ = make_window(qtbot)

    assert isinstance(window.navigation_tabs, QTabWidget)
    assert window.navigation_tabs.tabPosition() == QTabWidget.West
    assert tuple(
        window.navigation_tabs.tabText(index)
        for index in range(window.navigation_tabs.count())
    ) == EXPECTED_TAB_LABELS
    assert window.navigation_tabs.widget(0) is window.connection_tab
    assert window.navigation_tabs.widget(1) is window.echo_tab
    assert tuple(window.pages_by_key) == tuple(spec.key for spec in TEST_SPECS)
    assert window.tab_index_by_key == {
        spec.key: index + 2 for index, spec in enumerate(TEST_SPECS)
    }
    for key, index in window.tab_index_by_key.items():
        assert window.navigation_tabs.widget(index) is window.pages_by_key[key]

    for removed_name in (
        "echo_mode_button",
        "hardware_mode_button",
        "test_tree",
        "parameter_form",
        "response_table",
    ):
        assert not hasattr(window, removed_name)


def test_initial_log_page_preserves_default_echo_parser(qtbot) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session

    assert window.navigation_tabs.currentWidget() is window.connection_tab
    assert session.enabled
    assert session.enabled[-1] is False
    assert "回显" in window.parser_mode_label.text()


def test_entering_echo_and_hardware_tabs_switches_parser_without_reconnecting(
    qtbot,
) -> None:
    window, channel = make_window(qtbot)
    session = window.fake_session

    window.navigation_tabs.setCurrentWidget(window.echo_tab)
    assert session.enabled[-1] is False
    assert "回显" in window.parser_mode_label.text()

    system_index = window.tab_index_by_key["system"]
    window.navigation_tabs.setCurrentIndex(system_index)
    assert session.enabled[-1] is True
    assert "产品协议" in window.parser_mode_label.text()

    calls_before_log = list(session.enabled)
    window.navigation_tabs.setCurrentWidget(window.connection_tab)
    assert session.enabled == calls_before_log
    assert "产品协议" in window.parser_mode_label.text()

    window.navigation_tabs.setCurrentWidget(window.echo_tab)
    assert session.enabled[-1] is False
    assert channel.opened_configs == []


def test_busy_disables_echo_tab_but_keeps_log_and_all_hardware_tabs_browsable(
    qtbot,
) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    tabs = window.navigation_tabs

    tabs.setCurrentIndex(window.tab_index_by_key["system"])
    session.busy_changed.emit(True)

    assert not tabs.isTabEnabled(tabs.indexOf(window.echo_tab))
    assert tabs.isTabEnabled(tabs.indexOf(window.connection_tab))
    assert all(
        tabs.isTabEnabled(window.tab_index_by_key[spec.key])
        for spec in TEST_SPECS
    )

    browsable_indexes = [tabs.indexOf(window.connection_tab)] + [
        window.tab_index_by_key[spec.key] for spec in TEST_SPECS
    ]
    for index in browsable_indexes:
        tabs.setCurrentIndex(index)
        assert tabs.currentIndex() == index

    assert not window.execute_all_button.isEnabled()
    assert window.stop_button.isEnabled()
    qtbot.mouseClick(window.stop_button, Qt.LeftButton)
    assert session.stop_calls == 1

    session.busy_changed.emit(False)
    assert tabs.isTabEnabled(tabs.indexOf(window.echo_tab))


def test_common_commands_keep_icons_and_log_can_be_cleared(qtbot) -> None:
    window, _ = make_window(qtbot)

    assert not window.refresh_button.icon().isNull()
    assert not window.connect_button.icon().isNull()
    assert not window.clear_log_button.icon().isNull()
    assert not window.execute_all_button.icon().isNull()
    assert not window.stop_button.icon().isNull()

    window.log_edit.setPlainText("待清空日志")
    qtbot.mouseClick(window.clear_log_button, Qt.LeftButton)
    assert window.log_edit.toPlainText() == ""


def test_every_page_run_request_calls_session_run_test(qtbot) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    expected_calls = []

    for spec in TEST_SPECS:
        page = window.pages_by_key[spec.key]
        window.navigation_tabs.setCurrentIndex(window.tab_index_by_key[spec.key])
        parameters = page.collect_parameters()
        page.run_requested.emit(spec.key, parameters)
        expected_calls.append((spec.key, dict(parameters)))

    assert session.run_test_calls == expected_calls


def test_continuous_run_waits_for_finish_then_repeats_after_200_ms(qtbot) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    page = window.pages_by_key["system"]
    window.navigation_tabs.setCurrentIndex(window.tab_index_by_key["system"])

    qtbot.mouseClick(page.continuous_button, Qt.LeftButton)

    assert window._continuous_timer.isSingleShot()
    assert window._continuous_timer.interval() == 200
    assert session.run_test_calls == [("system", {})]
    qtbot.wait(220)
    assert session.run_test_calls == [("system", {})]

    session.test_finished.emit(
        "system", HardwareTestStatus.COMPLETED.value, "收到执行响应"
    )

    assert window.stop_button.isEnabled()
    assert not window.execute_all_button.isEnabled()
    assert not page.run_button.isEnabled()
    assert page.continuous_button.isEnabled()
    qtbot.wait(160)
    assert len(session.run_test_calls) == 1
    qtbot.wait(70)
    assert session.run_test_calls == [("system", {}), ("system", {})]


def test_stopping_continuous_run_cancels_pending_iteration(qtbot) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    page = window.pages_by_key["system"]
    window.navigation_tabs.setCurrentIndex(window.tab_index_by_key["system"])
    qtbot.mouseClick(page.continuous_button, Qt.LeftButton)
    session.test_finished.emit(
        "system", HardwareTestStatus.COMPLETED.value, "收到执行响应"
    )
    assert window._continuous_timer.isActive()

    qtbot.mouseClick(page.continuous_button, Qt.LeftButton)
    qtbot.wait(230)

    assert session.run_test_calls == [("system", {})]
    assert not window._continuous_timer.isActive()
    assert not page.continuous_button.isChecked()


def test_electrical_health_continuous_run_saves_all_received_samples(
    qtbot, tmp_path
) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    page = window.pages_by_key["electrical_health"]
    page.save_directory_edit.setText(str(tmp_path))
    window.navigation_tabs.setCurrentIndex(
        window.tab_index_by_key["electrical_health"]
    )

    qtbot.mouseClick(page.continuous_button, Qt.LeftButton)
    session.response_received.emit(
        "electrical_health",
        decoded_response(
            session,
            "elec_health_status_response",
            {"c_volt": 11.1, "b_volt": 12.2, "activate_bits": 0},
            sequence=300,
        ),
    )
    session.test_finished.emit(
        "electrical_health",
        HardwareTestStatus.COMPLETED.value,
        "收到执行响应",
    )
    qtbot.wait(230)
    session.response_received.emit(
        "electrical_health",
        decoded_response(
            session,
            "elec_health_status_response",
            {"c_volt": 21.1, "b_volt": 22.2, "activate_bits": 1},
            sequence=301,
        ),
    )
    session.test_finished.emit(
        "electrical_health",
        HardwareTestStatus.COMPLETED.value,
        "收到执行响应",
    )

    qtbot.mouseClick(page.continuous_button, Qt.LeftButton)

    paths = list(tmp_path.glob("ElectricalHealth_data_*.txt"))
    assert len(paths) == 1
    text = paths[0].read_text(encoding="utf-8-sig")
    data_lines = [
        line for line in text.splitlines() if line and not line.startswith("#")
    ]
    assert len(data_lines) == 3
    assert "\t300\t" in data_lines[1]
    assert "\t301\t" in data_lines[2]
    assert "\t11.1\t12.2\t" in data_lines[1]
    assert "\t21.1\t22.2\t" in data_lines[2]
    assert page.saved_file_label.text() == str(paths[0])
    assert "电气健康连续数据已保存" in window.log_edit.toPlainText()


def test_stopping_inflight_electrical_health_run_keeps_final_response(
    qtbot, tmp_path
) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    page = window.pages_by_key["electrical_health"]
    page.save_directory_edit.setText(str(tmp_path))
    window.navigation_tabs.setCurrentIndex(
        window.tab_index_by_key["electrical_health"]
    )

    qtbot.mouseClick(page.continuous_button, Qt.LeftButton)
    session.busy_changed.emit(True)
    qtbot.mouseClick(page.continuous_button, Qt.LeftButton)

    session.response_received.emit(
        "electrical_health",
        decoded_response(
            session,
            "elec_health_status_response",
            {"c_volt": 31.1, "b_volt": 32.2, "activate_bits": 1},
            sequence=302,
        ),
    )
    session.test_finished.emit(
        "electrical_health",
        HardwareTestStatus.COMPLETED.value,
        "收到执行响应",
    )

    paths = list(tmp_path.glob("ElectricalHealth_data_*.txt"))
    assert len(paths) == 1
    data_lines = [
        line
        for line in paths[0].read_text(encoding="utf-8-sig").splitlines()
        if line and not line.startswith("#")
    ]
    assert len(data_lines) == 2
    assert "\t302\t" in data_lines[1]


def test_session_start_resets_only_the_started_page(qtbot, monkeypatch) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    reset_calls = []

    for key, page in window.pages_by_key.items():
        monkeypatch.setattr(
            page,
            "reset_for_run",
            lambda page_key=key: reset_calls.append(page_key),
        )

    session.test_started.emit("dh")

    assert reset_calls == ["dh"]


def test_page_request_resets_once_via_session_start(qtbot, monkeypatch) -> None:
    window, _ = make_window(qtbot)
    page = window.pages_by_key["dh"]
    reset_calls = []
    monkeypatch.setattr(page, "reset_for_run", lambda: reset_calls.append("dh"))

    page.run_requested.emit("dh", page.collect_parameters())

    assert reset_calls == ["dh"]


@pytest.mark.parametrize(
    ("status", "detail"),
    (
        (HardwareTestStatus.COMPLETED.value, "已接收全部 DH 回告"),
        (HardwareTestStatus.EXECUTION_FAILED.value, "板端返回失败"),
        (HardwareTestStatus.EXECUTION_FAILED.value, "用户停止"),
    ),
)
def test_dh_finish_saves_reports_and_logs_path(
    qtbot, monkeypatch, tmp_path, status, detail
) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    page = window.pages_by_key["dh"]
    saved_path = tmp_path / "DH_data_20260720_131415_123456.txt"
    save_calls = []

    def save_reports(saved_status, saved_detail):
        save_calls.append((saved_status, saved_detail))
        return saved_path

    monkeypatch.setattr(page, "save_reports", save_reports, raising=False)

    session.test_finished.emit("dh", status, detail)

    assert save_calls == [(status, detail)]
    assert "DH 回告数据已保存：{}".format(saved_path) in window.log_edit.toPlainText()


def test_dh_save_failure_is_logged_without_escaping_signal_handler(
    qtbot, monkeypatch
) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    page = window.pages_by_key["dh"]

    def fail_to_save(_status, _detail):
        raise OSError("磁盘已满")

    monkeypatch.setattr(page, "save_reports", fail_to_save, raising=False)

    session.test_finished.emit(
        "dh", HardwareTestStatus.EXECUTION_FAILED.value, "用户停止"
    )

    assert "DH 回告数据保存失败：磁盘已满" in window.log_edit.toPlainText()


def test_execute_all_collects_parameters_from_every_page(qtbot) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    connect_window(qtbot, window)
    window.navigation_tabs.setCurrentIndex(window.tab_index_by_key["system"])
    expected_overrides = {
        spec.key: window.pages_by_key[spec.key].collect_parameters()
        for spec in TEST_SPECS
    }

    qtbot.mouseClick(window.execute_all_button, Qt.LeftButton)

    assert session.run_all_calls == [expected_overrides]


def test_execute_all_locates_invalid_page_without_sending_any_test(qtbot) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    connect_window(qtbot, window)
    window.navigation_tabs.setCurrentIndex(window.tab_index_by_key["system"])
    bus_page = window.pages_by_key["bus"]
    bus_page.bus_mode_control.set_current_data("echo")
    bus_page.data_input.setText("GG")

    qtbot.mouseClick(window.execute_all_button, Qt.LeftButton)

    assert window.navigation_tabs.currentIndex() == window.tab_index_by_key["bus"]
    assert session.run_all_calls == []
    assert session.run_test_calls == []


def test_session_status_and_response_are_routed_only_to_key_page(
    qtbot, monkeypatch
) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    status_calls = []

    for key, page in window.pages_by_key.items():
        monkeypatch.setattr(
            page,
            "set_test_status",
            lambda status, detail, page_key=key: status_calls.append(
                (page_key, status, detail)
            ),
        )

    session.test_status_changed.emit(
        "system", HardwareTestStatus.RUNNING.value, "开始"
    )
    assert status_calls == [
        ("system", HardwareTestStatus.RUNNING.value, "开始")
    ]

    session.response_received.emit(
        "system",
        decoded_response(
            session,
            "system_status_response",
            {"cpu_usage": 12.5},
        ),
    )

    assert window.pages_by_key["system"].last_response_name == (
        "system_status_response"
    )
    assert window.pages_by_key["system"].metrics.value("cpu_usage") == 12.5
    assert all(
        page.last_response_name is None
        for key, page in window.pages_by_key.items()
        if key != "system"
    )


def test_timer_stop_ack_keeps_start_statistics_visible(qtbot) -> None:
    window, _ = make_window(qtbot)
    session = window.fake_session
    timer_page = window.pages_by_key["timer"]
    start_values = {
        "buckets[{}]".format(index): (index + 1) * 25
        for index in range(8)
    }
    start_values.update({"avg_jitter": 1.25, "max_jitter": 3.5})

    session.response_received.emit(
        "timer",
        decoded_response(
            session, "timer_jitter_start_response", start_values
        ),
    )
    buckets_before_stop = list(timer_page.bucket_values)

    session.response_received.emit(
        "timer",
        decoded_response(session, "timer_jitter_stop_response"),
    )

    assert timer_page.bucket_values == buckets_before_stop
    assert timer_page.metrics.value("avg_jitter") == 1.25
    assert timer_page.metrics.value("max_jitter") == 3.5


@pytest.mark.parametrize("width,height", ((820, 620), (1080, 760)))
def test_window_shell_has_non_overlapping_visible_geometry(
    qtbot, width, height
) -> None:
    window, _ = make_window(qtbot)
    window.resize(width, height)
    QApplication.processEvents()

    assert (window.width(), window.height()) == (width, height)
    top_controls = (
        window.current_port_label,
        window.status_label,
        window.parser_mode_label,
        window.connect_button,
        window.execute_all_button,
        window.stop_button,
    )
    assert_visible_widgets_do_not_overlap(window, top_controls)
    navigation_geometry = widget_geometry_in_window(
        window, window.navigation_tabs
    )
    assert navigation_geometry.width() > 0
    assert navigation_geometry.height() > 0
    for control in top_controls:
        assert not navigation_geometry.intersects(
            widget_geometry_in_window(window, control)
        )

    window.navigation_tabs.setCurrentWidget(window.connection_tab)
    QApplication.processEvents()
    assert_visible_widgets_do_not_overlap(
        window,
        (
            window.port_combo,
            window.refresh_button,
            window.baud_combo,
            window.data_bits_combo,
            window.parity_combo,
            window.stop_bits_combo,
            window.flow_control_combo,
            window.log_edit,
            window.clear_log_button,
        ),
    )

    window.navigation_tabs.setCurrentWidget(window.echo_tab)
    QApplication.processEvents()
    assert_visible_widgets_do_not_overlap(
        window,
        (window.payload_input, window.send_button, window.result_label),
    )


def test_dh_channels_stay_inside_parameter_viewport_at_minimum_size(qtbot) -> None:
    application_main.configure_application(QApplication.instance())
    window, _ = make_window(qtbot)
    window.resize(820, 620)
    page = window.pages_by_key["dh"]
    window.navigation_tabs.setCurrentWidget(page)
    QApplication.processEvents()

    scroll_area = page.findChild(QScrollArea, "parameterScrollArea")
    assert scroll_area is not None
    assert_widgets_fit_scroll_viewport_width(
        scroll_area, page.channel_checks
    )


def test_dh_pulse_width_editors_stay_inside_parameter_viewport_at_minimum_size(
    qtbot,
) -> None:
    application_main.configure_application(QApplication.instance())
    window, _ = make_window(qtbot)
    window.resize(820, 620)
    page = window.pages_by_key["dh_pulse_config"]
    window.navigation_tabs.setCurrentWidget(page)
    QApplication.processEvents()

    scroll_area = page.findChild(QScrollArea, "parameterScrollArea")
    assert scroll_area is not None
    assert_widgets_fit_scroll_viewport_width(
        scroll_area, page.pulse_width_spins
    )


def test_hardware_parameter_rows_fit_at_minimum_size(qtbot) -> None:
    application_main.configure_application(QApplication.instance())
    window, _ = make_window(qtbot)
    window.resize(820, 620)
    failures = []
    for key, page in window.pages_by_key.items():
        window.navigation_tabs.setCurrentWidget(page)
        QApplication.processEvents()

        scroll_area = page.findChild(QScrollArea, "parameterScrollArea")
        assert scroll_area is not None
        visible_row_widgets = []
        for row in range(page.parameter_layout.rowCount()):
            for role in (
                QFormLayout.LabelRole,
                QFormLayout.FieldRole,
                QFormLayout.SpanningRole,
            ):
                item = page.parameter_layout.itemAt(row, role)
                widget = item.widget() if item is not None else None
                if widget is not None and widget.isVisible():
                    visible_row_widgets.append(widget)
        try:
            assert_widgets_fit_scroll_viewport_width(
                scroll_area, visible_row_widgets
            )
        except AssertionError as exc:
            failures.append("{}: {}".format(key, exc))
    assert not failures, "\n".join(failures)


def test_bit_grid_labels_fit_at_minimum_size(qtbot) -> None:
    application_main.configure_application(QApplication.instance())
    window, _ = make_window(qtbot)
    window.resize(820, 620)
    failures = []
    for key, page in window.pages_by_key.items():
        window.navigation_tabs.setCurrentWidget(page)
        QApplication.processEvents()
        for grid in page.findChildren(BitGrid):
            for label in grid.name_labels:
                if label.sizeHint().width() > label.width():
                    failures.append(
                        "{}: {} 需要 {} px，实际 {} px".format(
                            key,
                            label.text(),
                            label.sizeHint().width(),
                            label.width(),
                        )
                    )
    assert not failures, "\n".join(failures)


def test_helm_board_controls_fit_at_minimum_size(qtbot) -> None:
    application_main.configure_application(QApplication.instance())
    window, _ = make_window(qtbot)
    window.resize(820, 620)
    page = window.pages_by_key["helm_board"]
    window.navigation_tabs.setCurrentWidget(page)
    QApplication.processEvents()

    scroll_area = page.findChild(QScrollArea, "parameterScrollArea")
    assert scroll_area is not None
    assert_widgets_fit_scroll_viewport_width(
        scroll_area, page.pwm_duty_percent_spins + page.direction_checks
    )


def test_timer_y_axis_label_fits_at_minimum_size(qtbot) -> None:
    application_main.configure_application(QApplication.instance())
    window, _ = make_window(qtbot)
    window.resize(820, 620)
    page = window.pages_by_key["timer"]
    window.navigation_tabs.setCurrentWidget(page)
    QApplication.processEvents()

    page.canvas.draw()
    label_bounds = page.axes.yaxis.label.get_window_extent(
        page.canvas.get_renderer()
    )
    assert label_bounds.x0 >= page.figure.bbox.x0
