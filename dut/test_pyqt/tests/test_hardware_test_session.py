import struct

from PyQt5.QtCore import QObject, pyqtSignal
import pytest

from test_pyqt.hardware_test_session import (
    BUS_TIMEOUT_MS,
    DEFAULT_TIMEOUT_MS,
    MEMORY_TIMEOUT_MS,
    SPI_FLASH_TIMEOUT_MS,
    LEGACY_HELM_TEST_SPEC,
    TEST_ORDER,
    TEST_SPECS,
    HardwareTestSession,
    TestStatus,
    dh_report_gap_timeout_ms,
    dh_timeout_ms,
)
from test_pyqt.protocol_catalog import ProtocolCatalog
from test_pyqt.product_protocol import ProductProtocol, decode_payload, encode_payload


class FakeChannel(QObject):
    frame_received = pyqtSignal(bytes, bytes)
    protocol_error = pyqtSignal(str, str, bytes)
    send_failed = pyqtSignal(bytes, str)
    io_error = pyqtSignal(str)
    disconnected = pyqtSignal()

    def __init__(self) -> None:
        super().__init__()
        self.sent_payloads = []
        self.send_result = True

    def send_payload(self, payload):
        self.sent_payloads.append(bytes(payload))
        return self.send_result


def make_session(initial_sequence=0):
    channel = FakeChannel()
    catalog = ProtocolCatalog.load_default()
    protocol = ProductProtocol(catalog, initial_sequence=initial_sequence)
    session = HardwareTestSession(channel, protocol)
    session.set_enabled(True)
    return session, channel, catalog


def response(catalog, name, values=None, sequence=900):
    definition = catalog.get(name)
    merged = {"status": 0, "err_code": 0}
    if values:
        merged.update(values)
    payload = encode_payload(definition, merged, sequence)
    return payload


def emit_response(channel, payload):
    channel.frame_received.emit(payload, b"wire")


def emit_active_response(session, channel, catalog, name, values=None):
    assert session.active_sequence is not None
    emit_response(
        channel,
        response(catalog, name, values, sequence=session.active_sequence),
    )


def test_test_order_and_timeout_classes_are_fixed() -> None:
    assert TEST_ORDER == (
        "system",
        "memory",
        "spi_flash",
        "bus",
        "di",
        "do",
        "electrical_health",
        "dh_pulse_config",
        "dh",
        "helm_board",
        "timer",
    )
    assert DEFAULT_TIMEOUT_MS == 2000
    assert BUS_TIMEOUT_MS == 60000
    assert MEMORY_TIMEOUT_MS == 120000
    assert SPI_FLASH_TIMEOUT_MS == 180000


def test_helm_start_phase_parameter_label_declares_radians() -> None:
    start = next(
        parameter
        for parameter in LEGACY_HELM_TEST_SPEC.parameters
        if parameter.name == "start"
    )

    assert start.label == "起始相位 rad"


def test_helm_board_test_sends_four_duty_percentages_and_four_directions() -> None:
    session, channel, catalog = make_session()
    parameters = {
        "pwm_duty_percent[0]": 0,
        "pwm_duty_percent[1]": 25,
        "pwm_duty_percent[2]": 50,
        "pwm_duty_percent[3]": 100,
        "direction[0]": 0,
        "direction[1]": 1,
        "direction[2]": 1,
        "direction[3]": 0,
    }

    assert session.run_test("helm_board", parameters)
    values = decode_payload(
        catalog.get("helm_board_test_request"), channel.sent_payloads[-1]
    )
    assert [
        values["pwm_duty_percent[{}]".format(index)] for index in range(4)
    ] == [
        0,
        25,
        50,
        100,
    ]
    assert [values["direction[{}]".format(index)] for index in range(4)] == [
        0,
        1,
        1,
        0,
    ]

    emit_active_response(
        session, channel, catalog, "helm_board_test_response"
    )
    assert session.status_for("helm_board") == TestStatus.COMPLETED


