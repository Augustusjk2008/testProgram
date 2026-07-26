import codecs
import importlib
from datetime import datetime

from test_pyqt.product_protocol import DecodedMessage


EXPECTED_COLUMNS = (
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

EXPECTED_ELECTRICAL_HEALTH_COLUMNS = (
    "report_index",
    "sample_time_us",
    "seq",
    "response_status",
    "err_code",
    "c_volt_V",
    "b_volt_V",
    "external_vol_V",
    "core_vol_V",
    "assist_vol_V",
    "v28_5_V",
    "js_5V_V",
    "dyt_5V_V",
    "power_24V_V",
    "value_YX_V",
    "activate_bits",
    "bc_activate_good",
)


def report(sequence, *, status=0, err_code=0, telemetry_offset=0.0):
    values = {
        "status": status,
        "err_code": err_code,
        "power_enable_readback": 1,
        "return_enable_readback": 0,
    }
    values.update(
        {
            "dh_status.ch{}".format(channel): channel % 3
            for channel in range(23)
        }
    )
    values.update(
        {
            "telemetry[{}]".format(channel): telemetry_offset + channel / 10.0
            for channel in range(23)
        }
    )
    return DecodedMessage(
        name="dh_control_response",
        sequence=sequence,
        type_group=0x06,
        sub_type=0x02,
        values=values,
        payload=b"",
    )


def electrical_health_report(sequence, voltage_offset=0.0):
    values = {
        "status": 0,
        "err_code": 0,
        "activate_bits": sequence & 1,
    }
    for index, name in enumerate(
        (
            "c_volt",
            "b_volt",
            "external_vol",
            "core_vol",
            "assist_vol",
            "v28_5",
            "js_5V",
            "dyt_5V",
            "power_24V",
            "value_YX",
        ),
        start=1,
    ):
        values[name] = voltage_offset + index / 10.0
    return DecodedMessage(
        name="elec_health_status_response",
        sequence=sequence,
        type_group=0x04,
        sub_type=0x02,
        values=values,
        payload=b"",
    )


def write_report(tmp_path, reports):
    storage = importlib.import_module("test_pyqt.dh_report_storage")
    return storage.write_dh_text_report(
        tmp_path,
        reports,
        {
            "report_count": len(reports),
            "interval_us": 2500,
            "delay_us": 400,
            "channel[0]": 0x007FFFFF,
            "channel[1]": 0,
        },
        started_at=datetime(2026, 7, 20, 13, 14, 15, 123456),
        finished_at=datetime(2026, 7, 20, 13, 14, 16, 654321),
        final_status="已完成",
        final_detail="已接收全部 DH 回告",
    )


def parsed_rows(path):
    text = path.read_bytes().decode("utf-8-sig")
    lines = [
        line
        for line in text.splitlines()
        if line and not line.startswith("#")
    ]
    header = tuple(lines[0].split("\t"))
    rows = [dict(zip(header, line.split("\t"))) for line in lines[1:]]
    return text, header, rows


def test_write_dh_text_report_uses_fixed_timestamped_name_and_utf8_sig(
    tmp_path,
) -> None:
    path = write_report(tmp_path, [report(100)])

    assert path.parent == tmp_path
    assert path.name == "DH_data_20260720_131415_123456.txt"
    assert path.read_bytes().startswith(codecs.BOM_UTF8)


def test_write_dh_text_report_writes_complete_long_form_rows(tmp_path) -> None:
    path = write_report(
        tmp_path,
        [report(100, telemetry_offset=10.0), report(101, telemetry_offset=20.0)],
    )

    text, header, rows = parsed_rows(path)

    assert header == EXPECTED_COLUMNS
    assert len(rows) == 46
    assert all(set(row) == set(EXPECTED_COLUMNS) for row in rows)
    assert rows[0] == {
        "report_index": "1",
        "sample_time_us": "400",
        "seq": "100",
        "response_status": "0",
        "err_code": "0x0000",
        "power_enable_readback": "1",
        "return_enable_readback": "0",
        "channel": "DH0",
        "dh_status": "0",
        "status_text": "未 DH",
        "telemetry_V": "10",
    }
    assert rows[-1]["report_index"] == "2"
    assert rows[-1]["sample_time_us"] == "2900"
    assert rows[-1]["seq"] == "101"
    assert rows[-1]["channel"] == "DH22"
    assert rows[-1]["dh_status"] == "1"
    assert rows[-1]["status_text"] == "成功"
    assert rows[-1]["telemetry_V"] == "22.2"
    assert "# final_status=已完成" in text
    assert "# final_detail=已接收全部 DH 回告" in text


def test_write_dh_text_report_marks_failed_telemetry_unavailable(tmp_path) -> None:
    path = write_report(
        tmp_path,
        [report(0xFFFF, status=1, err_code=0x0203, telemetry_offset=99.0)],
    )

    _text, _header, rows = parsed_rows(path)

    assert len(rows) == 23
    assert {row["response_status"] for row in rows} == {"1"}
    assert {row["err_code"] for row in rows} == {"0x0203"}
    assert {row["telemetry_V"] for row in rows} == {"NA"}
    assert rows[22]["channel"] == "DH22"
    assert rows[22]["dh_status"] == "1"
    assert rows[22]["status_text"] == "成功"


def test_write_electrical_health_text_report_keeps_every_sample_in_utf8_sig_tsv(
    tmp_path,
) -> None:
    storage = importlib.import_module("test_pyqt.dh_report_storage")
    started_at = datetime(2026, 7, 22, 9, 8, 7, 654321)
    path = storage.write_electrical_health_text_report(
        tmp_path,
        (
            (started_at, electrical_health_report(200, 10.0)),
            (
                datetime(2026, 7, 22, 9, 8, 7, 854321),
                electrical_health_report(201, 20.0),
            ),
        ),
        started_at=started_at,
        finished_at=datetime(2026, 7, 22, 9, 8, 8, 123456),
        final_status="用户停止",
        final_detail="连续采集已停止",
    )

    text, header, rows = parsed_rows(path)

    assert path.name == "ElectricalHealth_data_20260722_090807_654321.txt"
    assert path.read_bytes().startswith(codecs.BOM_UTF8)
    assert header == EXPECTED_ELECTRICAL_HEALTH_COLUMNS
    assert len(rows) == 2
    assert rows[0]["sample_time_us"] == "0"
    assert rows[0]["seq"] == "200"
    assert rows[0]["c_volt_V"] == "10.1"
    assert rows[0]["value_YX_V"] == "11"
    assert rows[0]["activate_bits"] == "0x00"
    assert rows[0]["bc_activate_good"] == "0"
    assert rows[1]["sample_time_us"] == "200000"
    assert rows[1]["seq"] == "201"
    assert rows[1]["c_volt_V"] == "20.1"
    assert rows[1]["activate_bits"] == "0x01"
    assert rows[1]["bc_activate_good"] == "1"
    assert "# final_status=用户停止" in text
    assert "# final_detail=连续采集已停止" in text
