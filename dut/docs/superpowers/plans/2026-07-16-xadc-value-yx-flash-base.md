# XADC value_YX 与 Flash 基址更新 Implementation Plan

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将寄存器事实源切换到 `origin_v3`，新增 XADC `value_YX` 只读采样并接入 `AD_READ`，同时把 PCIe Flash 全局基址更新为 `0x160000`。

**Architecture:** XADC 作为独立 `MB_DDF_HW` Device，通过局部寄存器 `0x260` 读取原始 32 位值，提取 `Data[15:4]` 后按 `ADC_code / 4096 * 10.09` 换算。`HardwareTestProvider` 组合 XADC 与现有 AD7606 快照生成完整 `AD_READ` 响应；电气健康仍保持显式失败，直到剩余板级映射全部确认。PCIe Flash 继续通过 `Registers::Flash::UserBase` 集中维护全局窗口地址。

**Tech Stack:** C++20、CMake、GoogleTest、PowerShell、Python 3/openpyxl、AArch64 交叉编译。

---

### Task 1: 将寄存器导出事实源切换到 origin_v3

**Files:**
- Modify: `docs/design/xxm_ip_addr/tools/export_register_maps.py`
- Modify: `docs/design/xxm_ip_addr/tools/test_export_register_maps.py`
- Regenerate: `docs/design/xxm_ip_addr/generated/*`

- [ ] **Step 1: 更新导出器测试，要求默认源为 origin_v3**

```python
self.assertEqual(DEFAULT_SOURCE, base / "origin_v3")
```

- [ ] **Step 2: 添加 XADC 事实回归测试**

```python
def test_xadc_exports_value_yx_register_and_formula(self):
    workbook = Path(__file__).resolve().parent.parent / "origin_v3" / (
        "XADC IP核通用型地址分配表（公开） .xlsx"
    )
    records = parse_workbook(workbook)
    value_yx = next(record for record in records if record["byte_offset"] == 0x260)
    self.assertEqual(value_yx["access"], "read")
    self.assertIn("YXJB", value_yx["name"])
    self.assertIn("ADC_code * 1.0 / 4096", value_yx["description"])
    self.assertIn("10.09", value_yx["name"])
```

- [ ] **Step 3: 运行测试并确认 RED**

Run: `python -m unittest docs.design.xxm_ip_addr.tools.test_export_register_maps`

Expected: 默认源仍为 `origin_v2`，测试失败。

- [ ] **Step 4: 把默认源改为 origin_v3 并适配 v3 Flash 原表名称**

```python
DEFAULT_SOURCE = DEFAULT_BASE / "origin_v3"
```

- [ ] **Step 5: 重新生成规范化寄存器资产**

Run: `python .\docs\design\xxm_ip_addr\tools\export_register_maps.py`

Expected: 从 8 份 v3 工作簿导出，生成 `XADC.csv`，`registers.csv/json/md` 包含局部偏移 `0x260`。

### Task 1A: 同步 origin_v3 的 ADS1258 配置增量

**Files:**
- Modify: `src/MB_DDF_HW/Device/Registers/Ads1258Registers.h`
- Modify: `src/MB_DDF_HW/Device/Ads1258Device.h`
- Modify: `src/MB_DDF_HW/Device/Ads1258Device.cpp`
- Modify: `tests/hw_unit/test_ads1258_device.cpp`

- [ ] **Step 1: 添加 v3 新寄存器编译期断言和读写测试**

```cpp
static_assert(Registers::Ads1258::DrdyReadDelay == 0x68u);
Ads1258Config config{};
config.drdy_read_delay = 1250;
ASSERT_TRUE(device.configure(config));
EXPECT_EQ(transport.accesses().back().offset, 0x68u);
```

- [ ] **Step 2: 交叉编译并确认 RED**

Run: `powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests`

Expected: `DrdyReadDelay` 和 `drdy_read_delay` 尚未定义，编译失败。

- [ ] **Step 3: 扩展配置结构和非连续寄存器访问**

