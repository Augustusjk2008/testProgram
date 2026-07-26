"""Text storage for complete DH report bursts."""

from __future__ import annotations

import csv
from datetime import datetime
from pathlib import Path
from typing import Mapping, Sequence, Tuple

from .product_protocol import DecodedMessage


_STATUS_TEXT = {
    0: "未 DH",
    1: "成功",
    2: "失败",
    3: "保留",
}

_COLUMNS = (
    "report_index",
    "sample_time_us",
    "seq",
    "response_status",
    "err_code",
    "power_enable_readback",
    "return_enable_readback",
    "channel",
    "dh_status",
    "status_text",
    "telemetry_V",
)

_ELECTRICAL_HEALTH_FIELDS = (
    ("c_volt", "c_volt_V"),
    ("b_volt", "b_volt_V"),
    ("external_vol", "external_vol_V"),
    ("core_vol", "core_vol_V"),
    ("assist_vol", "assist_vol_V"),
    ("v28_5", "v28_5_V"),
    ("js_5V", "js_5V_V"),
    ("dyt_5V", "dyt_5V_V"),
    ("power_24V", "power_24V_V"),
    ("value_YX", "value_YX_V"),
)

_ELECTRICAL_HEALTH_COLUMNS = (
    "report_index",
    "sample_time_us",
    "seq",
    "response_status",
    "err_code",
) + tuple(column for _field, column in _ELECTRICAL_HEALTH_FIELDS) + (
    "activate_bits",
    "bc_activate_good",
)


def _integer(value: object, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _field_text(value: object) -> str:
    if value is None:
        return "NA"
    if isinstance(value, float):
        return "{:g}".format(value)
    return str(value)


def _metadata_text(value: object) -> str:
    return str(value).replace("\r", " ").replace("\n", " ")


def write_dh_text_report(
    directory: object,
    reports: Sequence[DecodedMessage],
    parameters: Mapping[str, object],
    *,
    started_at: datetime,
    finished_at: datetime,
    final_status: str,
    final_detail: str = "",
) -> Path:
    """Write one complete DH burst as a human-readable UTF-8-SIG TSV file."""

    output_directory = Path(directory).expanduser()
    output_directory.mkdir(parents=True, exist_ok=True)
    output_path = output_directory / started_at.strftime(
        "DH_data_%Y%m%d_%H%M%S_%f.txt"
    )

    interval_us = _integer(parameters.get("interval_us"), 2500)
    delay_us = _integer(parameters.get("delay_us"), 0)
    selected_mask = _integer(parameters.get("channel[0]")) & 0x007FFFFF
    selected_channels = ",".join(
        "DH{}".format(index)
        for index in range(23)
        if selected_mask & (1 << index)
    )

    metadata = (
        ("started_at", started_at.isoformat(sep=" ", timespec="microseconds")),
        ("finished_at", finished_at.isoformat(sep=" ", timespec="microseconds")),
        ("final_status", final_status),
        ("final_detail", final_detail),
        ("requested_report_count", parameters.get("report_count", "NA")),
        ("interval_us", interval_us),
        ("delay_us", delay_us),
        ("selected_channels", selected_channels or "none"),
    )

    with output_path.open("w", encoding="utf-8-sig", newline="") as stream:
        stream.write("# DH 回告数据\n")
        for name, value in metadata:
            stream.write("# {}={}\n".format(name, _metadata_text(value)))
        stream.write("\n")

        writer = csv.DictWriter(
            stream,
            fieldnames=_COLUMNS,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        for report_index, report in enumerate(reports, start=1):
            response_status = _integer(report.values.get("status"))
            err_code = _integer(report.values.get("err_code")) & 0xFFFF
            failed = response_status != 0 or err_code != 0
            sample_time_us = delay_us + (report_index - 1) * interval_us
            for channel in range(23):
                status = report.values.get("dh_status.ch{}".format(channel))
                status_value = None if status is None else _integer(status)
                telemetry = report.values.get("telemetry[{}]".format(channel))
                writer.writerow(
                    {
                        "report_index": report_index,
                        "sample_time_us": sample_time_us,
                        "seq": int(report.sequence) & 0xFFFF,
                        "response_status": response_status,
                        "err_code": "0x{:04X}".format(err_code),
                        "power_enable_readback": _field_text(
                            report.values.get("power_enable_readback")
                        ),
                        "return_enable_readback": _field_text(
                            report.values.get("return_enable_readback")
                        ),
                        "channel": "DH{}".format(channel),
                        "dh_status": _field_text(status),
                        "status_text": (
                            _STATUS_TEXT.get(status_value, "未知")
                            if status_value is not None
                            else "不可用"
                        ),
                        "telemetry_V": "NA" if failed else _field_text(telemetry),
                    }
                )

    return output_path


def write_electrical_health_text_report(
    directory: object,
    reports: Sequence[Tuple[datetime, DecodedMessage]],
    *,
    started_at: datetime,
    finished_at: datetime,
    final_status: str,
    final_detail: str = "",
) -> Path:
    """Write all user-initiated continuous electrical-health samples."""

    output_directory = Path(directory).expanduser()
    output_directory.mkdir(parents=True, exist_ok=True)
    output_path = output_directory / started_at.strftime(
        "ElectricalHealth_data_%Y%m%d_%H%M%S_%f.txt"
    )
    metadata = (
        ("started_at", started_at.isoformat(sep=" ", timespec="microseconds")),
        ("finished_at", finished_at.isoformat(sep=" ", timespec="microseconds")),
        ("final_status", final_status),
        ("final_detail", final_detail),
        ("sample_count", len(reports)),
        ("repeat_delay_ms", 200),
    )

    with output_path.open("w", encoding="utf-8-sig", newline="") as stream:
        stream.write("# 电气健康连续采集数据\n")
        for name, value in metadata:
            stream.write("# {}={}\n".format(name, _metadata_text(value)))
        stream.write("\n")

        writer = csv.DictWriter(
            stream,
            fieldnames=_ELECTRICAL_HEALTH_COLUMNS,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        for report_index, (sampled_at, report) in enumerate(reports, start=1):
            values = report.values
            response_status = _integer(values.get("status"))
            err_code = _integer(values.get("err_code")) & 0xFFFF
            failed = response_status != 0 or err_code != 0
            sample_time_us = max(
                0,
                int((sampled_at - started_at).total_seconds() * 1000000),
            )
            row = {
                "report_index": report_index,
                "sample_time_us": sample_time_us,
                "seq": int(report.sequence) & 0xFFFF,
                "response_status": response_status,
                "err_code": "0x{:04X}".format(err_code),
            }
            for field, column in _ELECTRICAL_HEALTH_FIELDS:
                row[column] = "NA" if failed else _field_text(values.get(field))
            if failed or "activate_bits" not in values:
                row["activate_bits"] = "NA"
                row["bc_activate_good"] = "NA"
            else:
                activate_bits = _integer(values.get("activate_bits")) & 0xFF
                row["activate_bits"] = "0x{:02X}".format(activate_bits)
                row["bc_activate_good"] = activate_bits & 1
            writer.writerow(row)

    return output_path


__all__ = [
    "write_dh_text_report",
    "write_electrical_health_text_report",
]