@pytest.mark.parametrize(
    "field_name,value",
    (
        ("pwm_duty_percent[0]", -1),
        ("pwm_duty_percent[3]", 101),
        ("pwm_duty_percent[1]", 50.5),
        ("direction[0]", 2),
        ("direction[3]", -1),
    ),
)
def test_helm_board_test_rejects_invalid_duty_or_direction(field_name, value) -> None:
    session, channel, _ = make_session()

    assert not session.run_test("helm_board", {field_name: value})
    assert channel.sent_payloads == []
    assert session.status_for("helm_board") == TestStatus.EXECUTION_FAILED


def test_timer_mode_is_limited_to_baseline_and_c2h_load_choices() -> None:
    timer = next(spec for spec in TEST_SPECS if spec.key == "timer")
    mode = next(parameter for parameter in timer.parameters if parameter.name == "mode")

    assert mode.choices == (0, 1)


def test_only_one_ordinary_request_can_be_in_flight() -> None:
    session, channel, _ = make_session()

    assert session.run_test("system")
    assert not session.run_test("di")
    assert len(channel.sent_payloads) == 1
    assert session.status_for("system") == TestStatus.RUNNING


def test_normal_response_must_echo_the_active_request_sequence() -> None:
    session, channel, catalog = make_session()
    finished = []
    session.test_finished.connect(
        lambda key, status, detail: finished.append((key, status, detail))
    )
    assert session.run_test("system")

    emit_response(
        channel,
        response(catalog, "system_status_response", {"cpu_usage": 22.5}, 0xBEEF),
    )
    assert session.status_for("system") == TestStatus.RUNNING

    emit_active_response(
        session, channel, catalog, "system_status_response", {"cpu_usage": 22.5}
    )

    assert session.status_for("system") == TestStatus.COMPLETED
    assert finished[-1][0:2] == ("system", TestStatus.COMPLETED.value)


def test_wrong_command_is_ignored_until_expected_response_arrives() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("system")

    emit_response(
        channel,
        response(
            catalog,
            "di_read_response",
            sequence=session.active_sequence,
        ),
    )
    assert session.status_for("system") == TestStatus.RUNNING

    emit_active_response(session, channel, catalog, "system_status_response")
    assert session.status_for("system") == TestStatus.COMPLETED


def test_execution_error_and_communication_timeout_have_distinct_statuses() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("di")
    emit_active_response(
        session,
        channel,
        catalog,
        "di_read_response",
        {"status": 1, "err_code": 0x0101},
    )
    assert session.status_for("di") == TestStatus.EXECUTION_FAILED

    assert session.run_test("system")
    session._on_response_timeout()
    assert session.status_for("system") == TestStatus.COMMUNICATION_FAILED
    assert len(channel.sent_payloads) == 2


def test_timeout_selection_and_dh_formula() -> None:
    session, _, _ = make_session()

    assert session.run_test("bus")
    assert session.response_timer.interval() == BUS_TIMEOUT_MS
    session.stop()
    assert session.run_test("memory")
    assert session.response_timer.interval() == MEMORY_TIMEOUT_MS
    session.stop()
    assert session.run_test("spi_flash")
    assert session.response_timer.interval() == SPI_FLASH_TIMEOUT_MS
    session.stop()

    assert dh_timeout_ms(3, 2500, 500) == 2000
    assert dh_timeout_ms(120, 2500, 0) == 2000
    assert dh_report_gap_timeout_ms(2500) == 2003


def test_dh_uses_fifty_reports_2500_us_and_five_delay_frames_as_defaults() -> None:
    session, channel, catalog = make_session()

    assert session.run_test("dh")
    request_values = decode_payload(
        catalog.get("dh_control_request"), channel.sent_payloads[-1]
    )

    assert request_values["report_count"] == 50
    assert request_values["interval_us"] == 2500
    assert request_values["delay_frames"] == 5
    assert "delay_us" not in request_values
    emit_active_response(
        session,
        channel,
        catalog,
        "dh_control_response",
        {"status": 1, "err_code": 0x0102},
    )


def test_stop_does_not_cancel_an_accepted_dh_burst() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("dh", {"report_count": 2, "delay_frames": 0})

    session.stop()

    assert session.active_test_key == "dh"
    assert session.is_busy
    emit_active_response(session, channel, catalog, "dh_control_response")
    assert session.active_test_key == "dh"
    emit_response(
        channel,
        response(
            catalog,
            "dh_control_response",
            sequence=(session.active_sequence + 1) & 0xFFFF,
        ),
    )
    assert session.status_for("dh") == TestStatus.COMPLETED


