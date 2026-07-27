#!/usr/bin/env python3
"""Validate product-protocol CSV files and generate C++ descriptors."""

import argparse
import csv
import json
import math
import re
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


SCHEMA = [
    "index",
    "length",
    "type",
    "name_cn",
    "name_en",
    "lsb",
    "default",
    "is_valid",
]
EXPECTED_ASSETS = {
    "bus_echo_test_request.csv",
    "bus_echo_test_response.csv",
    "bus_loop_test_request.csv",
    "bus_loop_test_response.csv",
    "dh_control_request.csv",
    "dh_control_response.csv",
    "dh_pulse_config_request.csv",
    "dh_pulse_config_response.csv",
    "di_read_request.csv",
    "di_read_response.csv",
    "do_write_request.csv",
    "do_write_response.csv",
    "elec_health_status_request.csv",
    "elec_health_status_response.csv",
    "error_response.csv",
    "helm_board_test_request.csv",
    "helm_board_test_response.csv",
    "helm_feedback_response.csv",
    "helm_start_request.csv",
    "helm_start_response.csv",
    "helm_stop_request.csv",
    "helm_stop_response.csv",
    "imu_stream_feedback_response.csv",
    "imu_stream_start_request.csv",
    "imu_stream_start_response.csv",
    "imu_stream_stop_request.csv",
    "imu_stream_stop_response.csv",
    "memperf_test_request.csv",
    "memperf_test_response.csv",
    "spi_flash_test_request.csv",
    "spi_flash_test_response.csv",
    "system_status_request.csv",
    "system_status_response.csv",
    "timer_jitter_start_request.csv",
    "timer_jitter_start_response.csv",
    "timer_jitter_stop_request.csv",
    "timer_jitter_stop_response.csv",
}


class ProtocolValidationError(ValueError):
    """Raised when a protocol CSV violates the schema contract."""


INDEX_PATTERN = re.compile(r"^B([1-9][0-9]*)(?:-([1-9][0-9]*))?$")
FIXED_BYTE_LENGTHS = {
    "U8": 1,
    "U16": 2,
    "U32": 4,
    "S16": 2,
    "S16F": 2,
    "S32F": 4,
    "F32": 4,
}
ALLOWED_TYPES = set(FIXED_BYTE_LENGTHS) | {"BIT", "CONST", "RESERVED"}
FIXED_POINT_TYPES = {"S16F", "S32F"}


@dataclass(frozen=True)
class Field:
    index: str
    start: int
    end: int
    length: int
    field_type: str
    name_cn: str
    name_en: str
    lsb: Optional[float]
    default: Optional[str]
    is_valid: bool
    bit_offset: int = 0

    @property
    def frame_offset(self) -> int:
        return self.start - 1

    @property
    def byte_length(self) -> int:
        return 1 if self.field_type == "BIT" else self.length


@dataclass(frozen=True)
class Message:
    name: str
    role: str
    data_length: int
    type_group: int
    sub_type: int
    fields: Tuple[Field, ...]


def load_protocol_files(protocol_dir: Path, require_complete_catalog: bool) -> List[Path]:
    if not protocol_dir.is_dir():
        raise ProtocolValidationError("协议目录不存在: {}".format(protocol_dir))
    files = sorted(protocol_dir.glob("*.csv"), key=lambda path: path.name)
    if not files:
        raise ProtocolValidationError("协议目录中没有 CSV 文件: {}".format(protocol_dir))
    if require_complete_catalog:
        actual = {path.name for path in files}
        missing = sorted(EXPECTED_ASSETS - actual)
        extra = sorted(actual - EXPECTED_ASSETS)
        if missing or extra:
            details = []
            if missing:
                details.append("缺少: {}".format(", ".join(missing)))
            if extra:
                details.append("多余: {}".format(", ".join(extra)))
            raise ProtocolValidationError(
                "协议资产集合必须严格包含 37 份计划文件；{}".format("；".join(details))
            )
    return files


def read_csv(path: Path) -> Tuple[List[str], List[Dict[str, str]]]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        header = reader.fieldnames or []
        rows = list(reader)
    return header, rows


