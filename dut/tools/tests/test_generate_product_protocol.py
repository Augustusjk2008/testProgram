import csv
import subprocess
import sys
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "generate_product_protocol.py"
PROTOCOL_DIR = SCRIPT.parent.parent / "docs" / "design" / "product_protocol_csv"
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
ASSET_NAMES = [
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
]


def run_generator(*args: object) -> subprocess.CompletedProcess:
    command = [sys.executable, str(SCRIPT), "--allow-partial"] + [
        str(arg) for arg in args
    ]
    return subprocess.run(command, capture_output=True, text=True, check=False)


def run_generator_strict(*args: object) -> subprocess.CompletedProcess:
    command = [sys.executable, str(SCRIPT)] + [str(arg) for arg in args]
    return subprocess.run(command, capture_output=True, text=True, check=False)


def write_rows(path: Path, rows: list, fieldnames=SCHEMA) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def read_asset_fields(name: str) -> dict:
    with (PROTOCOL_DIR / name).open(
        "r", encoding="utf-8-sig", newline=""
    ) as stream:
        return {row["name_en"]: row for row in csv.DictReader(stream)}


def make_row(
    index: str,
    length: int,
    field_type: str,
    name_en: str,
    default: str = "",
    lsb: str = "",
) -> dict:
    return {
        "index": index,
        "length": str(length),
        "type": field_type,
        "name_cn": name_en,
        "name_en": name_en,
        "lsb": lsb,
        "default": default,
        "is_valid": "1",
    }


def valid_rows() -> list:
    return [
        make_row("B1", 1, "CONST", "sync[0]", "0x55"),
        make_row("B2", 1, "CONST", "sync[1]", "0xAA"),
        make_row("B3", 1, "U8", "len", "48"),
        make_row("B4", 1, "CONST", "version", "0x11"),
        make_row("B5", 1, "U8", "type_group", "0x01"),
        make_row("B6", 1, "U8", "sub_type", "0x01"),
        make_row("B7-8", 2, "U16", "seq"),
        make_row("B9-51", 43, "RESERVED", "pad", "0"),
        make_row("B52-53", 2, "U16", "crc"),
    ]


def run_check(tmp_path: Path, rows: list) -> subprocess.CompletedProcess:
    protocol_dir = tmp_path / "protocol"
    protocol_dir.mkdir()
    write_rows(protocol_dir / "sample_request.csv", rows)
    return run_generator("--check", protocol_dir)


def write_complete_catalog(protocol_dir: Path) -> None:
    protocol_dir.mkdir()
    for index, name in enumerate(ASSET_NAMES, 1):
        rows = valid_rows()
        rows[4]["default"] = "0x20"
        rows[5]["default"] = "0x{:02X}".format(index)
        write_rows(protocol_dir / name, rows)


def test_check_rejects_invalid_schema(tmp_path: Path) -> None:
    protocol_dir = tmp_path / "protocol"
    protocol_dir.mkdir()
    write_rows(
        protocol_dir / "sample.csv",
        [{"index": "B1", "length": "1"}],
        fieldnames=["index", "length"],
    )

    result = run_generator("--check", protocol_dir)

    assert result.returncode == 1
    assert "列名" in result.stderr


