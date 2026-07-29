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
