import re
from pathlib import Path


def test_helm_dds_endpoint_treats_publisher_write_as_boolean() -> None:
    """Publisher::write reports success, not a byte count."""
    dut_root = Path(__file__).resolve().parents[2]
    source = (
        dut_root / "src" / "MB_DDF_HW_Test" / "HelmDdsTestBridge.cpp"
    ).read_text(encoding="utf-8")

    assignment = re.search(
        r"const\s+(\w+)\s+(\w+)\s*=\s*command_writer_->write\(", source
    )
    assert assignment is not None
    result_type, result_name = assignment.groups()
    assert result_type == "bool"
    assert re.search(rf"if\s*\(\s*!\s*{re.escape(result_name)}\s*\)", source)


def test_helm_dds_endpoint_skips_feedback_retained_before_start() -> None:
    """Each HELM run must subscribe after the feedback sequence visible at start."""
    dut_root = Path(__file__).resolve().parents[2]
    source = (
        dut_root / "src" / "MB_DDF_HW_Test" / "HelmDdsTestBridge.cpp"
    ).read_text(encoding="utf-8")

    assert re.search(
        r'feedback_reader_\s*=\s*dds\.create_reader_after_current_sequence\(\s*'
        r'"local:://helm_feedback"',
        source,
    )
