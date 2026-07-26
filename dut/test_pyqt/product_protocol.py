"""Encoding and decoding for product-protocol data segments.

This module intentionally knows nothing about ``55 AA``, the physical length
byte, or CRC.  ``SerialChannel`` remains the only physical-frame owner.
"""

import math
import re
import struct
from dataclasses import dataclass
from typing import Dict, Mapping, Optional, Sequence, Tuple

from .protocol_catalog import MessageDefinition, ProtocolCatalog, ProtocolField


_ARRAY_INDEX_PATTERN = re.compile(r"\[([0-9]+)\]")


class ProductProtocolError(ValueError):
    """Raised for malformed fields or product data segments."""


@dataclass(frozen=True)
class OutboundMessage:
    name: str
    sequence: int
    payload: bytes
    values: Mapping[str, object]


@dataclass(frozen=True)
class DecodedMessage:
    name: str
    sequence: int
    type_group: int
    sub_type: int
    values: Mapping[str, object]
    payload: bytes

    @property
    def status(self) -> int:
        return int(self.values.get("status", 0))

    @property
    def err_code(self) -> int:
        return int(self.values.get("err_code", 0))


def _indexed_value(values: Mapping[str, object], field_name: str):
    if field_name in values:
        return True, values[field_name]
    first_bracket = field_name.find("[")
    if first_bracket < 0:
        return False, None
    base_name = field_name[:first_bracket]
    if base_name not in values:
        return False, None
    current = values[base_name]
    try:
        for match in _ARRAY_INDEX_PATTERN.finditer(field_name[first_bracket:]):
            current = current[int(match.group(1))]  # type: ignore[index]
    except (IndexError, KeyError, TypeError):
        raise ProductProtocolError("字段 {} 的数组值不完整".format(field_name))
    return True, current


def _field_value(field: ProtocolField, values: Mapping[str, object]):
    found, value = _indexed_value(values, field.name)
    if found:
        return value
    if field.default is not None:
        return field.default
    return 0


def _integer_raw(field: ProtocolField, value: object, signed: bool) -> int:
    try:
        scaled = float(value) / field.lsb
    except (TypeError, ValueError, ZeroDivisionError) as exc:
        raise ProductProtocolError("字段 {} 不是有效数值".format(field.name)) from exc
    raw = int(round(scaled))
    bits = (field.bit_length or field.byte_length * 8)
    minimum = -(1 << (bits - 1)) if signed else 0
    maximum = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
    if not minimum <= raw <= maximum:
        raise ProductProtocolError(
            "字段 {} 超出 {} 位{}整数范围".format(
                field.name, bits, "有符号" if signed else "无符号"
            )
        )
    return raw


def _encode_field(field: ProtocolField, value: object) -> bytes:
    type_name = field.type_name
    if type_name == "F32":
        try:
            scaled = float(value) / field.lsb
        except (OverflowError, TypeError, ValueError) as exc:
            raise ProductProtocolError("字段 {} 不是有效 F32".format(field.name)) from exc
        if not math.isfinite(scaled):
            raise ProductProtocolError("字段 {} 的 F32 必须是有限值".format(field.name))
        try:
            return struct.pack("<f", scaled)
        except OverflowError as exc:
            raise ProductProtocolError("字段 {} 不是有效 F32".format(field.name)) from exc
    if type_name in ("S16", "S16F", "S32F"):
        raw = _integer_raw(field, value, signed=True)
        return raw.to_bytes(field.byte_length, "little", signed=True)
    if type_name in ("CONST", "U8", "U16", "U32"):
        raw = _integer_raw(field, value, signed=False)
        return raw.to_bytes(field.byte_length, "little", signed=False)
    raise ProductProtocolError("字段 {} 的类型 {} 不能直接编码".format(field.name, type_name))


def _decoded_scaled(raw: int, lsb: float):
    scaled = raw * lsb
    if float(lsb).is_integer() and float(scaled).is_integer():
        return int(scaled)
    return scaled


def _decode_field(field: ProtocolField, data: bytes):
    if field.type_name == "F32":
        if len(data) != 4:
            raise ProductProtocolError("字段 {} 的 F32 长度不是 4".format(field.name))
        value = struct.unpack("<f", data)[0] * field.lsb
        if not math.isfinite(value):
            raise ProductProtocolError("字段 {} 的 F32 必须是有限值".format(field.name))
        return value
    signed = field.type_name in ("S16", "S16F", "S32F")
    raw = int.from_bytes(data, "little", signed=signed)
    return _decoded_scaled(raw, field.lsb)


