"""Runtime catalog for the CSV-defined product protocol."""

import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple


CSV_COLUMNS = (
    "index",
    "length",
    "type",
    "name_cn",
    "name_en",
    "lsb",
    "default",
    "is_valid",
)

SUPPORTED_FIELD_TYPES = {
    "BIT",
    "CONST",
    "F32",
    "RESERVED",
    "S16",
    "S16F",
    "S32F",
    "U8",
    "U16",
    "U32",
}

_INDEX_PATTERN = re.compile(r"^B([1-9][0-9]*)(?:-([1-9][0-9]*))?$")


class ProtocolCatalogError(ValueError):
    """Raised when the CSV catalog violates its runtime contract."""


def default_catalog_directory() -> Path:
    return (
        Path(__file__).resolve().parent.parent
        / "docs"
        / "design"
        / "product_protocol_csv"
    )


def _parse_number(text: str):
    value = text.strip()
    if not value:
        return None
    try:
        return int(value, 0)
    except ValueError:
        try:
            return float(value)
        except ValueError as exc:
            raise ProtocolCatalogError("无法解析数值 {!r}".format(text)) from exc


@dataclass(frozen=True)
class ProtocolField:
    name: str
    name_cn: str
    type_name: str
    start_byte: int
    end_byte: int
    byte_length: int
    declared_length: int
    lsb: float
    default: object
    is_valid: bool
    bit_offset: Optional[int] = None
    bit_length: Optional[int] = None

    @property
    def payload_offset(self) -> int:
        """Zero-based offset in the B4..CRC-predecessor data segment."""

        return self.start_byte - 4


@dataclass(frozen=True)
class MessageDefinition:
    name: str
    direction: str
    source_path: Path
    fields: Tuple[ProtocolField, ...]
    payload_length: int
    type_group: int
    sub_type: int

    def field(self, name: str) -> ProtocolField:
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError("{} 中不存在字段 {}".format(self.name, name))

    @property
    def payload_fields(self) -> Tuple[ProtocolField, ...]:
        last_payload_byte = self.payload_length + 3
        return tuple(
            field
            for field in self.fields
            if field.is_valid
            and field.start_byte >= 4
            and field.end_byte <= last_payload_byte
        )