def validate_schema(path: Path, header: List[str]) -> None:
    if header != SCHEMA:
        raise ProtocolValidationError(
            "{}: 列名必须严格为 {}，实际为 {}".format(
                path.name, ",".join(SCHEMA), ",".join(header or [])
            )
        )


def parse_int(value: str, path: Path, row_number: int, field_name: str) -> int:
    try:
        return int(value, 0)
    except ValueError:
        raise ProtocolValidationError(
            "{}:{}: {} 必须是整数，实际为 {!r}".format(
                path.name, row_number, field_name, value
            )
        )


def parse_index(value: str, path: Path, row_number: int) -> Tuple[int, int]:
    match = INDEX_PATTERN.fullmatch(value)
    if match is None:
        raise ProtocolValidationError(
            "{}:{}: index 格式无效: {!r}".format(path.name, row_number, value)
        )
    start = int(match.group(1))
    end = int(match.group(2) or match.group(1))
    if end < start:
        raise ProtocolValidationError(
            "{}:{}: index 结束位置早于开始位置: {!r}".format(
                path.name, row_number, value
            )
        )
    return start, end


def parse_optional_lsb(value: str, path: Path, row_number: int) -> Optional[float]:
    if not value:
        return None
    try:
        result = float(value)
    except ValueError:
        raise ProtocolValidationError(
            "{}:{}: lsb 必须是数值，实际为 {!r}".format(path.name, row_number, value)
        )
    if not math.isfinite(result) or result <= 0.0:
        raise ProtocolValidationError(
            "{}:{}: lsb 必须是大于 0 的有限数值".format(path.name, row_number)
        )
    return result


def parse_field(path: Path, row: Dict[str, str], row_number: int) -> Field:
    values = {key: (row.get(key) or "").strip() for key in SCHEMA}
    start, end = parse_index(values["index"], path, row_number)
    length = parse_int(values["length"], path, row_number, "length")
    if length <= 0:
        raise ProtocolValidationError(
            "{}:{}: length 必须大于 0".format(path.name, row_number)
        )

    field_type = values["type"]
    if field_type not in ALLOWED_TYPES:
        raise ProtocolValidationError(
            "{}:{}: 不支持的字段类型 {!r}".format(path.name, row_number, field_type)
        )
    if not values["name_cn"] or not values["name_en"]:
        raise ProtocolValidationError(
            "{}:{}: name_cn 和 name_en 均不能为空".format(path.name, row_number)
        )
    if values["is_valid"] not in {"0", "1"}:
        raise ProtocolValidationError(
            "{}:{}: is_valid 只能是 0 或 1".format(path.name, row_number)
        )

    if field_type == "BIT":
        if start != end or length > 8:
            raise ProtocolValidationError(
                "{}:{}: BIT 必须使用单字节 index，length 表示 1..8 位".format(
                    path.name, row_number
                )
            )
    else:
        span = end - start + 1
        if span != length:
            raise ProtocolValidationError(
                "{}:{}: index/length 不一致，{} 跨 {} 字节但 length={}".format(
                    path.name, row_number, values["index"], span, length
                )
            )

    expected_length = FIXED_BYTE_LENGTHS.get(field_type)
    if expected_length is not None and length != expected_length:
        raise ProtocolValidationError(
            "{}:{}: {} 字段 length 必须为 {}".format(
                path.name, row_number, field_type, expected_length
            )
        )

    default = values["default"] or None
    if field_type == "CONST":
        if default is None:
            raise ProtocolValidationError(
                "{}:{}: CONST 字段必须提供 default".format(path.name, row_number)
            )
        try:
            const_value = int(default, 0)
        except ValueError:
            raise ProtocolValidationError(
                "{}:{}: CONST default 必须是整数，实际为 {!r}".format(
                    path.name, row_number, default
                )
            )
        const_max = (1 << (length * 8)) - 1
        if not 0 <= const_value <= const_max:
            raise ProtocolValidationError(
                "{}:{}: CONST default {} 超出 {} 字节无符号范围".format(
                    path.name, row_number, default, length
                )
            )
    if field_type == "RESERVED" and default is None:
        default = "0"
    if field_type == "RESERVED" and default not in {"0", "0x0", "0X0"}:
        raise ProtocolValidationError(
            "{}:{}: RESERVED 显式 default 只能为 0".format(
                path.name, row_number
            )
        )

    lsb = parse_optional_lsb(values["lsb"], path, row_number)
    if field_type in FIXED_POINT_TYPES and lsb is None:
        lsb = 1.0

    return Field(
        index=values["index"],
        start=start,
        end=end,
        length=length,
        field_type=field_type,
        name_cn=values["name_cn"],
        name_en=values["name_en"],
        lsb=lsb,
        default=default,
        is_valid=values["is_valid"] == "1",
    )