在原连续 21 个配置寄存器之后，单独读写局部 `0x68`；不得把中间的使能/状态寄存器误当成连续配置区。

- [ ] **Step 4: 重新交叉编译**

Run: `powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests`

Expected: exit code 0。

### Task 2: 新增 XADC Device 与换算测试

**Files:**
- Create: `src/MB_DDF_HW/Device/Registers/XadcRegisters.h`
- Create: `src/MB_DDF_HW/Device/XadcDevice.h`
- Create: `src/MB_DDF_HW/Device/XadcDevice.cpp`
- Create: `tests/hw_unit/test_xadc_device.cpp`
- Modify: `src/MB_DDF_HW/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 添加读取地址、位提取和换算测试**

```cpp
TEST(HwXadcDevice, ReadsValueYxFromVaux8) {
    RecordingTransport transport;
    ASSERT_TRUE(transport.open());
    transport.preset(0x260, 0xABCD5A5Fu);
    XadcDevice device(transport);
    const auto result = device.read_value_yx();
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().adc_code, 0x5A5u);
    EXPECT_DOUBLE_EQ(result.value().voltage,
                     static_cast<double>(0x5A5u) * 10.09 / 4096.0);
    EXPECT_EQ(transport.accesses().back().offset, 0x260u);
}
```

- [ ] **Step 2: 交叉编译并确认 RED**

Run: `powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests`

Expected: `XadcDevice` 尚不存在，编译失败。

- [ ] **Step 3: 定义 XADC 地址和只读测量接口**

```cpp
namespace MB_DDF::HW::Registers::Xadc {
inline constexpr uint64_t UserBase = 0x150000;
inline constexpr uint64_t Status = 0x004;
inline constexpr uint64_t ValueYx = 0x260;
}

