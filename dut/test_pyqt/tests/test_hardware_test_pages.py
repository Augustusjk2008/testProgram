from copy import deepcopy

import pytest

from test_pyqt.hardware_test_pages import (
    PAGE_CLASSES,
    BusTestPage,
    DhPulseConfigPage,
    DhTestPage,
    DiTestPage,
    DoTestPage,
    ElectricalHealthPage,
    HelmBoardTestPage,
    HelmTestPage,
    MemoryTestPage,
    SpiFlashTestPage,
    SystemStatusPage,
    TimerTestPage,
    create_test_page,
)
from test_pyqt.hardware_test_session import LEGACY_HELM_TEST_SPEC, TEST_SPECS
from test_pyqt.product_protocol import DecodedMessage, ProductProtocol
from test_pyqt.protocol_catalog import ProtocolCatalog


EXPECTED_PAGE_CLASSES = {
    "system": SystemStatusPage,
    "memory": MemoryTestPage,
    "spi_flash": SpiFlashTestPage,
    "bus": BusTestPage,
    "di": DiTestPage,
    "do": DoTestPage,
    "electrical_health": ElectricalHealthPage,
    "dh_pulse_config": DhPulseConfigPage,
    "dh": DhTestPage,
    "helm_board": HelmBoardTestPage,
    "helm": HelmTestPage,
    "timer": TimerTestPage,
}


@pytest.fixture(scope="module")
def catalog():
    return ProtocolCatalog.load_default()


def make_page(qtbot, catalog, key):
    spec = (
        LEGACY_HELM_TEST_SPEC
        if key == "helm"
        else next(item for item in TEST_SPECS if item.key == key)
    )
    page = create_test_page(spec, catalog)
    qtbot.addWidget(page)
    return page


def response(catalog, name, values=None, sequence=100):
    definition = catalog.get(name)
    merged = {"status": 0, "err_code": 0}
    if values:
        merged.update(values)
    return DecodedMessage(
        name=name,
        sequence=sequence,
        type_group=definition.type_group,
        sub_type=definition.sub_type,
        values=merged,
        payload=b"",
    )


def select_combo_data(combo, value):
    index = combo.findData(value)
    assert index >= 0, "控件缺少选项 {!r}".format(value)
    combo.setCurrentIndex(index)


def test_page_registry_and_factory_cover_all_specs(qtbot, catalog) -> None:
    assert PAGE_CLASSES == EXPECTED_PAGE_CLASSES

    pages = {}
    for spec in TEST_SPECS:
        page = create_test_page(spec, catalog)
        qtbot.addWidget(page)
        pages[spec.key] = page

    assert set(pages) == set(EXPECTED_PAGE_CLASSES) - {"helm"}
    assert all(
        isinstance(pages[key], expected_type)
        for key, expected_type in EXPECTED_PAGE_CLASSES.items()
        if key != "helm"
    )
    legacy_page = create_test_page(LEGACY_HELM_TEST_SPEC, catalog)
    qtbot.addWidget(legacy_page)
    assert isinstance(legacy_page, HelmTestPage)


def test_memory_parameters_match_protocol_request_fields(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "memory")
    select_combo_data(page.memory_type_combo, 4)
    page.length_spin.setValue(32768)
    page.seed_input.set_value(0x12345678)

    parameters = page.collect_parameters()
    outbound = ProductProtocol(catalog).build_request(
        "memperf_test_request", parameters
    )

    assert parameters == {
        "memperf_type": 4,
        "length": 32768,
        "seed": 0x12345678,
    }
    assert outbound.values["memperf_type"] == 4
    assert outbound.values["length"] == 32768
    assert outbound.values["seed"] == 0x12345678


