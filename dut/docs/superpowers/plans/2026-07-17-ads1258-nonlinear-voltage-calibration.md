# ADS1258 Nonlinear Voltage Calibration Implementation Plan

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在板端按通道完成 ADS1258 运放定标，并让板上电压与 23 路 DH 点火遥测通过协议 LSB 传输工程电压。

**Architecture:** `Ads1258Device` 保留原始快照，并提供独立的 ADC 输入电压与通道工程电压换算。产品协议从生成描述中的 LSB 进行有界定点编码，DH Provider 读取一次 ADS1258 快照后按固定通道表填充遥测；PC 端继续只按 CSV LSB 解码。`activate_bits` 仍无来源，电气健康请求保持明确失败且不返回猜测值。

**Tech Stack:** C++20、GoogleTest、Python 3.8/PyQt5/pytest、CSV 协议生成器、officecli、openpyxl 寄存器导出器、AArch64 交叉编译与目标板测试。

---

### Task 1: ADS1258 通道换算

**Files:**
- Modify: `tests/hw_unit/test_ads1258_device.cpp`
- Modify: `src/MB_DDF_HW/Device/Ads1258Device.h`

- [x] **Step 1: 写入失败测试**

  用 `adc_input_voltage()` 和 `channel_voltage()` 表达预期 API，覆盖高 8 位忽略、前三路 `19.18`、`a=3` 的多项式分支以及刚大于 `3` 的 `16.23` 分支：

  ```cpp
  constexpr uint32_t code_for_three_volts =
      static_cast<uint32_t>(3.0 * static_cast<double>(0x780000) / 4.096);
  EXPECT_NEAR(Ads1258Device::adc_input_voltage(0xAB780000u), 4.096, 1.0e-12);
  EXPECT_NEAR(Ads1258Device::channel_voltage(0, 0x00780000u),
              4.096 * 19.18, 1.0e-12);
  const double low_gain = -1.2128 * 3.0 * 3.0 + 4.8762 * 3.0 + 11.7818;
  EXPECT_NEAR(Ads1258Device::channel_voltage(3, code_for_three_volts),
              Ads1258Device::adc_input_voltage(code_for_three_volts) * low_gain,
              1.0e-9);
  EXPECT_NEAR(Ads1258Device::channel_voltage(3, code_for_three_volts + 1),
              Ads1258Device::adc_input_voltage(code_for_three_volts + 1) * 16.23,
              1.0e-9);
  ```

- [x] **Step 2: 运行 RED 验证**

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
  ```

  预期：编译失败，指出 `adc_input_voltage` / `channel_voltage` 尚不存在，证明测试命中新 API。

- [x] **Step 3: 实现最小换算接口**

  在 `Ads1258Device` 中实现：

  ```cpp
  inline static constexpr double ReferenceVoltage = 4.096;
  inline static constexpr uint32_t PositiveFullScaleCode = 0x780000u;
  inline static constexpr double FirstThreeChannelGain = 19.18;
  inline static constexpr double HighRangeGain = 16.23;

  static constexpr double adc_input_voltage(uint32_t data) noexcept {
      return static_cast<double>(data & 0x00FFFFFFu) * ReferenceVoltage /
             static_cast<double>(PositiveFullScaleCode);
  }

  static constexpr double channel_voltage(size_t channel, uint32_t data) noexcept {
      const double a = adc_input_voltage(data);
      const double gain = channel < 3
          ? FirstThreeChannelGain
          : a <= 3.0
              ? -1.2128 * a * a + 4.8762 * a + 11.7818
              : HighRangeGain;
      return a * gain;
  }
  ```

- [x] **Step 4: 重新构建并在目标板运行聚焦测试**

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
  .\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Tests -TestFilter 'HwAds1258Device.*'
  ```

  预期：交叉编译成功，目标板 ADS1258 单元测试通过。

### Task 2: 协议工程量定点编码与 LSB

**Files:**
- Modify: `tests/hw_unit/test_product_protocol.cpp`
- Modify: `src/MB_DDF_HW_Test/ProductProtocol.h`
- Modify: `src/MB_DDF_HW_Test/ProductProtocol.cpp`
- Modify: `docs/design/product_protocol_csv/elec_health_status_response.csv`
- Modify: `docs/design/product_protocol_csv/dh_control_response.csv`
- Modify: `test_pyqt/tests/test_protocol_catalog.py`
- Modify: `test_pyqt/tests/test_product_protocol.py`

