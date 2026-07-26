"""Reusable widgets shared by the protocol-specific hardware-test pages."""

import math
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

from PyQt5.QtCore import QRegExp, Qt, pyqtSignal
from PyQt5.QtGui import QRegExpValidator
from PyQt5.QtWidgets import (
    QAbstractItemView,
    QButtonGroup,
    QFormLayout,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLayout,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSplitter,
    QStyle,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)


UNAVAILABLE_TEXT = "不可用"


def _refresh_style(widget: QWidget) -> None:
    style = widget.style()
    style.unpolish(widget)
    style.polish(widget)
    widget.update()


def _format_value(value: object) -> str:
    if value is None:
        return UNAVAILABLE_TEXT
    if isinstance(value, bytes):
        return " ".join("{:02X}".format(item) for item in value)
    if isinstance(value, bytearray):
        return " ".join("{:02X}".format(item) for item in bytes(value))
    if isinstance(value, float):
        if not math.isfinite(value):
            return str(value)
        return "{:g}".format(value)
    if isinstance(value, (list, tuple)):
        return "[{}]".format(", ".join(_format_value(item) for item in value))
    return str(value)


class ParameterValidationError(ValueError):
    """Parameter error that identifies the editor which should receive focus."""

    def __init__(self, message: object, widget: Optional[QWidget] = None) -> None:
        super().__init__(str(message))
        self.widget = widget


class SegmentedControl(QWidget):
    """Compact exclusive choice control whose choices carry arbitrary data."""

    current_changed = pyqtSignal(object)
    value_changed = pyqtSignal(object)

    def __init__(
        self,
        items: Iterable[Tuple[object, object]],
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)
        self.button_group = QButtonGroup(self)
        self.button_group.setExclusive(True)
        self.buttons: List[QPushButton] = []
        self._data: List[object] = []

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        normalized = list(items.items()) if isinstance(items, Mapping) else list(items)
        for item in normalized:
            if isinstance(item, (tuple, list)) and len(item) == 2:
                label, data = item
            else:
                label = item
                data = item
            button = QPushButton(str(label), self)
            button.setCheckable(True)
            button.setProperty("segmented", True)
            button.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            button.clicked.connect(
                lambda _checked=False, selected=button: self._button_clicked(selected)
            )
            self.button_group.addButton(button)
            self.buttons.append(button)
            self._data.append(data)
            layout.addWidget(button)

        for index, button in enumerate(self.buttons):
            if index == 0:
                position = "first"
            elif index == len(self.buttons) - 1:
                position = "last"
            else:
                position = "middle"
            if len(self.buttons) == 1:
                position = "last"
            button.setProperty("segmentPosition", position)
        if self.buttons:
            self.buttons[0].setChecked(True)

    def current_data(self):
        for index, button in enumerate(self.buttons):
            if button.isChecked():
                return self._data[index]
        return None

    def set_current_data(self, data: object) -> None:
        for index, candidate in enumerate(self._data):
            if candidate == data:
                changed = not self.buttons[index].isChecked()
                self.buttons[index].setChecked(True)
                if changed:
                    self.current_changed.emit(candidate)
                    self.value_changed.emit(candidate)
                return
        raise ValueError("分段控件不包含选项 {!r}".format(data))

    def _button_clicked(self, button: QPushButton) -> None:
        try:
            index = self.buttons.index(button)
        except ValueError:
            return
        value = self._data[index]
        self.current_changed.emit(value)
        self.value_changed.emit(value)


