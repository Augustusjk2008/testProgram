"""Asynchronous PC-side orchestration for the product hardware tests."""

import math
from dataclasses import dataclass
from enum import Enum
from typing import Dict, Mapping, Optional, Tuple

from PyQt5.QtCore import QObject, QTimer, pyqtSignal

from .product_protocol import DecodedMessage, ProductProtocol, ProductProtocolError


DEFAULT_TIMEOUT_MS = 2000
BUS_TIMEOUT_MS = 60000
MEMORY_TIMEOUT_MS = 120000
SPI_FLASH_TIMEOUT_MS = 180000
DH_TIMEOUT_MARGIN_MS = 2000

TEST_ORDER = (
    "system",
    "memory",
    "spi_flash",
    "bus",
    "di",
    "do",
    "electrical_health",
    "dh_pulse_config",
    "dh",
    "helm_board",
    "timer",
)


class TestStatus(str, Enum):
    __test__ = False

    NOT_RUN = "未执行"
    RUNNING = "执行中"
    COMPLETED = "执行完成"
    EXECUTION_FAILED = "执行失败"
    COMMUNICATION_FAILED = "通信失败"


@dataclass(frozen=True)
class ParameterSpec:
    name: str
    label: str
    value_type: str
    default: object
    choices: Tuple[object, ...] = ()


@dataclass(frozen=True)
class TestSpec:
    key: str
    label: str
    request_name: str
    response_name: str
    timeout_ms: int
    parameters: Tuple[ParameterSpec, ...] = ()

    @property
    def defaults(self) -> Dict[str, object]:
        return {parameter.name: parameter.default for parameter in self.parameters}


def dh_timeout_ms(report_count: int, interval_us: int, delay_us: int) -> int:
    """Return the deadline for the first DH report.

    Subsequent reports refresh the timer with ``dh_report_gap_timeout_ms``.
    Keep the full request signature so existing callers do not need a special
    first-report timeout API.
    """

    del report_count, interval_us
    return DH_TIMEOUT_MARGIN_MS + int(
        math.ceil(max(0, int(delay_us)) / 1000.0)
    )


def dh_report_gap_timeout_ms(interval_us: int) -> int:
    """Return the inactivity deadline after one valid DH report arrives."""

    return DH_TIMEOUT_MARGIN_MS + int(
        math.ceil(max(0, int(interval_us)) / 1000.0)
    )


LEGACY_HELM_TEST_SPEC = TestSpec(
    "helm",
    "舵控",
    "helm_start_request",
    "helm_start_response",
    DEFAULT_TIMEOUT_MS,
    (
        ParameterSpec("waveform", "波形", "int", 0),
        ParameterSpec("freq", "频率 Hz", "float", 0.3),
        ParameterSpec("ampl", "幅值 deg", "float", 30.0),
        ParameterSpec("offset", "偏置 deg", "float", 0.0),
        ParameterSpec("start", "起始相位 rad", "float", 0.0),
        ParameterSpec("max_freq", "扫频上限 Hz", "float", 0.0),
        ParameterSpec("enable", "使能位图", "int", 1),
    ),
)