- [x] **Step 1: 写入板端和 PC 端失败测试**

  板端测试要求 `set_scaled_signed()` 使用生成字段 LSB、四舍五入并拒绝非有限值或超量程值：

  ```cpp
  auto health = protocol.create_message("elec_health_status_response", false);
  ASSERT_TRUE(health.set_scaled_signed("c_volt", 28.505));
  EXPECT_EQ(health.get_signed("c_volt").value_or(0), 2851);
  EXPECT_FALSE(health.set_scaled_signed("c_volt", 400.0));

  auto dh = protocol.create_message("dh_control_response", false);
  ASSERT_TRUE(dh.set_scaled_signed("telemetry[0]", 12.3454));
  EXPECT_EQ(dh.get_signed("telemetry[0]").value_or(0), 12345);
  ```

  PC 测试要求 `c_volt/b_volt/v28_5` 的 LSB 为 `0.01`，23 个 `telemetry[]` 的 LSB 为 `0.001`，并验证编码后解码回工程电压。

- [x] **Step 2: 运行 RED 验证**

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
  $Py='C:\Users\JiangKai\.conda\envs\pyqt5_env\python.exe'
  $env:QT_QPA_PLATFORM='offscreen'
  & $Py -m pytest -q .\test_pyqt\tests\test_protocol_catalog.py .\test_pyqt\tests\test_product_protocol.py
  ```

  预期：C++ 因新方法缺失而失败，Python 因 CSV 仍默认 `LSB=1` 而失败。

- [x] **Step 3: 实现由生成描述驱动的有界编码**

  在 `ProductMessage` 增加：

  ```cpp
  bool set_scaled_signed(std::string_view field_name, double value);
  ```

  实现只接受 `S16F/S32F`、正且有限的 LSB 与有限工程值；计算 `value / field->lsb`，检查字段有符号范围，使用 `std::llround` 后调用 `set_signed()`，不做静默饱和。

- [x] **Step 4: 更新协议事实源**

  - `c_volt`、`b_volt`、`v28_5`：`lsb=0.01`
  - `telemetry[0..22]`：`lsb=0.001`
  - 其他非 ADS1258 电气健康字段保持现有定义，不借本次改动猜测新定标。

- [x] **Step 5: 生成检查并运行 GREEN 验证**

  ```powershell
  python .\tools\generate_product_protocol.py --check .\docs\design\product_protocol_csv
  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
  & $Py -m pytest -q .\test_pyqt\tests\test_protocol_catalog.py .\test_pyqt\tests\test_product_protocol.py
  ```

  预期：协议检查、交叉编译和 PC 聚焦测试全部通过。

### Task 3: DH 23 路 ADS1258 遥测接线

**Files:**
- Modify: `tests/hw_unit/test_hardware_test_provider.cpp`
- Modify: `src/MB_DDF_HW_Test/HardwareTestProviderDetail.h`
- Modify: `src/MB_DDF_HW_Test/HardwareTestProvider.cpp`

- [x] **Step 1: 写入失败映射测试**

  构造 32 路快照并要求：

  ```cpp
  inline constexpr std::array<size_t, 23> kDhTelemetryAds1258Channels{
      1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
      15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
  };
  ```

  `telemetry[0]` 使用 `raw[1]` 的前三路线性公式，`telemetry[1]` 与 `telemetry[22]` 分别使用 `raw[4]`、`raw[25]` 的分段公式，并按 `0.001 V/LSB` 写入协议整数。

- [x] **Step 2: 运行 RED 验证**

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
  ```

  预期：因 DH 遥测填充函数或通道表尚不存在而编译失败。

- [x] **Step 3: 实现映射与硬件读取**

  - 在 Detail 层增加 `populate_dh_telemetry(const HW::Ads1258Snapshot&, ProductMessage&)`，逐路调用 `Ads1258Device::channel_voltage()` 和 `set_scaled_signed()`。
  - 在 Provider 中增加 ADS1258 全局窗口 `0x20000` 的 `XdmaDeviceContext<Ads1258Device>`。
  - `handle_dh_control_report()` 读取 DH 状态后读取一次 ADS1258 快照，填满 23 路遥测；任一步失败返回对应协议错误。
  - 删除“23 路遥测电压源与定标尚未确认”的旧报错与置零逻辑。
  - `ELEC_HEALTH_STATUS` 仍因 `activate_bits` 无来源直接返回 `TASK_EXEC_FAILED`，不读取或发送部分健康数据。

