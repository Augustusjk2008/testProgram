"""Encoding and incremental decoding for the MB_DDF COM wire protocol."""

from dataclasses import dataclass
from enum import Enum
import time
from typing import Callable, Iterable, List, Optional

from .app_config import ProtocolConfig


class ProtocolErrorCode(str, Enum):
    INVALID_LENGTH = "invalid_length"
    CRC_MISMATCH = "crc_mismatch"
    FRAME_TIMEOUT = "frame_timeout"


class FrameEventType(str, Enum):
    FRAME = "frame"
    ERROR = "error"


@dataclass(frozen=True)
class FrameEvent:
    kind: FrameEventType
    payload: bytes = b""
    frame: bytes = b""
    error_code: Optional[ProtocolErrorCode] = None
    message: str = ""

    @classmethod
    def decoded(cls, payload: bytes, frame: bytes) -> "FrameEvent":
        return cls(kind=FrameEventType.FRAME, payload=payload, frame=frame)

    @classmethod
    def error(
        cls, code: ProtocolErrorCode, message: str, frame: bytes
    ) -> "FrameEvent":
        return cls(
            kind=FrameEventType.ERROR,
            frame=frame,
            error_code=code,
            message=message,
        )


def crc16_xmodem(
    data: Iterable[int],
    polynomial: int = 0x1021,
    initial: int = 0x0000,
    xor_out: int = 0x0000,
) -> int:
    """Return CRC-16/XMODEM (non-reflected) for *data*."""

    if not 0 <= polynomial <= 0xFFFF:
        raise ValueError("CRC 多项式必须是 16 位无符号整数")
    if not 0 <= initial <= 0xFFFF:
        raise ValueError("CRC 初值必须是 16 位无符号整数")
    if not 0 <= xor_out <= 0xFFFF:
        raise ValueError("CRC 异或输出必须是 16 位无符号整数")
    crc = initial & 0xFFFF
    for value in data:
        byte = int(value)
        if not 0 <= byte <= 0xFF:
            raise ValueError("CRC 输入必须由字节组成")
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ polynomial) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return (crc ^ xor_out) & 0xFFFF


def encode_frame(
    payload: bytes, config: Optional[ProtocolConfig] = None
) -> bytes:
    """Encode one payload, rejecting lengths not representable by the protocol."""

    protocol = config or ProtocolConfig()
    data = bytes(payload)
    if not protocol.minimum_payload_length <= len(data) <= protocol.maximum_payload_length:
        raise ValueError(
            "有效数据长度必须在 {}..{} 字节范围内".format(
                protocol.minimum_payload_length, protocol.maximum_payload_length
            )
        )

    length_and_payload = bytes((len(data),)) + data
    crc = crc16_xmodem(
        length_and_payload,
        polynomial=protocol.crc_polynomial,
        initial=protocol.crc_initial,
        xor_out=protocol.crc_xor_out,
    )
    byte_order = "little" if protocol.crc_little_endian else "big"
    return protocol.header + length_and_payload + crc.to_bytes(2, byte_order)


