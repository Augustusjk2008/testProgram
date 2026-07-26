"""Immutable configuration values for one serial channel."""

from dataclasses import dataclass, field


DEFAULT_FRAME_HEADER = b"\x55\xAA"
DEFAULT_BAUD_RATE = 614400
DEFAULT_DATA_BITS = 8
DEFAULT_PARITY = "Even"
DEFAULT_STOP_BITS = "1"
DEFAULT_FLOW_CONTROL = "None"


@dataclass(frozen=True)
class ProtocolConfig:
    """Wire protocol configuration owned by a single channel instance."""

    header: bytes = DEFAULT_FRAME_HEADER
    minimum_payload_length: int = 1
    maximum_payload_length: int = 255
    crc_polynomial: int = 0x1021
    crc_initial: int = 0x0000
    crc_reflect_input: bool = False
    crc_reflect_output: bool = False
    crc_xor_out: int = 0x0000
    crc_little_endian: bool = True
    frame_timeout_ms: int = 500

    def __post_init__(self) -> None:
        header = bytes(self.header)
        object.__setattr__(self, "header", header)
        if len(header) != 2:
            raise ValueError("帧头必须恰好包含 2 字节")
        if not 1 <= self.minimum_payload_length <= 255:
            raise ValueError("最小有效数据长度必须在 1..255 范围内")
        if not self.minimum_payload_length <= self.maximum_payload_length <= 255:
            raise ValueError("最大有效数据长度必须在最小值..255 范围内")
        if not 0 <= self.crc_polynomial <= 0xFFFF:
            raise ValueError("CRC 多项式必须是 16 位无符号整数")
        if not 0 <= self.crc_initial <= 0xFFFF:
            raise ValueError("CRC 初值必须是 16 位无符号整数")
        if not 0 <= self.crc_xor_out <= 0xFFFF:
            raise ValueError("CRC 异或输出必须是 16 位无符号整数")
        if self.crc_reflect_input or self.crc_reflect_output:
            raise ValueError("首版协议只支持 RefIn=false、RefOut=false")
        if self.frame_timeout_ms <= 0:
            raise ValueError("半帧超时必须大于 0 ms")


@dataclass(frozen=True)
class SerialPortConfig:
    """Physical settings for one Windows serial port."""

    port_name: str = ""
    baud_rate: int = DEFAULT_BAUD_RATE
    data_bits: int = DEFAULT_DATA_BITS
    parity: str = DEFAULT_PARITY
    stop_bits: str = DEFAULT_STOP_BITS
    flow_control: str = DEFAULT_FLOW_CONTROL
    protocol: ProtocolConfig = field(default_factory=ProtocolConfig)

    def __post_init__(self) -> None:
        if self.baud_rate <= 0:
            raise ValueError("波特率必须大于 0")
        if self.data_bits not in (5, 6, 7, 8):
            raise ValueError("数据位必须是 5、6、7 或 8")
        if self.parity not in ("None", "Even", "Odd", "Mark", "Space"):
            raise ValueError("不支持的校验方式")
        if self.stop_bits not in ("1", "1.5", "2"):
            raise ValueError("不支持的停止位")
        if self.flow_control not in ("None", "Hardware", "Software"):
            raise ValueError("不支持的流控方式")