def test_dh_long_burst_switches_to_progress_based_timeout() -> None:
    session, channel, catalog = make_session()

    assert session.run_test(
        "dh", {"report_count": 120, "interval_us": 2500, "delay_frames": 9}
    )
    assert session.response_timer.interval() == dh_timeout_ms(120, 2500, 9)
    request_values = decode_payload(
        catalog.get("dh_control_request"), channel.sent_payloads[-1]
    )
    assert request_values["delay_frames"] == 9

    emit_active_response(session, channel, catalog, "dh_control_response")

    assert session.status_for("dh") == TestStatus.RUNNING
    assert session.response_timer.interval() == dh_report_gap_timeout_ms(2500)


def test_dh_pulse_configuration_uses_defaults_and_completes_as_ordinary_request() -> None:
    session, channel, catalog = make_session()

    assert session.run_test("dh_pulse_config")
    request_values = decode_payload(
        catalog.get("dh_pulse_config_request"), channel.sent_payloads[-1]
    )

    assert request_values["config_enable"] == 1
    assert request_values["pulse_width[0]"] == 80
    assert [
        request_values["pulse_width[{}]".format(index)] for index in range(1, 23)
    ] == [63] * 22

    emit_active_response(session, channel, catalog, "dh_pulse_config_response")
    assert session.status_for("dh_pulse_config") == TestStatus.COMPLETED


def test_dh_collects_only_consecutive_request_sequences_with_wraparound() -> None:
    session, channel, catalog = make_session(initial_sequence=0xFFFF)
    reports = []
    session.dh_report_received.connect(reports.append)
    assert session.run_test(
        "dh", {"report_count": 2, "interval_us": 2500, "delay_frames": 5}
    )
    request_values = decode_payload(
        catalog.get("dh_control_request"), channel.sent_payloads[-1]
    )
    assert request_values["power_enable"] == 1
    assert request_values["return_enable"] == 1
    assert request_values["channel[0]"] == 0x007FFFFF
    assert request_values["channel[1]"] == 0

    emit_response(channel, response(catalog, "dh_control_response", sequence=12))
    assert session.status_for("dh") == TestStatus.RUNNING
    assert reports == []

    emit_response(channel, response(catalog, "dh_control_response", sequence=0xFFFF))
    assert session.status_for("dh") == TestStatus.RUNNING
    emit_response(channel, response(catalog, "dh_control_response", sequence=0))

    assert len(reports) == 2
    assert session.status_for("dh") == TestStatus.COMPLETED


def test_dh_collects_all_failed_frames_before_reporting_execution_failure() -> None:
    session, channel, catalog = make_session()
    reports = []
    session.dh_report_received.connect(reports.append)
    assert session.run_test("dh", {"report_count": 2})

    emit_active_response(
        session,
        channel,
        catalog,
        "dh_control_response",
        {"status": 1, "err_code": 0x0203},
    )
    assert len(reports) == 1
    assert session.status_for("dh") == TestStatus.RUNNING

    emit_response(
        channel,
        response(
            catalog,
            "dh_control_response",
            {"status": 1, "err_code": 0x0203},
            sequence=(session.active_sequence + 1) & 0xFFFF,
        ),
    )

    assert len(reports) == 2
    assert session.status_for("dh") == TestStatus.EXECUTION_FAILED


def test_dh_non_telemetry_failure_finishes_on_the_first_response() -> None:
    session, channel, catalog = make_session()
    reports = []
    session.dh_report_received.connect(reports.append)
    assert session.run_test("dh", {"report_count": 3})

    emit_active_response(
        session,
        channel,
        catalog,
        "dh_control_response",
        {"status": 1, "err_code": 0x0103},
    )

    assert len(reports) == 1
    assert session.status_for("dh") == TestStatus.EXECUTION_FAILED
    assert not session.is_busy