def test_check_rejects_unsupported_data_length(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[2]["default"] = "256"

    result = run_check(tmp_path, rows)

    assert result.returncode == 1
    assert "len 必须在 1..255" in result.stderr


def test_check_rejects_index_length_mismatch(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7]["length"] = "42"

    result = run_check(tmp_path, rows)

    assert result.returncode == 1
    assert "index/length" in result.stderr


def test_check_rejects_const_without_default(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[3]["default"] = ""

    result = run_check(tmp_path, rows)

    assert result.returncode == 1
    assert "CONST" in result.stderr


def test_check_rejects_nonzero_reserved_default(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7]["default"] = "1"

    result = run_check(tmp_path, rows)

    assert result.returncode == 1
    assert "RESERVED" in result.stderr


def test_check_rejects_incomplete_bit_byte(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7:8] = [
        make_row("B9", 2, "BIT", "flags.a"),
        make_row("B9", 2, "BIT", "flags.b"),
        make_row("B9", 2, "BIT", "flags.c"),
        make_row("B10-51", 42, "RESERVED", "pad", "0"),
    ]

    result = run_check(tmp_path, rows)

    assert result.returncode == 1
    assert "BIT" in result.stderr


def test_check_accepts_fixed_point_without_lsb_and_defaults_to_one(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7:8] = [
        make_row("B9-10", 2, "S16F", "voltage"),
        make_row("B11-51", 41, "RESERVED", "pad", "0"),
    ]

    protocol_dir = tmp_path / "protocol"
    protocol_dir.mkdir()
    write_rows(protocol_dir / "sample_request.csv", rows)
    output = tmp_path / "ProductProtocolGenerated.h"

    result = run_generator("--output", output, protocol_dir)

    assert result.returncode == 0, result.stderr
    content = output.read_text(encoding="utf-8")
    assert 'FieldType::S16Fixed, 1.0, true' in content


def test_check_accepts_valid_contract(tmp_path: Path) -> None:
    result = run_check(tmp_path, valid_rows())

    assert result.returncode == 0, result.stderr
    assert "1 份 CSV" in result.stdout


def test_check_accepts_complete_bit_bytes(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7:8] = [
        make_row("B9", 2, "BIT", "flags.a"),
        make_row("B9", 2, "BIT", "flags.b"),
        make_row("B9", 2, "BIT", "flags.c"),
        make_row("B9", 2, "BIT", "flags.d"),
        make_row("B10-51", 42, "RESERVED", "pad", "0"),
    ]

    result = run_check(tmp_path, rows)

    assert result.returncode == 0, result.stderr


def test_check_rejects_frame_length_not_equal_to_len_plus_five(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7] = make_row("B9-50", 42, "RESERVED", "pad", "0")
    rows[8] = make_row("B51-52", 2, "U16", "crc")

    result = run_check(tmp_path, rows)

    assert result.returncode == 1
    assert "完整帧长度必须等于 len + 5" in result.stderr


def test_output_generates_stable_cpp17_descriptors_without_source_paths(
    tmp_path: Path,
) -> None:
    protocol_dir = tmp_path / "source" / "protocol"
    protocol_dir.mkdir(parents=True)
    write_rows(protocol_dir / "sample_request.csv", valid_rows())
    write_rows(protocol_dir / "sample_response.csv", valid_rows())
    output = tmp_path / "generated" / "ProductProtocolGenerated.h"

    result = run_generator("--output", output, protocol_dir)

    assert result.returncode == 0, result.stderr
    content = output.read_text(encoding="utf-8")
    assert "namespace GeneratedProductProtocol" in content
    assert '"sample_request", MessageRole::Request' in content
    assert '"sample_response", MessageRole::Response' in content
    assert '"version", 3, 0, 1' in content
    assert '"crc", 51, -1, 2' in content
    assert "find_message(std::string_view name)" in content
    assert str(protocol_dir) not in content


def test_output_preserves_bit_offsets_and_zero_fills_reserved(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7:8] = [
        make_row("B9", 2, "BIT", "flags.a"),
        make_row("B9", 2, "BIT", "flags.b"),
        make_row("B9", 2, "BIT", "flags.c"),
        make_row("B9", 2, "BIT", "flags.d"),
        make_row("B10-51", 42, "RESERVED", "pad", "0"),
    ]
    protocol_dir = tmp_path / "protocol"
    protocol_dir.mkdir()
    write_rows(protocol_dir / "bit_request.csv", rows)
    output = tmp_path / "ProductProtocolGenerated.h"

    result = run_generator("--output", output, protocol_dir)

    assert result.returncode == 0, result.stderr
    content = output.read_text(encoding="utf-8")
    assert '"flags.a", 8, 5, 1, 0, 2, FieldType::Bit' in content
    assert '"flags.d", 8, 5, 1, 6, 2, FieldType::Bit' in content
    assert 'FieldType::Reserved, 0.0, false, "0", true' in content


def test_check_accepts_reserved_without_explicit_zero_and_defaults_to_zero(
    tmp_path: Path,
) -> None:
    rows = valid_rows()
    rows[7]["default"] = ""

    protocol_dir = tmp_path / "protocol"
    protocol_dir.mkdir()
    write_rows(protocol_dir / "sample_request.csv", rows)
    output = tmp_path / "ProductProtocolGenerated.h"

    result = run_generator("--output", output, protocol_dir)

    assert result.returncode == 0, result.stderr
    content = output.read_text(encoding="utf-8")
    assert 'FieldType::Reserved, 0.0, false, "0", true' in content


def test_check_rejects_non_numeric_const_default(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7:8] = [
        make_row("B9", 1, "CONST", "marker", "not-a-number"),
        make_row("B10-51", 42, "RESERVED", "pad", "0"),
    ]

    result = run_check(tmp_path, rows)

    assert result.returncode == 1
    assert "CONST default" in result.stderr


def test_check_rejects_const_default_that_does_not_fit(tmp_path: Path) -> None:
    rows = valid_rows()
    rows[7:8] = [
        make_row("B9", 1, "CONST", "marker", "0x100"),
        make_row("B10-51", 42, "RESERVED", "pad", "0"),
    ]

    result = run_check(tmp_path, rows)

    assert result.returncode == 1
    assert "CONST default" in result.stderr
    assert "超出" in result.stderr


def test_strict_check_rejects_missing_protocol_asset(tmp_path: Path) -> None:
    protocol_dir = tmp_path / "protocol"
    write_complete_catalog(protocol_dir)
    (protocol_dir / ASSET_NAMES[-1]).unlink()

    result = run_generator_strict("--check", protocol_dir)

    assert result.returncode == 1
    assert "协议资产集合" in result.stderr
    assert ASSET_NAMES[-1] in result.stderr


def test_strict_check_rejects_extra_protocol_asset(tmp_path: Path) -> None:
    protocol_dir = tmp_path / "protocol"
    write_complete_catalog(protocol_dir)
    write_rows(protocol_dir / "extra_request.csv", valid_rows())

    result = run_generator_strict("--check", protocol_dir)

    assert result.returncode == 1
    assert "协议资产集合" in result.stderr
    assert "extra_request.csv" in result.stderr


def test_strict_check_reports_complete_catalog_summary(tmp_path: Path) -> None:
    protocol_dir = tmp_path / "protocol"
    write_complete_catalog(protocol_dir)

    result = run_generator_strict("--check", protocol_dir)

    assert result.returncode == 0, result.stderr
    assert "37 份 CSV" in result.stdout
    assert "len=1..255" in result.stdout
    assert "帧长=len+5" in result.stdout


def test_repository_catalog_matches_planned_protocol_revisions() -> None:
    command_ids = {
        "spi_flash_test_request.csv": (0x02, 0x02),
        "spi_flash_test_response.csv": (0x02, 0x02),
        "bus_echo_test_request.csv": (0x03, 0x02),
        "bus_echo_test_response.csv": (0x03, 0x02),
        "elec_health_status_request.csv": (0x05, 0x01),
        "elec_health_status_response.csv": (0x05, 0x01),
        "helm_board_test_request.csv": (0x07, 0x02),
        "helm_board_test_response.csv": (0x07, 0x02),
        "imu_stream_feedback_response.csv": (0x09, 0x01),
        "imu_stream_start_request.csv": (0x09, 0x10),
        "imu_stream_start_response.csv": (0x09, 0x10),
        "imu_stream_stop_request.csv": (0x09, 0x11),
        "imu_stream_stop_response.csv": (0x09, 0x11),
    }
    for asset_name, expected in command_ids.items():
        fields = read_asset_fields(asset_name)
        actual = (
            int(fields["type_group"]["default"], 0),
            int(fields["sub_type"]["default"], 0),
        )
        assert actual == expected

    reserved_layouts = {
        "dh_control_request.csv": ("B25-51", "27"),
        "do_write_request.csv": ("B17-51", "35"),
        "do_write_response.csv": ("B20-51", "32"),
        "elec_health_status_response.csv": ("B33-51", "19"),
        "helm_board_test_request.csv": ("B14-51", "38"),
        "helm_board_test_response.csv": ("B45-51", "7"),
        "helm_start_request.csv": ("B41-51", "11"),
        "helm_start_response.csv": ("B13-51", "39"),
        "imu_stream_start_request.csv": ("B9-51", "43"),
        "imu_stream_start_response.csv": ("B12-51", "40"),
        "imu_stream_stop_request.csv": ("B9-51", "43"),
        "imu_stream_stop_response.csv": ("B12-51", "40"),
        "memperf_test_response.csv": ("B33-51", "19"),
        "spi_flash_test_response.csv": ("B16-51", "36"),
    }
    for asset_name, (expected_index, expected_length) in reserved_layouts.items():
        reserved = read_asset_fields(asset_name)["pad"]
        assert reserved["index"] == expected_index
        assert reserved["length"] == expected_length
        assert reserved["default"] in {"", "0"}

    feedback = read_asset_fields("helm_feedback_response.csv")
    assert feedback["len"]["default"] == "232"
    assert feedback["sample_count"]["index"] == "B12"
    assert feedback["first_timestamp_us_low"]["index"] == "B13-16"
    assert feedback["first_timestamp_us_high"]["index"] == "B17-20"
    assert feedback["sample[0].serial_b"]["index"] == "B23-24"
    assert feedback["sample[4].serial_a"]["index"] == "B218-219"
    assert feedback["sample[4].ins[3]"]["index"] == "B232-235"

    imu_feedback = read_asset_fields("imu_stream_feedback_response.csv")
    assert imu_feedback["len"]["default"] == "123"
    expected_imu_fields = {
        "status": ("B9", "U8"),
        "err_code": ("B10-11", "U16"),
        "source_seq": ("B12-13", "U16"),
        "delta_angle_x": ("B14-17", "F32"),
        "delta_angle_y": ("B18-21", "F32"),
        "delta_angle_z": ("B22-25", "F32"),
        "delta_velocity_x": ("B26-29", "F32"),
        "delta_velocity_y": ("B30-33", "F32"),
        "delta_velocity_z": ("B34-37", "F32"),
        "angular_rate_x": ("B38-41", "F32"),
        "angular_rate_y": ("B42-45", "F32"),
        "angular_rate_z": ("B46-49", "F32"),
        "acceleration_x": ("B50-53", "F32"),
        "acceleration_y": ("B54-57", "F32"),
        "acceleration_z": ("B58-61", "F32"),
        "temperature": ("B62-63", "S16F"),
        "self_test_status": ("B64-65", "U16"),
        "work_status": ("B66", "U8"),
        "software_version": ("B67-68", "U16"),
        "source_reserved": ("B69-70", "U16"),
        "pad": ("B71-126", "RESERVED"),
    }
    for field_name, (index, field_type) in expected_imu_fields.items():
        assert imu_feedback[field_name]["index"] == index
        assert imu_feedback[field_name]["type"] == field_type
    assert float(imu_feedback["temperature"]["lsb"]) == 0.1

    dh_request = read_asset_fields("dh_control_request.csv")
    assert dh_request["report_count"]["default"] == "50"
    assert dh_request["interval_us"]["default"] == "2500"

    helm_request = read_asset_fields("helm_start_request.csv")
    helm_response = read_asset_fields("helm_start_response.csv")
    assert "helm_version" not in helm_request
    assert helm_request["sweep_duration_s"]["index"] == "B37-40"
    assert helm_request["sweep_duration_s"]["type"] == "F32"
    assert helm_response["helm_version"]["type"] == "CONST"
    assert helm_response["helm_version"]["default"] == "0x01"
    for field_name in ("waveform", "enable"):
        assert helm_request[field_name]["type"] == "U32"
        assert helm_request[field_name]["length"] == "4"
    for field_name in ("freq", "ampl", "offset", "start", "max_freq"):
        assert helm_request[field_name]["type"] == "F32"

    assert read_asset_fields("spi_flash_test_response.csv")["sjl_result"]["type"] == "F32"

    dh_control = read_asset_fields("dh_control_response.csv")
    dh_pulse_request = read_asset_fields("dh_pulse_config_request.csv")
    dh_pulse_response = read_asset_fields("dh_pulse_config_response.csv")
    assert (int(dh_control["type_group"]["default"], 0), int(dh_control["sub_type"]["default"], 0)) == (0x06, 0x02)
    assert (int(dh_pulse_request["type_group"]["default"], 0), int(dh_pulse_request["sub_type"]["default"], 0)) == (0x06, 0x01)
    assert (int(dh_pulse_response["type_group"]["default"], 0), int(dh_pulse_response["sub_type"]["default"], 0)) == (0x06, 0x01)
    for index in range(23):
        assert float(dh_control["telemetry[{}]".format(index)]["lsb"]) == 0.001

    electrical_health = read_asset_fields("elec_health_status_response.csv")
    for field_name in ("c_volt", "b_volt", "v28_5"):
        assert float(electrical_health[field_name]["lsb"]) == 0.01
    assert electrical_health["value_YX"]["index"] == "B31-32"
    assert electrical_health["value_YX"]["type"] == "S16F"
    assert float(electrical_health["value_YX"]["lsb"]) == 10.09 / 4096.0

    helm_board_request = read_asset_fields("helm_board_test_request.csv")
    helm_board_response = read_asset_fields("helm_board_test_response.csv")
    assert helm_board_request["pwm_command_reserved"]["index"] == "B9"
    assert helm_board_request["pwm_command_reserved"]["length"] == "4"
    assert helm_board_request["pwm_command_reserved"]["default"] == "0"
    for index in range(4):
        percent = helm_board_request["pwm_duty_percent[{}]".format(index)]
        assert percent["index"] == "B{}".format(10 + index)
        assert percent["type"] == "U8"
        assert percent["default"] == "0"
        assert helm_board_request["direction[{}]".format(index)]["index"] == "B9"
        assert helm_board_response["pwm_duty_match[{}]".format(index)]["index"] == "B12"

    timer_request = read_asset_fields("timer_jitter_start_request.csv")
    timer_response = read_asset_fields("timer_jitter_start_response.csv")
    assert timer_request["mode"]["type"] == "U32"
    assert timer_request["mode"]["length"] == "4"
    for index in range(8):
        bucket = timer_response["buckets[{}]".format(index)]
        assert bucket["type"] == "U32"
        assert bucket["length"] == "4"
    assert timer_response["avg_jitter"]["type"] == "F32"
    assert timer_response["max_jitter"]["type"] == "F32"