def encode_payload(
    definition: MessageDefinition,
    values: Optional[Mapping[str, object]] = None,
    sequence: int = 0,
) -> bytes:
    """Encode exactly one 48/123-byte B4..CRC-predecessor data segment."""

    if definition.payload_length not in (48, 123):
        raise ProductProtocolError("产品数据段长度必须是 48 或 123 字节")
    if not 0 <= int(sequence) <= 0xFFFF:
        raise ProductProtocolError("序号必须在 0..65535 范围内")
    supplied = values or {}
    payload = bytearray(definition.payload_length)
    for field in definition.payload_fields:
        offset = field.payload_offset
        if field.type_name == "RESERVED":
            payload[offset : offset + field.byte_length] = bytes(field.byte_length)
            continue
        value = int(sequence) if field.name == "seq" else _field_value(field, supplied)
        if field.type_name == "CONST":
            value = field.default
        if field.type_name == "BIT":
            raw = _integer_raw(field, value, signed=False)
            bit_offset = int(field.bit_offset or 0)
            bit_length = int(field.bit_length or 0)
            mask = ((1 << bit_length) - 1) << bit_offset
            payload[offset] = (payload[offset] & ~mask) | ((raw << bit_offset) & mask)
            continue
        encoded = _encode_field(field, value)
        if len(encoded) != field.byte_length:
            raise ProductProtocolError("字段 {} 编码长度错误".format(field.name))
        payload[offset : offset + field.byte_length] = encoded
    return bytes(payload)


def decode_payload(definition: MessageDefinition, payload: bytes) -> Dict[str, object]:
    """Decode one segment using an already selected CSV definition."""

    data = bytes(payload)
    if len(data) != definition.payload_length:
        raise ProductProtocolError(
            "{} 数据段长度应为 {}，实际为 {}".format(
                definition.name, definition.payload_length, len(data)
            )
        )
    values: Dict[str, object] = {}
    for field in definition.payload_fields:
        if field.type_name == "RESERVED":
            offset = field.payload_offset
            chunk = data[offset : offset + field.byte_length]
            if any(chunk):
                raise ProductProtocolError(
                    "{} 字段 {} 的 RESERVED 字节必须全零".format(
                        definition.name, field.name
                    )
                )
            continue
        offset = field.payload_offset
        if field.type_name == "BIT":
            bit_offset = int(field.bit_offset or 0)
            bit_length = int(field.bit_length or 0)
            raw = (data[offset] >> bit_offset) & ((1 << bit_length) - 1)
            value = _decoded_scaled(raw, field.lsb)
        else:
            chunk = data[offset : offset + field.byte_length]
            value = _decode_field(field, chunk)
        if field.type_name == "CONST" and value != field.default:
            raise ProductProtocolError(
                "{} 字段 {} 常量错误：期望 {}，实际 {}".format(
                    definition.name, field.name, field.default, value
                )
            )
        values[field.name] = value

    if int(values.get("type_group", -1)) != definition.type_group:
        raise ProductProtocolError("{} type_group 不匹配".format(definition.name))
    if int(values.get("sub_type", -1)) != definition.sub_type:
        raise ProductProtocolError("{} sub_type 不匹配".format(definition.name))
    return values


class ProductProtocol:
    """Stateful PC endpoint with an independent transmit sequence."""

    def __init__(
        self,
        catalog: Optional[ProtocolCatalog] = None,
        initial_sequence: int = 0,
    ) -> None:
        if not 0 <= int(initial_sequence) <= 0xFFFF:
            raise ValueError("初始序号必须在 0..65535 范围内")
        self.catalog = catalog or ProtocolCatalog.load_default()
        self._next_sequence = int(initial_sequence)

    @property
    def next_sequence(self) -> int:
        return self._next_sequence

    def build_request(
        self, name: str, values: Optional[Mapping[str, object]] = None
    ) -> OutboundMessage:
        definition = self.catalog.get(name)
        if definition.direction != "request":
            raise ProductProtocolError("{} 不是请求定义".format(name))
        sequence = self._next_sequence
        payload = encode_payload(definition, values, sequence)
        self._next_sequence = (self._next_sequence + 1) & 0xFFFF
        decoded_values = decode_payload(definition, payload)
        return OutboundMessage(
            name=name,
            sequence=sequence,
            payload=payload,
            values=decoded_values,
        )

    def decode_response(self, payload: bytes) -> DecodedMessage:
        data = bytes(payload)
        if len(data) not in (48, 123):
            raise ProductProtocolError(
                "产品数据段长度必须是 48 或 123 字节，实际为 {}".format(len(data))
            )
        type_group = data[1]
        sub_type = data[2]
        definition = self.catalog.response_for(type_group, sub_type)
        if definition is None:
            raise ProductProtocolError(
                "未知响应命令 0x{:02X}/0x{:02X}".format(type_group, sub_type)
            )
        if len(data) != definition.payload_length:
            raise ProductProtocolError(
                "{} 数据段长度应为 {}，实际为 {}".format(
                    definition.name, definition.payload_length, len(data)
                )
            )
        values = decode_payload(definition, data)
        return DecodedMessage(
            name=definition.name,
            sequence=int(values["seq"]),
            type_group=type_group,
            sub_type=sub_type,
            values=values,
            payload=data,
        )


__all__ = [
    "DecodedMessage",
    "OutboundMessage",
    "ProductProtocol",
    "ProductProtocolError",
    "decode_payload",
    "encode_payload",
]