TEST_SPECS = (
    TestSpec("system", "系统状态", "system_status_request", "system_status_response", DEFAULT_TIMEOUT_MS),
    TestSpec(
        "memory",
        "内存",
        "memperf_test_request",
        "memperf_test_response",
        MEMORY_TIMEOUT_MS,
        (
            ParameterSpec("memperf_type", "测试类型", "int", 0, tuple(range(7))),
            ParameterSpec("length", "长度 KB", "int", 65536),
            ParameterSpec("seed", "种子", "int", 0x5A5A5A5A),
        ),
    ),
    TestSpec("spi_flash", "SPI Flash", "spi_flash_test_request", "spi_flash_test_response", SPI_FLASH_TIMEOUT_MS),
    TestSpec(
        "bus",
        "总线",
        "bus_loop_test_request",
        "bus_loop_test_response",
        BUS_TIMEOUT_MS,
        (
            ParameterSpec("bus_mode", "测试方式", "str", "loop", ("loop", "echo")),
            ParameterSpec("link_id", "链路", "int", 0, (0, 1, 3)),
            ParameterSpec("total_count", "总次数", "int", 1000),
            ParameterSpec("data_hex", "回显数据", "hex", "4D 42 31"),
        ),
    ),
    TestSpec("di", "DI", "di_read_request", "di_read_response", DEFAULT_TIMEOUT_MS),
    TestSpec(
        "do",
        "DO",
        "do_write_request",
        "do_write_response",
        DEFAULT_TIMEOUT_MS,
        (
            ParameterSpec("channel[0]", "通道位图 0", "int", 0),
            ParameterSpec("channel[1]", "通道位图 1", "int", 0),
        ),
    ),
    TestSpec(
        "electrical_health",
        "电气健康",
        "elec_health_status_request",
        "elec_health_status_response",
        DEFAULT_TIMEOUT_MS,
    ),
    TestSpec(
        "dh_pulse_config",
        "DH 脉宽配置",
        "dh_pulse_config_request",
        "dh_pulse_config_response",
        DEFAULT_TIMEOUT_MS,
        (ParameterSpec("config_enable", "配置使能", "int", 1, (0, 1)),)
        + tuple(
            ParameterSpec(
                "pulse_width[{}]".format(index),
                "DH{} 脉宽 ms".format(index),
                "int",
                80 if index == 0 else 63,
            )
            for index in range(23)
        ),
    ),
    TestSpec(
        "dh",
        "DH",
        "dh_control_request",
        "dh_control_response",
        DEFAULT_TIMEOUT_MS,
        (
            ParameterSpec("power_enable", "DH 电源使能", "int", 1, (0, 1)),
            ParameterSpec("return_enable", "DH 回线使能", "int", 1, (0, 1)),
            ParameterSpec("channel[0]", "通道位图 0", "int", 0x007FFFFF),
            ParameterSpec("channel[1]", "通道位图 1", "int", 0),
            ParameterSpec("report_count", "回告次数", "int", 50),
            ParameterSpec("interval_us", "回告间隔 us", "int", 2500),
            ParameterSpec("delay_us", "首帧等待 us", "int", 0),
        ),
    ),
    TestSpec(
        "helm_board",
        "舵控板级",
        "helm_board_test_request",
        "helm_board_test_response",
        DEFAULT_TIMEOUT_MS,
        tuple(
            ParameterSpec(
                "pwm_duty_percent[{}]".format(index),
                "舵{} PWM占空比 %".format(index + 1),
                "int",
                0,
            )
            for index in range(4)
        )
        + tuple(
            ParameterSpec(
                "direction[{}]".format(index),
                "舵{}方向".format(index + 1),
                "int",
                0,
                (0, 1),
            )
            for index in range(4)
        ),
    ),
    TestSpec(
        "timer",
        "定时器",
        "timer_jitter_start_request",
        "timer_jitter_start_response",
        DEFAULT_TIMEOUT_MS,
        (ParameterSpec("mode", "模式", "int", 0, (0, 1)),),
    ),
)

_SPEC_BY_KEY = {spec.key: spec for spec in TEST_SPECS}
_SPEC_BY_KEY[LEGACY_HELM_TEST_SPEC.key] = LEGACY_HELM_TEST_SPEC
_STATUS_KEYS = TEST_ORDER + (LEGACY_HELM_TEST_SPEC.key,)


