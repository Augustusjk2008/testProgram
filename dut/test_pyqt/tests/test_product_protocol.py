import math
import struct

import pytest

from test_pyqt.protocol_catalog import ProtocolCatalog
from test_pyqt.product_protocol import (
    ProductProtocol,
    ProductProtocolError,
    decode_payload,
    encode_payload,
)
from test_pyqt.serial_protocol import encode_frame


@pytest.fixture(scope="module")
def catalog() -> ProtocolCatalog:
    return ProtocolCatalog.load_default()


def test_request_is_only_b4_through_crc_predecessor(catalog) -> None:
    protocol = ProductProtocol(catalog, initial_sequence=0x1234)

    outbound = protocol.build_request("system_status_request")

    assert len(outbound.payload) == 48
    assert outbound.payload[:5] == bytes.fromhex("11 01 01 34 12")
    assert not outbound.payload.startswith(bytes.fromhex("55 AA"))
    wire_frame = encode_frame(outbound.payload)
    assert wire_frame[:3] == bytes.fromhex("55 AA 30")
    assert wire_frame[3:-2] == outbound.payload


def test_pc_sequence_increments_independently_and_wraps(catalog) -> None:
    protocol = ProductProtocol(catalog, initial_sequence=0xFFFF)

    first = protocol.build_request("di_read_request")
    second = protocol.build_request("elec_health_status_request")
    third = protocol.build_request("system_status_request")

    assert [first.sequence, second.sequence, third.sequence] == [0xFFFF, 0, 1]
    assert first.payload[3:5] == b"\xFF\xFF"
    assert second.payload[3:5] == b"\x00\x00"


def test_defaults_reserved_and_little_endian_fields_are_encoded(catalog) -> None:
    definition = catalog.get("memperf_test_request")
    payload = encode_payload(definition, {}, sequence=7)
    decoded = decode_payload(definition, payload)

    assert decoded["memperf_type"] == 0
    assert decoded["length"] == 65536
    assert decoded["seed"] == 0x5A5A5A5A
    assert payload[6:10] == b"\x00\x00\x01\x00"
    reserved = definition.field("pad")
    start = reserved.payload_offset
    assert payload[start : start + reserved.byte_length] == bytes(reserved.byte_length)


def test_array_values_and_float_values_round_trip(catalog) -> None:
    definition = catalog.get("helm_start_request")
    payload = encode_payload(
        definition,
        {
            "waveform": 2,
            "freq": 1.25,
            "ampl": 12.5,
            "offset": -1.5,
            "start": 0.5,
            "max_freq": 4.0,
            "enable": 0x0F,
        },
        sequence=9,
    )
    decoded = decode_payload(definition, payload)

    assert decoded["waveform"] == 2
    assert math.isclose(decoded["freq"], 1.25, rel_tol=1e-6)
    assert math.isclose(decoded["offset"], -1.5, rel_tol=1e-6)
    assert decoded["enable"] == 0x0F
    assert "helm_version" not in decoded


def test_base_array_input_expands_to_indexed_csv_fields(catalog) -> None:
    definition = catalog.get("do_write_request")
    payload = encode_payload(definition, {"channel": [0x12345678, 0x9ABCDEF0]}, 5)
    decoded = decode_payload(definition, payload)

    assert decoded["channel[0]"] == 0x12345678
    assert decoded["channel[1]"] == 0x9ABCDEF0


def test_response_decode_uses_type_and_state_not_request_sequence(catalog) -> None:
    definition = catalog.get("system_status_response")
    payload = encode_payload(
        definition,
        {"status": 0, "err_code": 0, "cpu_usage": 12.5},
        sequence=0xBEEF,
    )

    decoded = ProductProtocol(catalog).decode_response(payload)

    assert decoded.name == "system_status_response"
    assert decoded.sequence == 0xBEEF
    assert decoded.status == 0
    assert math.isclose(decoded.values["cpu_usage"], 12.5, rel_tol=1e-6)


