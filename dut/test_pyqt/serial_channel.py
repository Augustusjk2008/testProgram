"""Asynchronous QtSerialPort channel with protocol framing and send queuing."""

from collections import deque
from typing import Deque, Optional

from PyQt5.QtCore import QIODevice, QObject, QTimer, pyqtSignal
from PyQt5.QtSerialPort import QSerialPort

from .app_config import SerialPortConfig
from .serial_protocol import FrameDecoder, FrameEventType, encode_frame


# Keep transport failures distinct from the UI's 1000 ms echo deadline.
WRITE_TIMEOUT_MS = 750


class SerialChannel(QObject):
    """Own one serial backend, one immutable config and one frame decoder."""

    connected = pyqtSignal()
    disconnected = pyqtSignal()
    frame_received = pyqtSignal(bytes, bytes)  # payload, complete wire frame
    protocol_error = pyqtSignal(str, str, bytes)  # code, message, candidate frame
    frame_queued = pyqtSignal(bytes)
    send_completed = pyqtSignal(bytes)
    send_failed = pyqtSignal(bytes, str)
    io_error = pyqtSignal(str)

    _DATA_BITS = {
        5: QSerialPort.Data5,
        6: QSerialPort.Data6,
        7: QSerialPort.Data7,
        8: QSerialPort.Data8,
    }
    _PARITY = {
        "None": QSerialPort.NoParity,
        "Even": QSerialPort.EvenParity,
        "Odd": QSerialPort.OddParity,
        "Mark": QSerialPort.MarkParity,
        "Space": QSerialPort.SpaceParity,
    }
    _STOP_BITS = {
        "1": QSerialPort.OneStop,
        "1.5": QSerialPort.OneAndHalfStop,
        "2": QSerialPort.TwoStop,
    }
    _FLOW_CONTROL = {
        "None": QSerialPort.NoFlowControl,
        "Hardware": QSerialPort.HardwareControl,
        "Software": QSerialPort.SoftwareControl,
    }

    def __init__(
        self,
        config: Optional[SerialPortConfig] = None,
        serial_port: Optional[QObject] = None,
        parent: Optional[QObject] = None,
        write_timeout_ms: int = WRITE_TIMEOUT_MS,
    ) -> None:
        super().__init__(parent)
        if write_timeout_ms <= 0:
            raise ValueError("发送超时必须大于 0 ms")
        self._config = config or SerialPortConfig()
        self._serial = serial_port if serial_port is not None else QSerialPort(self)
        self._decoder = FrameDecoder(self._config.protocol)
        self._send_queue: Deque[bytes] = deque()
        self._accepted_bytes = 0
        self._acknowledged_bytes = 0
        self._pending_ack_bytes = 0
        self._pumping = False
        self._closing = False
        self._connected_state = False

        self._decoder_timer = QTimer(self)
        self._decoder_timer.setInterval(
            max(10, min(100, self._config.protocol.frame_timeout_ms // 5))
        )
        self._decoder_timer.timeout.connect(self._check_decoder_timeout)

        self._write_retry_timer = QTimer(self)
        self._write_retry_timer.setSingleShot(True)
        self._write_retry_timer.setInterval(10)
        self._write_retry_timer.timeout.connect(self._pump_write)

        self._write_watchdog_timer = QTimer(self)
        self._write_watchdog_timer.setSingleShot(True)
        self._write_watchdog_timer.setInterval(write_timeout_ms)
        self._write_watchdog_timer.timeout.connect(self._on_write_timeout)

        self._serial.readyRead.connect(self._on_ready_read)
        self._serial.bytesWritten.connect(self._on_bytes_written)
        self._serial.errorOccurred.connect(self._on_serial_error)

    @property
    def config(self) -> SerialPortConfig:
        return self._config

    @property
    def serial_port(self) -> QObject:
        return self._serial

    @property
    def is_open(self) -> bool:
        return bool(self._serial.isOpen())

    def set_config(self, config: SerialPortConfig) -> bool:
        if self.is_open:
            self.io_error.emit("串口已打开，不能修改配置")
            return False
        self._config = config
        self._decoder = FrameDecoder(config.protocol)
        self._decoder_timer.setInterval(
            max(10, min(100, config.protocol.frame_timeout_ms // 5))
        )
        return True

    def open(self, config: Optional[SerialPortConfig] = None) -> bool:
        if self.is_open:
            return True
        if config is not None and not self.set_config(config):
            return False
        if not self._config.port_name.strip():
            self.io_error.emit("未选择 Windows 串口")
            return False
        if not self._apply_config():
            return False
        if not self._serial.open(QIODevice.ReadWrite):
            self.io_error.emit("打开串口失败：{}".format(self._error_string()))
            return False

        self._decoder.reset()
        self._decoder_timer.start()
        self._connected_state = True
        self.connected.emit()
        return True

    def close(self) -> None:
        was_connected = self._connected_state
        self._connected_state = False
        self._decoder_timer.stop()
        self._write_retry_timer.stop()
        self._write_watchdog_timer.stop()
        self._decoder.reset()
        if self._send_queue:
            self._fail_all_pending("串口已关闭")
        if self.is_open:
            self._closing = True
            try:
                self._serial.close()
            finally:
                self._closing = False
        if was_connected:
            self.disconnected.emit()

    def send_payload(self, payload: bytes) -> bool:
        try:
            frame = encode_frame(payload, self._config.protocol)
        except (TypeError, ValueError) as exc:
            self.send_failed.emit(b"", str(exc))
            return False
        if not self.is_open:
            self.send_failed.emit(frame, "串口未连接")
            return False

        queue_was_empty = not self._send_queue
        self._send_queue.append(frame)
        if queue_was_empty:
            self._write_watchdog_timer.start()
        self.frame_queued.emit(frame)
        return self._pump_write()

    def _apply_config(self) -> bool:
        if self._serial.setPortName(self._config.port_name) is False:
            self.io_error.emit("设置端口名失败：{}".format(self._error_string()))
            return False
        settings = (
            ("波特率", self._serial.setBaudRate, self._config.baud_rate),
            ("数据位", self._serial.setDataBits, self._DATA_BITS[self._config.data_bits]),
            ("校验", self._serial.setParity, self._PARITY[self._config.parity]),
            ("停止位", self._serial.setStopBits, self._STOP_BITS[self._config.stop_bits]),
            (
                "流控",
                self._serial.setFlowControl,
                self._FLOW_CONTROL[self._config.flow_control],
            ),
        )
        for name, setter, value in settings:
            if setter(value) is False:
                self.io_error.emit("设置{}失败：{}".format(name, self._error_string()))
                return False
        return True

    def _on_ready_read(self) -> None:
        try:
            chunk = bytes(self._serial.readAll())
        except Exception as exc:  # Qt backends report most failures through errorOccurred.
            self.io_error.emit("读取串口失败：{}".format(exc))
            return
        if not chunk:
            return
        self._dispatch_decoder_events(self._decoder.feed(chunk))

    def _check_decoder_timeout(self) -> None:
        self._dispatch_decoder_events(self._decoder.check_timeout())

    def _dispatch_decoder_events(self, events: list) -> None:
        for event in events:
            if event.kind == FrameEventType.FRAME:
                self.frame_received.emit(event.payload, event.frame)
            else:
                code = event.error_code.value if event.error_code is not None else "unknown"
                self.protocol_error.emit(code, event.message, event.frame)

    def _pump_write(self) -> bool:
        if self._pumping or not self._send_queue:
            return True
        self._write_retry_timer.stop()
        if not self.is_open:
            self._fail_all_pending("串口未连接")
            return False

        self._pumping = True
        try:
            frame = self._send_queue[0]
            while self._accepted_bytes < len(frame):
                remaining = frame[self._accepted_bytes :]
                try:
                    accepted = int(self._serial.write(remaining))
                except Exception as exc:
                    self._write_failure("串口写入异常：{}".format(exc))
                    return False
                if accepted < 0:
                    self._write_failure("串口写入失败：{}".format(self._error_string()))
                    return False
                if accepted > len(remaining):
                    self._write_failure("串口后端返回了无效的写入长度")
                    return False
                if accepted == 0:
                    self._apply_acknowledgements()
                    if self._accepted_bytes == self._acknowledged_bytes:
                        self._write_retry_timer.start()
                    return True
                self._accepted_bytes += accepted
            self._apply_acknowledgements()
            return True
        finally:
            self._pumping = False

    def _on_bytes_written(self, count: int) -> None:
        self._pending_ack_bytes += max(0, int(count))
        self._apply_acknowledgements()
        if not self._pumping:
            self._pump_write()

    def _apply_acknowledgements(self) -> None:
        if not self._send_queue:
            self._pending_ack_bytes = 0
            return
        remaining_ack = max(0, self._accepted_bytes - self._acknowledged_bytes)
        acknowledged = min(self._pending_ack_bytes, remaining_ack)
        self._pending_ack_bytes -= acknowledged
        self._acknowledged_bytes += acknowledged
        if (
            self._send_queue
            and self._accepted_bytes == len(self._send_queue[0])
            and self._acknowledged_bytes == self._accepted_bytes
        ):
            completed = self._send_queue.popleft()
            self._accepted_bytes = 0
            self._acknowledged_bytes = 0
            self._pending_ack_bytes = 0
            self._write_watchdog_timer.stop()
            self.send_completed.emit(completed)
            if self._send_queue:
                self._write_watchdog_timer.start()
                if self._pumping:
                    self._write_retry_timer.start()

    def _write_failure(self, message: str) -> None:
        self.io_error.emit(message)
        self._fail_all_pending(message)

    def _on_write_timeout(self) -> None:
        if self._send_queue:
            self._write_failure(
                "串口发送超过 {} ms，队列已清空".format(
                    self._write_watchdog_timer.interval()
                )
            )

    def _fail_all_pending(self, message: str) -> None:
        self._write_retry_timer.stop()
        self._write_watchdog_timer.stop()
        failed = list(self._send_queue)
        self._send_queue.clear()
        self._accepted_bytes = 0
        self._acknowledged_bytes = 0
        self._pending_ack_bytes = 0
        for frame in failed:
            self.send_failed.emit(frame, message)

    def _on_serial_error(self, error_code: int) -> None:
        if self._closing or error_code == QSerialPort.NoError:
            return
        message = "串口 I/O 错误：{}".format(self._error_string())
        self.io_error.emit(message)
        if self._send_queue:
            self._fail_all_pending(message)
        fatal_errors = (
            QSerialPort.DeviceNotFoundError,
            QSerialPort.PermissionError,
            QSerialPort.OpenError,
            QSerialPort.NotOpenError,
            QSerialPort.ResourceError,
            QSerialPort.ReadError,
            QSerialPort.WriteError,
            QSerialPort.UnsupportedOperationError,
            QSerialPort.UnknownError,
        )
        if error_code in fatal_errors:
            self.close()

    def _error_string(self) -> str:
        try:
            message = self._serial.errorString()
        except Exception:
            message = "未知错误"
        return message or "未知错误"
