from pathlib import Path

import pytest

from test_pyqt.protocol_catalog import (
    CSV_COLUMNS,
    ProtocolCatalog,
    ProtocolCatalogError,
)


def test_default_catalog_loads_all_product_protocol_csv_files() -> None:
    catalog = ProtocolCatalog.load_default()

    assert len(catalog) == 32
    assert {definition.payload_length for definition in catalog} == {48, 123}
    assert catalog.get("system_status_request").direction == "request"
    assert catalog.get("system_status_response").direction == "response"


def test_catalog_exposes_payload_offsets_without_physical_framing_fields() -> None:
    definition = ProtocolCatalog.load_default().get("memperf_test_request")

    assert definition.field("version").payload_offset == 0
    assert definition.field("type_group").payload_offset == 1
    assert definition.field("sub_type").payload_offset == 2
    assert definition.field("seq").payload_offset == 3
    assert definition.field("memperf_type").payload_offset == 5
    assert definition.field("crc") not in definition.payload_fields
    assert definition.field("sync[0]") not in definition.payload_fields
    assert definition.field("len") not in definition.payload_fields


def test_catalog_assigns_bit_offsets_for_shared_dh_status_bytes() -> None:
    definition = ProtocolCatalog.load_default().get("dh_control_response")

    assert definition.field("dh_status.ch0").bit_offset == 0
    assert definition.field("dh_status.ch1").bit_offset == 2
    assert definition.field("dh_status.ch3").bit_offset == 6
    assert definition.field("dh_status.ch4").bit_offset == 0


def test_catalog_exposes_voltage_lsb_and_reserved_defaults() -> None:
    catalog = ProtocolCatalog.load_default()

    health = catalog.get("elec_health_status_response")
    for name in ("c_volt", "b_volt", "v28_5"):
        assert health.field(name).lsb == pytest.approx(0.01)
    assert health.field("value_YX").lsb == pytest.approx(10.09 / 4096.0)

    dh = catalog.get("dh_control_response")
    for index in range(23):
        assert dh.field("telemetry[{}]".format(index)).lsb == pytest.approx(0.001)

    assert catalog.get("dh_pulse_config_request").field("pad").default == 0
    assert catalog.get("helm_start_response").field("helm_version").default == 0x01


def test_removed_power_and_ad_commands_are_absent() -> None:
    catalog = ProtocolCatalog.load_default()

    for name in (
        "power_switch_request",
        "power_switch_response",
        "ad_read_request",
        "ad_read_response",
    ):
        with pytest.raises(KeyError):
            catalog.get(name)


def test_helm_board_test_exposes_duty_percent_direction_and_readback_fields() -> None:
    catalog = ProtocolCatalog.load_default()
    request = catalog.get("helm_board_test_request")
    response = catalog.get("helm_board_test_response")

    assert (request.type_group, request.sub_type) == (0x07, 0x02)
    assert (response.type_group, response.sub_type) == (0x07, 0x02)
    compatibility_bits = [
        field
        for field in request.fields
        if field.start_byte == 9 and field.bit_offset == 0 and field.bit_length == 4
    ]
    assert len(compatibility_bits) == 1
    assert compatibility_bits[0].default == 0
    assert not compatibility_bits[0].name.startswith("pwm_level")
    for index in range(4):
        duty_percent = request.field("pwm_duty_percent[{}]".format(index))
        assert duty_percent.type_name == "U8"
        assert duty_percent.start_byte == 10 + index
        assert duty_percent.default == 0
        assert request.field("direction[{}]".format(index)).bit_offset == index + 4
        assert response.field("pwm_duty_match[{}]".format(index)).bit_offset == index
        assert response.field("direction_readback[{}]".format(index)).bit_offset == index + 4
        assert response.field("pwm_duty[{}]".format(index)).byte_length == 4
        assert response.field("helm_AD_value[{}]".format(index)).lsb == pytest.approx(
            10.0 / 65536.0
        )


def test_electrical_health_uses_c_then_b_voltage_fields() -> None:
    definition = ProtocolCatalog.load_default().get("elec_health_status_response")

    assert definition.field("c_volt").payload_offset == 8
    assert definition.field("b_volt").payload_offset == 10
    assert definition.field("value_YX").start_byte == 31
    assert "c_threshold" not in {field.name for field in definition.fields}
    assert "b_threshold" not in {field.name for field in definition.fields}


def test_catalog_rejects_wrong_schema(tmp_path: Path) -> None:
    csv_path = tmp_path / "bad_request.csv"
    csv_path.write_text("index,length,type,name_en\nB1,1,U8,value\n", encoding="utf-8")

    with pytest.raises(ProtocolCatalogError, match="列名"):
        ProtocolCatalog(tmp_path)

def test_catalog_rejects_const_without_default(tmp_path: Path) -> None:
    csv_path = tmp_path / "bad_request.csv"
    rows = [
        CSV_COLUMNS,
        ("B1", "1", "CONST", "同步字0", "sync[0]", "1", "0x55", "1"),
        ("B2", "1", "CONST", "同步字1", "sync[1]", "1", "0xAA", "1"),
        ("B3", "1", "U8", "长度", "len", "1", "48", "1"),
        ("B4", "1", "CONST", "版本", "version", "1", "", "1"),
        ("B5", "1", "U8", "类型组", "type_group", "1", "1", "1"),
        ("B6", "1", "U8", "子类型", "sub_type", "1", "1", "1"),
        ("B7-8", "2", "U16", "序号", "seq", "1", "", "1"),
        ("B9-51", "43", "RESERVED", "填充", "pad", "1", "", "1"),
        ("B52-53", "2", "U16", "校验", "crc", "1", "", "1"),
    ]
    csv_path.write_text(
        "\n".join(",".join(row) for row in rows) + "\n", encoding="utf-8"
    )

    with pytest.raises(ProtocolCatalogError, match="CONST.*default"):
        ProtocolCatalog(tmp_path)