def test_dh_bit_fields_decode_from_shared_bytes(catalog) -> None:
    definition = catalog.get("dh_control_response")
    payload = encode_payload(
        definition,
        {"dh_status.ch0": 1, "dh_status.ch1": 2, "dh_status.ch4": 3},
        sequence=0,
    )
    decoded = decode_payload(definition, payload)

    assert decoded["dh_status.ch0"] == 1
    assert decoded["dh_status.ch1"] == 2
    assert decoded["dh_status.ch4"] == 3


def test_dh_pulse_configuration_round_trips_all_twenty_three_widths(catalog) -> None:
    definition = catalog.get("dh_pulse_config_request")
    widths = [80] + [63] * 22

    payload = encode_payload(
        definition,
        {"config_enable": 1, "pulse_width": widths},
        sequence=0x1234,
    )
    decoded = decode_payload(definition, payload)

    assert decoded["config_enable"] == 1
    assert [decoded["pulse_width[{}]".format(index)] for index in range(23)] == widths


def test_dh_control_round_trips_enable_and_valid_channel_bitmap(catalog) -> None:
    definition = catalog.get("dh_control_request")
    payload = encode_payload(
        definition,
        {
            "power_enable": 1,
            "return_enable": 1,
            "channel[0]": 0x007FFFFF,
            "channel[1]": 0,
            "report_count": 3,
            "interval_us": 65535,
            "delay_frames": 65535,
        },
        sequence=7,
    )
    decoded = decode_payload(definition, payload)

    assert decoded["power_enable"] == 1
    assert decoded["return_enable"] == 1
    assert decoded["channel[0]"] == 0x007FFFFF
    assert decoded["channel[1]"] == 0
    assert decoded["delay_frames"] == 65535
    assert "delay_us" not in decoded


def test_dh_control_request_uses_burst_defaults(catalog) -> None:
    definition = catalog.get("dh_control_request")

    payload = encode_payload(definition, {}, sequence=8)
    decoded = decode_payload(definition, payload)

    assert decoded["report_count"] == 50
    assert decoded["interval_us"] == 2500
    assert decoded["delay_frames"] == 5


def test_dh_control_default_request_keeps_0602_layout_and_crc_golden(catalog) -> None:
    outbound = ProductProtocol(catalog, initial_sequence=0x1234).build_request(
        "dh_control_request"
    )

    assert encode_frame(outbound.payload) == bytes.fromhex(
        "55 AA 30 11 06 02 34 12 "
        "00 00 00 00 00 00 00 00 "
        "00 00 32 00 C4 09 05 00 "
        "00 00 00 00 00 00 00 00 "
        "00 00 00 00 00 00 00 00 "
        "00 00 00 00 00 00 00 00 "
        "00 00 00 55 B2"
    )


def test_dh_control_codec_rejects_the_removed_delay_us_alias(catalog) -> None:
    definition = catalog.get("dh_control_request")

    with pytest.raises(ProductProtocolError, match="delay_frames"):
        encode_payload(definition, {"delay_us": 5}, sequence=8)


def test_spi_flash_result_remains_f32_seconds(catalog) -> None:
    definition = catalog.get("spi_flash_test_response")

    assert definition.field("sjl_result").type_name == "F32"
    payload = encode_payload(definition, {"sjl_result": 1.25}, sequence=8)

    assert math.isclose(
        decode_payload(definition, payload)["sjl_result"], 1.25, rel_tol=1e-6
    )


def test_electrical_health_decodes_raw_fuze_code_with_csv_lsb(catalog) -> None:
    definition = catalog.get("elec_health_status_response")
    payload = bytearray(encode_payload(definition, {}, sequence=8))
    field = definition.field("value_YX")
    payload[field.payload_offset : field.payload_offset + 2] = struct.pack("<h", 0x0800)

    decoded = decode_payload(definition, bytes(payload))

    assert decoded["value_YX"] == pytest.approx(5.045)


def test_helm_board_request_round_trips_integer_duty_percentages(catalog) -> None:
    definition = catalog.get("helm_board_test_request")
    values = {
        "pwm_duty_percent[0]": 0,
        "pwm_duty_percent[1]": 25,
        "pwm_duty_percent[2]": 50,
        "pwm_duty_percent[3]": 100,
        "direction[0]": 0,
        "direction[1]": 1,
        "direction[2]": 0,
        "direction[3]": 1,
    }

    decoded = decode_payload(definition, encode_payload(definition, values, sequence=8))

    for name, value in values.items():
        assert decoded[name] == value