struct XadcVoltageSample {
    uint16_t adc_code{};
    double voltage{};
};
```

- [ ] **Step 4: 实现低 16 位高 12 位提取与单极性换算**

```cpp
const uint16_t code = static_cast<uint16_t>((raw.value() >> 4u) & 0x0FFFu);
return XadcVoltageSample{code, static_cast<double>(code) * 10.09 / 4096.0};
```

- [ ] **Step 5: 把新 Device 和测试加入 CMake**

```cmake
Device/XadcDevice.cpp
${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_xadc_device.cpp
```

### Task 3: 接入 AD_READ 并保持电气健康隔离

**Files:**
- Modify: `src/MB_DDF_HW_Test/HardwareTestProvider.cpp`
- Modify: `src/MB_DDF_HW_Test/HardwareTestProviderDetail.h`
- Modify: `tests/hw_unit/test_hardware_test_provider.cpp`

- [ ] **Step 1: 添加响应填充回归测试**

```cpp
std::array<int16_t, 8> ad7606_raw{};
ad7606_raw[1] = 32767;
ad7606_raw[2] = -32768;
ad7606_raw[3] = 16384;
EXPECT_EQ(Detail::populate_ad_read_response(0x0800, ad7606_raw, response),
           ProductErrorCode::Ok);
EXPECT_EQ(response.get_signed("value_YX"), 0x0800);
EXPECT_EQ(response.get_signed("helm_AD_value[1]"), 32767);
EXPECT_EQ(response.get_signed("helm_AD_value[2]"), -32768);
EXPECT_EQ(response.get_signed("helm_AD_value[3]"), 16384);
```

- [ ] **Step 2: 运行目标并确认 RED**

Run: `powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests`

Expected: `populate_ad_read_response` 尚未定义，编译失败。

- [ ] **Step 3: 实现 AD 响应填充**

```cpp
response.set_signed("value_YX", value_yx_adc_code);
response.set_signed(field_name, raw[channel]);
```

后续确认采用 FPGA 原始码传输：`value_YX` 的 CSV LSB 为 `10.09/4096 V/code`，四路 AD7606 的 CSV LSB 为 `10/65536 V/code`。

- [ ] **Step 4: 在 Provider 中依次读取 XADC 和 AD7606**

```cpp
XdmaOpenDeviceContext<HW::XadcDevice> xadc{
    HW::Registers::Xadc::UserBase, HW::Registers::Xadc::WindowSize};
```

该上下文只打开窗口，不读取 XADC offset `0` 做 `0xAAAABBBB` 签名校验，因为该地址是
XADC 软件复位写寄存器。

`ad_read_request` 调用新的 `handle_ad()`；`elec_health_status_request` 继续返回 `TASK_EXEC_FAILED`，错误日志只说明尚未确认的电气健康字段。

### Task 4: 更新 Flash 基址与只读 smoke

**Files:**
- Modify: `src/MB_DDF_HW/Device/Registers/FlashRegisters.h`
- Modify: `tests/hw_unit/test_flash_device.cpp`
- Modify: `tests/hardware/test_mb_ddf_hw_smoke.cpp`

- [ ] **Step 1: 添加 Flash 全局基址断言**

```cpp
static_assert(Registers::Flash::UserBase == 0x160000u);
```

- [ ] **Step 2: 运行目标并确认 RED**

Run: `powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests`

Expected: 当前值仍为 `0x150000`，`static_assert` 使交叉编译失败。

- [ ] **Step 3: 更新集中式基址常量**

```cpp
inline constexpr uint64_t UserBase = 0x160000;
```

- [ ] **Step 4: smoke 增加只读 XADC 检查**

在 `0x150000` 打开 XADC，只执行 `value_YX` 读取；Flash 继续使用 `Registers::Flash::UserBase`，因此自动切换到 `0x160000`。

### Task 5: 同步当前设计与使用文档

**Files:**
- Modify: `docs/design/hardware-layer-architecture.md`
- Modify: `docs/design/product_protocol_csv/codedesign.md`
- Modify: `docs/design/product_protocol_csv/产品端-上位机通讯协议 V1.1（简版）.md`
- Modify: `docs/guides/demo-usage.md`
- Modify: `docs/tests/README.md`

- [ ] **Step 1: 更新 FPGA 地址窗口表**

记录 XADC=`0x150000/0x10000`、Flash=`0x160000/0x10000`，说明 v3 为当前原始寄存器事实源。

- [ ] **Step 2: 记录 value_YX 数据路径**

记录 `XADC VAUX8 -> local 0x260 -> Data[15:4] -> ADC_code -> ADC_code/4096*10.09 V -> AD_READ.value_YX`。

- [ ] **Step 3: 明确当前协议精度边界**

记录 `ad_read_response.csv` 直接承载 FPGA 原始码，并由 CSV LSB 在 PC 端恢复工程值。

- [ ] **Step 4: 更新 smoke 和剩余未确认项**

说明 XADC/Flash 均为只读 smoke；`AD_READ` 已具备完整来源。`ELEC_HEALTH_STATUS` 的模拟量映射均已确认，仍因 `activate_bits` 暂无来源而显式失败。

### Task 6: 验证与影响面审计

**Files:**
- Verify: all modified files

- [ ] **Step 1: 校验协议 CSV**

Run: `python .\tools\generate_product_protocol.py --check .\docs\design\product_protocol_csv`

Expected: exit code 0。

- [ ] **Step 2: 运行寄存器导出测试并检查生成物可重现**

Run: `python -m unittest docs.design.xxm_ip_addr.tools.test_export_register_maps`

Run exporter again, then `git diff --exit-code -- docs/design/xxm_ip_addr/generated` after recording the intended generated diff.

- [ ] **Step 3: 运行 AArch64 硬件测试构建**

Run: `powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests`

Expected: exit code 0；测试二进制只在目标板执行。

- [ ] **Step 4: 审计旧基址与旧事实源残留**

Run: `rg -n "0x150000|origin_v2|0x160000" src tests docs build.ps1 debug.ps1`

Expected: `0x150000` 只指 XADC，`0x160000` 只指 PCIe Flash；`origin_v2` 只允许出现在历史计划或明确的历史说明中。

- [ ] **Step 5: 刷新 CodeGraph**

Run: `codegraph sync .`

Expected: exit code 0，索引包含 XadcDevice 和新地址常量。