def test_bus_loop_and_echo_parameters_are_kept_separate(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "bus")
    assert [page.link_combo.itemData(index) for index in range(page.link_combo.count())] == [
        0,
        1,
        3,
    ]
    assert page.count_spin.minimum() == 1
    assert page.count_spin.maximum() == 100000
    assert "仅通过 COM3 控制口" in page.bus_boundary_label.text()
    assert "外部 ECHO 回送端" in page.bus_boundary_label.text()
    select_combo_data(page.link_combo, 3)

    page.bus_mode_control.set_current_data("loop")
    page.count_spin.setValue(77)
    loop_parameters = page.collect_parameters()

    assert loop_parameters["bus_mode"] == "loop"
    assert loop_parameters["link_id"] == 3
    assert loop_parameters["total_count"] == 77

    page.bus_mode_control.set_current_data("echo")
    page.data_input.setText("4D 42 31 00 FF")
    echo_parameters = page.collect_parameters()

    assert echo_parameters["bus_mode"] == "echo"
    assert echo_parameters["link_id"] == 3
    assert echo_parameters["data_hex"] == "4D 42 31 00 FF"


def test_bus_echo_displays_all_actual_returned_bytes(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "bus")
    page.bus_mode_control.set_current_data("echo")
    page.data_input.setText("4D 42 31")
    page.collect_parameters()
    actual = bytes(range(114))

    page.render_response(
        response(
            catalog,
            "bus_echo_test_response",
            {"data[{}]".format(index): value for index, value in enumerate(actual)},
        )
    )

    displayed = page.received_data_edit.text().split()
    assert len(displayed) == 114
    assert displayed[:3] == ["00", "01", "02"]
    assert displayed[-3:] == ["6F", "70", "71"]


def test_bus_page_hides_legacy_repeat_control_and_points_to_root_scheduler(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "bus")

    assert page.continuous_button.isHidden()
    assert "pc_periodic" in page.bus_boundary_label.text()


def test_do_parameters_pack_sixteen_checks_and_fix_second_word_to_zero(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "do")
    for index in (0, 7, 15):
        page.channel_checks[index].setChecked(True)

    parameters = page.collect_parameters()
    outbound = ProductProtocol(catalog).build_request("do_write_request", parameters)

    assert parameters == {"channel[0]": 0x00008081, "channel[1]": 0}
    assert outbound.values["channel[0]"] == 0x00008081
    assert outbound.values["channel[1]"] == 0


def test_dido_pages_show_origin_v3_signal_names_and_protect_fixed_outputs(
    qtbot, catalog
) -> None:
    di_page = make_page(qtbot, catalog, "di")
    assert di_page.bit_grid.name_labels[0].text() == "DI0"
    assert di_page.bit_grid.name_labels[8].text() == "DI8"
    for signal in (
        "DI0 联锁、电气弹动（高有效）",
        "DI1 引信报警（高有效）",
        "DI2 引信起爆指令（高有效）",
        "DI3 锁相环锁定指示（高有效）",
        "DI8 投放允许（低有效）",
    ):
        assert signal in di_page.signal_legend.text()
    assert "0x140080" in di_page.bit_grid.name_labels[0].toolTip()

    do_page = make_page(qtbot, catalog, "do")
    assert do_page.channel_checks[3].text() == "DO3"
    assert do_page.channel_checks[4].text() == "DO4"
    for signal in (
        "DO0 舵锁使能",
        "DO1 数控衰减器控制",
        "DO2 数遥发送使能",
        "DO3 24V_EN（物理低使能）",
        "DO4 DYT_5V_EN（物理低使能）",
        "DO5 DI_EN1（无需控制，恒拉低）",
        "DO6 DO_EN（无需控制，恒拉低）",
    ):
        assert signal in do_page.signal_legend.text()
    assert not do_page.channel_checks[5].isEnabled()
    assert not do_page.channel_checks[6].isEnabled()
    assert "DI_EN1" in do_page.applied_bits.name_labels[5].toolTip()
    assert "bit=0" in do_page.applied_bits.name_labels[3].toolTip()

    do_page._set_all_channels(True)
    assert not do_page.channel_checks[5].isChecked()
    assert not do_page.channel_checks[6].isChecked()
    do_page.channel_checks[5].setChecked(True)
    do_page.channel_checks[6].setChecked(True)
    assert do_page.collect_parameters()["channel[0]"] & ((1 << 5) | (1 << 6)) == 0


