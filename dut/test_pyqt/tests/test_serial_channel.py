from collections import deque

from PyQt5.QtCore import QObject, QTimer, pyqtSignal
from PyQt5.QtSerialPort import QSerialPort

from test_pyqt.app_config import ProtocolConfig, SerialPortConfig
from test_pyqt.serial_channel import SerialChannel
from test_pyqt.serial_protocol import ProtocolErrorCode, encode_frame


class FakeSerialPort(QObject):
    readyRead = pyqtSignal()
    bytesWritten = pyqtSignal(int)
    errorOccurred = pyqtSignal(int)

    def __init__(self) -> None:
        super().__init__()
        self.open_result = True
        self.opened = False
        self.error_text = "fake error"
        self.setter_results = {}
        self.settings = {}
        self.read_buffer = bytearray()
        self.write_results = deque()
        self.write_default_result = None
        self.write_calls = []
        self.synchronous_ack = False

    def setPortName(self, value):
        self.settings["port_name"] = value

    def _set(self, name, value):
        self.settings[name] = value
        return self.setter_results.get(name, True)

    def setBaudRate(self, value):
        return self._set("baud_rate", value)

    def setDataBits(self, value):
        return self._set("data_bits", value)

    def setParity(self, value):
        return self._set("parity", value)

    def setStopBits(self, value):
        return self._set("stop_bits", value)

    def setFlowControl(self, value):
        return self._set("flow_control", value)

    def open(self, mode):
        self.settings["open_mode"] = mode
        self.opened = self.open_result
        return self.open_result

    def close(self):
        self.opened = False

    def isOpen(self):
        return self.opened

    def errorString(self):
        return self.error_text

    def readAll(self):
        data = bytes(self.read_buffer)
        self.read_buffer.clear()
        return data

    def write(self, data):
        raw = bytes(data)
        self.write_calls.append(raw)
        if self.write_results:
            result = self.write_results.popleft()
        elif self.write_default_result is not None:
            result = self.write_default_result
        else:
            result = len(raw)
        if self.synchronous_ack and result > 0:
            self.bytesWritten.emit(result)
        return result

    def inject(self, data):
        self.read_buffer.extend(data)
        self.readyRead.emit()

    def ack(self, count):
        self.bytesWritten.emit(count)


def make_open_channel(config=None):
    fake = FakeSerialPort()
    channel = SerialChannel(config or SerialPortConfig(port_name="COM7"), fake)
    assert channel.open()
    return channel, fake


def test_open_applies_614400_8e1_no_flow_mapping(qtbot) -> None:
    fake = FakeSerialPort()
    channel = SerialChannel(SerialPortConfig(port_name="COM7"), fake)
    connected = []
    channel.connected.connect(lambda: connected.append(True))

    assert channel.open()
    assert connected == [True]
    assert fake.settings["port_name"] == "COM7"
    assert fake.settings["baud_rate"] == 614400
    assert fake.settings["data_bits"] == QSerialPort.Data8
    assert fake.settings["parity"] == QSerialPort.EvenParity
    assert fake.settings["stop_bits"] == QSerialPort.OneStop
    assert fake.settings["flow_control"] == QSerialPort.NoFlowControl


def test_open_reports_setter_and_backend_failures(qtbot) -> None:
    fake = FakeSerialPort()
    fake.setter_results["parity"] = False
    channel = SerialChannel(SerialPortConfig(port_name="COM8"), fake)
    errors = []
    channel.io_error.connect(errors.append)
    assert not channel.open()
    assert "设置校验失败" in errors[-1]

    fake.setter_results["parity"] = True
    fake.open_result = False
    assert not channel.open()
    assert "打开串口失败" in errors[-1]


def test_ready_read_decodes_all_available_bytes(qtbot) -> None:
    channel, fake = make_open_channel()
    received = []
    channel.frame_received.connect(lambda payload, frame: received.append((payload, frame)))
    first = encode_frame(b"first")
    second = encode_frame(b"second")

    fake.inject(first[:4])
    assert received == []
    fake.inject(first[4:] + second)

    assert received == [(b"first", first), (b"second", second)]


def test_crc_failure_emits_protocol_error_only(qtbot) -> None:
    channel, fake = make_open_channel()
    received = []
    errors = []
    channel.frame_received.connect(lambda payload, frame: received.append(payload))
    channel.protocol_error.connect(lambda code, message, frame: errors.append((code, frame)))
    broken = bytearray(encode_frame(b"bad"))
    broken[-1] ^= 1

    fake.inject(broken)

    assert received == []
    assert errors == [(ProtocolErrorCode.CRC_MISMATCH.value, bytes(broken))]


def test_send_queue_handles_partial_acceptance_and_bytes_written(qtbot) -> None:
    channel, fake = make_open_channel()
    frame = encode_frame(b"MB1")
    fake.write_results.extend([2, 3, len(frame) - 5])
    completed = []
    channel.send_completed.connect(completed.append)

    assert channel.send_payload(b"MB1")
    assert [len(call) for call in fake.write_calls] == [len(frame), len(frame) - 2, len(frame) - 5]
    assert completed == []

    fake.ack(3)
    assert completed == []
    fake.ack(len(frame) - 3)
    assert completed == [frame]