class ProtocolCatalog:
    """Validated collection of all request and response CSV definitions."""

    def __init__(self, directory: Optional[Path] = None) -> None:
        self.directory = Path(directory) if directory is not None else default_catalog_directory()
        self._definitions: Dict[str, MessageDefinition] = {}
        self._request_by_command: Dict[Tuple[int, int], MessageDefinition] = {}
        self._response_by_command: Dict[Tuple[int, int], MessageDefinition] = {}
        self._load()

    @classmethod
    def load_default(cls) -> "ProtocolCatalog":
        return cls(default_catalog_directory())

    def __len__(self) -> int:
        return len(self._definitions)

    def __iter__(self) -> Iterator[MessageDefinition]:
        return iter(self._definitions.values())

    @property
    def requests(self) -> Tuple[MessageDefinition, ...]:
        return tuple(
            definition
            for definition in self._definitions.values()
            if definition.direction == "request"
        )

    @property
    def responses(self) -> Tuple[MessageDefinition, ...]:
        return tuple(
            definition
            for definition in self._definitions.values()
            if definition.direction == "response"
        )

    def get(self, name: str) -> MessageDefinition:
        try:
            return self._definitions[name]
        except KeyError as exc:
            raise KeyError("未知协议定义：{}".format(name)) from exc

    def request_for(self, type_group: int, sub_type: int) -> Optional[MessageDefinition]:
        return self._request_by_command.get((int(type_group), int(sub_type)))

    def response_for(self, type_group: int, sub_type: int) -> Optional[MessageDefinition]:
        return self._response_by_command.get((int(type_group), int(sub_type)))

    def _load(self) -> None:
        if not self.directory.is_dir():
            raise ProtocolCatalogError("协议 CSV 目录不存在：{}".format(self.directory))
        csv_paths = sorted(self.directory.glob("*.csv"), key=lambda path: path.name)
        if not csv_paths:
            raise ProtocolCatalogError("协议 CSV 目录中没有 CSV 文件：{}".format(self.directory))

        for path in csv_paths:
            definition = self._load_definition(path)
            if definition.name in self._definitions:
                raise ProtocolCatalogError("协议名称重复：{}".format(definition.name))
            self._definitions[definition.name] = definition
            command = (definition.type_group, definition.sub_type)
            command_map = (
                self._request_by_command
                if definition.direction == "request"
                else self._response_by_command
            )
            if command in command_map:
                raise ProtocolCatalogError(
                    "{} 命令 0x{:02X}/0x{:02X} 重复".format(
                        definition.direction, command[0], command[1]
                    )
                )
            command_map[command] = definition

    def _load_definition(self, path: Path) -> MessageDefinition:
        name = path.stem
        if name.endswith("_request"):
            direction = "request"
        elif name.endswith("_response"):
            direction = "response"
        else:
            raise ProtocolCatalogError("协议文件名必须以 _request 或 _response 结尾：{}".format(path.name))

        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream)
            actual_columns = tuple(reader.fieldnames or ())
            if actual_columns != CSV_COLUMNS:
                raise ProtocolCatalogError(
                    "{} 列名不符合合同：期望 {}，实际 {}".format(
                        path.name, ",".join(CSV_COLUMNS), ",".join(actual_columns)
                    )
                )
            rows = list(reader)
        if not rows:
            raise ProtocolCatalogError("{} 不包含字段".format(path.name))

        fields = self._parse_fields(path, rows)
        by_name = {field.name: field for field in fields}
        required = ("sync[0]", "sync[1]", "len", "version", "type_group", "sub_type", "seq", "crc")
        missing = [field_name for field_name in required if field_name not in by_name]
        if missing:
            raise ProtocolCatalogError(
                "{} 缺少字段：{}".format(path.name, ", ".join(missing))
            )

        payload_length = self._required_int_default(path, by_name["len"])
        if payload_length not in (48, 123):
            raise ProtocolCatalogError(
                "{} 的 len 只能是 48 或 123，实际为 {}".format(path.name, payload_length)
            )
        type_group = self._required_int_default(path, by_name["type_group"])
        sub_type = self._required_int_default(path, by_name["sub_type"])
        full_frame_length = payload_length + 5
        crc = by_name["crc"]
        if crc.start_byte != full_frame_length - 1 or crc.end_byte != full_frame_length:
            raise ProtocolCatalogError(
                "{} 完整帧末尾与 len 不一致：CRC 位于 B{}-{}，预期 B{}-{}".format(
                    path.name,
                    crc.start_byte,
                    crc.end_byte,
                    full_frame_length - 1,
                    full_frame_length,
                )
            )
        self._validate_byte_coverage(path, fields, full_frame_length)
        return MessageDefinition(
            name=name,
            direction=direction,
            source_path=path,
            fields=tuple(fields),
            payload_length=payload_length,
            type_group=type_group,
            sub_type=sub_type,
        )

    def _parse_fields(self, path: Path, rows: Sequence[dict]) -> List[ProtocolField]:
        fields: List[ProtocolField] = []
        names = set()
        bit_usage: Dict[int, int] = {}
        for row_number, row in enumerate(rows, start=2):
            prefix = "{}:{}".format(path.name, row_number)
            index_text = (row.get("index") or "").strip()
            match = _INDEX_PATTERN.fullmatch(index_text)
            if match is None:
                raise ProtocolCatalogError("{} index 无效：{}".format(prefix, index_text))
            start_byte = int(match.group(1))
            explicit_end = int(match.group(2)) if match.group(2) else start_byte
            try:
                declared_length = int((row.get("length") or "").strip())
            except ValueError as exc:
                raise ProtocolCatalogError("{} length 不是整数".format(prefix)) from exc
            if declared_length <= 0:
                raise ProtocolCatalogError("{} length 必须大于 0".format(prefix))

            type_name = (row.get("type") or "").strip().upper()
            if type_name not in SUPPORTED_FIELD_TYPES:
                raise ProtocolCatalogError("{} type 不支持：{}".format(prefix, type_name))
            name = (row.get("name_en") or "").strip()
            if not name:
                raise ProtocolCatalogError("{} name_en 不能为空".format(prefix))
            if name in names:
                raise ProtocolCatalogError("{} name_en 重复：{}".format(prefix, name))
            names.add(name)

            bit_offset = None
            bit_length = None
            if type_name == "BIT":
                if explicit_end != start_byte or declared_length > 8:
                    raise ProtocolCatalogError("{} BIT 的 index/length 无效".format(prefix))
                bit_offset = bit_usage.get(start_byte, 0)
                if bit_offset + declared_length > 8:
                    raise ProtocolCatalogError("{} BIT 超出所在字节".format(prefix))
                bit_usage[start_byte] = bit_offset + declared_length
                byte_length = 1
                end_byte = start_byte
                bit_length = declared_length
            else:
                byte_length = declared_length
                end_byte = explicit_end
                if end_byte - start_byte + 1 != byte_length:
                    raise ProtocolCatalogError(
                        "{} index 范围与 length 不一致".format(prefix)
                    )

            default = _parse_number(row.get("default") or "")
            if type_name == "CONST" and default is None:
                raise ProtocolCatalogError("{} CONST 必须填写 default".format(prefix))
            if type_name == "RESERVED" and default is None:
                default = 0
            lsb_value = _parse_number(row.get("lsb") or "")
            lsb = 1.0 if lsb_value is None else float(lsb_value)
            if lsb <= 0:
                raise ProtocolCatalogError("{} lsb 必须大于 0".format(prefix))
            valid_text = (row.get("is_valid") or "").strip()
            if valid_text not in ("0", "1"):
                raise ProtocolCatalogError("{} is_valid 只能是 0 或 1".format(prefix))

            fields.append(
                ProtocolField(
                    name=name,
                    name_cn=(row.get("name_cn") or "").strip(),
                    type_name=type_name,
                    start_byte=start_byte,
                    end_byte=end_byte,
                    byte_length=byte_length,
                    declared_length=declared_length,
                    lsb=lsb,
                    default=default,
                    is_valid=valid_text == "1",
                    bit_offset=bit_offset,
                    bit_length=bit_length,
                )
            )
        return fields

    @staticmethod
    def _required_int_default(path: Path, field: ProtocolField) -> int:
        if field.default is None or isinstance(field.default, float):
            raise ProtocolCatalogError(
                "{} 的 {} 必须提供整数 default".format(path.name, field.name)
            )
        return int(field.default)

    @staticmethod
    def _validate_byte_coverage(
        path: Path, fields: Sequence[ProtocolField], full_frame_length: int
    ) -> None:
        owners: Dict[int, str] = {}
        bit_bytes = {field.start_byte for field in fields if field.type_name == "BIT"}
        for field in fields:
            if field.type_name == "BIT":
                continue
            for byte_index in range(field.start_byte, field.end_byte + 1):
                if byte_index in owners:
                    raise ProtocolCatalogError(
                        "{} 的 {} 与 {} 在 B{} 重叠".format(
                            path.name, field.name, owners[byte_index], byte_index
                        )
                    )
                owners[byte_index] = field.name
        for byte_index in bit_bytes:
            if byte_index in owners:
                raise ProtocolCatalogError(
                    "{} 的 BIT 字段与 {} 在 B{} 重叠".format(
                        path.name, owners[byte_index], byte_index
                    )
                )
            owners[byte_index] = "BIT"
        expected = set(range(1, full_frame_length + 1))
        actual = set(owners)
        if actual != expected:
            missing = sorted(expected - actual)
            extra = sorted(actual - expected)
            raise ProtocolCatalogError(
                "{} 字段覆盖不连续，缺失 {}，越界 {}".format(path.name, missing, extra)
            )


__all__ = [
    "CSV_COLUMNS",
    "MessageDefinition",
    "ProtocolCatalog",
    "ProtocolCatalogError",
    "ProtocolField",
    "default_catalog_directory",
]