def test_dh_pulse_configuration_defaults_and_round_trips_all_channels(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "dh_pulse_config")

    parameters = page.collect_parameters()
    outbound = ProductProtocol(catalog).build_request(
        "dh_pulse_config_request", parameters
    )

    assert parameters["config_enable"] == 1
    assert parameters["pulse_width[0]"] == 80
    assert [parameters["pulse_width[{}]".format(index)] for index in range(1, 23)] == [63] * 22
    assert outbound.values["pulse_width[22]"] == 63


def test_dh_pulse_configuration_disables_width_editors_when_config_is_off(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "dh_pulse_config")

    page.config_enable_check.setChecked(False)

    assert all(not editor.isEnabled() for editor in page.pulse_width_spins)
    assert page.collect_parameters()["config_enable"] == 0


def test_dh_parameters_pack_only_twenty_three_channels(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "dh")
    for channel_check in page.channel_checks:
        channel_check.setChecked(False)
    for index in (0, 7, 22):
        page.channel_checks[index].setChecked(True)
    page.power_enable_check.setChecked(True)
    page.return_enable_check.setChecked(False)
    page.report_count_spin.setValue(3)
    page.interval_spin.setValue(2500)
    page.delay_spin.setValue(400)

    parameters = page.collect_parameters()
    outbound = ProductProtocol(catalog).build_request(
        "dh_control_request", parameters
    )

    assert parameters == {
        "power_enable": 1,
        "return_enable": 0,
        "channel[0]": 0x00400081,
        "channel[1]": 0,
        "report_count": 3,
        "interval_us": 2500,
        "delay_us": 400,
    }
    assert outbound.values["channel[0]"] == 0x00400081
    assert outbound.values["channel[1]"] == 0


def test_dh_page_initial_selection_preserves_session_defaults(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "dh")

    parameters = page.collect_parameters()

    assert parameters["channel[0]"] == 0x007FFFFF
    assert parameters["channel[1]"] == 0
    assert parameters["power_enable"] == 1
    assert parameters["return_enable"] == 1
    assert parameters["report_count"] == 50
    assert parameters["interval_us"] == 2500
    assert page.report_count_spin.value() == 50
    assert page.interval_spin.minimum() == 2500
    assert page.interval_spin.value() == 2500


def test_dh_page_allows_the_output_directory_to_be_selected(
    qtbot, catalog, monkeypatch, tmp_path
) -> None:
    page = make_page(qtbot, catalog, "dh")
    monkeypatch.setattr(
        "test_pyqt.hardware_test_pages.QFileDialog.getExistingDirectory",
        lambda *_args: str(tmp_path),
    )

    page.save_directory_button.click()

    assert page.save_directory_edit.objectName() == "dhSaveDirectoryEdit"
    assert page.save_directory_button.objectName() == "dhSaveDirectoryButton"
    assert page.save_directory_edit.text() == str(tmp_path)
    assert page.saved_file_label.objectName() == "dhSavedFileLabel"


def test_common_page_places_checkable_continuous_button_next_to_run(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "system")
    requests = []
    page.continuous_requested.connect(
        lambda key, parameters, enabled: requests.append(
            (key, parameters, enabled)
        )
    )

    assert page.action_layout.indexOf(page.run_button) == 0
    assert page.action_layout.indexOf(page.continuous_button) == 1
    assert page.continuous_button.text() == "连续"
    assert page.continuous_button.isCheckable()

    page.continuous_button.click()
    assert requests == [("system", {}, True)]
    page.continuous_button.click()
    assert requests[-1] == ("system", {}, False)