- [x] **Step 4: 构建并运行目标板聚焦测试**

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
  .\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Tests -TestFilter 'HardwareTestProviderTest.*Ads1258*:*HardwareTestProviderTest.*DhTelemetry*'
  ```

  预期：映射与定点编码测试通过。

### Task 4: 更新 origin_v3 XLSX 与生成寄存器资料

**Files:**
- Modify: `docs/design/xxm_ip_addr/tools/test_export_register_maps.py`
- Modify: `docs/design/xxm_ip_addr/origin_v3/ADS1258 IP核通用型地址分配表（公开） .xlsx` (`Sheet1!D55`, merged through `D86`)
- Regenerate: `docs/design/xxm_ip_addr/generated/ADS1258.csv`
- Regenerate: `docs/design/xxm_ip_addr/generated/registers.csv`
- Regenerate: `docs/design/xxm_ip_addr/generated/registers.json`
- Regenerate: `docs/design/xxm_ip_addr/generated/registers.md`

- [x] **Step 1: 先改导出测试并验证 RED**

  测试要求生成描述同时包含 `0x80/0x84/0x88` 的 `19.18`、`a<=3` 多项式、`a>3` 的 `16.23` 以及 `a = (Data & 0x00FFFFFF) * 4.096 / 0x780000`。

  ```powershell
  python .\docs\design\xxm_ip_addr\tools\test_export_register_maps.py
  ```

  预期：旧工作簿仍含 `16.0` 和“其他通道运放系数暂无”，测试失败。

- [x] **Step 2: 使用 officecli 修改合并说明单元格**

  ```powershell
  $Book='.\docs\design\xxm_ip_addr\origin_v3\ADS1258 IP核通用型地址分配表（公开） .xlsx'
  officecli open $Book
  officecli set $Book '/Sheet1/D55' --prop value='回读，低24位有效，高8位为0。\n先计算运放前电压 a = (Data & 0x00FFFFFF) * 4.096 / 0x780000。\n芯片1通道0、1、2（地址0x80、0x84、0x88）：电压值 = a * 19.18。\n其余通道：a <= 3时，电压值 = a * (-1.2128a^2 + 4.8762a + 11.7818)；a > 3时，电压值 = a * 16.23。'
  officecli close $Book
  ```

- [x] **Step 3: 校验工作簿并重新导出**

  ```powershell
  officecli validate $Book
  officecli view $Book issues
  officecli view $Book annotated --start 55 --end 86
  officecli view $Book html
  python .\docs\design\xxm_ip_addr\tools\export_register_maps.py
  python .\docs\design\xxm_ip_addr\tools\test_export_register_maps.py
  ```

  预期：OpenXML 无错误，问题扫描无新增异常，导出仍为 544 条寄存器，测试通过。

### Task 5: 同步当前设计文档

**Files:**
- Modify: `docs/design/hardware-layer-architecture.md`
- Modify: `docs/design/product_protocol_csv/codedesign.md`
- Modify: `docs/design/product_protocol_csv/产品端-上位机通讯协议 V1.1（简版）.md`
- Modify: `docs/guides/demo-usage.md`

- [x] **Step 1: 替换旧定标与缺失来源描述**

  文档统一说明：

  - 低 24 位无符号码先换算为 `a`；`raw[0..2]` 用 `19.18`，其余用给定分段系数。
  - `c_volt/b_volt/v28_5` 为 `0.01 V/LSB`；`telemetry[]` 为 `0.001 V/LSB`。
  - DH 映射为 `raw[1]` 加 `raw[4..25]`。
  - DH 遥测来源已确认，不再置零报错；`activate_bits` 仍是电气健康唯一缺失来源。

- [x] **Step 2: 扫描旧说法**

  ```powershell
  rg -n '运放系数均为.?16\.0|其他通道运放系数暂无|23路遥测电压源与定标.*尚未确认|telemetry\[\].*置零' .\src .\tests .\docs .\test_pyqt
  ```

  预期：当前设计、代码和测试中无旧说法；历史 `docs/plan` 若保留，只注明为历史计划，不回写。

### Task 6: 全量生成、构建与目标板验证

**Files:**
- Verify only: all modified files
- Refresh: `.codegraph/codegraph.db`

- [x] **Step 1: 协议与 Python 全量验证**

  ```powershell
  python .\tools\generate_product_protocol.py --check .\docs\design\product_protocol_csv
  $Py='C:\Users\JiangKai\.conda\envs\pyqt5_env\python.exe'
  $env:QT_QPA_PLATFORM='offscreen'
  & $Py -m pytest -q .\test_pyqt\tests
  python .\docs\design\xxm_ip_addr\tools\test_export_register_maps.py
  ```

- [x] **Step 2: AArch64 构建**

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_test_debug
  ```

- [x] **Step 3: 目标板单元测试与只读 smoke**

  ```powershell
  .\tests\test-deploy.ps1 -RemoteHost 192.168.1.29 -TestBinaryName MB_DDF_HW_Tests
  .\tests\test-deploy.ps1 -RemoteHost 192.168.1.29 -TestBinaryName MB_DDF_HW_Smoke
  ```

  预期：目标板测试均通过；smoke 只读 ADS1258 快照，不触发 DH 点火或其他写操作。

- [x] **Step 4: 刷新导航索引并复核差异**

  ```powershell
  codegraph sync .
  git diff --check
  git status --short
  ```

  预期：CodeGraph 同步完成，`git diff --check` 无空白错误，差异仅包含本次换算、协议、事实源、生成资料、测试和文档。