class HardwareTestSession(QObject):
    """One ordinary request at a time, plus stateful DH/helm routing."""

    test_started = pyqtSignal(str)
    test_status_changed = pyqtSignal(str, str, str)
    test_finished = pyqtSignal(str, str, str)
    response_received = pyqtSignal(str, object)
    dh_report_received = pyqtSignal(object)
    helm_feedback_received = pyqtSignal(object)
    busy_changed = pyqtSignal(bool)
    all_finished = pyqtSignal()
    log_event = pyqtSignal(str, str)

    def __init__(
        self,
        channel: QObject,
        protocol: Optional[ProductProtocol] = None,
        parent: Optional[QObject] = None,
    ) -> None:
        super().__init__(parent)
        self.channel = channel
        self.protocol = protocol or ProductProtocol()
        self._enabled = False
        self._statuses = {key: TestStatus.NOT_RUN for key in _STATUS_KEYS}
        self._active_test_key: Optional[str] = None
        self._active_request_name: Optional[str] = None
        self._pending_response_name: Optional[str] = None
        self._active_sequence: Optional[int] = None
        self._active_parameters: Dict[str, object] = {}
        self._phase = ""
        self._dh_remaining = 0
        self._dh_next_sequence: Optional[int] = None
        self._dh_failure_detail = ""
        self._dh_report_gap_timeout_ms = DH_TIMEOUT_MARGIN_MS
        self._helm_terminal_status: Optional[TestStatus] = None
        self._helm_terminal_detail = ""
        self._running_all = False
        self._run_all_queue = []
        self._stopping = False

        self._response_timer = QTimer(self)
        self._response_timer.setSingleShot(True)
        self._response_timer.timeout.connect(self._on_response_timeout)

        self._next_all_timer = QTimer(self)
        self._next_all_timer.setSingleShot(True)
        self._next_all_timer.timeout.connect(self._run_next_all)

        channel.frame_received.connect(self._on_frame_received)
        channel.protocol_error.connect(self._on_protocol_error)
        channel.send_failed.connect(self._on_send_failed)
        channel.io_error.connect(self._on_io_error)
        channel.disconnected.connect(self._on_disconnected)

    @property
    def test_specs(self) -> Tuple[TestSpec, ...]:
        return TEST_SPECS

    @property
    def response_timer(self) -> QTimer:
        return self._response_timer

    @property
    def active_request_name(self) -> Optional[str]:
        return self._active_request_name

    @property
    def active_test_key(self) -> Optional[str]:
        return self._active_test_key

    @property
    def active_sequence(self) -> Optional[int]:
        return self._active_sequence

    @property
    def is_busy(self) -> bool:
        return self._active_test_key is not None or self._running_all

    def set_enabled(self, enabled: bool) -> None:
        self._enabled = bool(enabled)
        if not self._enabled and self.is_busy:
            self.stop()

    def status_for(self, key: str) -> TestStatus:
        if key not in self._statuses:
            raise KeyError("未知测试项：{}".format(key))
        return self._statuses[key]

    def parameter_fields(self, key: str) -> Tuple[ParameterSpec, ...]:
        return self._spec(key).parameters

    def run_test(
        self, key: str, parameters: Optional[Mapping[str, object]] = None
    ) -> bool:
        spec = self._spec(key)
        if not self._enabled:
            self.log_event.emit("ERROR", "硬件测试模式未启用")
            return False
        if self._active_test_key is not None:
            self.log_event.emit("WARNING", "已有普通请求在途")
            return False

        merged = spec.defaults
        if parameters:
            merged.update(dict(parameters))
        if key == "bus":
            try:
                link_id = self._bounded_integer(
                    merged.get("link_id", 0), "link_id", 0, 3
                )
            except ValueError as exc:
                return self._reject_before_send(key, str(exc))
            if link_id == 2:
                return self._reject_before_send(
                    key, "link 2 是 COM3 控制口，不能执行总线测试"
                )
            if link_id not in (0, 1, 3):
                return self._reject_before_send(
                    key, "总线仅支持 link 0/1/3（COM1/COM2/COM4）"
                )
            bus_mode = str(merged.get("bus_mode", "loop"))
            if bus_mode not in ("loop", "echo"):
                return self._reject_before_send(key, "总线测试方式必须是 loop 或 echo")
            merged["bus_mode"] = bus_mode
            if bus_mode == "loop":
                try:
                    merged["total_count"] = self._bounded_integer(
                        merged.get("total_count", 1000), "总线收发次数", 1, 100000
                    )
                except ValueError as exc:
                    return self._reject_before_send(key, str(exc))
        if key == "helm_board":
            try:
                for index in range(4):
                    duty_field = "pwm_duty_percent[{}]".format(index)
                    merged[duty_field] = self._bounded_integer(
                        merged.get(duty_field, 0),
                        "舵{} PWM占空比".format(index + 1),
                        0,
                        100,
                    )
                    direction_field = "direction[{}]".format(index)
                    merged[direction_field] = self._bounded_integer(
                        merged.get(direction_field, 0),
                        "舵{}方向".format(index + 1),
                        0,
                        1,
                    )
            except ValueError as exc:
                return self._reject_before_send(key, str(exc))
        if key == "timer":
            raw_mode = merged.get("mode", 0)
            try:
                mode = (
                    int(raw_mode.strip(), 0)
                    if isinstance(raw_mode, str)
                    else int(raw_mode)
                )
            except (TypeError, ValueError):
                return self._reject_before_send(key, "定时器模式必须是 0 或 1")
            if mode not in (0, 1) or (
                isinstance(raw_mode, float) and raw_mode != mode
            ):
                return self._reject_before_send(key, "定时器模式必须是 0 或 1")
            merged["mode"] = mode

        self._active_test_key = key
        self._active_parameters = merged
        self._phase = key
        self._set_status(key, TestStatus.RUNNING, "开始执行")
        self.test_started.emit(key)
        self.busy_changed.emit(True)

        if key == "bus" and str(merged.get("bus_mode", "loop")) == "echo":
            try:
                values = self._bus_echo_values(merged)
            except (ProductProtocolError, TypeError, ValueError) as exc:
                return self._finish_and_return(
                    TestStatus.EXECUTION_FAILED, str(exc)
                )
            return self._send_request(
                "bus_echo_test_request", "bus_echo_test_response", values, spec.timeout_ms
            )
        if key == "dh_pulse_config":
            try:
                config_enable = self._bounded_integer(
                    merged.get("config_enable", 1), "配置使能", 0, 1
                )
                widths = [
                    self._bounded_integer(
                        merged.get(
                            "pulse_width[{}]".format(index),
                            80 if index == 0 else 63,
                        ),
                        "DH{} 脉宽".format(index),
                        0,
                        65535,
                    )
                    for index in range(23)
                ]
            except ValueError as exc:
                return self._finish_and_return(
                    TestStatus.EXECUTION_FAILED, str(exc)
                )
            merged["config_enable"] = config_enable
            for index, width in enumerate(widths):
                merged["pulse_width[{}]".format(index)] = width
        if key == "dh":
            try:
                merged["power_enable"] = self._bounded_integer(
                    merged.get("power_enable", 1), "DH 电源使能", 0, 1
                )
                merged["return_enable"] = self._bounded_integer(
                    merged.get("return_enable", 1), "DH 回线使能", 0, 1
                )
                merged["channel[0]"] = self._bounded_integer(
                    merged.get("channel[0]", 0), "DH 通道位图 0", 0, 0x007FFFFF
                )
                merged["channel[1]"] = self._bounded_integer(
                    merged.get("channel[1]", 0), "DH 通道位图 1", 0, 0
                )
                report_count = self._bounded_integer(
                    merged.get("report_count", 50), "DH 回告次数", 1, 65535
                )
                interval_us = self._bounded_integer(
                    merged.get("interval_us", 2500), "DH 回告间隔", 2500, 65535
                )
                delay_us = self._bounded_integer(
                    merged.get("delay_us", 0), "DH 首帧等待", 0, 65535
                )
            except ValueError as exc:
                return self._finish_and_return(
                    TestStatus.EXECUTION_FAILED, str(exc)
                )
            merged["report_count"] = report_count
            merged["interval_us"] = interval_us
            merged["delay_us"] = delay_us
            self._dh_remaining = report_count
            self._dh_failure_detail = ""
            self._dh_report_gap_timeout_ms = dh_report_gap_timeout_ms(interval_us)
            timeout = dh_timeout_ms(report_count, interval_us, delay_us)
            return self._send_request(spec.request_name, spec.response_name, merged, timeout)
        if key == "helm":
            self._phase = "helm_start"
        elif key == "timer":
            self._phase = "timer_start"
        return self._send_request(
            spec.request_name, spec.response_name, merged, spec.timeout_ms
        )

    def run_all(
        self, parameter_overrides: Optional[Mapping[str, Mapping[str, object]]] = None
    ) -> bool:
        if not self._enabled or self.is_busy:
            return False
        for key in TEST_ORDER:
            self._set_status(key, TestStatus.NOT_RUN, "")
        self._run_all_queue = list(TEST_ORDER)
        self._run_all_overrides = dict(parameter_overrides or {})
        self._running_all = True
        self._run_next_all()
        return True

    def stop(self) -> None:
        self._next_all_timer.stop()
        self._run_all_queue = []
        self._running_all = False
        self._stopping = True
        if self._active_test_key is not None:
            if self._active_test_key == "helm":
                self._request_helm_stop(
                    TestStatus.EXECUTION_FAILED,
                    "用户停止",
                )
            else:
                self._finish(TestStatus.EXECUTION_FAILED, "用户停止")
        else:
            self._response_timer.stop()
            self.busy_changed.emit(False)
        self._stopping = False

    def _spec(self, key: str) -> TestSpec:
        try:
            return _SPEC_BY_KEY[key]
        except KeyError as exc:
            raise KeyError("未知测试项：{}".format(key)) from exc

    def _reject_before_send(self, key: str, detail: str) -> bool:
        self._set_status(key, TestStatus.EXECUTION_FAILED, detail)
        self.test_finished.emit(key, TestStatus.EXECUTION_FAILED.value, detail)
        self.log_event.emit("ERROR", detail)
        return False

    def _finish_and_return(self, status: TestStatus, detail: str) -> bool:
        self._finish(status, detail)
        return False

    @staticmethod
    def _bounded_integer(value: object, label: str, minimum: int, maximum: int) -> int:
        if isinstance(value, float) and not value.is_integer():
            raise ValueError("{}必须是整数".format(label))
        try:
            number = int(value.strip(), 0) if isinstance(value, str) else int(value)
        except (TypeError, ValueError) as exc:
            raise ValueError("{}必须是整数".format(label)) from exc
        if not minimum <= number <= maximum:
            raise ValueError(
                "{}必须在 {}..{} 范围内".format(label, minimum, maximum)
            )
        return number

    def _send_request(
        self,
        request_name: str,
        response_name: str,
        values: Mapping[str, object],
        timeout_ms: int,
    ) -> bool:
        try:
            outbound = self.protocol.build_request(request_name, values)
        except (KeyError, ProductProtocolError, TypeError, ValueError) as exc:
            return self._finish_and_return(TestStatus.EXECUTION_FAILED, str(exc))
        self._active_request_name = request_name
        self._pending_response_name = response_name
        self._active_sequence = outbound.sequence
        if request_name == "dh_control_request":
            self._dh_next_sequence = outbound.sequence
        self.log_event.emit(
            "INFO", "发送 {}，PC seq={}".format(request_name, outbound.sequence)
        )
        accepted = bool(self.channel.send_payload(outbound.payload))
        if not accepted:
            if self._active_test_key is not None:
                self._finish(TestStatus.COMMUNICATION_FAILED, "串口未接受发送请求")
            return False
        if self._active_test_key is not None:
            self._response_timer.start(max(1, int(timeout_ms)))
        return True

    @staticmethod
    def _bus_echo_values(parameters: Mapping[str, object]) -> Dict[str, object]:
        values = {"link_id": int(parameters.get("link_id", 0))}
        text = str(parameters.get("data_hex", "")).replace(",", " ")
        try:
            raw = bytes.fromhex(text)
        except ValueError as exc:
            raise ProductProtocolError("总线回显数据不是有效十六进制") from exc
        if len(raw) > 114:
            raise ProductProtocolError("总线回显数据不能超过 114 字节")
        values["data"] = raw + bytes(114 - len(raw))
        return values

    def _on_frame_received(self, payload: bytes, _wire_frame: bytes) -> None:
        if not self._enabled and not self._helm_cleanup_pending():
            return
        try:
            decoded = self.protocol.decode_response(payload)
        except ProductProtocolError as exc:
            if self._active_test_key is not None:
                detail = "产品协议解析失败：{}".format(exc)
                if self._active_test_key == "helm" and self._phase == "helm_feedback":
                    self._request_helm_stop(TestStatus.COMMUNICATION_FAILED, detail)
                else:
                    self._finish(TestStatus.COMMUNICATION_FAILED, detail)
            else:
                self.log_event.emit("WARNING", "忽略非产品协议帧：{}".format(exc))
            return

        if decoded.name == "helm_feedback_response":
            if self._active_test_key == "helm" and self._phase == "helm_feedback":
                self.response_received.emit("helm", decoded)
            self._handle_helm_feedback(decoded)
            return
        if decoded.name == "error_response":
            self._handle_error_response(decoded)
            return
        if self._active_test_key is None:
            self.log_event.emit("WARNING", "收到无在途请求的响应 {}".format(decoded.name))
            return
        if decoded.name != self._pending_response_name:
            self.log_event.emit(
                "WARNING",
                "当前等待 {}，忽略 {}".format(self._pending_response_name, decoded.name),
            )
            return

        if self._active_test_key == "dh":
            if decoded.sequence != self._dh_next_sequence:
                self.log_event.emit(
                    "WARNING",
                    "当前等待 DH seq={}，忽略 seq={}".format(
                        self._dh_next_sequence, decoded.sequence
                    ),
                )
                return
            self._dh_next_sequence = (decoded.sequence + 1) & 0xFFFF
            self.response_received.emit("dh", decoded)
            self.dh_report_received.emit(decoded)
            self._dh_remaining -= 1
            self._response_timer.start(self._dh_report_gap_timeout_ms)
            if decoded.status != 0 or decoded.err_code != 0:
                detail = "status={} err_code=0x{:04X}".format(
                    decoded.status, decoded.err_code
                )
                if decoded.err_code != 0x0203:
                    self._finish(TestStatus.EXECUTION_FAILED, detail)
                    return
                if not self._dh_failure_detail:
                    self._dh_failure_detail = detail
            if self._dh_remaining <= 0:
                if self._dh_failure_detail:
                    self._finish(
                        TestStatus.EXECUTION_FAILED, self._dh_failure_detail
                    )
                else:
                    self._finish(TestStatus.COMPLETED, "已接收全部 DH 回告")
            return

        if decoded.sequence != self._active_sequence:
            self.log_event.emit(
                "WARNING",
                "当前等待 seq={}，忽略 seq={}".format(
                    self._active_sequence, decoded.sequence
                ),
            )
            return
        self.response_received.emit(self._active_test_key, decoded)
        if decoded.status != 0 or decoded.err_code != 0:
            self._finish(
                TestStatus.EXECUTION_FAILED,
                "status={} err_code=0x{:04X}".format(decoded.status, decoded.err_code),
            )
            return

        self._response_timer.stop()
        if self._phase == "helm_start":
            if self._helm_terminal_status is not None:
                self._phase = "helm_feedback"
                self._request_helm_stop(
                    self._helm_terminal_status,
                    self._helm_terminal_detail,
                )
            else:
                self._phase = "helm_feedback"
                self._active_request_name = None
                self._pending_response_name = "helm_feedback_response"
                self._active_sequence = None
                self._response_timer.start(DEFAULT_TIMEOUT_MS)
            return
        if self._phase == "helm_stop":
            status = self._helm_terminal_status or TestStatus.COMPLETED
            detail = self._helm_terminal_detail or "舵控反馈和停止响应已完成"
            self._finish(status, detail)
            return
        if self._phase == "timer_start":
            self._phase = "timer_stop"
            self._send_request(
                "timer_jitter_stop_request",
                "timer_jitter_stop_response",
                {},
                DEFAULT_TIMEOUT_MS,
            )
            return
        if self._phase == "timer_stop":
            self._finish(TestStatus.COMPLETED, "定时器统计和停止响应已完成")
            return
        self._finish(TestStatus.COMPLETED, "收到执行响应")

    def _handle_helm_feedback(self, decoded: DecodedMessage) -> None:
        if self._active_test_key != "helm" or self._phase != "helm_feedback":
            self.log_event.emit("WARNING", "忽略当前状态之外的舵反馈")
            return
        if decoded.status != 0 or decoded.err_code != 0:
            self._request_helm_stop(
                TestStatus.EXECUTION_FAILED,
                "舵反馈 status={} err_code=0x{:04X}".format(
                    decoded.status, decoded.err_code
                ),
            )
            return
        self.helm_feedback_received.emit(decoded)
        if self._running_all:
            self._request_helm_stop(
                TestStatus.COMPLETED,
                "舵控反馈和停止响应已完成",
            )
        else:
            self._response_timer.start(DEFAULT_TIMEOUT_MS)

    def _helm_cleanup_pending(self) -> bool:
        return (
            self._active_test_key == "helm"
            and self._helm_terminal_status is not None
        )

    def _request_helm_stop(self, status: TestStatus, detail: str) -> None:
        if self._active_test_key != "helm":
            return
        if self._helm_terminal_status is None:
            self._helm_terminal_status = status
            self._helm_terminal_detail = detail
        if self._phase == "helm_start":
            return
        if self._phase == "helm_stop":
            return
        self._response_timer.stop()
        self._phase = "helm_stop"
        self._send_request(
            "helm_stop_request", "helm_stop_response", {}, DEFAULT_TIMEOUT_MS
        )

    def _handle_error_response(self, decoded: DecodedMessage) -> None:
        if (
            self._active_test_key is None
            or self._active_request_name is None
            or self._active_sequence is None
        ):
            self.log_event.emit("WARNING", "收到无可匹配请求的错误响应")
            return
        request_definition = self.protocol.catalog.get(self._active_request_name)
        orig_tg = int(decoded.values.get("orig_tg", -1))
        orig_st = int(decoded.values.get("orig_st", -1))
        if (orig_tg, orig_st) != (
            request_definition.type_group,
            request_definition.sub_type,
        ):
            self.log_event.emit("WARNING", "忽略不属于当前请求的错误响应")
            return
        orig_seq = int(decoded.values.get("orig_seq", -1))
        if decoded.sequence != self._active_sequence or orig_seq != self._active_sequence:
            self.log_event.emit("WARNING", "忽略序号不属于当前请求的错误响应")
            return
        self.response_received.emit(self._active_test_key, decoded)
        self._finish(
            TestStatus.EXECUTION_FAILED,
            "板端错误响应 err_code=0x{:04X}".format(decoded.err_code),
        )

    def _on_response_timeout(self) -> None:
        if self._active_test_key is None:
            return
        if self._active_test_key == "helm" and self._phase == "helm_feedback":
            self._request_helm_stop(
                TestStatus.COMMUNICATION_FAILED,
                "等待舵反馈超时",
            )
            return
        detail = (
            "等待舵控停止响应超时"
            if self._active_test_key == "helm" and self._phase == "helm_stop"
            else "等待响应超时"
        )
        self._finish(TestStatus.COMMUNICATION_FAILED, detail)

    def _on_protocol_error(self, code: str, message: str, _frame: bytes) -> None:
        if (self._enabled or self._helm_cleanup_pending()) and self._active_test_key is not None:
            detail = "物理协议错误 [{}] {}".format(code, message)
            if self._active_test_key == "helm" and self._phase == "helm_feedback":
                self._request_helm_stop(TestStatus.COMMUNICATION_FAILED, detail)
            else:
                self._finish(TestStatus.COMMUNICATION_FAILED, detail)

    def _on_send_failed(self, _frame: bytes, message: str) -> None:
        if (self._enabled or self._helm_cleanup_pending()) and self._active_test_key is not None:
            self._finish(TestStatus.COMMUNICATION_FAILED, message)

    def _on_io_error(self, message: str) -> None:
        if (self._enabled or self._helm_cleanup_pending()) and self._active_test_key is not None:
            self._finish(TestStatus.COMMUNICATION_FAILED, message)

    def _on_disconnected(self) -> None:
        if (self._enabled or self._helm_cleanup_pending()) and self._active_test_key is not None:
            self._finish(TestStatus.COMMUNICATION_FAILED, "串口断开")

    def _set_status(self, key: str, status: TestStatus, detail: str) -> None:
        self._statuses[key] = status
        self.test_status_changed.emit(key, status.value, detail)

    def _finish(self, status: TestStatus, detail: str) -> None:
        key = self._active_test_key
        if key is None:
            return
        self._response_timer.stop()
        self._set_status(key, status, detail)
        self.test_finished.emit(key, status.value, detail)
        level = "COMPLETE" if status == TestStatus.COMPLETED else "ERROR"
        self.log_event.emit(level, "{}：{}".format(_SPEC_BY_KEY[key].label, detail))
        self._active_test_key = None
        self._active_request_name = None
        self._pending_response_name = None
        self._active_sequence = None
        self._active_parameters = {}
        self._phase = ""
        self._dh_remaining = 0
        self._dh_next_sequence = None
        self._dh_failure_detail = ""
        self._dh_report_gap_timeout_ms = DH_TIMEOUT_MARGIN_MS
        self._helm_terminal_status = None
        self._helm_terminal_detail = ""
        self.busy_changed.emit(False)

        self._schedule_next_all()

    def _schedule_next_all(self) -> None:
        if (
            self._running_all
            and not self._stopping
            and not self._next_all_timer.isActive()
        ):
            self._next_all_timer.start(0)

    def _run_next_all(self) -> None:
        if not self._running_all:
            return
        if self._active_test_key is not None:
            return
        if not self._run_all_queue:
            self._running_all = False
            self.busy_changed.emit(False)
            self.all_finished.emit()
            return
        key = self._run_all_queue.pop(0)
        parameters = self._run_all_overrides.get(key, {})
        if not self.run_test(key, parameters):
            self._schedule_next_all()


__all__ = [
    "BUS_TIMEOUT_MS",
    "DEFAULT_TIMEOUT_MS",
    "DH_TIMEOUT_MARGIN_MS",
    "LEGACY_HELM_TEST_SPEC",
    "dh_report_gap_timeout_ms",
    "HardwareTestSession",
    "MEMORY_TIMEOUT_MS",
    "ParameterSpec",
    "SPI_FLASH_TIMEOUT_MS",
    "TEST_ORDER",
    "TEST_SPECS",
    "TestSpec",
    "TestStatus",
    "dh_timeout_ms",
]