def test_electrical_health_page_allows_continuous_output_directory_selection(
    qtbot, catalog, monkeypatch, tmp_path
) -> None:
    page = make_page(qtbot, catalog, "electrical_health")
    monkeypatch.setattr(
        "test_pyqt.hardware_test_pages.QFileDialog.getExistingDirectory",
        lambda *_args: str(tmp_path),
    )

    page.save_directory_button.click()

    assert page.save_directory_edit.objectName() == "electricalHealthSaveDirectoryEdit"
    assert page.save_directory_button.objectName() == "electricalHealthSaveDirectoryButton"
    assert page.save_directory_edit.text() == str(tmp_path)
    assert page.saved_file_label.objectName() == "electricalHealthSavedFileLabel"


def test_dh_page_saves_each_duplicate_report_only_once(
    qtbot, catalog, tmp_path
) -> None:
    page = make_page(qtbot, catalog, "dh")
    page.save_directory_edit.setText(str(tmp_path))
    decoded = response(
        catalog,
        "dh_control_response",
        {
            "power_enable_readback": 1,
            "return_enable_readback": 1,
            "dh_status.ch0": 1,
            "telemetry[0]": 12.345,
        },
        sequence=201,
    )
    page.reset_for_run()
    page.render_response(decoded)
    page.render_response(decoded)

    saved_path = page.save_reports("已完成", "已接收全部 DH 回告")

    assert len(page.reports) == 1
    assert saved_path.exists()
    assert page.saved_file_label.text() == str(saved_path)
    lines = [
        line
        for line in saved_path.read_text(encoding="utf-8-sig").splitlines()
        if line and not line.startswith("#")
    ]
    assert len(lines) == 24


def test_helm_board_page_serializes_outputs_and_renders_readback(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "helm_board")
    requested_duty = (25, 0, 75, 100)
    for spin, value in zip(page.pwm_duty_percent_spins, requested_duty):
        spin.setValue(value)
    page.direction_checks[1].setChecked(True)
    page.direction_checks[2].setChecked(True)

    assert all(spin.minimum() == 0 for spin in page.pwm_duty_percent_spins)
    assert all(spin.maximum() == 100 for spin in page.pwm_duty_percent_spins)
    assert all("%" in spin.suffix() for spin in page.pwm_duty_percent_spins)

    parameters = page.collect_parameters()
    outbound = ProductProtocol(catalog).build_request(
        "helm_board_test_request", parameters
    )

    assert [
        outbound.values["pwm_duty_percent[{}]".format(index)]
        for index in range(4)
    ] == list(requested_duty)
    assert [outbound.values["direction[{}]".format(index)] for index in range(4)] == [
        0,
        1,
        1,
        0,
    ]

    assert page.render_response(
        response(
            catalog,
            "helm_board_test_response",
            {
                "pwm_duty_match[0]": 1,
                "pwm_duty_match[1]": 1,
                "pwm_duty_match[2]": 1,
                "pwm_duty_match[3]": 1,
                "direction_readback[1]": 1,
                "direction_readback[2]": 1,
                "pwm_duty[0]": 250,
                "pwm_duty[1]": 0,
                "pwm_duty[2]": 750,
                "pwm_duty[3]": 1000,
                "pwm_peak": 1000,
                "pwm_enable_mask": 0x0F,
                "pwm_update_enabled": 1,
                "ad_acquisition_enabled": 1,
                "ad_filter_enabled": 1,
                "helm_AD_value[2]": -5.0,
            },
        )
    )
    assert float(page.readback_table.item(0, 1).text().strip(" %")) == 25.0
    assert page.readback_table.item(0, 2).text() != "不可用"
    assert float(page.readback_table.item(0, 3).text().strip(" %")) == 25.0
    assert page.readback_table.item(2, 2).text() != "不可用"
    assert float(page.readback_table.item(2, 3).text().strip(" %")) == 75.0
    assert page.readback_table.item(1, 5).text() == "1"
    assert page.readback_table.item(2, 6).text() == "-5 V"
    assert page.metrics.value("pwm_peak") == 1000
    assert page.metrics.value("pwm_enable_mask") == 0x0F


