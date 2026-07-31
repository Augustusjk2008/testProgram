import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from export_register_maps import (
    DEFAULT_OUTPUT,
    DEFAULT_SOURCE,
    core_name,
    parse_workbook,
    select_workbooks,
)


class RegisterWorkbookExportTest(unittest.TestCase):
    def test_default_cli_paths_use_the_versioned_sources_and_generated_directory(self):
        base = Path(__file__).resolve().parent.parent

        self.assertEqual(DEFAULT_SOURCE, base / "origin_v4")
        self.assertEqual(DEFAULT_OUTPUT, base / "generated")

    def test_select_workbooks_excludes_only_the_named_golden_image_workbook(self):
        source = Path(__file__).resolve().parent.parent / "origin_v4"
        golden = "GOLDEN_image_IP_VERSION IP核通用型地址分配表（公开） .xlsx"

        selected = select_workbooks(sorted(source.glob("*.xlsx")))

        self.assertEqual(len(selected), 9)
        self.assertNotIn(golden, {workbook.name for workbook in selected})
        self.assertEqual(
            {core_name(workbook) for workbook in selected},
            {
                "AD7606_HELM",
                "ADS1258",
                "COM",
                "DH_ctrl",
                "DIDO_ctrl",
                "FPGA_update_state",
                "PWM_ctrl",
                "UPDATE_image_IP_VERSION",
                "XADC",
            },
        )

    def test_prefers_the_operation_workbook_for_the_same_core(self):
        source = Path("synthetic")
        old = source / "FLASH IP核通用型地址分配表（公开） .xlsx"
        operation = source / "FLASH IP核通用型地址分配表_带操作（公开）.xlsx"
        excel_lock = source / "~$FLASH IP核通用型地址分配表_带操作（公开）.xlsx"

        selected = select_workbooks([old, operation, excel_lock])

        self.assertEqual(selected, [operation])
        self.assertEqual(core_name(operation), "FLASH")

    def test_xadc_exports_value_yx_register_and_formula(self):
        workbook = (
            Path(__file__).resolve().parent.parent
            / "origin_v4"
            / "XADC IP核通用型地址分配表（公开） .xlsx"
        )

        records = parse_workbook(workbook)
        value_yx = next(
            record for record in records
            if record["access"] == "read" and record["byte_offset"] == 0x260
        )

        self.assertEqual(value_yx["ip_core"], "XADC")
        self.assertIn("YXJB", value_yx["name"])
        self.assertIn("10.09", value_yx["name"])
        self.assertIn("ADC_code * 1.0 / 4096", value_yx["description"])

        js_5v = next(
            record for record in records
            if record["access"] == "read" and record["byte_offset"] == 0x240
        )
        self.assertIn("5V", js_5v["name"])
        self.assertNotIn("5V_JS", js_5v["name"])
        self.assertIn("10.09", js_5v["name"])

    def test_exports_v4_register_additions_and_ads1258_layout(self):
        base = Path(__file__).resolve().parent.parent
        ad7606 = parse_workbook(
            base / "origin_v4" / "AD7606_HELM IP核通用型地址分配表（公开） .xlsx"
        )
        pwm = parse_workbook(
            base / "origin_v4" / "PWM_ctrl IP核通用型地址分配表（公开） .xlsx"
        )
        workbook = (
            base
            / "origin_v4"
            / "ADS1258 IP核通用型地址分配表（公开） .xlsx"
        )

        records = parse_workbook(workbook)

        self.assertTrue(
            any(record["access"] == "write" and record["byte_offset"] == 0x44
                for record in ad7606)
        )
        self.assertTrue(
            any(record["access"] == "read" and record["byte_offset"] == 0x44
                for record in ad7606)
        )
        self.assertTrue(
            any(record["access"] == "write" and record["byte_offset"] == 0x3C
                for record in pwm)
        )
        self.assertTrue(
            any(record["access"] == "read" and record["byte_offset"] == 0x40
                for record in pwm)
        )

        diagnostics = {
            record["byte_offset"]
            for record in records
            if record["access"] == "read" and 0x100 <= record["byte_offset"] <= 0x124
        }
        errors = {
            record["byte_offset"]
            for record in records
            if record["access"] == "read" and 0x128 <= record["byte_offset"] <= 0x148
        }
        self.assertEqual(diagnostics, set(range(0x100, 0x128, 4)))
        self.assertEqual(errors, set(range(0x128, 0x14C, 4)))

    def test_generator_publishes_only_selected_v4_cores_and_cleans_stale_core_csvs(self):
        base = Path(__file__).resolve().parent.parent
        source = base / "origin_v4"
        script = Path(__file__).resolve().parent / "export_register_maps.py"

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            stale_flash = output / "FLASH.csv"
            stale_flash.write_text("obsolete\n", encoding="utf-8")

            completed = subprocess.run(
                [sys.executable, str(script), "--source", str(source), "--output", str(output)],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            payload = json.loads((output / "registers.json").read_text(encoding="utf-8"))
            core_names = {item["name"] for item in payload["ip_cores"]}
            self.assertEqual(payload["register_count"], 584)
            self.assertEqual(
                core_names,
                {
                    "AD7606_HELM",
                    "ADS1258",
                    "COM",
                    "DH_ctrl",
                    "DIDO_ctrl",
                    "FPGA_update_state",
                    "PWM_ctrl",
                    "UPDATE_image_IP_VERSION",
                    "XADC",
                },
            )
            self.assertFalse(stale_flash.exists())
            self.assertFalse((output / "GOLDEN_image_IP_VERSION.csv").exists())
            self.assertTrue((output / "FPGA_update_state.csv").is_file())
            self.assertTrue((output / "UPDATE_image_IP_VERSION.csv").is_file())
            self.assertTrue((output / "registers.csv").is_file())
            self.assertTrue((output / "registers.md").is_file())
            self.assertNotIn(b"\r\n", (output / "COM.csv").read_bytes())


if __name__ == "__main__":
    unittest.main()