class FrameDecoder:
    """Incremental byte-state decoder suitable for ``readyRead`` chunks."""

    _WAIT_HEADER_1 = 0
    _WAIT_HEADER_2 = 1
    _WAIT_LENGTH = 2
    _WAIT_PAYLOAD = 3
    _WAIT_CRC_1 = 4
    _WAIT_CRC_2 = 5

    def __init__(
        self,
        config: Optional[ProtocolConfig] = None,
        clock_ms: Optional[Callable[[], float]] = None,
    ) -> None:
        self.config = config or ProtocolConfig()
        self._clock_ms = clock_ms or (lambda: time.monotonic() * 1000.0)
        self.reset()

    @property
    def has_partial_frame(self) -> bool:
        return self._state != self._WAIT_HEADER_1

    def reset(self) -> None:
        self._state = self._WAIT_HEADER_1
        self._frame = bytearray()
        self._payload = bytearray()
        self._expected_length = 0
        self._received_crc_low = 0
        self._started_at_ms: Optional[float] = None

    def check_timeout(self, now_ms: Optional[float] = None) -> List[FrameEvent]:
        """Expire an incomplete frame and return one protocol-error event."""

        now = self._clock_ms() if now_ms is None else now_ms
        if (
            self.has_partial_frame
            and self._started_at_ms is not None
            and now - self._started_at_ms >= self.config.frame_timeout_ms
        ):
            partial = bytes(self._frame)
            self.reset()
            return [
                FrameEvent.error(
                    ProtocolErrorCode.FRAME_TIMEOUT,
                    "半帧接收超过 {} ms".format(self.config.frame_timeout_ms),
                    partial,
                )
            ]
        return []

    def feed(
        self, chunk: bytes, now_ms: Optional[float] = None
    ) -> List[FrameEvent]:
        """Consume one arbitrary-size chunk and return decoded/error events."""

        now = self._clock_ms() if now_ms is None else now_ms
        events = self.check_timeout(now)
        for value in bytes(chunk):
            events.extend(self._feed_byte(value, now))
        return events

    def feed_byte(
        self, value: int, now_ms: Optional[float] = None
    ) -> List[FrameEvent]:
        """Consume one byte; exposed for serial-channel integrations and tests."""

        if not 0 <= value <= 0xFF:
            raise ValueError("输入必须是单个字节")
        now = self._clock_ms() if now_ms is None else now_ms
        events = self.check_timeout(now)
        events.extend(self._feed_byte(value, now))
        return events

    def _start_header(self, now_ms: float) -> None:
        self._state = self._WAIT_HEADER_2
        self._frame = bytearray((self.config.header[0],))
        self._payload = bytearray()
        self._expected_length = 0
        self._started_at_ms = now_ms

    def _reset_with_overlap(self, value: int, now_ms: float) -> None:
        self.reset()
        if value == self.config.header[0]:
            self._start_header(now_ms)

    def _feed_byte(self, value: int, now_ms: float) -> List[FrameEvent]:
        if self._state == self._WAIT_HEADER_1:
            if value == self.config.header[0]:
                self._start_header(now_ms)
            return []

        if self._state == self._WAIT_HEADER_2:
            if value == self.config.header[1]:
                self._frame.append(value)
                self._state = self._WAIT_LENGTH
            elif value == self.config.header[0]:
                # Keep a possible overlapping first header byte (55 55 AA).
                self._start_header(now_ms)
            else:
                self.reset()
            return []

        if self._state == self._WAIT_LENGTH:
            self._frame.append(value)
            if not (
                self.config.minimum_payload_length
                <= value
                <= self.config.maximum_payload_length
            ):
                candidate = bytes(self._frame)
                self._reset_with_overlap(value, now_ms)
                return [
                    FrameEvent.error(
                        ProtocolErrorCode.INVALID_LENGTH,
                        "无效的有效数据长度：{}".format(value),
                        candidate,
                    )
                ]
            self._expected_length = value
            self._state = self._WAIT_PAYLOAD
            return []

        if self._state == self._WAIT_PAYLOAD:
            self._frame.append(value)
            self._payload.append(value)
            if len(self._payload) == self._expected_length:
                self._state = self._WAIT_CRC_1
            return []

        if self._state == self._WAIT_CRC_1:
            self._frame.append(value)
            self._received_crc_low = value
            self._state = self._WAIT_CRC_2
            return []

        self._frame.append(value)
        if self.config.crc_little_endian:
            received_crc = self._received_crc_low | (value << 8)
        else:
            received_crc = (self._received_crc_low << 8) | value
        crc_input = bytes((self._expected_length,)) + bytes(self._payload)
        expected_crc = crc16_xmodem(
            crc_input,
            polynomial=self.config.crc_polynomial,
            initial=self.config.crc_initial,
            xor_out=self.config.crc_xor_out,
        )
        frame = bytes(self._frame)
        payload = bytes(self._payload)
        if received_crc == expected_crc:
            self.reset()
            return [FrameEvent.decoded(payload, frame)]

        self._reset_with_overlap(value, now_ms)
        return [
            FrameEvent.error(
                ProtocolErrorCode.CRC_MISMATCH,
                "CRC 错误：收到 0x{:04X}，期望 0x{:04X}".format(
                    received_crc, expected_crc
                ),
                frame,
            )
        ]
