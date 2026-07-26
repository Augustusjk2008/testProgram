from dataclasses import FrozenInstanceError

import pytest

from test_pyqt.app_config import ProtocolConfig, SerialPortConfig
from test_pyqt.serial_protocol import (
    FrameDecoder,
    FrameEventType,
    ProtocolErrorCode,
    crc16_xmodem,
    encode_frame,
)


KNOWN_PAYLOAD = bytes.fromhex("4D 42 31")
KNOWN_FRAME = bytes.fromhex("55 AA 03 4D 42 31 FC 89")


def test_crc16_xmodem_known_vector() -> None:
    assert crc16_xmodem(bytes.fromhex("03 4D 42 31")) == 0x89FC


def test_encode_frame_known_vector_and_crc_byte_order() -> None:
    assert encode_frame(KNOWN_PAYLOAD) == KNOWN_FRAME


def test_payload_length_boundaries() -> None:
    with pytest.raises(ValueError):
        encode_frame(b"")

    payload = bytes(range(255))
    frame = encode_frame(payload)
    assert frame[2] == 255
    assert len(frame) == 260

    with pytest.raises(ValueError):
        encode_frame(bytes(256))


def test_config_objects_are_immutable_and_not_shared() -> None:
    first = SerialPortConfig()
    second = SerialPortConfig()
    assert first.protocol is not second.protocol
    assert not first.protocol.crc_reflect_input
    assert not first.protocol.crc_reflect_output
    with pytest.raises(FrozenInstanceError):
        first.baud_rate = 115200  # type: ignore[misc]


def test_decoder_handles_frame_split_at_every_byte() -> None:
    decoder = FrameDecoder()
    events = []
    for value in KNOWN_FRAME:
        events.extend(decoder.feed(bytes((value,)), now_ms=10))

    assert len(events) == 1
    assert events[0].kind == FrameEventType.FRAME
    assert events[0].payload == KNOWN_PAYLOAD
    assert events[0].frame == KNOWN_FRAME


def test_decoder_handles_sticky_frames() -> None:
    second_payload = bytes.fromhex("55 AA 00 FF")
    events = FrameDecoder().feed(KNOWN_FRAME + encode_frame(second_payload), now_ms=0)

    assert [event.payload for event in events] == [KNOWN_PAYLOAD, second_payload]


def test_decoder_skips_noise_and_preserves_overlapping_header() -> None:
    stream = bytes.fromhex("00 FF 55 01 55 55") + KNOWN_FRAME[1:]
    events = FrameDecoder().feed(stream, now_ms=0)

    frames = [event for event in events if event.kind == FrameEventType.FRAME]
    assert len(frames) == 1
    assert frames[0].payload == KNOWN_PAYLOAD


def test_invalid_zero_length_is_an_error_and_not_a_frame() -> None:
    events = FrameDecoder().feed(bytes.fromhex("55 AA 00"), now_ms=0)

    assert len(events) == 1
    assert events[0].kind == FrameEventType.ERROR
    assert events[0].error_code == ProtocolErrorCode.INVALID_LENGTH


def test_crc_error_is_not_reported_as_valid_payload() -> None:
    corrupted = KNOWN_FRAME[:-1] + bytes((KNOWN_FRAME[-1] ^ 0x01,))
    events = FrameDecoder().feed(corrupted, now_ms=0)

    assert len(events) == 1
    assert events[0].kind == FrameEventType.ERROR
    assert events[0].error_code == ProtocolErrorCode.CRC_MISMATCH
    assert events[0].payload == b""
    assert events[0].frame == corrupted


def test_half_frame_times_out_at_500_ms_and_decoder_recovers() -> None:
    decoder = FrameDecoder()
    assert decoder.feed(KNOWN_FRAME[:5], now_ms=100) == []
    assert decoder.check_timeout(now_ms=599) == []

    events = decoder.check_timeout(now_ms=600)
    assert len(events) == 1
    assert events[0].error_code == ProtocolErrorCode.FRAME_TIMEOUT
    assert events[0].frame == KNOWN_FRAME[:5]

    recovered = decoder.feed(KNOWN_FRAME, now_ms=601)
    assert [event.payload for event in recovered] == [KNOWN_PAYLOAD]


def test_decoder_recovers_after_crc_error_in_same_chunk() -> None:
    corrupted = bytearray(KNOWN_FRAME)
    corrupted[-2] ^= 0x80
    events = FrameDecoder().feed(bytes(corrupted) + KNOWN_FRAME, now_ms=0)

    assert [event.kind for event in events] == [
        FrameEventType.ERROR,
        FrameEventType.FRAME,
    ]
    assert events[0].error_code == ProtocolErrorCode.CRC_MISMATCH
    assert events[1].payload == KNOWN_PAYLOAD


def test_custom_protocol_config_is_used_by_encoder_and_decoder() -> None:
    config = ProtocolConfig(header=b"\xA5\x5A", frame_timeout_ms=25)
    frame = encode_frame(b"x", config)
    assert frame[:2] == b"\xA5\x5A"
    events = FrameDecoder(config).feed(frame, now_ms=0)
    assert [event.payload for event in events] == [b"x"]