def test_send_completion_handles_synchronous_fake_backend_ack(qtbot) -> None:
    channel, fake = make_open_channel()
    fake.synchronous_ack = True
    completed = []
    channel.send_completed.connect(completed.append)
    frame = encode_frame(b"sync")

    assert channel.send_payload(b"sync")
    assert completed == [frame]


def test_second_frame_waits_for_first_send_completion(qtbot) -> None:
    channel, fake = make_open_channel()
    first = encode_frame(b"one")
    second = encode_frame(b"two")

    assert channel.send_payload(b"one")
    assert channel.send_payload(b"two")
    assert fake.write_calls == [first]

    fake.ack(len(first))
    assert fake.write_calls == [first, second]


def test_negative_write_result_fails_pending_send(qtbot) -> None:
    channel, fake = make_open_channel()
    fake.write_results.append(-1)
    failed = []
    io_errors = []
    channel.send_failed.connect(lambda frame, message: failed.append((frame, message)))
    channel.io_error.connect(io_errors.append)

    assert not channel.send_payload(b"no")
    assert len(failed) == 1
    assert failed[0][0] == encode_frame(b"no")
    assert "写入失败" in failed[0][1]
    assert io_errors


def test_zero_write_is_retried_without_blocking(qtbot) -> None:
    channel, fake = make_open_channel()
    fake.write_results.extend([0, 8])

    assert channel.send_payload(b"abc")
    qtbot.waitUntil(lambda: len(fake.write_calls) == 2, timeout=200)
    fake.ack(len(encode_frame(b"abc")))


def test_write_watchdog_fails_stalled_queue_and_allows_recovery(qtbot) -> None:
    fake = FakeSerialPort()
    channel = SerialChannel(
        SerialPortConfig(port_name="COM7"), fake, write_timeout_ms=30
    )
    assert channel.open()
    fake.write_default_result = 0
    failed = []
    completed = []
    channel.send_failed.connect(lambda frame, message: failed.append((frame, message)))
    channel.send_completed.connect(completed.append)

    assert channel.send_payload(b"stalled")
    qtbot.waitUntil(lambda: bool(failed), timeout=250)
    assert "发送超过 30 ms" in failed[0][1]

    fake.write_default_result = None
    recovery_frame = encode_frame(b"recovered")
    assert channel.send_payload(b"recovered")
    fake.ack(len(recovery_frame))
    assert completed == [recovery_frame]


def test_write_watchdog_fails_when_bytes_written_never_arrives(qtbot) -> None:
    fake = FakeSerialPort()
    channel = SerialChannel(
        SerialPortConfig(port_name="COM7"), fake, write_timeout_ms=30
    )
    assert channel.open()
    failed = []
    channel.send_failed.connect(lambda frame, message: failed.append(message))

    assert channel.send_payload(b"accepted-without-ack")
    qtbot.waitUntil(lambda: bool(failed), timeout=250)
    assert "发送超过 30 ms" in failed[0]


def test_decoder_timeout_is_emitted_by_channel_timer(qtbot) -> None:
    config = SerialPortConfig(
        port_name="COM7", protocol=ProtocolConfig(frame_timeout_ms=20)
    )
    channel, fake = make_open_channel(config)
    errors = []
    channel.protocol_error.connect(lambda code, message, frame: errors.append(code))

    fake.inject(bytes.fromhex("55 AA 03 4D"))
    qtbot.waitUntil(lambda: bool(errors), timeout=250)
    assert errors == [ProtocolErrorCode.FRAME_TIMEOUT.value]


def test_close_clears_partial_frame_and_pending_send(qtbot) -> None:
    channel, fake = make_open_channel()
    frame = encode_frame(b"reset")
    failed = []
    received = []
    channel.send_failed.connect(lambda wire, message: failed.append((wire, message)))
    channel.frame_received.connect(lambda payload, wire: received.append(payload))

    fake.inject(frame[:4])
    channel.send_payload(b"pending")
    channel.close()

    assert not channel.is_open
    assert failed and failed[0][0] == encode_frame(b"pending")
    assert channel.open()
    fake.inject(frame[4:])
    assert received == []
    fake.inject(frame)
    assert received == [b"reset"]


def test_no_error_notification_is_ignored(qtbot) -> None:
    channel, fake = make_open_channel()
    errors = []
    channel.io_error.connect(errors.append)

    fake.errorOccurred.emit(QSerialPort.NoError)
    assert errors == []


def test_fatal_io_error_disconnects_channel_for_recovery(qtbot) -> None:
    channel, fake = make_open_channel()
    disconnected = []
    channel.disconnected.connect(lambda: disconnected.append(True))
    fake.error_text = "device removed"

    fake.errorOccurred.emit(QSerialPort.ResourceError)

    assert disconnected == [True]
    assert not channel.is_open