@pytest.mark.parametrize(
    "parameters",
    (
        {"power_enable": 2, "return_enable": 3},
        {"channel[0]": 0, "channel[1]": 1},
        {"report_count": 0},
        {"interval_us": 0},
        {"interval_us": 2499},
        {"report_count": 2, "delay_frames": 2},
        {"channel[0]": 0, "channel[1]": 0},
    ),
)
def test_dh_accepts_all_values_within_their_wire_encoding_ranges(parameters) -> None:
    session, channel, catalog = make_session()

    assert session.run_test("dh", parameters)
    request_values = decode_payload(
        catalog.get("dh_control_request"), channel.sent_payloads[-1]
    )
    for name, value in parameters.items():
        assert request_values[name] == value
    emit_active_response(
        session,
        channel,
        catalog,
        "dh_control_response",
        {"status": 1, "err_code": 0x0102},
    )


@pytest.mark.parametrize(
    "parameters",
    (
        {"power_enable": -1},
        {"power_enable": 256},
        {"return_enable": -1},
        {"return_enable": 256},
        {"channel[0]": -1},
        {"channel[0]": 1 << 32},
        {"channel[1]": -1},
        {"channel[1]": 1 << 32},
        {"report_count": -1},
        {"report_count": 65536},
        {"interval_us": -1},
        {"interval_us": 65536},
        {"delay_frames": -1},
        {"delay_frames": 65536},
        {"delay_frames": 1.5},
    ),
)
def test_dh_rejects_only_non_integer_or_out_of_wire_range_values(parameters) -> None:
    session, channel, _ = make_session()

    assert not session.run_test("dh", parameters)
    assert channel.sent_payloads == []
    assert session.status_for("dh") == TestStatus.EXECUTION_FAILED


@pytest.mark.parametrize(
    "parameters",
    (
        {"config_enable": 2},
        {"pulse_width[0]": -1},
        {"pulse_width[22]": 65536},
    ),
)
def test_dh_pulse_configuration_rejects_invalid_bounds(parameters) -> None:
    session, channel, _ = make_session()

    assert not session.run_test("dh_pulse_config", parameters)
    assert channel.sent_payloads == []
    assert session.status_for("dh_pulse_config") == TestStatus.EXECUTION_FAILED


def test_helm_start_ack_precedes_continuous_feedback_until_stop() -> None:
    session, channel, catalog = make_session()
    feedback = []
    session.helm_feedback_received.connect(feedback.append)
    assert session.run_test("helm")
    assert session.active_request_name == "helm_start_request"

    emit_active_response(session, channel, catalog, "helm_start_response")
    emit_response(channel, response(catalog, "helm_feedback_response", sequence=3))
    emit_response(channel, response(catalog, "helm_feedback_response", sequence=44))

    assert len(feedback) == 2
    assert session.status_for("helm") == TestStatus.RUNNING
    assert session.active_request_name is None

    session.stop()
    assert session.active_request_name == "helm_stop_request"
    assert len(channel.sent_payloads) == 2
    emit_active_response(session, channel, catalog, "helm_stop_response")
    assert session.status_for("helm") == TestStatus.EXECUTION_FAILED


def test_user_stop_during_helm_feedback_sends_one_stop_and_waits_for_ack() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("helm")
    emit_active_response(session, channel, catalog, "helm_start_response")

    session.stop()
    session.stop()

    assert session.status_for("helm") == TestStatus.RUNNING
    assert session.active_request_name == "helm_stop_request"
    assert len(channel.sent_payloads) == 2
    stop_values = decode_payload(
        catalog.get("helm_stop_request"), channel.sent_payloads[-1]
    )
    assert stop_values["type_group"] == 0x07
    assert stop_values["sub_type"] == 0x11

    emit_active_response(session, channel, catalog, "helm_stop_response")

    assert session.status_for("helm") == TestStatus.EXECUTION_FAILED


def test_helm_feedback_error_sends_stop_before_reporting_execution_failure() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("helm")
    emit_active_response(session, channel, catalog, "helm_start_response")

    emit_response(
        channel,
        response(
            catalog,
            "helm_feedback_response",
            {"status": 1, "err_code": 0x0203},
        ),
    )

    assert session.status_for("helm") == TestStatus.RUNNING
    assert session.active_request_name == "helm_stop_request"
    assert len(channel.sent_payloads) == 2

    emit_active_response(session, channel, catalog, "helm_stop_response")

    assert session.status_for("helm") == TestStatus.EXECUTION_FAILED