def validate_field_order(path: Path, fields: List[Field]) -> List[Field]:
    expected_byte = 1
    active_bit_byte: Optional[int] = None
    active_bit_width = 0
    result: List[Field] = []

    def finish_bit_group() -> None:
        nonlocal expected_byte, active_bit_byte, active_bit_width
        if active_bit_byte is None:
            return
        if active_bit_width != 8:
            raise ProtocolValidationError(
                "{}: B{} 的 BIT 字段累计 {} 位，必须恰好填满 8 位".format(
                    path.name, active_bit_byte, active_bit_width
                )
            )
        expected_byte += 1
        active_bit_byte = None
        active_bit_width = 0

    for field in fields:
        if field.field_type == "BIT":
            if active_bit_byte is None:
                if field.start != expected_byte:
                    raise ProtocolValidationError(
                        "{}: 字段 {} 的 index 不连续，期望 B{}，实际 {}".format(
                            path.name, field.name_en, expected_byte, field.index
                        )
                    )
                active_bit_byte = field.start
            elif field.start != active_bit_byte:
                finish_bit_group()
                if field.start != expected_byte:
                    raise ProtocolValidationError(
                        "{}: BIT index 不连续，期望 B{}，实际 {}".format(
                            path.name, expected_byte, field.index
                        )
                    )
                active_bit_byte = field.start
            if active_bit_width + field.length > 8:
                raise ProtocolValidationError(
                    "{}: B{} 的 BIT 字段超过 8 位".format(path.name, field.start)
                )
            result.append(replace(field, bit_offset=active_bit_width))
            active_bit_width += field.length
            continue

        finish_bit_group()
        if field.start != expected_byte:
            raise ProtocolValidationError(
                "{}: 字段 {} 的 index 不连续，期望 B{}，实际 {}".format(
                    path.name, field.name_en, expected_byte, field.index
                )
            )
        result.append(field)
        expected_byte = field.end + 1

    finish_bit_group()
    return result


def require_field(
    path: Path,
    fields: List[Field],
    position: int,
    name: str,
    index: str,
    field_type: str,
) -> Field:
    if position >= len(fields):
        raise ProtocolValidationError("{}: 缺少公共字段 {}".format(path.name, name))
    field = fields[position]
    if field.name_en != name or field.index != index or field.field_type != field_type:
        raise ProtocolValidationError(
            "{}: 公共字段应为 {} {} {}，实际为 {} {} {}".format(
                path.name,
                index,
                field_type,
                name,
                field.index,
                field.field_type,
                field.name_en,
            )
        )
    return field


