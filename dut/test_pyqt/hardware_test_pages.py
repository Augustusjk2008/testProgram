"""Protocol-specific pages for the product hardware tests."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple, Type

from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg
from matplotlib.figure import Figure
from matplotlib.font_manager import FontProperties
from PyQt5.QtCore import QStandardPaths, Qt, QTimer
from PyQt5.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QPushButton,
    QSizePolicy,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .dh_report_storage import (
    write_dh_text_report,
    write_electrical_health_text_report,
)
from .hardware_test_session import TestSpec
from .product_protocol import DecodedMessage
from .protocol_catalog import ProtocolCatalog
from .test_page_widgets import (
    BaseHardwareTestPage,
    BitGrid,
    ParameterValidationError,
    SegmentedControl,
    U32HexEdit,
)


CHART_FONT = FontProperties(family="Microsoft YaHei")


# Signal names are copied from origin_v3/DIDO_ctrl. Bit cells stay compact;
# the visible legend carries the complete board names and polarity.
DIDO_DI_SIGNALS = {
    0: "DI0 联锁、电气弹动（高有效）",
    1: "DI1 引信报警（高有效）",
    2: "DI2 引信起爆指令（高有效）",
    3: "DI3 锁相环锁定指示（高有效）",
    8: "DI8 投放允许（低有效）",
}
DIDO_DI_TOOLTIPS = {
    0: "DI0 / 0x140080：联锁、电气弹动；DI0~DI7 高有效",
    1: "DI1 / 0x140084：引信报警；DI0~DI7 高有效",
    2: "DI2 / 0x140088：引信起爆指令；DI0~DI7 高有效",
    3: "DI3 / 0x14008C：锁相环锁定指示；DI0~DI7 高有效",
    8: "DI8 / 0x1400A0：投放允许；DI8~DI15 低有效",
}
DIDO_DO_SIGNALS = {
    0: "DO0 舵锁使能",
    1: "DO1 数控衰减器控制",
    2: "DO2 数遥发送使能",
    3: "DO3 24V_EN（物理低使能）；DO_WRITE bit=0 为使能、bit=1 为失能",
    4: "DO4 DYT_5V_EN（物理低使能）；DO_WRITE bit=0 为使能、bit=1 为失能",
    5: "DO5 DI_EN1（无需控制，恒拉低）",
    6: "DO6 DO_EN（无需控制，恒拉低）",
}
DIDO_DO_TOOLTIPS = {
    0: "DO0 / 0x140004：舵锁使能；DO0~DO7 高有效",
    1: "DO1 / 0x140008：数控衰减器控制；DO0~DO7 高有效",
    2: "DO2 / 0x14000C：数遥发送使能；DO0~DO7 高有效",
    3: "DO3 / 0x140010：24V_EN；物理低使能；回读 bit=0 表示使能，bit=1 表示失能",
    4: "DO4 / 0x140014：DYT_5V_EN；物理低使能；回读 bit=0 表示使能，bit=1 表示失能",
    5: "DO5 / 0x140018：DI_EN1；地址表标注无需控制、恒拉低，PC 已禁用此控件，勿作为 5V_JS 控制",
    6: "DO6 / 0x14001C：DO_EN；点火、PWM 和通用 DO 输出使能；地址表标注无需控制、恒拉低，PC 已禁用此控件",
}


def _make_dido_legend(
    signals: Mapping[int, str], parent: QWidget, object_name: str
) -> QLabel:
    legend = QLabel("板级信号：\n" + "；\n".join(signals.values()), parent)
    legend.setObjectName(object_name)
    legend.setProperty("muted", True)
    legend.setWordWrap(True)
    legend.setTextInteractionFlags(Qt.TextSelectableByMouse)
    return legend


def _set_dido_tooltips(grid: BitGrid, tooltips: Mapping[int, str]) -> None:
    for index, tooltip in tooltips.items():
        if index < len(grid.cells):
            grid.cells[index].setToolTip(tooltip)
            grid.name_labels[index].setToolTip(tooltip)
            grid.state_labels[index].setToolTip(tooltip)


def _message_failed(decoded: DecodedMessage) -> bool:
    return decoded.status != 0 or decoded.err_code != 0


def _format_scalar(value: object) -> str:
    if isinstance(value, float):
        return "{:g}".format(value)
    return str(value)


def _format_bytes(data: Iterable[int]) -> str:
    return " ".join("{:02X}".format(int(value) & 0xFF) for value in data)


def _set_form_row_visible(
    form: QFormLayout, field: QWidget, visible: bool
) -> None:
    label = form.labelForField(field)
    if label is not None:
        label.setVisible(visible)
    field.setVisible(visible)


def _set_bits(grid: BitGrid, words: Sequence[int], count: int) -> None:
    for index in range(count):
        word_index, bit_index = divmod(index, 32)
        word = int(words[word_index]) if word_index < len(words) else 0
        grid.set_bit(index, bool(word & (1 << bit_index)))


def _configure_result_table(
    table: QTableWidget, headers: Sequence[str]
) -> None:
    table.setHorizontalHeaderLabels(list(headers))
    table.setEditTriggers(QTableWidget.NoEditTriggers)
    table.setSelectionBehavior(QTableWidget.SelectRows)
    table.verticalHeader().setVisible(False)
    header = table.horizontalHeader()
    for column in range(len(headers)):
        header.setSectionResizeMode(column, QHeaderView.Stretch)


def _make_channel_selector(
    checks: Sequence[QCheckBox],
    clear_button: QPushButton,
    select_all_button: QPushButton,
    columns: int,
) -> QWidget:
    container = QWidget()
    layout = QVBoxLayout(container)
    layout.setContentsMargins(0, 0, 0, 0)
    actions = QHBoxLayout()
    actions.addWidget(clear_button)
    actions.addWidget(select_all_button)
    actions.addStretch(1)
    layout.addLayout(actions)

    grid = QGridLayout()
    grid.setContentsMargins(0, 0, 0, 0)
    for index, check in enumerate(checks):
        grid.addWidget(check, index // columns, index % columns)
    layout.addLayout(grid)
    return container


class _MetricPage(BaseHardwareTestPage):
    metric_definitions: Tuple[Tuple[str, str, str], ...] = ()

    def build_result_widget(self) -> QWidget:
        for key, label, unit in self.metric_definitions:
            self.metrics.add_metric(key, label, unit)
        return self.metrics

    def render_success(self, decoded: DecodedMessage) -> None:
        self.metrics.clear()
        for key, _label, _unit in self.metric_definitions:
            if key in decoded.values:
                self.metrics.set_value(key, decoded.values[key])
            else:
                self.metrics.set_unavailable(key)

    def clear_results(self) -> None:
        self.metrics.clear()

    def measurements_unavailable(self) -> None:
        for key, _label, _unit in self.metric_definitions:
            self.metrics.set_unavailable(key)


class SystemStatusPage(_MetricPage):
    accepted_response_names = ("system_status_response",)
    metric_definitions = (
        ("cpu_usage", "CPU 占用率", "%"),
        ("mem_usage", "内存占用率", "%"),
        ("cpu_freq_little", "CPU 小核频率", "MHz"),
        ("cpu_freq_big", "CPU 大核频率", "MHz"),
        ("pcie_speed", "PCIe 链路速率", "GT/s"),
        ("pcie_width", "PCIe 链路宽度", "lane"),
        ("net_init_time", "网络初始化时间", "s"),
        ("cpu_temp", "处理器中心温度", "°C"),
        ("rk_temp", "RK3588 温度", "°C"),
        ("k7_temp", "K7 温度", "°C"),
        ("power_on_sec", "累计通电时间", "s"),
    )


class MemoryTestPage(_MetricPage):
    accepted_response_names = ("memperf_test_response",)
    metric_definitions = (
        ("error_count", "错误计数", ""),
        ("first_fail_addr", "首个错误地址", "byte"),
        ("elapsed_ms", "执行耗时", "ms"),
        ("write_bandwidth", "写带宽", "MB/s"),
        ("read_bandwidth", "读带宽", "MB/s"),
    )

    MEMORY_TYPES = (
        ("固定种子图样", 0),
        ("种子/反码交替图样", 1),
        ("种子异或地址图样", 2),
        ("读取带宽", 3),
        ("写入带宽", 4),
        ("拷贝带宽", 5),
        ("NT Store 带宽", 6),
    )

    def build_parameters(self) -> None:
        self.memory_type_combo = QComboBox(self)
        self.memory_type_combo.setObjectName("memoryTypeCombo")
        for label, value in self.MEMORY_TYPES:
            self.memory_type_combo.addItem(label, value)
        self.memory_type_combo.setCurrentIndex(
            self.memory_type_combo.findData(int(self.spec.defaults["memperf_type"]))
        )

        self.length_spin = QSpinBox(self)
        self.length_spin.setObjectName("memoryLengthSpin")
        self.length_spin.setRange(1, 262144)
        self.length_spin.setSuffix(" KB")
        self.length_spin.setValue(int(self.spec.defaults["length"]))

        self.seed_input = U32HexEdit(int(self.spec.defaults["seed"]), self)
        self.seed_input.setObjectName("memorySeedInput")

        self.add_parameter_row("测试类型", self.memory_type_combo)
        self.add_parameter_row("校验长度", self.length_spin)
        self.add_parameter_row("图样种子", self.seed_input)

    def collect_parameters(self) -> Dict[str, object]:
        return {
            "memperf_type": int(self.memory_type_combo.currentData()),
            "length": int(self.length_spin.value()),
            "seed": int(self.seed_input.value()),
        }


class SpiFlashTestPage(_MetricPage):
    accepted_response_names = ("spi_flash_test_response",)
    metric_definitions = (("sjl_result", "固定测试区耗时", "s"),)

    def build_parameters(self) -> None:
        self.risk_label = QLabel(
            "该测试会擦除、写入并读回 SPI Flash 固定 4 KiB 测试区，"
            "不备份、不恢复。仅可在允许写入的隔离目标板上执行。",
            self,
        )
        self.risk_label.setObjectName("spiFlashRiskLabel")
        self.risk_label.setWordWrap(True)
        self.risk_label.setProperty("role", "warning")
        self.add_parameter_row("写入风险", self.risk_label)


class BusTestPage(BaseHardwareTestPage):
    accepted_response_names = (
        "bus_loop_test_response",
        "bus_echo_test_response",
    )

    LINK_OPTIONS = (
        ("link 0 · COM1", 0),
        ("link 1 · COM2", 1),
        ("link 3 · COM4", 3),
    )

    def __init__(
        self,
        spec: TestSpec,
        catalog: ProtocolCatalog,
        parent: Optional[QWidget] = None,
    ) -> None:
        self._last_echo_bytes = b""
        super().__init__(spec, catalog, parent)
        self.action_layout.removeWidget(self.continuous_button)
        self.continuous_button.hide()

    def build_parameters(self) -> None:
        self.bus_mode_control = SegmentedControl(
            (("LOOP", "loop"), ("ECHO", "echo")), self
        )
        self.bus_mode_control.setObjectName("busModeControl")
        self.bus_mode_control.set_current_data(
            str(self.spec.defaults.get("bus_mode", "loop"))
        )

        self.link_combo = QComboBox(self)
        self.link_combo.setObjectName("busLinkCombo")
        for label, link_id in self.LINK_OPTIONS:
            self.link_combo.addItem(label, link_id)
        default_link = int(self.spec.defaults.get("link_id", 0))
        self.link_combo.setCurrentIndex(self.link_combo.findData(default_link))

        self.count_spin = QSpinBox(self)
        self.count_spin.setObjectName("busCountSpin")
        self.count_spin.setRange(1, 100000)
        self.count_spin.setValue(int(self.spec.defaults.get("total_count", 1000)))

        self.data_input = QLineEdit(self)
        self.data_input.setObjectName("busEchoDataInput")
        self.data_input.setPlaceholderText("4D 42 31")
        self.data_input.setText(str(self.spec.defaults.get("data_hex", "4D 42 31")))

        self.bus_boundary_label = QLabel(
            "本工具仅通过 COM3 控制口发送产品协议；link 2 是控制口，始终不可测。"
            "BUS_ECHO 的外部 ECHO 回送端必须是 COM1/COM2/COM4 上的独立 PC 或夹具，"
            "本工具不是外部 ECHO 回送端，也不构成该链路验收。"
            "连续轮次由根宿主 pc_periodic 管理。",
            self,
        )
        self.bus_boundary_label.setObjectName("busBoundaryLabel")
        self.bus_boundary_label.setWordWrap(True)
        self.bus_boundary_label.setProperty("muted", True)

        self.add_parameter_row("测试方式", self.bus_mode_control)
        self.add_parameter_row("测试链路", self.link_combo)
        self.add_parameter_row("收发次数", self.count_spin)
        self.add_parameter_row("回显数据", self.data_input)
        self.add_parameter_row("使用边界", self.bus_boundary_label)

        self.bus_mode_control.current_changed.connect(
            lambda _value: self._update_mode_controls()
        )
        self._update_mode_controls()

    def build_result_widget(self) -> QWidget:
        self.metrics.add_metric("link_id", "响应链路", "")
        self.metrics.add_metric("error_count", "错误计数", "")
        self.metrics.add_metric("total_count", "实际收发次数", "")
        self.metrics.add_metric("elapsed_ms", "执行耗时", "ms")

        root = QWidget(self)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.metrics)

        self.echo_comparison_group = QGroupBox("发送 / 实际接收数据对照（114 字节）", root)
        comparison_form = QFormLayout(self.echo_comparison_group)
        self.sent_data_edit = QLineEdit(self.echo_comparison_group)
        self.sent_data_edit.setObjectName("busSentData")
        self.sent_data_edit.setReadOnly(True)
        self.received_data_edit = QLineEdit(self.echo_comparison_group)
        self.received_data_edit.setObjectName("busReceivedData")
        self.received_data_edit.setReadOnly(True)
        comparison_form.addRow("发送（协议 114 B）", self.sent_data_edit)
        comparison_form.addRow("接收（实际）", self.received_data_edit)
        layout.addWidget(self.echo_comparison_group)
        layout.addStretch(1)
        self.echo_comparison_group.setVisible(False)
        return root

    def _update_mode_controls(self) -> None:
        is_loop = self.bus_mode_control.current_data() == "loop"
        _set_form_row_visible(self.parameter_layout, self.count_spin, is_loop)
        _set_form_row_visible(self.parameter_layout, self.data_input, not is_loop)

    def _echo_bytes(self) -> bytes:
        text = self.data_input.text().strip().replace(",", " ")
        try:
            data = bytes.fromhex(text)
        except ValueError as exc:
            raise ParameterValidationError(
                "总线回显数据不是有效十六进制", self.data_input
            ) from exc
        if len(data) > 114:
            raise ParameterValidationError(
                "总线回显数据不能超过 114 字节", self.data_input
            )
        return data

    def collect_parameters(self) -> Dict[str, object]:
        mode = str(self.bus_mode_control.current_data())
        link_id = int(self.link_combo.currentData())
        if mode == "loop":
            return {
                "bus_mode": "loop",
                "link_id": link_id,
                "total_count": int(self.count_spin.value()),
            }
        if mode != "echo":
            raise ParameterValidationError("总线测试方式无效", self.bus_mode_control)
        self._last_echo_bytes = self._echo_bytes()
        return {
            "bus_mode": "echo",
            "link_id": link_id,
            "data_hex": _format_bytes(self._last_echo_bytes),
        }

    def render_success(self, decoded: DecodedMessage) -> None:
        self.metrics.clear()
        if "link_id" in decoded.values:
            self.metrics.set_value("link_id", decoded.values["link_id"])
        if decoded.name == "bus_loop_test_response":
            self.echo_comparison_group.setVisible(False)
            for key in ("error_count", "total_count", "elapsed_ms"):
                if key in decoded.values:
                    self.metrics.set_value(key, decoded.values[key])
                else:
                    self.metrics.set_unavailable(key)
            return

        self.echo_comparison_group.setVisible(True)
        expected = self._last_echo_bytes
        if not expected:
            try:
                expected = self._echo_bytes()
            except ParameterValidationError:
                expected = b""
        expected = expected + bytes(max(0, 114 - len(expected)))
        received = bytes(
            int(decoded.values.get("data[{}]".format(index), 0)) & 0xFF
            for index in range(114)
        )
        self.sent_data_edit.setText(_format_bytes(expected))
        self.received_data_edit.setText(_format_bytes(received))
        for key in ("error_count", "total_count", "elapsed_ms"):
            self.metrics.set_unavailable(key)

    def clear_results(self) -> None:
        self.metrics.clear()
        self.sent_data_edit.clear()
        self.received_data_edit.clear()
        self.echo_comparison_group.setVisible(
            self.bus_mode_control.current_data() == "echo"
        )

    def measurements_unavailable(self) -> None:
        for key in ("link_id", "error_count", "total_count", "elapsed_ms"):
            self.metrics.set_unavailable(key)


class DiTestPage(BaseHardwareTestPage):
    accepted_response_names = ("di_read_response",)

    def build_result_widget(self) -> QWidget:
        result = QWidget(self)
        layout = QVBoxLayout(result)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(8)
        self.signal_legend = _make_dido_legend(
            DIDO_DI_SIGNALS, result, "diSignalLegend"
        )
        self.bit_grid = BitGrid(
            64,
            tuple("DI{}".format(index) for index in range(64)),
            result,
            columns=8,
        )
        self.bit_grid.setObjectName("diStateGrid")
        _set_dido_tooltips(self.bit_grid, DIDO_DI_TOOLTIPS)
        layout.addWidget(self.signal_legend)
        layout.addWidget(self.bit_grid, 1)
        return result

    def render_success(self, decoded: DecodedMessage) -> None:
        _set_bits(
            self.bit_grid,
            (
                int(decoded.values.get("di_state[0]", 0)),
                int(decoded.values.get("di_state[1]", 0)),
            ),
            64,
        )

    def clear_results(self) -> None:
        self.bit_grid.clear()

    def measurements_unavailable(self) -> None:
        self.bit_grid.set_unavailable()


class DoTestPage(BaseHardwareTestPage):
    accepted_response_names = ("do_write_response",)

    def build_parameters(self) -> None:
        self.channel_checks = [
            QCheckBox("DO{}".format(index), self) for index in range(16)
        ]
        for index, check in enumerate(self.channel_checks):
            check.setObjectName("doChannel{}".format(index))
            if index in DIDO_DO_TOOLTIPS:
                check.setToolTip(DIDO_DO_TOOLTIPS[index])
            if index in (5, 6):
                check.setEnabled(False)
        self.clear_channels_button = QPushButton("全清", self)
        self.select_all_channels_button = QPushButton("全选", self)
        self.clear_channels_button.clicked.connect(
            lambda: self._set_all_channels(False)
        )
        self.select_all_channels_button.clicked.connect(
            lambda: self._set_all_channels(True)
        )
        selector = _make_channel_selector(
            self.channel_checks,
            self.clear_channels_button,
            self.select_all_channels_button,
            2,
        )
        self.add_parameter_row("输出通道", selector)

    def build_result_widget(self) -> QWidget:
        result = QWidget(self)
        layout = QVBoxLayout(result)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(8)
        self.signal_legend = _make_dido_legend(
            DIDO_DO_SIGNALS, result, "doSignalLegend"
        )
        self.applied_bits = BitGrid(
            16,
            tuple("DO{}".format(index) for index in range(16)),
            result,
            columns=4,
        )
        self.applied_bits.setObjectName("doAppliedStateGrid")
        _set_dido_tooltips(self.applied_bits, DIDO_DO_TOOLTIPS)
        layout.addWidget(self.signal_legend)
        layout.addWidget(self.applied_bits, 1)
        return result

    def _set_all_channels(self, checked: bool) -> None:
        for channel_check in self.channel_checks:
            if channel_check.isEnabled():
                channel_check.setChecked(checked)

    def collect_parameters(self) -> Dict[str, object]:
        first_word = 0
        for index, channel_check in enumerate(self.channel_checks):
            if channel_check.isEnabled() and channel_check.isChecked():
                first_word |= 1 << index
        return {"channel[0]": first_word, "channel[1]": 0}

    def render_success(self, decoded: DecodedMessage) -> None:
        _set_bits(
            self.applied_bits,
            (int(decoded.values.get("applied_state[0]", 0)),),
            16,
        )

    def clear_results(self) -> None:
        self.applied_bits.clear()

    def measurements_unavailable(self) -> None:
        self.applied_bits.set_unavailable()


class ElectricalHealthPage(BaseHardwareTestPage):
    accepted_response_names = ("elec_health_status_response",)

    METRICS = (
        ("c_volt", "C 组电压", "V"),
        ("b_volt", "B 组电压", "V"),
        ("external_vol", "3.3V 外部电压", "V"),
        ("core_vol", "K7 内核电压", "V"),
        ("assist_vol", "K7 辅助电压", "V"),
        ("v28_5", "28.5V 一次电源", "V"),
        ("js_5V", "5V_JS 电源轨", "V"),
        ("dyt_5V", "5V_DYT 电源轨", "V"),
        ("power_24V", "24V_SY 电源轨", "V"),
        ("value_YX", "引信检测遥测", "V"),
    )

    def __init__(
        self,
        spec: TestSpec,
        catalog: ProtocolCatalog,
        parent: Optional[QWidget] = None,
    ) -> None:
        self._continuous_capture_active = False
        self._continuous_capture_started_at: Optional[datetime] = None
        self._continuous_reports: List[Tuple[datetime, DecodedMessage]] = []
        super().__init__(spec, catalog, parent)

    def build_parameters(self) -> None:
        documents_directory = QStandardPaths.writableLocation(
            QStandardPaths.DocumentsLocation
        )
        default_directory = documents_directory or str(Path.home())
        self.save_directory_edit = QLineEdit(default_directory, self)
        self.save_directory_edit.setObjectName("electricalHealthSaveDirectoryEdit")
        self.save_directory_button = QPushButton("选择...", self)
        self.save_directory_button.setObjectName(
            "electricalHealthSaveDirectoryButton"
        )
        self.save_directory_button.clicked.connect(self._choose_save_directory)
        save_directory_widget = QWidget(self)
        save_directory_layout = QHBoxLayout(save_directory_widget)
        save_directory_layout.setContentsMargins(0, 0, 0, 0)
        save_directory_layout.setSpacing(4)
        save_directory_layout.addWidget(self.save_directory_edit, 1)
        save_directory_layout.addWidget(self.save_directory_button)
        self.add_parameter_row("连续保存目录", save_directory_widget)

        self.saved_file_label = QLabel("尚未保存", self)
        self.saved_file_label.setObjectName("electricalHealthSavedFileLabel")
        self.saved_file_label.setProperty("muted", True)
        self.saved_file_label.setWordWrap(True)
        self.saved_file_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.add_parameter_row("连续保存文件", self.saved_file_label)

    def _choose_save_directory(self) -> None:
        directory = QFileDialog.getExistingDirectory(
            self,
            "选择电气健康数据保存目录",
            self.save_directory_edit.text().strip(),
        )
        if directory:
            self.save_directory_edit.setText(directory)

    def build_result_widget(self) -> QWidget:
        for key, label, unit in self.METRICS:
            self.metrics.add_metric(key, label, unit)
        root = QWidget(self)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.metrics)
        self.activate_bits = BitGrid(
            1,
            ("BC激活好",),
            root,
            columns=1,
        )
        self.activate_bits.setObjectName("electricalActivateBits")
        layout.addWidget(self.activate_bits)
        layout.addStretch(1)
        return root

    def render_success(self, decoded: DecodedMessage) -> None:
        self.metrics.clear()
        for key, _label, _unit in self.METRICS:
            if key in decoded.values:
                self.metrics.set_value(key, decoded.values[key])
            else:
                self.metrics.set_unavailable(key)
        _set_bits(
            self.activate_bits,
            (int(decoded.values.get("activate_bits", 0)),),
            1,
        )

    def render_response(self, decoded: DecodedMessage) -> bool:
        handled = super().render_response(decoded)
        if handled and self._continuous_capture_active:
            self._continuous_reports.append((datetime.now(), decoded))
        return handled

    def start_continuous_capture(self) -> None:
        self._continuous_capture_active = True
        self._continuous_capture_started_at = datetime.now()
        self._continuous_reports = []
        self.saved_file_label.setText("连续采集中")

    def finish_continuous_capture(
        self, final_status: str, final_detail: str = ""
    ) -> Optional[Path]:
        if not self._continuous_capture_active:
            return None
        self._continuous_capture_active = False
        started_at = self._continuous_capture_started_at or datetime.now()
        saved_path = write_electrical_health_text_report(
            self.save_directory_edit.text().strip(),
            tuple(self._continuous_reports),
            started_at=started_at,
            finished_at=datetime.now(),
            final_status=final_status,
            final_detail=final_detail,
        )
        self.saved_file_label.setText(str(saved_path))
        return saved_path

    def clear_results(self) -> None:
        self.metrics.clear()
        self.activate_bits.clear()

    def measurements_unavailable(self) -> None:
        for key, _label, _unit in self.METRICS:
            self.metrics.set_unavailable(key)
        self.activate_bits.set_unavailable()


class DhPulseConfigPage(BaseHardwareTestPage):
    accepted_response_names = ("dh_pulse_config_response",)

    def build_parameters(self) -> None:
        self.config_enable_check = QCheckBox("使能", self)
        self.config_enable_check.setObjectName("dhPulseConfigEnable")
        self.config_enable_check.setChecked(
            bool(self.spec.defaults.get("config_enable", 1))
        )
        self.add_parameter_row("配置使能", self.config_enable_check)

        self.pulse_width_spins = []
        for index in range(23):
            editor = QSpinBox(self)
            editor.setObjectName("dhPulseWidth{}".format(index))
            editor.setRange(0, 65535)
            editor.setSuffix(" ms")
            editor.setValue(
                int(
                    self.spec.defaults.get(
                        "pulse_width[{}]".format(index),
                        80 if index == 0 else 63,
                    )
                )
            )
            self.pulse_width_spins.append(editor)
            self.add_parameter_row("DH{} 脉宽".format(index), editor)
        self.config_enable_check.toggled.connect(self._set_widths_enabled)
        self._set_widths_enabled(self.config_enable_check.isChecked())

    def _set_widths_enabled(self, enabled: bool) -> None:
        for editor in self.pulse_width_spins:
            editor.setEnabled(bool(enabled))

    def collect_parameters(self) -> Dict[str, object]:
        values = {
            "config_enable": int(self.config_enable_check.isChecked())
        }
        for index, editor in enumerate(self.pulse_width_spins):
            values["pulse_width[{}]".format(index)] = int(editor.value())
        return values

    def build_result_widget(self) -> QWidget:
        self.readback_table = QTableWidget(23, 2, self)
        self.readback_table.setObjectName("dhPulseReadbackTable")
        _configure_result_table(self.readback_table, ("通道", "脉宽回读"))
        for index in range(23):
            self.readback_table.setItem(
                index, 0, QTableWidgetItem("DH{}".format(index))
            )
            self.readback_table.setItem(index, 1, QTableWidgetItem("不可用"))
        return self.readback_table

    def render_success(self, decoded: DecodedMessage) -> None:
        for index in range(23):
            value = decoded.values.get(
                "pulse_width_readback[{}]".format(index)
            )
            text = "不可用" if value is None else "{} ms".format(_format_scalar(value))
            self.readback_table.item(index, 1).setText(text)

    def clear_results(self) -> None:
        for index in range(23):
            self.readback_table.item(index, 1).setText("不可用")


class DhTestPage(BaseHardwareTestPage):
    accepted_response_names = ("dh_control_response",)

    _UI_REFRESH_INTERVAL_MS = 100
    _IMMEDIATE_REPORTS = 2
    _MAX_CHART_POINTS = 5000

    _STATUS_TEXT = {
        0: "未 DH",
        1: "成功",
        2: "失败",
        3: "保留",
    }

    def __init__(
        self,
        spec: TestSpec,
        catalog: ProtocolCatalog,
        parent: Optional[QWidget] = None,
    ) -> None:
        self.reports: List[DecodedMessage] = []
        self._report_keys = set()
        self._selector_synced_count = 0
        self._sample_interval_us = 2500
        self._sample_delay_us = 0
        self._run_started_at: Optional[datetime] = None
        self._run_parameters: Dict[str, object] = {}
        self._selected_channel_mask = (
            int(spec.defaults.get("channel[0]", 0)) & 0x007FFFFF
        )
        self.sample_times_ms: List[float] = []
        self.status_series: List[List[float]] = [[] for _ in range(23)]
        self.telemetry_series: List[List[float]] = [[] for _ in range(23)]
        super().__init__(spec, catalog, parent)
        self._ui_refresh_timer = QTimer(self)
        self._ui_refresh_timer.setSingleShot(True)
        self._ui_refresh_timer.setInterval(self._UI_REFRESH_INTERVAL_MS)
        self._ui_refresh_timer.timeout.connect(self._flush_pending_ui)

    def build_parameters(self) -> None:
        self.power_enable_check = QCheckBox("使能", self)
        self.power_enable_check.setObjectName("dhPowerEnable")
        self.power_enable_check.setChecked(
            bool(self.spec.defaults.get("power_enable", 1))
        )
        self.return_enable_check = QCheckBox("使能", self)
        self.return_enable_check.setObjectName("dhReturnEnable")
        self.return_enable_check.setChecked(
            bool(self.spec.defaults.get("return_enable", 1))
        )
        self.add_parameter_row("DH 电源", self.power_enable_check)
        self.add_parameter_row("DH 回线", self.return_enable_check)

        self.channel_checks = [
            QCheckBox("DH{}".format(index), self) for index in range(23)
        ]
        default_words = (
            int(self.spec.defaults.get("channel[0]", 0)),
            int(self.spec.defaults.get("channel[1]", 0)),
        )
        for index, check in enumerate(self.channel_checks):
            check.setObjectName("dhChannel{}".format(index))
            word_index, bit_index = divmod(index, 32)
            check.setChecked(bool(default_words[word_index] & (1 << bit_index)))
        self.clear_channels_button = QPushButton("全清", self)
        self.select_all_channels_button = QPushButton("全选", self)
        self.clear_channels_button.clicked.connect(
            lambda: self._set_all_channels(False)
        )
        self.select_all_channels_button.clicked.connect(
            lambda: self._set_all_channels(True)
        )
        selector = _make_channel_selector(
            self.channel_checks,
            self.clear_channels_button,
            self.select_all_channels_button,
            2,
        )

        self.report_count_spin = QSpinBox(self)
        self.report_count_spin.setObjectName("dhReportCountSpin")
        self.report_count_spin.setRange(1, 65535)
        self.report_count_spin.setValue(
            int(self.spec.defaults.get("report_count", 1))
        )
        self.interval_spin = QSpinBox(self)
        self.interval_spin.setObjectName("dhIntervalSpin")
        self.interval_spin.setRange(2500, 65535)
        self.interval_spin.setSuffix(" us")
        self.interval_spin.setValue(int(self.spec.defaults.get("interval_us", 2500)))
        self.delay_spin = QSpinBox(self)
        self.delay_spin.setObjectName("dhDelaySpin")
        self.delay_spin.setRange(0, 65535)
        self.delay_spin.setSuffix(" us")
        self.delay_spin.setValue(int(self.spec.defaults.get("delay_us", 0)))

        self.add_parameter_row("通道选择", selector)
        self.add_parameter_row("回告次数", self.report_count_spin)
        self.add_parameter_row("回告间隔", self.interval_spin)
        self.add_parameter_row("首帧等待", self.delay_spin)

        default_directory = QStandardPaths.writableLocation(
            QStandardPaths.DocumentsLocation
        )
        if not default_directory:
            default_directory = str(Path.home())
        self.save_directory_edit = QLineEdit(default_directory, self)
        self.save_directory_edit.setObjectName("dhSaveDirectoryEdit")
        self.save_directory_button = QPushButton("选择...", self)
        self.save_directory_button.setObjectName("dhSaveDirectoryButton")
        self.save_directory_button.clicked.connect(self._choose_save_directory)
        save_directory_widget = QWidget(self)
        save_directory_layout = QHBoxLayout(save_directory_widget)
        save_directory_layout.setContentsMargins(0, 0, 0, 0)
        save_directory_layout.setSpacing(4)
        save_directory_layout.addWidget(self.save_directory_edit, 1)
        save_directory_layout.addWidget(self.save_directory_button)
        self.add_parameter_row("保存目录", save_directory_widget)

        self.saved_file_label = QLabel("尚未保存", self)
        self.saved_file_label.setObjectName("dhSavedFileLabel")
        self.saved_file_label.setProperty("muted", True)
        self.saved_file_label.setWordWrap(True)
        self.saved_file_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.add_parameter_row("保存文件", self.saved_file_label)

    def build_result_widget(self) -> QWidget:
        for index in range(23):
            self.metrics.add_metric(
                "telemetry[{}]".format(index), "DH{} 遥测".format(index), "V"
            )
        self.metrics.setVisible(False)

        root = QWidget(self)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)

        self.figure = Figure(figsize=(8.0, 5.2), dpi=100)
        self.canvas = FigureCanvasQTAgg(self.figure)
        self.canvas.setObjectName("dhReportChart")
        self.canvas.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.canvas.setMinimumHeight(360)
        self.status_axes = self.figure.add_subplot(211)
        self.telemetry_axes = self.figure.add_subplot(212, sharex=self.status_axes)
        self.status_axes.set_ylabel("点火状态", fontproperties=CHART_FONT)
        self.status_axes.set_yticks((0, 1, 2, 3))
        self.status_axes.set_yticklabels(
            ("未 DH", "成功", "失败", "保留"), fontproperties=CHART_FONT
        )
        self.status_axes.set_ylim(-0.2, 3.2)
        self.status_axes.grid(True, alpha=0.25)
        self.telemetry_axes.set_xlabel("时间 (ms)", fontproperties=CHART_FONT)
        self.telemetry_axes.set_ylabel("遥测电压 (V)", fontproperties=CHART_FONT)
        self.telemetry_axes.grid(True, alpha=0.25)

        palette = (
            "#006D77", "#D97706", "#2563EB", "#C026D3", "#2E7D32",
            "#C2413B", "#5B5FC7", "#00897B", "#9A6700", "#7C3AED",
            "#0277BD", "#AD1457", "#558B2F", "#EF6C00", "#3949AB",
            "#00838F", "#6D4C41", "#8E24AA", "#43A047", "#E53935",
            "#1E88E5", "#F9A825", "#546E7A",
        )
        self.status_lines = []
        self.telemetry_lines = []
        for index, color in enumerate(palette):
            (status_line,) = self.status_axes.plot(
                [], [], color=color, linewidth=0.9, drawstyle="steps-post"
            )
            (telemetry_line,) = self.telemetry_axes.plot(
                [], [], color=color, linewidth=0.9, label="DH{}".format(index)
            )
            self.status_lines.append(status_line)
            self.telemetry_lines.append(telemetry_line)
        self.chart_legend = self.figure.legend(
            self.telemetry_lines,
            tuple("DH{}".format(index) for index in range(23)),
            loc="upper center",
            ncol=12,
            frameon=False,
            handlelength=1.2,
            handletextpad=0.3,
            columnspacing=0.7,
            prop=FontProperties(family="Microsoft YaHei", size=7),
        )
        self.figure.subplots_adjust(
            left=0.12, right=0.98, top=0.88, bottom=0.12, hspace=0.18
        )
        layout.addWidget(self.canvas, 3)

        readback_row = QHBoxLayout()
        readback_row.addWidget(QLabel("电源回读", root))
        self.power_readback_label = QLabel("不可用", root)
        self.power_readback_label.setObjectName("dhPowerReadback")
        readback_row.addWidget(self.power_readback_label)
        readback_row.addSpacing(16)
        readback_row.addWidget(QLabel("回线回读", root))
        self.return_readback_label = QLabel("不可用", root)
        self.return_readback_label.setObjectName("dhReturnReadback")
        readback_row.addWidget(self.return_readback_label)
        readback_row.addStretch(1)
        layout.addLayout(readback_row)
        selector_row = QHBoxLayout()
        selector_row.addWidget(QLabel("报告", root))
        self.report_selector = QComboBox(root)
        self.report_selector.setObjectName("dhReportSelector")
        selector_row.addWidget(self.report_selector, 1)
        layout.addLayout(selector_row)

        self.report_table = QTableWidget(23, 3, root)
        self.report_table.setObjectName("dhReportTable")
        _configure_result_table(
            self.report_table, ("通道", "状态", "遥测电压")
        )
        for index in range(23):
            self.report_table.setItem(index, 0, QTableWidgetItem("DH{}".format(index)))
            self.report_table.setItem(index, 1, QTableWidgetItem("不可用"))
            self.report_table.setItem(index, 2, QTableWidgetItem("不可用"))
        self.report_table.setMaximumHeight(260)
        layout.addWidget(self.report_table, 2)
        self.report_selector.currentIndexChanged.connect(
            self._on_report_selected
        )
        return root

    def _set_all_channels(self, checked: bool) -> None:
        for channel_check in self.channel_checks:
            channel_check.setChecked(checked)

    def _choose_save_directory(self) -> None:
        directory = QFileDialog.getExistingDirectory(
            self,
            "选择 DH 数据保存目录",
            self.save_directory_edit.text().strip(),
        )
        if directory:
            self.save_directory_edit.setText(directory)

    def collect_parameters(self) -> Dict[str, object]:
        words = [0, 0]
        for index, channel_check in enumerate(self.channel_checks):
            if channel_check.isChecked():
                words[index // 32] |= 1 << (index % 32)
        self._selected_channel_mask = words[0]
        return {
            "power_enable": int(self.power_enable_check.isChecked()),
            "return_enable": int(self.return_enable_check.isChecked()),
            "channel[0]": words[0],
            "channel[1]": 0,
            "report_count": int(self.report_count_spin.value()),
            "interval_us": int(self.interval_spin.value()),
            "delay_us": int(self.delay_spin.value()),
        }

    @staticmethod
    def _report_key(decoded: DecodedMessage) -> Tuple[object, ...]:
        return (decoded.name, int(decoded.sequence), bytes(decoded.payload))

    def _append_report(self, decoded: DecodedMessage) -> None:
        key = self._report_key(decoded)
        if key in self._report_keys:
            return
        self._report_keys.add(key)
        self.reports.append(decoded)
        report_index = len(self.reports) - 1
        self.sample_times_ms.append(
            (self._sample_delay_us + report_index * self._sample_interval_us)
            / 1000.0
        )
        failed = _message_failed(decoded)
        for index in range(23):
            self.status_series[index].append(
                self._numeric_or_nan(
                    decoded.values.get("dh_status.ch{}".format(index))
                )
            )
            telemetry = decoded.values.get("telemetry[{}]".format(index))
            self.telemetry_series[index].append(
                float("nan") if failed else self._numeric_or_nan(telemetry)
            )

        if len(self.reports) <= self._IMMEDIATE_REPORTS:
            self._flush_pending_ui()
        elif not self._ui_refresh_timer.isActive():
            self._ui_refresh_timer.start()

    @staticmethod
    def _numeric_or_nan(value: object) -> float:
        try:
            return float(value)
        except (TypeError, ValueError):
            return float("nan")

    def _flush_pending_ui(self) -> None:
        if self._ui_refresh_timer.isActive():
            self._ui_refresh_timer.stop()
        if not self.reports:
            return
        previous_count = self._selector_synced_count
        current_index = self.report_selector.currentIndex()
        follow_latest = current_index < 0 or current_index == previous_count - 1
        pending_labels = [
            "报告 {} · seq {}".format(index + 1, self.reports[index].sequence)
            for index in range(previous_count, len(self.reports))
        ]
        if pending_labels:
            self.report_selector.blockSignals(True)
            self.report_selector.addItems(pending_labels)
            self._selector_synced_count = len(self.reports)
            if follow_latest:
                self.report_selector.setCurrentIndex(len(self.reports) - 1)
            self.report_selector.blockSignals(False)
        if follow_latest:
            self._display_report(self.reports[-1])
        self._refresh_chart()

    def _chart_indices(self) -> List[int]:
        count = len(self.sample_times_ms)
        if count <= self._MAX_CHART_POINTS:
            return list(range(count))
        step = (count + self._MAX_CHART_POINTS - 1) // self._MAX_CHART_POINTS
        indices = list(range(0, count, step))
        if indices[-1] != count - 1:
            indices.append(count - 1)
        return indices

    def _refresh_chart(self) -> None:
        indices = self._chart_indices()
        x_values = [self.sample_times_ms[index] for index in indices]
        legend_lines = self.chart_legend.get_lines()
        legend_texts = self.chart_legend.get_texts()
        for channel in range(23):
            selected = bool(self._selected_channel_mask & (1 << channel))
            self.status_lines[channel].set_visible(selected)
            self.telemetry_lines[channel].set_visible(selected)
            legend_lines[channel].set_visible(selected)
            legend_texts[channel].set_visible(selected)
            if selected:
                self.status_lines[channel].set_data(
                    x_values,
                    [self.status_series[channel][index] for index in indices],
                )
                self.telemetry_lines[channel].set_data(
                    x_values,
                    [self.telemetry_series[channel][index] for index in indices],
                )
            else:
                self.status_lines[channel].set_data([], [])
                self.telemetry_lines[channel].set_data([], [])
        self.status_axes.relim()
        self.status_axes.autoscale_view(scalex=True, scaley=False)
        self.telemetry_axes.relim()
        self.telemetry_axes.autoscale_view()
        if self.canvas is not None and self.canvas.isVisible():
            self.canvas.draw_idle()

    def _display_report(self, decoded: DecodedMessage) -> None:
        self.raw_table.set_response(decoded)
        failed = _message_failed(decoded)
        self.power_readback_label.setText(
            self._enable_text(decoded.values.get("power_enable_readback"))
        )
        self.return_readback_label.setText(
            self._enable_text(decoded.values.get("return_enable_readback"))
        )
        self.metrics.clear()
        for index in range(23):
            status_name = "dh_status.ch{}".format(index)
            telemetry_name = "telemetry[{}]".format(index)
            status = decoded.values.get(status_name)
            if status is None:
                status_text = "不可用"
            else:
                try:
                    status_text = self._STATUS_TEXT.get(int(status), "未知")
                except (TypeError, ValueError):
                    status_text = "不可用"
            if failed:
                telemetry_text = "不可用"
                self.metrics.set_unavailable(telemetry_name)
            else:
                telemetry = decoded.values.get(telemetry_name)
                if telemetry is None:
                    telemetry_text = "不可用"
                    self.metrics.set_unavailable(telemetry_name)
                else:
                    telemetry_text = _format_scalar(telemetry)
                    self.metrics.set_value(telemetry_name, telemetry)
            self.report_table.item(index, 1).setText(status_text)
            self.report_table.item(index, 2).setText(telemetry_text)

    @staticmethod
    def _enable_text(value: object) -> str:
        try:
            return "使能" if int(value) else "失能"
        except (TypeError, ValueError):
            return "不可用"

    def _on_report_selected(self, index: int) -> None:
        if not 0 <= index < len(self.reports):
            return
        decoded = self.reports[index]
        self._display_report(decoded)

    def render_success(self, decoded: DecodedMessage) -> None:
        self._append_report(decoded)

    def render_response(self, decoded: DecodedMessage) -> bool:
        if decoded.name != "dh_control_response":
            return super().render_response(decoded)
        self.last_response_name = decoded.name
        self.last_values = dict(decoded.values)
        self._append_report(decoded)
        if _message_failed(decoded):
            self.set_test_status(
                "执行失败",
                self._failure_detail(
                    decoded.name, decoded.status, decoded.err_code
                ),
            )
        return True

    def clear_results(self) -> None:
        if hasattr(self, "_ui_refresh_timer"):
            self._ui_refresh_timer.stop()
        self.reports = []
        self._report_keys.clear()
        self._selector_synced_count = 0
        self.sample_times_ms = []
        self.status_series = [[] for _ in range(23)]
        self.telemetry_series = [[] for _ in range(23)]
        self.report_selector.clear()
        self.metrics.clear()
        self.power_readback_label.setText("不可用")
        self.return_readback_label.setText("不可用")
        for index in range(23):
            self.report_table.item(index, 1).setText("不可用")
            self.report_table.item(index, 2).setText("不可用")
            self.status_lines[index].set_data([], [])
            self.telemetry_lines[index].set_data([], [])
        self.status_axes.relim()
        self.status_axes.autoscale_view(scalex=True, scaley=False)
        self.telemetry_axes.relim()
        self.telemetry_axes.autoscale_view()
        self.canvas.draw_idle()

    def measurements_unavailable(self) -> None:
        for index in range(23):
            self.metrics.set_unavailable("telemetry[{}]".format(index))

    def reset_for_run(self) -> None:
        self._sample_interval_us = int(self.interval_spin.value())
        self._sample_delay_us = int(self.delay_spin.value())
        self._run_parameters = self.collect_parameters()
        self._run_started_at = datetime.now()
        self.saved_file_label.setText("尚未保存")
        super().reset_for_run()

    def save_reports(self, final_status: str, final_detail: str = "") -> Path:
        started_at = self._run_started_at or datetime.now()
        parameters = self._run_parameters or self.collect_parameters()
        saved_path = write_dh_text_report(
            self.save_directory_edit.text().strip(),
            self.reports,
            parameters,
            started_at=started_at,
            finished_at=datetime.now(),
            final_status=final_status,
            final_detail=final_detail,
        )
        self.saved_file_label.setText(str(saved_path))
        return saved_path

    def showEvent(self, event) -> None:  # type: ignore[no-untyped-def]
        super().showEvent(event)
        if self.reports:
            self._refresh_chart()


class HelmBoardTestPage(BaseHardwareTestPage):
    accepted_response_names = ("helm_board_test_response",)

    def build_parameters(self) -> None:
        self._last_parameters: Dict[str, object] = {}
        self.pwm_duty_percent_spins = []
        self.direction_checks = []
        selector = QWidget(self)
        grid = QGridLayout(selector)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(14)
        grid.addWidget(QLabel("通道", selector), 0, 0)
        grid.addWidget(QLabel("占空比", selector), 0, 1)
        grid.addWidget(QLabel("方向", selector), 0, 2)
        for index in range(4):
            duty_spin = QSpinBox(selector)
            duty_spin.setObjectName(
                "helmBoardPwmDutyPercent{}".format(index + 1)
            )
            duty_spin.setRange(0, 100)
            duty_spin.setSuffix(" %")
            duty_spin.setValue(
                int(
                    self.spec.defaults.get(
                        "pwm_duty_percent[{}]".format(index), 0
                    )
                )
            )
            direction_check = QCheckBox("1", selector)
            direction_check.setObjectName(
                "helmBoardDirection{}".format(index + 1)
            )
            direction_check.setChecked(
                bool(self.spec.defaults.get("direction[{}]".format(index), 0))
            )
            self.pwm_duty_percent_spins.append(duty_spin)
            self.direction_checks.append(direction_check)
            grid.addWidget(QLabel("舵{}".format(index + 1), selector), index + 1, 0)
            grid.addWidget(duty_spin, index + 1, 1)
            grid.addWidget(direction_check, index + 1, 2)
        grid.setColumnStretch(3, 1)
        self.add_parameter_row("板级输出", selector)

    def collect_parameters(self) -> Dict[str, object]:
        values: Dict[str, object] = {}
        for index in range(4):
            values["pwm_duty_percent[{}]".format(index)] = int(
                self.pwm_duty_percent_spins[index].value()
            )
            values["direction[{}]".format(index)] = int(
                self.direction_checks[index].isChecked()
            )
        self._last_parameters = dict(values)
        return values

    def build_result_widget(self) -> QWidget:
        self.metrics.add_metric("pwm_peak", "PWM 波峰值", "")
        self.metrics.add_metric("pwm_enable_mask", "PWM 使能位图", "")
        self.metrics.add_metric("pwm_update_enabled", "PWM 更新使能", "")
        self.metrics.add_metric("ad_acquisition_enabled", "AD 采集使能", "")
        self.metrics.add_metric("ad_filter_enabled", "AD 滤波使能", "")

        root = QWidget(self)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.metrics)
        self.readback_table = QTableWidget(4, 7, root)
        self.readback_table.setObjectName("helmBoardReadbackTable")
        _configure_result_table(
            self.readback_table,
            (
                "通道",
                "占空比请求",
                "Duty匹配",
                "实际占空比",
                "方向请求",
                "方向回读",
                "AD (V)",
            ),
        )
        for index in range(4):
            self.readback_table.setItem(
                index, 0, QTableWidgetItem("舵{}".format(index + 1))
            )
            for column in range(1, 7):
                self.readback_table.setItem(
                    index, column, QTableWidgetItem("不可用")
                )
        layout.addWidget(self.readback_table, 1)
        return root

    def render_response(self, decoded: DecodedMessage) -> bool:
        handled = super().render_response(decoded)
        if not handled or decoded.name != "helm_board_test_response":
            return handled

        status = self._integer_value(decoded, "status")
        err_code = self._integer_value(decoded, "err_code")
        peak = self._integer_value(decoded, "pwm_peak")
        if (
            status is not None
            and status != 0
            and err_code == 0x0201
            and peak is not None
            and peak > 0
        ):
            # A readback mismatch is reported after the board has populated the
            # diagnostic fields. Keep those values visible while retaining the
            # failure status set by the base page.
            self.render_success(decoded)
        return handled

    def render_success(self, decoded: DecodedMessage) -> None:
        self.metrics.clear()
        for key in (
            "pwm_peak",
            "pwm_enable_mask",
            "pwm_update_enabled",
            "ad_acquisition_enabled",
            "ad_filter_enabled",
        ):
            if key in decoded.values:
                self.metrics.set_value(key, decoded.values[key])
            else:
                self.metrics.set_unavailable(key)

        peak = decoded.values.get("pwm_peak")
        for index in range(4):
            duty_request = self._last_parameters.get(
                "pwm_duty_percent[{}]".format(index),
                int(self.pwm_duty_percent_spins[index].value()),
            )
            direction_request = self._last_parameters.get(
                "direction[{}]".format(index),
                int(self.direction_checks[index].isChecked()),
            )
            duty_match = decoded.values.get(
                "pwm_duty_match[{}]".format(index)
            )
            raw_duty = decoded.values.get("pwm_duty[{}]".format(index))
            actual_percent = None
            if raw_duty is not None and peak is not None and float(peak) > 0.0:
                actual_percent = float(raw_duty) * 100.0 / float(peak)
            direction_readback = decoded.values.get(
                "direction_readback[{}]".format(index)
            )
            ad_value = decoded.values.get("helm_AD_value[{}]".format(index))

            self.readback_table.item(index, 1).setText(
                _format_scalar(duty_request) + " %"
            )
            self.readback_table.item(index, 2).setText(
                "不可用"
                if duty_match is None
                else ("匹配" if int(duty_match) != 0 else "不匹配")
            )
            self.readback_table.item(index, 3).setText(
                "不可用"
                if actual_percent is None
                else _format_scalar(actual_percent) + " %"
            )
            self.readback_table.item(index, 4).setText(
                _format_scalar(direction_request)
            )
            self.readback_table.item(index, 5).setText(
                "不可用"
                if direction_readback is None
                else _format_scalar(direction_readback)
            )
            self.readback_table.item(index, 6).setText(
                "不可用" if ad_value is None else _format_scalar(ad_value) + " V"
            )

    def clear_results(self) -> None:
        self.metrics.clear()
        for row in range(4):
            for column in range(1, 7):
                self.readback_table.item(row, column).setText("不可用")

    def measurements_unavailable(self) -> None:
        for key in (
            "pwm_peak",
            "pwm_enable_mask",
            "pwm_update_enabled",
            "ad_acquisition_enabled",
            "ad_filter_enabled",
        ):
            self.metrics.set_unavailable(key)
        for row in range(4):
            for column in (2, 3, 5, 6):
                self.readback_table.item(row, column).setText("不可用")


class HelmTestPage(BaseHardwareTestPage):
    accepted_response_names = (
        "helm_start_response",
        "helm_feedback_response",
        "helm_stop_response",
    )
    _ACK_NAMES = ("helm_start_response", "helm_stop_response")

    WAVEFORMS = (
        ("正弦波", 0),
        ("方波", 1),
        ("三角波", 2),
        ("直流", 3),
        ("扫频", 4),
    )

    def build_parameters(self) -> None:
        self.waveform_combo = QComboBox(self)
        self.waveform_combo.setObjectName("helmWaveformCombo")
        for label, value in self.WAVEFORMS:
            self.waveform_combo.addItem(label, value)
        default_waveform = int(self.spec.defaults.get("waveform", 0))
        self.waveform_combo.setCurrentIndex(
            self.waveform_combo.findData(default_waveform)
        )

        self.freq_spin = self._double_spin(
            "helmFrequencySpin", 0.0, 10000.0, " Hz", 0.3
        )
        self.amplitude_spin = self._double_spin(
            "helmAmplitudeSpin", -1.0e100, 1.0e100, " deg", 30.0
        )
        self.offset_spin = self._double_spin(
            "helmOffsetSpin", -1.0e100, 1.0e100, " deg", 0.0
        )
        self.phase_spin = self._double_spin(
            "helmPhaseSpin", -1000000.0, 1000000.0, " rad", 0.0
        )
        self.max_freq_spin = self._double_spin(
            "helmMaxFrequencySpin", 0.0, 10000.0, " Hz", 0.0
        )
        self.freq_spin.setValue(float(self.spec.defaults.get("freq", 0.3)))
        self.amplitude_spin.setValue(float(self.spec.defaults.get("ampl", 30.0)))
        self.offset_spin.setValue(float(self.spec.defaults.get("offset", 0.0)))
        self.phase_spin.setValue(float(self.spec.defaults.get("start", 0.0)))
        self.max_freq_spin.setValue(float(self.spec.defaults.get("max_freq", 0.0)))

        enable = int(self.spec.defaults.get("enable", 1))
        self.channel_checks = [
            QCheckBox("FK{}".format(index + 1), self) for index in range(4)
        ]
        channel_row = QWidget(self)
        channel_layout = QGridLayout(channel_row)
        channel_layout.setContentsMargins(0, 0, 0, 0)
        for index, check in enumerate(self.channel_checks):
            check.setObjectName("helmChannel{}".format(index + 1))
            check.setChecked(bool(enable & (1 << index)))
            channel_layout.addWidget(check, index // 2, index % 2)
        channel_layout.setColumnStretch(2, 1)

        self.add_parameter_row("波形", self.waveform_combo)
        self.add_parameter_row("频率", self.freq_spin)
        self.add_parameter_row("幅值", self.amplitude_spin)
        self.add_parameter_row("偏置", self.offset_spin)
        self.add_parameter_row("起始相位", self.phase_spin)
        self.add_parameter_row("扫频上限", self.max_freq_spin)
        self.add_parameter_row("通道使能", channel_row)

    def _double_spin(
        self,
        object_name: str,
        minimum: float,
        maximum: float,
        suffix: str,
        value: float,
    ) -> QDoubleSpinBox:
        spin = QDoubleSpinBox(self)
        spin.setObjectName(object_name)
        spin.setDecimals(6)
        spin.setRange(minimum, maximum)
        spin.setSuffix(suffix)
        spin.setValue(value)
        return spin

    def build_result_widget(self) -> QWidget:
        root = QWidget(self)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)

        self.figure = Figure(figsize=(7.0, 3.6), dpi=100)
        self.canvas = FigureCanvasQTAgg(self.figure)
        self.canvas.setObjectName("helmFeedbackChart")
        self.canvas.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.axes = self.figure.add_subplot(111)
        self.axes.set_xlabel("样本", fontproperties=CHART_FONT)
        self.axes.set_ylabel("指令 / 反馈", fontproperties=CHART_FONT)
        self.axes.grid(True, alpha=0.25)
        self.figure.subplots_adjust(left=0.10, right=0.98, top=0.93, bottom=0.24)

        self.feedback_series: Dict[str, List[float]] = {
            "command": [],
            "fk1": [],
            "fk2": [],
            "fk3": [],
            "fk4": [],
        }
        colors = ("#374151", "#00897B", "#2563EB", "#D97706", "#C026D3")
        labels = ("指令", "FK1", "FK2", "FK3", "FK4")
        self.feedback_lines = {}
        for key, label, color in zip(self.feedback_series, labels, colors):
            (line,) = self.axes.plot([], [], marker="o", label=label, color=color)
            self.feedback_lines[key] = line
        self.axes.legend(
            loc="upper center",
            bbox_to_anchor=(0.5, -0.16),
            ncol=5,
            frameon=False,
            handlelength=1.2,
            handletextpad=0.4,
            columnspacing=0.8,
            prop=CHART_FONT,
        )
        layout.addWidget(self.canvas, 3)

        self.self_check_table = QTableWidget(10, 2, root)
        self.self_check_table.setObjectName("helmSelfCheckTable")
        _configure_result_table(self.self_check_table, ("样本", "自检状态"))
        self.self_check_table.setMaximumHeight(190)
        for sample in range(10):
            self.self_check_table.setItem(
                sample, 0, QTableWidgetItem(str(sample + 1))
            )
            self.self_check_table.setItem(
                sample, 1, QTableWidgetItem("不可用")
            )
        layout.addWidget(self.self_check_table, 1)
        return root

    def collect_parameters(self) -> Dict[str, object]:
        waveform = int(self.waveform_combo.currentData())
        frequency = float(self.freq_spin.value())
        amplitude = float(self.amplitude_spin.value())
        offset = float(self.offset_spin.value())
        max_frequency = float(self.max_freq_spin.value())
        if frequency <= 0.0:
            raise ParameterValidationError("舵控频率必须大于 0 Hz", self.freq_spin)
        if waveform == 4 and max_frequency <= 0.0:
            raise ParameterValidationError(
                "扫频波形的扫频上限必须大于 0 Hz", self.max_freq_spin
            )
        enable = 0
        for index, channel_check in enumerate(self.channel_checks):
            if channel_check.isChecked():
                enable |= 1 << index
        return {
            "waveform": waveform,
            "freq": frequency,
            "ampl": amplitude,
            "offset": offset,
            "start": float(self.phase_spin.value()),
            "max_freq": max_frequency,
            "enable": enable,
        }

    def render_response(self, decoded: DecodedMessage) -> bool:
        if decoded.name in self._ACK_NAMES:
            self.last_response_name = decoded.name
            self.last_values = dict(decoded.values)
            self.raw_table.set_response(decoded)
            if _message_failed(decoded):
                self.set_test_status(
                    "执行失败",
                    self._failure_detail(
                        decoded.name, decoded.status, decoded.err_code
                    ),
                )
            return True
        return super().render_response(decoded)

    def render_success(self, decoded: DecodedMessage) -> None:
        if decoded.name != "helm_feedback_response":
            return
        self.feedback_series = {
            "command": [
                float(decoded.values.get("zl[{}][0]".format(sample), 0))
                for sample in range(10)
            ],
            "fk1": [
                float(decoded.values.get("fk[{}][0]".format(sample), 0))
                for sample in range(10)
            ],
            "fk2": [
                float(decoded.values.get("fk[{}][1]".format(sample), 0))
                for sample in range(10)
            ],
            "fk3": [
                float(decoded.values.get("fk[{}][2]".format(sample), 0))
                for sample in range(10)
            ],
            "fk4": [
                float(decoded.values.get("fk[{}][3]".format(sample), 0))
                for sample in range(10)
            ],
        }
        x_values = list(range(1, 11))
        for key, values in self.feedback_series.items():
            self.feedback_lines[key].set_data(x_values, values)
        self.axes.relim()
        self.axes.autoscale_view()
        self.canvas.draw_idle()
        for sample in range(10):
            self.self_check_table.item(sample, 1).setText(
                _format_scalar(
                    decoded.values.get("self_code[{}]".format(sample), 0)
                )
            )

    def clear_results(self) -> None:
        for key in self.feedback_series:
            self.feedback_series[key] = []
            self.feedback_lines[key].set_data([], [])
        self.axes.relim()
        self.axes.autoscale_view()
        self.canvas.draw_idle()
        for sample in range(10):
            self.self_check_table.item(sample, 1).setText("不可用")

    def measurements_unavailable(self) -> None:
        for sample in range(10):
            self.self_check_table.item(sample, 1).setText("不可用")


class TimerTestPage(BaseHardwareTestPage):
    accepted_response_names = (
        "timer_jitter_start_response",
        "timer_jitter_stop_response",
    )
    _STOP_ACK = "timer_jitter_stop_response"
    BUCKET_LABELS = (
        "<2",
        "2-<4",
        "4-<8",
        "8-<16",
        "16-<32",
        "32-<64",
        "64-<100",
        ">=100",
    )

    def __init__(
        self,
        spec: TestSpec,
        catalog: ProtocolCatalog,
        parent: Optional[QWidget] = None,
    ) -> None:
        self.bucket_values = [0] * 8
        super().__init__(spec, catalog, parent)

    def build_parameters(self) -> None:
        self.mode_control = SegmentedControl(
            (("基线", 0), ("C2H0 负载", 1)), self
        )
        self.mode_control.setObjectName("timerModeControl")
        self.mode_control.set_current_data(int(self.spec.defaults.get("mode", 0)))
        self.add_parameter_row("负载模式", self.mode_control)

    def build_result_widget(self) -> QWidget:
        self.metrics.add_metric("total_samples", "总样本数", "")
        self.metrics.add_metric("avg_jitter", "平均抖动", "us")
        self.metrics.add_metric("max_jitter", "最大抖动", "us")

        root = QWidget(self)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.metrics)
        self.figure = Figure(figsize=(7.0, 3.4), dpi=100)
        self.canvas = FigureCanvasQTAgg(self.figure)
        self.canvas.setObjectName("timerBucketChart")
        self.canvas.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.axes = self.figure.add_subplot(111)
        self.axes.set_xlabel("抖动区间 (us)", fontproperties=CHART_FONT)
        self.axes.set_ylabel("样本数", fontproperties=CHART_FONT)
        self.axes.grid(True, axis="y", alpha=0.25)
        positions = list(range(8))
        self.bucket_bars = self.axes.bar(
            positions,
            self.bucket_values,
            color=(
                "#00897B",
                "#1F9D8A",
                "#4C9F70",
                "#6B9B55",
                "#A58C3B",
                "#C7782B",
                "#C45B43",
                "#A94455",
            ),
        )
        self.axes.set_xticks(positions)
        self.axes.set_xticklabels(self.BUCKET_LABELS, rotation=25, ha="right")
        self.axes.set_ylim(0, 1)
        self.figure.subplots_adjust(left=0.16, right=0.98, top=0.95, bottom=0.25)
        layout.addWidget(self.canvas, 1)
        return root

    def collect_parameters(self) -> Dict[str, object]:
        mode = self.mode_control.current_data()
        if mode not in (0, 1):
            raise ParameterValidationError("定时器模式必须是 0 或 1", self.mode_control)
        return {"mode": int(mode)}

    def render_response(self, decoded: DecodedMessage) -> bool:
        if decoded.name == self._STOP_ACK:
            self.last_response_name = decoded.name
            self.last_values = dict(decoded.values)
            self.raw_table.set_response(decoded)
            if _message_failed(decoded):
                self.set_test_status(
                    "执行失败",
                    self._failure_detail(
                        decoded.name, decoded.status, decoded.err_code
                    ),
                )
            return True
        return super().render_response(decoded)

    def render_success(self, decoded: DecodedMessage) -> None:
        if decoded.name != "timer_jitter_start_response":
            return
        self.bucket_values = [
            int(decoded.values.get("buckets[{}]".format(index), 0))
            for index in range(8)
        ]
        for bar, value in zip(self.bucket_bars, self.bucket_values):
            bar.set_height(value)
        maximum = max(self.bucket_values) if self.bucket_values else 0
        self.axes.set_ylim(0, max(1.0, maximum * 1.12))
        self.canvas.draw_idle()
        self.metrics.clear()
        self.metrics.set_value("total_samples", sum(self.bucket_values))
        if "avg_jitter" in decoded.values:
            self.metrics.set_value("avg_jitter", decoded.values["avg_jitter"])
        else:
            self.metrics.set_unavailable("avg_jitter")
        if "max_jitter" in decoded.values:
            self.metrics.set_value("max_jitter", decoded.values["max_jitter"])
        else:
            self.metrics.set_unavailable("max_jitter")

    def clear_results(self) -> None:
        self.bucket_values = [0] * 8
        for bar in self.bucket_bars:
            bar.set_height(0)
        self.axes.set_ylim(0, 1)
        self.canvas.draw_idle()
        self.metrics.clear()

    def measurements_unavailable(self) -> None:
        for key in ("total_samples", "avg_jitter", "max_jitter"):
            self.metrics.set_unavailable(key)


PAGE_CLASSES: Dict[str, Type[BaseHardwareTestPage]] = {
    "system": SystemStatusPage,
    "memory": MemoryTestPage,
    "spi_flash": SpiFlashTestPage,
    "bus": BusTestPage,
    "di": DiTestPage,
    "do": DoTestPage,
    "electrical_health": ElectricalHealthPage,
    "dh_pulse_config": DhPulseConfigPage,
    "dh": DhTestPage,
    "helm_board": HelmBoardTestPage,
    "helm": HelmTestPage,
    "timer": TimerTestPage,
}


def create_test_page(
    spec: TestSpec,
    catalog: ProtocolCatalog,
    parent: Optional[QWidget] = None,
) -> BaseHardwareTestPage:
    try:
        page_class = PAGE_CLASSES[spec.key]
    except KeyError as exc:
        raise KeyError("未知硬件测试页面：{}".format(spec.key)) from exc
    return page_class(spec, catalog, parent)


__all__ = [
    "PAGE_CLASSES",
    "BusTestPage",
    "DhPulseConfigPage",
    "DhTestPage",
    "DiTestPage",
    "DoTestPage",
    "ElectricalHealthPage",
    "HelmBoardTestPage",
    "HelmTestPage",
    "MemoryTestPage",
    "SpiFlashTestPage",
    "SystemStatusPage",
    "TimerTestPage",
    "create_test_page",
]
