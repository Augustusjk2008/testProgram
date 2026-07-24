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

        self.assertEqual(DEFAULT_SOURCE, base / "origin_v3")
        self.assertEqual(DEFAULT_OUTPUT, base / "generated")

    def test_exports_v3_flash_register_rows(self):
        workbook = (
            Path(__file__).resolve().parent.parent
            / "origin_v3"
            / "FLASH IP核通用型地址分配表（公开） .xlsx"
        )

        records = parse_workbook(workbook)

        self.assertEqual(len(records), 14)
        self.assertEqual({record["ip_core"] for record in records}, {"FLASH"})
        self.assertNotIn("unknown", {record["access"] for record in records})
        self.assertEqual({record["source_workbook"] for record in records}, {workbook.name})
        self.assertTrue(
            any(
                record["access"] == "write"
                and record["byte_offset"] == 0x100
                and record["name"] == "写入flash写RAM数据"
                for record in records
            )
        )
        self.assertTrue(
            any(
                record["access"] == "read"
                and record["byte_offset"] == 0x000
                and record["name"] == "读取flash读RAM数据"
                for record in records
            )
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
            / "origin_v3"
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
        self.assertIn("5V_JS", js_5v["name"])
        self.assertIn("10.09", js_5v["name"])

    def test_ads1258_exports_channel_aware_voltage_calibration(self):
        workbook = (
            Path(__file__).resolve().parent.parent
            / "origin_v3"
            / "ADS1258 IP核通用型地址分配表（公开） .xlsx"
        )

        records = parse_workbook(workbook)
        samples = {
            record["byte_offset"]: record
            for record in records
            if record["access"] == "read"
            and record["byte_offset"] in {0x80, 0x84, 0x88, 0x8C, 0xFC}
        }

        self.assertEqual(set(samples), {0x80, 0x84, 0x88, 0x8C, 0xFC})
        self.assertIn("28.5V(C)", samples[0x80]["name"])
        self.assertIn("热电池激活", samples[0x84]["name"])
        self.assertIn("28.5V(B)", samples[0x88]["name"])
        self.assertIn("28.5V(1)", samples[0x8C]["name"])

        b_threshold = next(
            record for record in records
            if record["access"] == "write" and record["byte_offset"] == 0x50
        )
        self.assertIn("阈值寄存器使用系数 19.18", b_threshold["description"])
        self.assertIn("运行时通道 2 使用临时线性系数 18.6", b_threshold["description"])

        for sample in samples.values():
            description = sample["description"]
            self.assertIn(
                "a = (采样原始数据 & 0x00FFFFFF) * 4.096 / 0x780000",
                description,
            )
            self.assertIn("通道 1~3", description)
            self.assertIn("a * 18.6", description)
            self.assertIn("0x90~0xFC", description)
            self.assertIn("a <= 3", description)
            self.assertIn("-0.1594 * a^2 + 0.843 * a + 15.1", description)
            self.assertIn("a > 3", description)
            self.assertIn("a * 16.23", description)


if __name__ == "__main__":
    unittest.main()