class U32HexEdit(QLineEdit):
    """A validated, fixed-width hexadecimal editor for one unsigned U32."""

    def __init__(self, value: int = 0, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setObjectName("u32HexEdit")
        self.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        self.setMaxLength(10)
        self.setMaximumWidth(150)
        self.setPlaceholderText("0x00000000")
        expression = QRegExp(r"(?:0[xX]?)?[0-9A-Fa-f]{0,8}")
        self.setValidator(QRegExpValidator(expression, self))
        self.set_value(value)

    def value(self) -> int:
        text = self.text().strip()
        if text.lower().startswith("0x"):
            text = text[2:]
        if not text:
            raise ParameterValidationError("请输入 0..FFFFFFFF 的十六进制数", self)
        try:
            value = int(text, 16)
        except ValueError as exc:
            raise ParameterValidationError("U32 十六进制输入无效", self) from exc
        if not 0 <= value <= 0xFFFFFFFF:
            raise ParameterValidationError("U32 必须在 0..FFFFFFFF 范围内", self)
        return value

    def set_value(self, value: int) -> None:
        if isinstance(value, float) and not value.is_integer():
            raise ValueError("U32 必须是整数")
        try:
            number = int(value)
        except (TypeError, ValueError) as exc:
            raise ValueError("U32 必须是整数") from exc
        if not 0 <= number <= 0xFFFFFFFF:
            raise ValueError("U32 必须在 0..FFFFFFFF 范围内")
        self.setText("0x{:08X}".format(number))


class MetricGrid(QWidget):
    """Dense grid of named read-only measurements with explicit availability."""

    def __init__(self, parent: Optional[QWidget] = None, columns: int = 2) -> None:
        super().__init__(parent)
        if int(columns) <= 0:
            raise ValueError("指标列数必须大于 0")
        self.columns = int(columns)
        self._values: Dict[str, object] = {}
        self._available: Dict[str, bool] = {}
        self._units: Dict[str, str] = {}
        self._panels: Dict[str, QFrame] = {}
        self._name_labels: Dict[str, QLabel] = {}
        self._value_labels: Dict[str, QLabel] = {}

        self.grid_layout = QGridLayout(self)
        self.grid_layout.setContentsMargins(0, 0, 0, 0)
        self.grid_layout.setHorizontalSpacing(8)
        self.grid_layout.setVerticalSpacing(8)
        for column in range(self.columns):
            self.grid_layout.setColumnStretch(column, 1)

    def add_metric(self, key: str, label: str, unit: str = "") -> QLabel:
        name = str(key)
        if name in self._value_labels:
            self._name_labels[name].setText(str(label))
            self._units[name] = str(unit)
            return self._value_labels[name]

        panel = QFrame(self)
        panel.setProperty("metricPanel", True)
        panel.setMinimumHeight(58)
        panel_layout = QVBoxLayout(panel)
        panel_layout.setContentsMargins(8, 5, 8, 5)
        panel_layout.setSpacing(1)

        name_label = QLabel(str(label), panel)
        name_label.setProperty("metricName", True)
        name_label.setWordWrap(True)
        value_label = QLabel(UNAVAILABLE_TEXT, panel)
        value_label.setProperty("metricValue", True)
        value_label.setProperty("available", False)
        value_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        panel_layout.addWidget(name_label)
        panel_layout.addWidget(value_label)

        index = len(self._value_labels)
        self.grid_layout.addWidget(panel, index // self.columns, index % self.columns)
        self._panels[name] = panel
        self._name_labels[name] = name_label
        self._value_labels[name] = value_label
        self._values[name] = None
        self._available[name] = False
        self._units[name] = str(unit)
        return value_label

    def set_value(self, key: str, value: object) -> None:
        name = self._ensure_metric(key)
        self._values[name] = value
        self._available[name] = True
        text = _format_value(value)
        unit = self._units[name]
        self._value_labels[name].setText(
            "{} {}".format(text, unit) if unit else text
        )
        self._value_labels[name].setProperty("available", True)
        _refresh_style(self._value_labels[name])

    def set_unavailable(self, key: str, text: str = UNAVAILABLE_TEXT) -> None:
        name = self._ensure_metric(key)
        self._values[name] = None
        self._available[name] = False
        self._value_labels[name].setText(str(text))
        self._value_labels[name].setProperty("available", False)
        _refresh_style(self._value_labels[name])

    def clear(self) -> None:
        for key in tuple(self._value_labels):
            self.set_unavailable(key)

    def value(self, key: str):
        return self._values[str(key)]

    def text(self, key: str) -> str:
        return self._value_labels[str(key)].text()

    def is_available(self, key: str) -> bool:
        return self._available[str(key)]

    def keys(self) -> Tuple[str, ...]:
        return tuple(self._value_labels)

    def _ensure_metric(self, key: object) -> str:
        name = str(key)
        if name not in self._value_labels:
            self.add_metric(name, name)
        return name


class BitGrid(QWidget):
    """Read-only bit matrix for one or more little-endian U32 words."""

    def __init__(
        self,
        count: int,
        labels: Optional[Sequence[object]] = None,
        parent: Optional[QWidget] = None,
        columns: int = 8,
    ) -> None:
        super().__init__(parent)
        if int(count) <= 0:
            raise ValueError("位数量必须大于 0")
        if int(columns) <= 0:
            raise ValueError("位网格列数必须大于 0")
        self.count = int(count)
        self.columns = int(columns)
        self._states: List[Optional[bool]] = [None] * self.count
        self.cells: List[QFrame] = []
        self.name_labels: List[QLabel] = []
        self.state_labels: List[QLabel] = []

        layout = QGridLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setHorizontalSpacing(5)
        layout.setVerticalSpacing(5)
        for column in range(self.columns):
            layout.setColumnStretch(column, 1)

        for index in range(self.count):
            label = self._label_for(labels, index)
            cell = QFrame(self)
            cell.setProperty("bitCell", True)
            cell.setProperty("state", "unavailable")
            cell.setMinimumSize(48, 43)
            cell_layout = QVBoxLayout(cell)
            cell_layout.setContentsMargins(4, 3, 4, 3)
            cell_layout.setSpacing(0)
            name_label = QLabel(label, cell)
            name_label.setProperty("bitName", True)
            name_label.setAlignment(Qt.AlignCenter)
            state_label = QLabel("--", cell)
            state_label.setProperty("bitState", True)
            state_label.setProperty("state", "unavailable")
            state_label.setAlignment(Qt.AlignCenter)
            cell_layout.addWidget(name_label)
            cell_layout.addWidget(state_label)
            layout.addWidget(cell, index // self.columns, index % self.columns)
            self.cells.append(cell)
            self.name_labels.append(name_label)
            self.state_labels.append(state_label)

    @staticmethod
    def _label_for(labels: Optional[Sequence[object]], index: int) -> str:
        if labels is None:
            return "CH{}".format(index)
        if isinstance(labels, Mapping):
            return str(labels.get(index, "CH{}".format(index)))
        if index < len(labels):
            return str(labels[index])
        return "CH{}".format(index)

    def set_bit(self, index: int, active: object) -> None:
        bit = self._checked_index(index)
        state = bool(active)
        self._states[bit] = state
        property_value = "active" if state else "inactive"
        self.cells[bit].setProperty("state", property_value)
        self.state_labels[bit].setProperty("state", property_value)
        self.state_labels[bit].setText("1" if state else "0")
        _refresh_style(self.cells[bit])
        _refresh_style(self.state_labels[bit])

    def set_words(self, words, *additional_words: int) -> None:
        if additional_words:
            values = [words]
            values.extend(additional_words)
        elif isinstance(words, int):
            values = [words]
        else:
            values = list(words)
        normalized: List[int] = []
        for word in values:
            try:
                number = int(word)
            except (TypeError, ValueError) as exc:
                raise ValueError("位图字必须是 U32") from exc
            if not 0 <= number <= 0xFFFFFFFF:
                raise ValueError("位图字必须在 0..FFFFFFFF 范围内")
            normalized.append(number)
        for index in range(self.count):
            word_index = index // 32
            if word_index >= len(normalized):
                self.set_unavailable(index)
            else:
                self.set_bit(index, (normalized[word_index] >> (index % 32)) & 1)

    def set_unavailable(self, index: Optional[int] = None) -> None:
        indexes = range(self.count) if index is None else (self._checked_index(index),)
        for bit in indexes:
            self._states[bit] = None
            self.cells[bit].setProperty("state", "unavailable")
            self.state_labels[bit].setProperty("state", "unavailable")
            self.state_labels[bit].setText("--")
            _refresh_style(self.cells[bit])
            _refresh_style(self.state_labels[bit])

    def clear(self) -> None:
        self.set_unavailable()

    def is_active(self, index: int) -> Optional[bool]:
        return self._states[self._checked_index(index)]

    def is_available(self, index: int) -> bool:
        return self.is_active(index) is not None

    def text(self, index: int) -> str:
        return self.state_labels[self._checked_index(index)].text()

    def _checked_index(self, index: object) -> int:
        try:
            bit = int(index)
        except (TypeError, ValueError) as exc:
            raise IndexError("位索引无效") from exc
        if not 0 <= bit < self.count:
            raise IndexError("位索引 {} 超出 0..{}".format(bit, self.count - 1))
        return bit


class RawResponseTable(QTableWidget):
    """Catalog-backed diagnostic view of the most recent decoded response."""

    HEADERS = ("中文名", "字段", "值", "类型")

    def __init__(self, catalog, parent: Optional[QWidget] = None) -> None:
        super().__init__(0, len(self.HEADERS), parent)
        self.catalog = catalog
        self.response = None
        self.setObjectName("rawResponseTable")
        self.setHorizontalHeaderLabels(list(self.HEADERS))
        self.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.setAlternatingRowColors(True)
        self.verticalHeader().setVisible(False)
        header = self.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.Stretch)
        header.setSectionResizeMode(2, QHeaderView.Stretch)
        header.setSectionResizeMode(3, QHeaderView.ResizeToContents)

    def clear(self) -> None:
        self.response = None
        self.setRowCount(0)

    def set_response(self, decoded: object) -> None:
        self.clear()
        self.response = decoded
        name = str(getattr(decoded, "name", ""))
        values = getattr(decoded, "values", {})
        if not isinstance(values, Mapping):
            values = {}
        try:
            definition = self.catalog.get(name)
        except (AttributeError, KeyError):
            definition = None

        if definition is None:
            rows = [("", str(key), value, "") for key, value in values.items()]
        else:
            rows = []
            for field in definition.payload_fields:
                found, value = self._lookup_value(values, field.name)
                rows.append(
                    (
                        field.name_cn,
                        field.name,
                        value if found else None,
                        field.type_name,
                    )
                )

        self.setRowCount(len(rows))
        for row_index, (name_cn, field_name, value, type_name) in enumerate(rows):
            items = (
                QTableWidgetItem(str(name_cn)),
                QTableWidgetItem(str(field_name)),
                QTableWidgetItem(_format_value(value)),
                QTableWidgetItem(str(type_name)),
            )
            items[2].setData(Qt.UserRole, value)
            items[2].setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)
            for column, item in enumerate(items):
                self.setItem(row_index, column, item)
        self.resizeRowsToContents()

    @staticmethod
    def _lookup_value(values: Mapping[str, object], field_name: str):
        if field_name in values:
            return True, values[field_name]
        first_bracket = field_name.find("[")
        if first_bracket < 0:
            return False, None
        base = field_name[:first_bracket]
        if base not in values:
            return False, None
        current = values[base]
        suffix = field_name[first_bracket:]
        while suffix:
            close = suffix.find("]")
            if not suffix.startswith("[") or close < 0:
                return False, None
            try:
                current = current[int(suffix[1:close])]  # type: ignore[index]
            except (IndexError, KeyError, TypeError, ValueError):
                return False, None
            suffix = suffix[close + 1 :]
        return True, current


class BaseHardwareTestPage(QWidget):
    """Common page shell and response contract for one hardware test."""

    run_requested = pyqtSignal(str, object)
    continuous_requested = pyqtSignal(str, object, bool)
    accepted_response_names: Tuple[str, ...] = ()

    def __init__(self, spec, catalog, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.spec = spec
        self.catalog = catalog
        self.last_response_name: Optional[str] = None
        self.last_values: Dict[str, object] = {}
        self.metrics = MetricGrid(columns=2)
        self._action_widgets: List[QWidget] = []

        root = QHBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        splitter = QSplitter(Qt.Horizontal, self)
        splitter.setChildrenCollapsible(False)
        root.addWidget(splitter)

        parameter_panel = QWidget(splitter)
        parameter_panel.setObjectName("parameterPanel")
        parameter_panel.setMinimumWidth(230)
        parameter_panel.setMaximumWidth(390)
        parameter_panel_layout = QVBoxLayout(parameter_panel)
        parameter_panel_layout.setContentsMargins(10, 10, 10, 10)
        parameter_panel_layout.setSpacing(8)

        heading = QLabel(str(getattr(spec, "label", "硬件测试")), parameter_panel)
        heading.setObjectName("pageHeading")
        heading_font = heading.font()
        heading_font.setBold(True)
        heading_font.setPointSize(max(heading_font.pointSize() + 2, 11))
        heading.setFont(heading_font)
        parameter_panel_layout.addWidget(heading)

        status_row = QHBoxLayout()
        status_caption = QLabel("状态", parameter_panel)
        self.status_label = QLabel("未执行", parameter_panel)
        self.status_label.setObjectName("testStatusLabel")
        self.status_label.setProperty("pageStatus", True)
        self.status_label.setProperty("statusLevel", "idle")
        self.status_label.setAlignment(Qt.AlignCenter)
        status_row.addWidget(status_caption)
        status_row.addWidget(self.status_label, 1)
        parameter_panel_layout.addLayout(status_row)

        self.detail_label = QLabel("", parameter_panel)
        self.detail_label.setObjectName("testDetailLabel")
        self.detail_label.setProperty("muted", True)
        self.detail_label.setWordWrap(True)
        self.detail_label.setMinimumHeight(34)
        self.detail_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        parameter_panel_layout.addWidget(self.detail_label)

        parameter_scroll = QScrollArea(parameter_panel)
        parameter_scroll.setObjectName("parameterScrollArea")
        parameter_scroll.setWidgetResizable(True)
        parameter_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        parameter_content = QWidget(parameter_scroll)
        parameter_content_layout = QVBoxLayout(parameter_content)
        parameter_content_layout.setContentsMargins(0, 0, 4, 0)
        parameter_content_layout.setSpacing(8)
        form_container = QWidget(parameter_content)
        self.parameter_layout = QFormLayout(form_container)
        self.parameter_layout.setContentsMargins(0, 0, 0, 0)
        self.parameter_layout.setHorizontalSpacing(8)
        self.parameter_layout.setVerticalSpacing(8)
        self.parameter_layout.setFieldGrowthPolicy(QFormLayout.AllNonFixedFieldsGrow)
        self.parameter_layout.setRowWrapPolicy(QFormLayout.WrapLongRows)
        parameter_content_layout.addWidget(form_container)
        parameter_content_layout.addStretch(1)
        parameter_scroll.setWidget(parameter_content)
        parameter_panel_layout.addWidget(parameter_scroll, 1)

        self.build_parameters()

        self.action_layout = QHBoxLayout()
        self.action_layout.setContentsMargins(0, 0, 0, 0)
        self.action_layout.setSpacing(6)

        self.run_button = QPushButton("执行", parameter_panel)
        self.run_button.setObjectName("runButton")
        self.run_button.setProperty("primary", True)
        self.run_button.setIcon(self.style().standardIcon(QStyle.SP_MediaPlay))
        self.run_button.clicked.connect(self._request_run)
        self._action_widgets.append(self.run_button)
        self.action_layout.addWidget(self.run_button)

        self.continuous_button = QPushButton("连续", parameter_panel)
        self.continuous_button.setObjectName("continuousButton")
        self.continuous_button.setCheckable(True)
        self.continuous_button.setToolTip("每轮完成后等待 200 ms 再执行；再次点击可停止")
        self.continuous_button.clicked.connect(self._request_continuous)
        self._action_widgets.append(self.continuous_button)
        self.action_layout.addWidget(self.continuous_button)
        parameter_panel_layout.addLayout(self.action_layout)

        self.result_tabs = QTabWidget(splitter)
        self.result_tabs.setObjectName("resultTabs")
        result_widget = self.build_result_widget()
        if isinstance(result_widget, QLayout):
            wrapper = QWidget(self.result_tabs)
            wrapper.setLayout(result_widget)
            result_widget = wrapper
        if not isinstance(result_widget, QWidget):
            result_widget = self.metrics
        self.result_tabs.addTab(result_widget, "结果")
        self.raw_table = RawResponseTable(catalog, self.result_tabs)
        self.result_tabs.addTab(self.raw_table, "原始字段")

        splitter.addWidget(parameter_panel)
        splitter.addWidget(self.result_tabs)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([280, 680])
        self.splitter = splitter

    def add_parameter_row(self, label: object, widget: QWidget) -> QWidget:
        label_widget = QLabel(str(label), widget.parentWidget() or self)
        label_widget.setBuddy(widget)
        self.parameter_layout.addRow(label_widget, widget)
        return widget

    def build_parameters(self) -> None:
        """Create page-specific editors; subclasses override this hook."""

    def build_result_widget(self) -> QWidget:
        """Create the specialized result view; subclasses may return a wrapper."""

        return self.metrics

    def collect_parameters(self) -> Dict[str, object]:
        return dict(getattr(self.spec, "defaults", {}))

    def reset_for_run(self) -> None:
        self.last_response_name = None
        self.last_values = {}
        self.raw_table.clear()
        self.clear_results()
        self.measurements_unavailable()
        self.set_test_status("执行中", "等待产品端响应")

    def render_response(self, decoded: object) -> bool:
        name = str(getattr(decoded, "name", ""))
        accepted = self._accepted_names()
        if name != "error_response" and name not in accepted:
            return False

        values = getattr(decoded, "values", {})
        self.last_response_name = name
        self.last_values = dict(values) if isinstance(values, Mapping) else {}
        self.raw_table.set_response(decoded)

        status = self._integer_value(decoded, "status")
        err_code = self._integer_value(decoded, "err_code")
        failed = (
            name == "error_response"
            or status is None
            or err_code is None
            or status != 0
            or err_code != 0
        )
        if failed:
            self.clear_results()
            self.measurements_unavailable()
            self.set_test_status(
                "执行失败", self._failure_detail(name, status, err_code)
            )
            return True

        self.render_success(decoded)
        return True

    def render_success(self, decoded: object) -> None:
        values = getattr(decoded, "values", {})
        if not isinstance(values, Mapping):
            return
        for key in self.metrics.keys():
            if key in values:
                self.metrics.set_value(key, values[key])

    def clear_results(self) -> None:
        self.metrics.clear()
        for bit_grid in self.findChildren(BitGrid):
            bit_grid.clear()

    def measurements_unavailable(self) -> None:
        self.metrics.clear()
        for bit_grid in self.findChildren(BitGrid):
            bit_grid.set_unavailable()

    def set_test_status(self, status: str, detail: str = "") -> None:
        status_text = str(status)
        self.status_label.setText(status_text)
        self.detail_label.setText(str(detail))
        normalized = status_text.lower()
        if "失败" in status_text or "错误" in status_text or "error" in normalized:
            level = "error"
        elif "执行中" in status_text or "运行" in status_text or "running" in normalized:
            level = "running"
        elif "完成" in status_text or "complete" in normalized:
            level = "complete"
        elif "警告" in status_text or "warning" in normalized:
            level = "warning"
        else:
            level = "idle"
        self.status_label.setProperty("statusLevel", level)
        _refresh_style(self.status_label)

    def set_actions_enabled(self, enabled: bool) -> None:
        state = bool(enabled)
        for widget in tuple(self._action_widgets):
            widget.setEnabled(state)
        if self.continuous_button.isChecked():
            self.continuous_button.setEnabled(True)

    def register_action_widget(self, widget: QWidget) -> QWidget:
        if widget not in self._action_widgets:
            self._action_widgets.append(widget)
        return widget

    def _request_run(self) -> None:
        parameters = self._validated_parameters()
        if parameters is not None:
            self.run_requested.emit(str(getattr(self.spec, "key", "")), parameters)

    def _request_continuous(self, enabled: bool) -> None:
        key = str(getattr(self.spec, "key", ""))
        if not enabled:
            self.continuous_requested.emit(key, {}, False)
            return
        parameters = self._validated_parameters()
        if parameters is None:
            self.continuous_button.blockSignals(True)
            self.continuous_button.setChecked(False)
            self.continuous_button.blockSignals(False)
            return
        self.continuous_requested.emit(key, parameters, True)

    def set_continuous_checked(self, checked: bool) -> None:
        self.continuous_button.blockSignals(True)
        self.continuous_button.setChecked(bool(checked))
        self.continuous_button.blockSignals(False)

    def _validated_parameters(self) -> Optional[Dict[str, object]]:
        try:
            parameters = self.collect_parameters()
            if not isinstance(parameters, Mapping):
                raise ParameterValidationError("页面参数必须是键值映射")
        except ParameterValidationError as exc:
            self.set_test_status("参数无效", str(exc))
            if exc.widget is not None:
                exc.widget.setFocus(Qt.OtherFocusReason)
                select_all = getattr(exc.widget, "selectAll", None)
                if callable(select_all):
                    select_all()
            return None
        except (TypeError, ValueError) as exc:
            self.set_test_status("参数无效", str(exc))
            return None
        return dict(parameters)

    def _accepted_names(self) -> Tuple[str, ...]:
        configured = self.accepted_response_names
        if isinstance(configured, str):
            return (configured,)
        if configured:
            return tuple(str(item) for item in configured)
        return (str(getattr(self.spec, "response_name", "")),)

    @staticmethod
    def _integer_value(decoded: object, name: str) -> Optional[int]:
        try:
            return int(getattr(decoded, name))
        except (AttributeError, TypeError, ValueError):
            values = getattr(decoded, "values", {})
            if isinstance(values, Mapping):
                try:
                    return int(values.get(name, 0))
                except (TypeError, ValueError):
                    return None
            return None

    def _failure_detail(
        self, name: str, status: Optional[int], err_code: Optional[int]
    ) -> str:
        values = self.last_values
        parts: List[str] = []
        if name == "error_response":
            if "orig_tg" in values and "orig_st" in values:
                try:
                    parts.append(
                        "原命令=0x{:02X}/0x{:02X}".format(
                            int(values["orig_tg"]), int(values["orig_st"])
                        )
                    )
                except (TypeError, ValueError):
                    pass
            if "orig_seq" in values:
                parts.append("原序号={}".format(values["orig_seq"]))
        if status is not None and (name != "error_response" or "status" in values):
            parts.append("status={}".format(status))
        if err_code is None:
            parts.append("err_code=无效")
        else:
            parts.append("err_code=0x{:04X}".format(err_code & 0xFFFF))
        if name == "error_response" and "detail" in values:
            try:
                parts.append("detail=0x{:08X}".format(int(values["detail"])))
            except (TypeError, ValueError):
                parts.append("detail={}".format(values["detail"]))
        return "，".join(parts) if parts else "响应表示执行失败"


__all__ = [
    "BaseHardwareTestPage",
    "BitGrid",
    "MetricGrid",
    "ParameterValidationError",
    "RawResponseTable",
    "SegmentedControl",
    "U32HexEdit",
    "UNAVAILABLE_TEXT",
]