def validate_message(path: Path) -> Message:
    header, rows = read_csv(path)
    validate_schema(path, header)
    if not rows:
        raise ProtocolValidationError("{}: CSV 不得为空".format(path.name))
    fields = [parse_field(path, row, number) for number, row in enumerate(rows, 2)]

    names = [field.name_en for field in fields]
    if len(names) != len(set(names)):
        raise ProtocolValidationError("{}: name_en 必须唯一".format(path.name))
    fields = validate_field_order(path, fields)

    sync0 = require_field(path, fields, 0, "sync[0]", "B1", "CONST")
    sync1 = require_field(path, fields, 1, "sync[1]", "B2", "CONST")
    length_field = require_field(path, fields, 2, "len", "B3", "U8")
    version = require_field(path, fields, 3, "version", "B4", "CONST")
    type_group_field = require_field(path, fields, 4, "type_group", "B5", "U8")
    sub_type_field = require_field(path, fields, 5, "sub_type", "B6", "U8")
    require_field(path, fields, 6, "seq", "B7-8", "U16")
    crc = fields[-1]
    if crc.name_en != "crc" or crc.field_type != "U16":
        raise ProtocolValidationError("{}: 最后一个字段必须是 U16 crc".format(path.name))

    if sync0.default != "0x55" or sync1.default != "0xAA":
        raise ProtocolValidationError("{}: 同步字必须固定为 0x55 0xAA".format(path.name))
    if version.default != "0x11":
        raise ProtocolValidationError("{}: version 必须固定为 0x11".format(path.name))

    data_length = parse_int(length_field.default or "", path, 4, "len.default")
    if data_length not in {48, 123}:
        raise ProtocolValidationError(
            "{}: len 只能是 48 或 123，实际为 {}".format(path.name, data_length)
        )
    expected_frame_length = data_length + 5
    if crc.end != expected_frame_length:
        raise ProtocolValidationError(
            "{}: 完整帧长度必须等于 len + 5，实际 {}，期望 {}".format(
                path.name, crc.end, expected_frame_length
            )
        )
    expected_crc_index = "B{}-{}".format(data_length + 4, data_length + 5)
    if crc.index != expected_crc_index:
        raise ProtocolValidationError(
            "{}: crc index 应为 {}，实际为 {}".format(
                path.name, expected_crc_index, crc.index
            )
        )

    type_group = parse_int(
        type_group_field.default or "", path, 6, "type_group.default"
    )
    sub_type = parse_int(sub_type_field.default or "", path, 7, "sub_type.default")
    if not 0 <= type_group <= 0xFF or not 0 <= sub_type <= 0xFF:
        raise ProtocolValidationError("{}: type_group/sub_type 必须为 U8".format(path.name))

    if path.stem.endswith("_request"):
        role = "Request"
    elif path.stem.endswith("_response"):
        role = "Response"
    else:
        raise ProtocolValidationError(
            "{}: 文件名必须以 _request 或 _response 结尾".format(path.name)
        )

    return Message(
        name=path.stem,
        role=role,
        data_length=data_length,
        type_group=type_group,
        sub_type=sub_type,
        fields=tuple(fields),
    )


def validate_directory(
    protocol_dir: Path, require_complete_catalog: bool = True
) -> List[Message]:
    files = load_protocol_files(protocol_dir, require_complete_catalog)
    messages = [validate_message(path) for path in files]
    seen = set()
    for message in messages:
        key = (message.role, message.type_group, message.sub_type)
        if key in seen:
            raise ProtocolValidationError(
                "消息方向/type_group/sub_type 重复: {} 0x{:02X}/0x{:02X}".format(
                    message.role, message.type_group, message.sub_type
                )
            )
        seen.add(key)
    return messages


CPP_FIELD_TYPES = {
    "BIT": "Bit",
    "CONST": "Const",
    "F32": "F32",
    "RESERVED": "Reserved",
    "S16": "S16",
    "S16F": "S16Fixed",
    "S32F": "S32Fixed",
    "U8": "U8",
    "U16": "U16",
    "U32": "U32",
}


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def cpp_identifier(name: str) -> str:
    parts = [part for part in re.split(r"[^A-Za-z0-9]+", name) if part]
    return "".join(part[:1].upper() + part[1:] for part in parts)


def field_data_offset(field: Field, data_length: int) -> int:
    data_end = data_length + 3
    if field.start >= 4 and field.end <= data_end:
        return field.start - 4
    return -1


def render_field(field: Field, data_length: int) -> str:
    lsb = repr(field.lsb) if field.lsb is not None else "0.0"
    has_lsb = "true" if field.lsb is not None else "false"
    if field.field_type == "RESERVED":
        default = cpp_string("0")
    elif field.default is None:
        default = "nullptr"
    else:
        default = cpp_string(field.default)
    is_valid = "true" if field.is_valid else "false"
    bit_length = field.length if field.field_type == "BIT" else 0
    return (
        "    {{{}, {}, {}, {}, {}, {}, FieldType::{}, {}, {}, {}, {}}},".format(
            cpp_string(field.name_en),
            field.frame_offset,
            field_data_offset(field, data_length),
            field.byte_length,
            field.bit_offset,
            bit_length,
            CPP_FIELD_TYPES[field.field_type],
            lsb,
            has_lsb,
            default,
            is_valid,
        )
    )