def test_helm_feedback_timeout_sends_stop_before_reporting_communication_failure() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("helm")
    emit_active_response(session, channel, catalog, "helm_start_response")

    session._on_response_timeout()

    assert session.status_for("helm") == TestStatus.RUNNING
    assert session.active_request_name == "helm_stop_request"
    assert len(channel.sent_payloads) == 2

    emit_active_response(session, channel, catalog, "helm_stop_response")

    assert session.status_for("helm") == TestStatus.COMMUNICATION_FAILED


def test_malformed_helm_feedback_sends_stop_before_reporting_decode_failure() -> None:
    session, channel, catalog = make_session()
    finished = []
    session.test_finished.connect(
        lambda key, status, detail: finished.append((key, status, detail))
    )
    assert session.run_test("helm")
    emit_active_response(session, channel, catalog, "helm_start_response")
    definition = catalog.get("helm_feedback_response")
    malformed = bytearray(response(catalog, "helm_feedback_response"))
    feedback = definition.field("sample[0].fdb[0]")
    malformed[
        feedback.payload_offset : feedback.payload_offset + feedback.byte_length
    ] = struct.pack("<f", float("nan"))

    emit_response(channel, bytes(malformed))

    assert session.status_for("helm") == TestStatus.RUNNING
    assert session.active_request_name == "helm_stop_request"
    assert len(channel.sent_payloads) == 2

    emit_active_response(session, channel, catalog, "helm_stop_response")

    assert session.status_for("helm") == TestStatus.COMMUNICATION_FAILED
    assert "产品协议解析失败" in finished[-1][2]


def test_protocol_error_during_helm_feedback_sends_stop_before_failure() -> None:
    session, channel, catalog = make_session()
    finished = []
    session.test_finished.connect(
        lambda key, status, detail: finished.append((key, status, detail))
    )
    assert session.run_test("helm")
    emit_active_response(session, channel, catalog, "helm_start_response")

    channel.protocol_error.emit("crc_mismatch", "CRC 校验失败", b"bad")

    assert session.status_for("helm") == TestStatus.RUNNING
    assert session.active_request_name == "helm_stop_request"
    assert len(channel.sent_payloads) == 2

    emit_active_response(session, channel, catalog, "helm_stop_response")

    assert session.status_for("helm") == TestStatus.COMMUNICATION_FAILED
    assert "物理协议错误 [crc_mismatch] CRC 校验失败" in finished[-1][2]


def test_disabling_helm_before_start_ack_waits_then_stops_and_parses_ack() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("helm")

    session.set_enabled(False)

    assert session.status_for("helm") == TestStatus.RUNNING
    assert session.active_request_name == "helm_start_request"
    assert len(channel.sent_payloads) == 1

    emit_active_response(session, channel, catalog, "helm_start_response")

    assert session.active_request_name == "helm_stop_request"
    assert len(channel.sent_payloads) == 2

    emit_active_response(session, channel, catalog, "helm_stop_response")

    assert session.status_for("helm") == TestStatus.EXECUTION_FAILED
    assert not session.run_test("system")
    assert len(channel.sent_payloads) == 2


def test_user_stop_before_failed_helm_start_ack_finishes_without_stop_request() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("helm")

    session.stop()
    emit_active_response(
        session,
        channel,
        catalog,
        "helm_start_response",
        {"status": 1, "err_code": 0x0203},
    )

    assert session.status_for("helm") == TestStatus.EXECUTION_FAILED
    assert len(channel.sent_payloads) == 1


def test_helm_stop_send_rejection_reports_communication_failure_without_retry() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("helm")
    emit_active_response(session, channel, catalog, "helm_start_response")
    channel.send_result = False

    session.stop()

    assert session.status_for("helm") == TestStatus.COMMUNICATION_FAILED
    assert len(channel.sent_payloads) == 2


def test_helm_stop_ack_failure_overrides_normal_completion() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("helm")
    emit_active_response(session, channel, catalog, "helm_start_response")
    emit_response(channel, response(catalog, "helm_feedback_response"))
    session.stop()

    emit_active_response(
        session,
        channel,
        catalog,
        "helm_stop_response",
        {"status": 1, "err_code": 0x0203},
    )

    assert session.status_for("helm") == TestStatus.EXECUTION_FAILED
    assert len(channel.sent_payloads) == 2