def test_helm_board_page_keeps_populated_diagnostics_on_readback_mismatch(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "helm_board")
    page.pwm_duty_percent_spins[2].setValue(75)
    page.collect_parameters()

    assert page.render_response(
        response(
            catalog,
            "helm_board_test_response",
            {
                "status": 1,
                "err_code": 0x0201,
                "pwm_duty_match[0]": 1,
                "pwm_duty_match[1]": 1,
                "pwm_duty_match[2]": 0,
                "pwm_duty_match[3]": 1,
                "pwm_duty[2]": 749,
                "pwm_peak": 1000,
                "pwm_enable_mask": 0x0F,
                "pwm_update_enabled": 1,
                "ad_acquisition_enabled": 1,
                "ad_filter_enabled": 1,
            },
        )
    )
    assert page.status_label.text() == "执行失败"
    assert page.readback_table.item(2, 2).text() == "不匹配"
    assert float(page.readback_table.item(2, 3).text().strip(" %")) == pytest.approx(
        74.9
    )
    assert page.metrics.value("pwm_peak") == 1000


def test_helm_parameters_serialize_waveform_and_four_channel_enable_bits(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "helm")
    select_combo_data(page.waveform_combo, 2)
    page.freq_spin.setValue(1.25)
    page.amplitude_spin.setValue(18.5)
    page.offset_spin.setValue(-2.0)
    page.phase_spin.setValue(0.75)
    page.max_freq_spin.setValue(4.5)
    page.channel_checks[0].setChecked(True)
    page.channel_checks[1].setChecked(False)
    page.channel_checks[2].setChecked(True)
    page.channel_checks[3].setChecked(True)

    parameters = page.collect_parameters()
    outbound = ProductProtocol(catalog).build_request(
        "helm_start_request", parameters
    )

    assert parameters == {
        "waveform": 2,
        "freq": 1.25,
        "ampl": 18.5,
        "offset": -2.0,
        "start": 0.75,
        "max_freq": 4.5,
        "enable": 0xD,
    }
    assert outbound.values["waveform"] == 2
    assert outbound.values["enable"] == 0xD


def test_legacy_helm_page_does_not_apply_a_product_angle_limit(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "helm")
    page.amplitude_spin.setValue(250.0)
    page.offset_spin.setValue(-100.0)

    parameters = page.collect_parameters()

    assert parameters["ampl"] == 250.0
    assert parameters["offset"] == -100.0


@pytest.mark.parametrize("mode", (0, 1))
def test_timer_parameters_only_serialize_supported_modes(
    qtbot, catalog, mode
) -> None:
    page = make_page(qtbot, catalog, "timer")
    page.mode_control.set_current_data(mode)

    parameters = page.collect_parameters()
    outbound = ProductProtocol(catalog).build_request(
        "timer_jitter_start_request", parameters
    )

    assert parameters == {"mode": mode}
    assert outbound.values["mode"] == mode


def test_rendering_one_page_does_not_change_another_page(qtbot, catalog) -> None:
    system_page = make_page(qtbot, catalog, "system")
    memory_page = make_page(qtbot, catalog, "memory")

    system_page.render_response(
        response(
            catalog,
            "system_status_response",
            {"cpu_usage": 27.5, "mem_usage": 41.0},
        )
    )

    assert system_page.last_response_name == "system_status_response"
    assert system_page.metrics.value("cpu_usage") == 27.5
    assert memory_page.last_response_name is None
    assert memory_page.last_values == {}

    memory_page.render_response(
        response(
            catalog,
            "memperf_test_response",
            {"error_count": 2, "elapsed_ms": 300},
        )
    )

    assert memory_page.last_response_name == "memperf_test_response"
    assert memory_page.metrics.value("error_count") == 2
    assert system_page.metrics.value("cpu_usage") == 27.5


@pytest.mark.parametrize(
    "key,response_name,metric_name,extra_values",
    (
        (
            "dh",
            "dh_control_response",
            "telemetry[0]",
            {
                "power_enable_readback": 1,
                "return_enable_readback": 1,
                "dh_status.ch0": 0,
                "telemetry[0]": 0,
            },
        ),
    ),
)
def test_failed_response_keeps_default_zero_measurements_unavailable(
    qtbot, catalog, key, response_name, metric_name, extra_values
) -> None:
    page = make_page(qtbot, catalog, key)
    values = {"status": 1, "err_code": 0x0203}
    values.update(extra_values)

    page.render_response(response(catalog, response_name, values))

    assert page.last_response_name == response_name
    assert page.last_values[metric_name] == 0
    assert not page.metrics.is_available(metric_name)