def render_cpp_header(messages: List[Message]) -> str:
    lines = [
        "#pragma once",
        "",
        "// Generated from product protocol CSV files. Do not edit.",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace MB_DDF {",
        "namespace HWTest {",
        "namespace GeneratedProductProtocol {",
        "",
        "enum class FieldType : std::uint8_t {",
        "    Bit,",
        "    Const,",
        "    F32,",
        "    Reserved,",
        "    S16,",
        "    S16Fixed,",
        "    S32Fixed,",
        "    U8,",
        "    U16,",
        "    U32,",
        "};",
        "",
        "enum class MessageRole : std::uint8_t {",
        "    Request,",
        "    Response,",
        "};",
        "",
        "struct FieldDescriptor {",
        "    const char* name;",
        "    std::uint16_t frame_offset;",
        "    std::int16_t data_offset;",
        "    std::uint16_t byte_length;",
        "    std::uint8_t bit_offset;",
        "    std::uint8_t bit_length;",
        "    FieldType type;",
        "    double lsb;",
        "    bool has_lsb;",
        "    const char* default_value;",
        "    bool is_valid;",
        "};",
        "",
        "struct MessageDescriptor {",
        "    const char* name;",
        "    MessageRole role;",
        "    std::uint8_t data_length;",
        "    std::uint8_t type_group;",
        "    std::uint8_t sub_type;",
        "    const FieldDescriptor* fields;",
        "    std::size_t field_count;",
        "};",
        "",
    ]

    array_names = []
    for message in messages:
        array_name = "k{}Fields".format(cpp_identifier(message.name))
        array_names.append(array_name)
        lines.append("inline constexpr FieldDescriptor {}[] = {{".format(array_name))
        lines.extend(render_field(field, message.data_length) for field in message.fields)
        lines.extend(["};", ""])

    lines.append("inline constexpr MessageDescriptor kMessages[] = {")
    for message, array_name in zip(messages, array_names):
        lines.append(
            "    {{{}, MessageRole::{}, {}, 0x{:02X}, 0x{:02X}, {}, "
            "sizeof({}) / sizeof({}[0])}},".format(
                cpp_string(message.name),
                message.role,
                message.data_length,
                message.type_group,
                message.sub_type,
                array_name,
                array_name,
                array_name,
            )
        )
    lines.extend(
        [
            "};",
            "",
            "inline constexpr std::size_t kMessageCount =",
            "    sizeof(kMessages) / sizeof(kMessages[0]);",
            "",
            "inline constexpr const MessageDescriptor* find_message(std::string_view name) noexcept {",
            "    for (const auto& message : kMessages) {",
            "        if (name == message.name) {",
            "            return &message;",
            "        }",
            "    }",
            "    return nullptr;",
            "}",
            "",
            "}  // namespace GeneratedProductProtocol",
            "}  // namespace HWTest",
            "}  // namespace MB_DDF",
            "",
        ]
    )
    return "\n".join(lines)


def write_cpp_header(output: Path, messages: List[Message]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(render_cpp_header(messages))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="只校验 CSV")
    parser.add_argument("--output", type=Path, help="生成 C++ 描述头文件")
    parser.add_argument("--allow-partial", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("protocol_dir", type=Path, help="产品协议 CSV 目录")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.check and args.output is not None:
        parser.error("--check 与 --output 不能同时使用")
    if not args.check and args.output is None:
        parser.error("必须指定 --check 或 --output")

    try:
        messages = validate_directory(
            args.protocol_dir, require_complete_catalog=not args.allow_partial
        )
    except (OSError, ProtocolValidationError) as error:
        print("协议校验失败: {}".format(error), file=sys.stderr)
        return 1

    if args.output is not None:
        try:
            write_cpp_header(args.output, messages)
        except OSError as error:
            print("头文件生成失败: {}".format(error), file=sys.stderr)
            return 1
        print("已生成 C++ 协议描述: {} 份 CSV -> {}".format(len(messages), args.output))
        return 0

    print(
        "协议校验通过: {} 份 CSV；len=48/123；帧长=53/128".format(len(messages))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