def test_helm_stop_ack_timeout_reports_communication_failure_without_retry() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("helm")
    emit_active_response(session, channel, catalog, "helm_start_response")
    emit_response(channel, response(catalog, "helm_feedback_response"))

    session._on_response_timeout()

    assert session.status_for("helm") == TestStatus.RUNNING
    assert session.active_request_name == "helm_stop_request"

    session._on_response_timeout()

    assert session.status_for("helm") == TestStatus.COMMUNICATION_FAILED
    assert len(channel.sent_payloads) == 2


def test_bus_session_exposes_only_com1_com2_and_com4_links() -> None:
    session, _channel, _catalog = make_session()

    link_field = next(
        field for field in session.parameter_fields("bus") if field.name == "link_id"
    )

    assert link_field.choices == (0, 1, 3)


@pytest.mark.parametrize("link_id", (2, 4, 5, 6, 7))
def test_control_and_unknown_bus_links_are_rejected_before_serial_send(link_id) -> None:
    session, channel, _catalog = make_session()

    assert not session.run_test("bus", {"link_id": link_id})
    assert channel.sent_payloads == []
    assert session.status_for("bus") == TestStatus.EXECUTION_FAILED


@pytest.mark.parametrize(
    "parameters",
    (
        {"link_id": 1.5},
        {"bus_mode": "invalid"},
    ),
)
def test_non_integral_link_or_unknown_bus_mode_is_rejected_before_serial_send(
    parameters,
) -> None:
    session, channel, _catalog = make_session()

    assert not session.run_test("bus", parameters)
    assert channel.sent_payloads == []
    assert session.status_for("bus") == TestStatus.EXECUTION_FAILED


@pytest.mark.parametrize("total_count", (0, 100001))
def test_bus_loop_count_outside_protocol_range_is_rejected_before_serial_send(
    total_count,
) -> None:
    session, channel, _catalog = make_session()

    assert not session.run_test(
        "bus", {"bus_mode": "loop", "link_id": 0, "total_count": total_count}
    )
    assert channel.sent_payloads == []
    assert session.status_for("bus") == TestStatus.EXECUTION_FAILED


def test_bus_loop_count_upper_boundary_is_encoded_for_com4() -> None:
    session, channel, catalog = make_session()

    assert session.run_test(
        "bus", {"bus_mode": "loop", "link_id": 3, "total_count": 100000}
    )
    assert len(channel.sent_payloads) == 1

    values = decode_payload(
        catalog.get("bus_loop_test_request"),
        channel.sent_payloads[0],
    )
    assert values["link_id"] == 3
    assert values["total_count"] == 100000


def test_bus_echo_sends_one_padded_one_hundred_fourteen_byte_request() -> None:
    session, channel, catalog = make_session()

    assert session.run_test(
        "bus", {"bus_mode": "echo", "link_id": 1, "data_hex": "4D 42 31"}
    )
    assert len(channel.sent_payloads) == 1

    values = decode_payload(
        catalog.get("bus_echo_test_request"), channel.sent_payloads[0]
    )
    assert values["link_id"] == 1
    assert [values["data[{}]".format(index)] for index in range(3)] == [
        0x4D,
        0x42,
        0x31,
    ]
    assert values["data[3]"] == 0
    assert values["data[113]"] == 0


def test_timer_mode_is_validated_before_serial_send() -> None:
    for invalid_mode in (2, -1, "invalid"):
        session, channel, _ = make_session()

        assert not session.run_test("timer", {"mode": invalid_mode})
        assert channel.sent_payloads == []
        assert session.status_for("timer") == TestStatus.EXECUTION_FAILED


def test_timer_mode_one_is_encoded_and_sent() -> None:
    session, channel, catalog = make_session()

    assert session.run_test("timer", {"mode": 1})
    assert len(channel.sent_payloads) == 1
    values = decode_payload(
        catalog.get("timer_jitter_start_request"),
        channel.sent_payloads[0],
    )
    assert values["mode"] == 1