def test_electrical_health_voltage_fields_round_trip_with_centivolt_lsb(
    catalog,
) -> None:
    definition = catalog.get("elec_health_status_response")
    payload = encode_payload(
        definition,
        {"c_volt": 28.5, "b_volt": 27.25, "v28_5": 12.34},
        sequence=8,
    )

    for name, raw in {"c_volt": 2850, "b_volt": 2725, "v28_5": 1234}.items():
        field = definition.field(name)
        assert payload[field.payload_offset : field.payload_offset + 2] == struct.pack(
            "<h", raw
        )

    decoded = decode_payload(definition, payload)
    assert decoded["c_volt"] == pytest.approx(28.5)
    assert decoded["b_volt"] == pytest.approx(27.25)
    assert decoded["v28_5"] == pytest.approx(12.34)


def test_dh_telemetry_round_trips_with_millivolt_lsb(catalog) -> None:
    definition = catalog.get("dh_control_response")
    payload = encode_payload(
        definition,
        {"telemetry[0]": 12.345, "telemetry[22]": -0.125},
        sequence=8,
    )

    first = definition.field("telemetry[0]")
    last = definition.field("telemetry[22]")
    assert payload[first.payload_offset : first.payload_offset + 4] == struct.pack(
        "<i", 12345
    )
    assert payload[last.payload_offset : last.payload_offset + 4] == struct.pack(
        "<i", -125
    )

    decoded = decode_payload(definition, payload)
    assert decoded["telemetry[0]"] == pytest.approx(12.345)
    assert decoded["telemetry[22]"] == pytest.approx(-0.125)


def test_decode_rejects_nonzero_reserved_bytes(catalog) -> None:
    definition = catalog.get("di_read_response")
    payload = bytearray(encode_payload(definition, {}, sequence=0))
    reserved = definition.field("pad")
    payload[reserved.payload_offset] = 1

    with pytest.raises(ProductProtocolError, match="RESERVED.*全零"):
        decode_payload(definition, bytes(payload))


@pytest.mark.parametrize("value", [math.nan, math.inf, -math.inf])
def test_encode_rejects_non_finite_f32(catalog, value) -> None:
    definition = catalog.get("system_status_response")

    with pytest.raises(ProductProtocolError, match="有限"):
        encode_payload(definition, {"cpu_usage": value}, sequence=0)


@pytest.mark.parametrize("value", [math.nan, math.inf, -math.inf])
def test_decode_rejects_non_finite_f32(catalog, value) -> None:
    definition = catalog.get("system_status_response")
    payload = bytearray(encode_payload(definition, {"cpu_usage": 0.0}, sequence=0))
    field = definition.field("cpu_usage")
    payload[field.payload_offset : field.payload_offset + field.byte_length] = (
        struct.pack("<f", value)
    )

    with pytest.raises(ProductProtocolError, match="有限"):
        decode_payload(definition, bytes(payload))


def test_decode_rejects_too_short_product_payloads(catalog) -> None:
    protocol = ProductProtocol(catalog)

    with pytest.raises(ProductProtocolError, match="5..255"):
        protocol.decode_response(bytes(4))


def test_all_request_definitions_build_with_defaults(catalog) -> None:
    protocol = ProductProtocol(catalog)

    for definition in catalog.requests:
        outbound = protocol.build_request(definition.name)
        assert len(outbound.payload) == definition.payload_length


def test_catalog_contains_every_product_command(catalog) -> None:
    assert {(item.type_group, item.sub_type) for item in catalog.requests} == {
        (0x01, 0x01),
        (0x02, 0x01),
        (0x02, 0x02),
        (0x03, 0x01),
        (0x03, 0x02),
        (0x04, 0x01),
        (0x04, 0x02),
        (0x05, 0x01),
        (0x06, 0x01),
        (0x06, 0x02),
        (0x07, 0x02),
        (0x07, 0x10),
        (0x07, 0x11),
        (0x08, 0x01),
        (0x08, 0x02),
        (0x09, 0x10),
        (0x09, 0x11),
    }