def test_failed_electrical_health_keeps_zero_measurements_and_bits_unavailable(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "electrical_health")

    page.render_response(
        response(
            catalog,
            "elec_health_status_response",
            {
                "status": 1,
                "err_code": 0x0203,
                "external_vol": 0,
                "activate_bits": 0,
            },
        )
    )

    assert not page.metrics.is_available("external_vol")
    assert page.activate_bits.count == 1
    assert page.activate_bits.name_labels[0].text() == "BC激活好"
    assert not page.activate_bits.is_available(0)


def test_dh_keeps_multiple_reports_and_selector_switches_visible_report(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "dh")
    first = response(
        catalog,
        "dh_control_response",
        {
            "power_enable_readback": 1,
            "return_enable_readback": 1,
            "dh_status.ch0": 1,
            "telemetry[0]": 110,
        },
        sequence=201,
    )
    second = response(
        catalog,
        "dh_control_response",
        {
            "power_enable_readback": 1,
            "return_enable_readback": 0,
            "dh_status.ch0": 2,
            "telemetry[0]": 220,
        },
        sequence=202,
    )

    page.render_response(first)
    page.render_response(second)

    assert page.reports == [first, second]
    assert page.report_selector.count() == 2
    page.report_selector.setCurrentIndex(0)
    assert page.metrics.value("telemetry[0]") == 110
    page.report_selector.setCurrentIndex(1)
    assert page.metrics.value("telemetry[0]") == 220


def test_dh_long_burst_batches_ui_and_keeps_all_channel_curves(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "dh")

    for report_index in range(120):
        values = {
            "dh_status.ch{}".format(channel): (report_index + channel) % 3
            for channel in range(23)
        }
        values.update(
            {
                "telemetry[{}]".format(channel): report_index + channel / 10.0
                for channel in range(23)
            }
        )
        page.render_response(
            response(
                catalog,
                "dh_control_response",
                values,
                sequence=1000 + report_index,
            )
        )

    assert len(page.reports) == 120
    assert page.report_selector.count() == page._IMMEDIATE_REPORTS

    page._flush_pending_ui()

    assert page.report_selector.count() == 120
    assert len(page.status_lines) == 23
    assert len(page.telemetry_lines) == 23
    assert all(len(series) == 120 for series in page.status_series)
    assert all(len(series) == 120 for series in page.telemetry_series)
    expected_last_time_ms = 119 * page.interval_spin.value() / 1000.0
    assert list(page.telemetry_lines[22].get_xdata())[-1] == pytest.approx(
        expected_last_time_ms
    )
    assert list(page.telemetry_lines[22].get_ydata())[-1] == pytest.approx(121.2)


def test_dh_status_and_enable_readbacks_use_protocol_labels(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "dh")
    decoded = response(
        catalog,
        "dh_control_response",
        {
            "power_enable_readback": 1,
            "return_enable_readback": 0,
            "dh_status.ch0": 0,
            "dh_status.ch1": 1,
            "dh_status.ch2": 2,
        },
    )

    page.render_response(decoded)

    assert page.power_readback_label.text() == "使能"
    assert page.return_readback_label.text() == "失能"
    assert page.report_table.item(0, 1).text() == "未 DH"
    assert page.report_table.item(1, 1).text() == "成功"
    assert page.report_table.item(2, 1).text() == "失败"