def test_error_response_must_match_command_and_request_sequence() -> None:
    session, channel, catalog = make_session()
    assert session.run_test("system")
    request_sequence = session.active_sequence
    emit_response(
        channel,
        response(
            catalog,
            "error_response",
            {
                "orig_tg": 0x01,
                "orig_st": 0x01,
                "orig_seq": request_sequence,
                "err_code": 0x0102,
                "detail": 7,
            },
            sequence=(request_sequence + 1) & 0xFFFF,
        ),
    )
    assert session.status_for("system") == TestStatus.RUNNING

    emit_response(
        channel,
        response(
            catalog,
            "error_response",
            {
                "orig_tg": 0x01,
                "orig_st": 0x01,
                "orig_seq": request_sequence,
                "err_code": 0x0102,
                "detail": 7,
            },
            sequence=request_sequence,
        ),
    )

    assert session.status_for("system") == TestStatus.EXECUTION_FAILED


def test_invalid_bus_echo_hex_fails_without_leaving_session_busy() -> None:
    session, channel, _ = make_session()

    assert not session.run_test(
        "bus", {"bus_mode": "echo", "link_id": 0, "data_hex": "GG"}
    )

    assert session.status_for("bus") == TestStatus.EXECUTION_FAILED
    assert not session.is_busy
    assert channel.sent_payloads == []


def test_stop_cancels_current_test_and_execute_all_queue() -> None:
    session, channel, _ = make_session()
    assert session.run_all()
    assert session.status_for("system") == TestStatus.RUNNING

    session.stop()

    assert session.status_for("system") == TestStatus.EXECUTION_FAILED
    assert not session.response_timer.isActive()
    assert len(channel.sent_payloads) == 1


def test_disabling_session_stops_execute_all_between_items(qtbot) -> None:
    session, channel, catalog = make_session()
    assert session.run_all()
    emit_active_response(session, channel, catalog, "system_status_response")
    assert session.active_test_key is None
    assert session.is_busy

    session.set_enabled(False)

    assert not session.is_busy
    assert session._run_all_queue == []
    assert not session._next_all_timer.isActive()
    assert len(channel.sent_payloads) == 1


def test_execute_all_advances_in_fixed_order_including_composite_tests(qtbot) -> None:
    session, channel, catalog = make_session()
    started = []
    session.test_started.connect(started.append)
    response_names = {
        "system": "system_status_response",
        "memory": "memperf_test_response",
        "spi_flash": "spi_flash_test_response",
        "bus": "bus_loop_test_response",
        "di": "di_read_response",
        "do": "do_write_response",
        "electrical_health": "elec_health_status_response",
        "dh_pulse_config": "dh_pulse_config_response",
        "dh": "dh_control_response",
        "helm_board": "helm_board_test_response",
    }

    assert session.run_all()
    for key in TEST_ORDER:
        qtbot.waitUntil(lambda key=key: session.active_test_key == key, timeout=1000)
        if key == "timer":
            emit_active_response(
                session, channel, catalog, "timer_jitter_start_response"
            )
            emit_active_response(
                session, channel, catalog, "timer_jitter_stop_response"
            )
        elif key == "dh":
            request_values = decode_payload(
                catalog.get("dh_control_request"), channel.sent_payloads[-1]
            )
            assert request_values["report_count"] == 50
            assert session.active_sequence is not None
            request_sequence = session.active_sequence
            for report_index in range(request_values["report_count"]):
                emit_response(
                    channel,
                    response(
                        catalog,
                        "dh_control_response",
                        sequence=(request_sequence + report_index) & 0xFFFF,
                    ),
                )
        else:
            emit_active_response(session, channel, catalog, response_names[key])

    qtbot.waitUntil(lambda: not session.is_busy, timeout=1000)

    assert tuple(started) == TEST_ORDER
    assert all(session.status_for(key) == TestStatus.COMPLETED for key in TEST_ORDER)


def test_execute_all_does_not_double_advance_after_synchronous_send_rejection(
    qtbot,
) -> None:
    session, channel, _ = make_session()
    started = []
    session.test_started.connect(started.append)
    send_results = [False, True]

    def send_payload(payload):
        channel.sent_payloads.append(bytes(payload))
        return send_results.pop(0)

    channel.send_payload = send_payload

    assert session.run_all()
    qtbot.waitUntil(lambda: session.active_test_key == "memory", timeout=1000)
    qtbot.wait(10)

    assert started == ["system", "memory"]
    assert session._run_all_queue[0] == "spi_flash"
    session.stop()