def test_failed_dh_report_keeps_readbacks_and_status_but_hides_telemetry(
    qtbot, catalog
) -> None:
    page = make_page(qtbot, catalog, "dh")

    page.render_response(
        response(
            catalog,
            "dh_control_response",
            {
                "status": 1,
                "err_code": 0x0203,
                "power_enable_readback": 1,
                "return_enable_readback": 0,
                "dh_status.ch0": 2,
                "telemetry[0]": 0,
            },
        )
    )

    assert page.power_readback_label.text() == "使能"
    assert page.return_readback_label.text() == "失能"
    assert page.report_table.item(0, 1).text() == "失败"
    assert page.report_table.item(0, 2).text() == "不可用"
    assert not page.metrics.is_available("telemetry[0]")


def test_spi_flash_page_keeps_f32_elapsed_seconds_semantics(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "spi_flash")

    page.render_response(
        response(catalog, "spi_flash_test_response", {"sjl_result": 1.25})
    )

    assert page.metric_definitions == (("sjl_result", "固定测试区耗时", "s"),)
    assert page.metrics.value("sjl_result") == 1.25


def test_electrical_health_uses_new_csv_field_names(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "electrical_health")
    expected = {
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
    }

    page.render_response(
        response(
            catalog,
            "elec_health_status_response",
            dict((name, index + 1) for index, name in enumerate(sorted(expected))),
        )
    )

    assert set(page.metrics.keys()) == expected
    assert all(page.metrics.is_available(name) for name in expected)


def test_electrical_health_only_displays_bc_activation_bit_zero(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "electrical_health")

    page.render_response(
        response(catalog, "elec_health_status_response", {"activate_bits": 0xFF})
    )

    assert page.activate_bits.count == 1
    assert len(page.activate_bits.cells) == 1
    assert page.activate_bits.name_labels[0].text() == "BC激活好"
    assert page.activate_bits.is_active(0) is True


@pytest.mark.parametrize(
    "ack_name", ("helm_start_response", "helm_stop_response")
)
def test_helm_start_and_stop_ack_do_not_overwrite_feedback_curve(
    qtbot, catalog, ack_name
) -> None:
    page = make_page(qtbot, catalog, "helm")
    feedback_values = {}
    for sample in range(10):
        feedback_values["zl[{}][0]".format(sample)] = sample * 10
        feedback_values["self_code[{}]".format(sample)] = sample
        for channel in range(4):
            feedback_values["fk[{}][{}]".format(sample, channel)] = (
                sample * 10 + channel + 1
            )

    page.render_response(
        response(catalog, "helm_feedback_response", feedback_values)
    )
    curve_before_ack = deepcopy(page.feedback_series)

    assert curve_before_ack
    assert any(curve_before_ack.values())
    page.render_response(response(catalog, ack_name))

    assert page.feedback_series == curve_before_ack


def test_timer_stop_ack_does_not_overwrite_start_statistics(qtbot, catalog) -> None:
    page = make_page(qtbot, catalog, "timer")
    start_values = {
        "buckets[{}]".format(index): (index + 1) * 10
        for index in range(8)
    }
    start_values.update({"avg_jitter": 3.25, "max_jitter": 12.5})

    page.render_response(
        response(catalog, "timer_jitter_start_response", start_values)
    )
    buckets_before_stop = list(page.bucket_values)

    page.render_response(response(catalog, "timer_jitter_stop_response"))

    assert page.bucket_values == buckets_before_stop
    assert page.metrics.value("avg_jitter") == 3.25
    assert page.metrics.value("max_jitter") == 12.5


def test_chart_labels_use_the_pinned_chinese_font(qtbot, catalog) -> None:
    helm_page = make_page(qtbot, catalog, "helm")
    timer_page = make_page(qtbot, catalog, "timer")

    assert helm_page.axes.xaxis.label.get_fontfamily() == ["Microsoft YaHei"]
    assert helm_page.axes.yaxis.label.get_fontfamily() == ["Microsoft YaHei"]
    assert all(
        text.get_fontfamily() == ["Microsoft YaHei"]
        for text in helm_page.axes.get_legend().get_texts()
    )
    assert timer_page.axes.xaxis.label.get_fontfamily() == ["Microsoft YaHei"]
    assert timer_page.axes.yaxis.label.get_fontfamily() == ["Microsoft YaHei"]
