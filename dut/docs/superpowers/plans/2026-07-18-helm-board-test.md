# 舵控板级测试 Implementation Plan

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增一次性 `07/02 HELM_BOARD_TEST`，通过 COM3 产品协议写四路 PWM 0/1 和方向 0/1，并回读 PWM 状态与四路 AD7606；PC 当前测试入口改用该板级测试，现有舵控扫频协议和实现继续保留。

**Architecture:** CSV 仍是字段布局唯一事实源。板端 Provider 使用已有 `PwmDevice` 与 `Ad7606Device`，把 PWM 位映射为 `0/peak_value`，方向位直接映射为方向掩码，写入后读取两个设备状态并生成普通单帧响应。PC 将新测试作为普通请求纳入“执行全部”，旧 `helm` 会话与页面类只保留为兼容实现，不再出现在当前导航和执行顺序中。

**Tech Stack:** C++20、CMake/AArch64 GNU、CSV 生成描述、Python 3.8、PyQt5 5.15、pytest/pytest-qt。

---

### Task 1: 固化协议字段布局

**Files:**
- Create: `docs/design/product_protocol_csv/helm_board_test_request.csv`
- Create: `docs/design/product_protocol_csv/helm_board_test_response.csv`
- Modify: `tools/generate_product_protocol.py`
- Test: `test_pyqt/tests/test_protocol_catalog.py`
- Test: `tests/hw_unit/test_product_protocol.cpp`

- [ ] **Step 1: 先写协议目录与 C++ 描述失败用例**

验证目录包含 36 份 CSV，且 `helm_board_test_request/response` 都是 `0x07/0x02`；请求字段为 `pwm_level[0..3]`、`direction[0..3]`，响应包含对应回读位、四路 duty、peak、四路 `helm_AD_value` 及设备使能状态。

- [ ] **Step 2: 运行失败校验**

```powershell
$Py='C:\Users\JiangKai\.conda\envs\pyqt5_env\python.exe'
& $Py -m pytest -q .\test_pyqt\tests\test_protocol_catalog.py
```

预期：新描述不存在或协议资产数量仍为 34，测试失败。

- [ ] **Step 3: 添加 53 字节请求/响应 CSV**

请求 `B9` 的低四位依次为 `pwm_level[0..3]`，高四位依次为 `direction[0..3]`。响应使用 `B9=status`、`B10-11=err_code`、`B12` 八个回读位、`B13-28=pwm_duty[0..3]`、`B29-32=pwm_peak`、`B33-40=helm_AD_value[0..3]`、`B41-44` 四个状态字节，其余为零填充。

- [ ] **Step 4: 运行生成器和协议测试**

```powershell
& $Py .\tools\generate_product_protocol.py --check .\docs\design\product_protocol_csv
& $Py -m pytest -q .\test_pyqt\tests\test_protocol_catalog.py .\test_pyqt\tests\test_product_protocol.py
```

预期：全部通过，生成器报告 36 份 CSV。

### Task 2: 实现板端单次写入与回读

**Files:**
- Modify: `src/MB_DDF_HW_Test/HardwareTestProviderDetail.h`
- Modify: `src/MB_DDF_HW_Test/HardwareTestProvider.cpp`
- Modify: `src/MB_DDF_HW_Test/HardwareTestService.cpp`
- Test: `tests/hw_unit/test_hardware_test_provider.cpp`
- Test: `tests/hw_unit/test_hardware_test_service.cpp`

- [ ] **Step 1: 写失败的纯逻辑 Provider 用例**

新增可注入 `PwmDevice`/`Ad7606Device` 的 `Detail::run_helm_board_test()`，使用记录 Transport 验证：PWM 1 写当前 peak、PWM 0 写 0，方向先于 duty 写入，四路 enable 打开，update 最终打开，并把 AD7606 低 16 位有符号码原样写入响应。

- [ ] **Step 2: 运行 AArch64 编译确认新接口缺失**

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
```

预期：测试因 `run_helm_board_test` 尚未定义而编译失败。

- [ ] **Step 3: 实现最小板端流程**

流程固定为：拒绝活动中的旧舵控任务；打开 PWM/AD7606；打开 AD 采集和滤波；读取非零 peak；关闭 update；切换无符号 duty；按请求构造 `{0, peak}` duty 和方向掩码、`enable_mask=0x0F`；应用输出；重新打开 update；读取 PWM 与 AD7606 状态；填充响应。寄存器或回读不一致返回 `REG_RW_FAILED`，不自动恢复输出。

- [ ] **Step 4: 路由普通请求/响应**

`HardwareTestService::response_name()` 添加 `helm_board_test_request -> helm_board_test_response`，`HardwareTestProvider::handle()` 路由到新流程；响应继续回显请求序号并共用现有发送锁。

- [ ] **Step 5: 交叉编译验证**

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
```

预期：`MB_DDF_HW_Tests` 与 `MB_DDF_HW_Smoke` 均构建成功。

### Task 3: 替换 PC 当前舵控测试入口

**Files:**
- Modify: `test_pyqt/hardware_test_session.py`
- Modify: `test_pyqt/hardware_test_pages.py`
- Modify: `test_pyqt/tests/test_hardware_test_session.py`
- Modify: `test_pyqt/tests/test_hardware_test_pages.py`
- Modify: `test_pyqt/tests/test_main_window.py`

- [ ] **Step 1: 写失败的会话和页面用例**

断言 `TEST_ORDER` 使用 `helm_board` 而不是 `helm`，执行全部发送 `helm_board_test_request`；页面可独立选择四路 PWM/方向位，并按通道展示 PWM 回读、raw duty 和 AD 电压。旧 `run_test("helm")` 与 `HelmTestPage` 测试继续保留。

- [ ] **Step 2: 运行 PC 测试确认失败**

```powershell
$env:QT_QPA_PLATFORM='offscreen'
& $Py -m pytest -q .\test_pyqt\tests\test_hardware_test_session.py .\test_pyqt\tests\test_hardware_test_pages.py .\test_pyqt\tests\test_main_window.py
```

预期：`helm_board` spec/page 尚不存在而失败。

- [ ] **Step 3: 实现普通单次会话与专用页面**

新增 `helm_board` spec，八个参数均限制为 0/1；导航只展示“舵控板级”，执行全部把它当普通请求。旧 `helm` spec 留在兼容查找表和状态表中，因此旧连续反馈及 STOP 清理逻辑不删除。页面使用复选框表达二值输出，并以四行表格显示请求值与回读值。

- [ ] **Step 4: 运行 PC 全量测试**

```powershell
$env:QT_QPA_PLATFORM='offscreen'
& $Py -m pytest -q .\test_pyqt\tests
```

预期：全部通过。

### Task 4: 同步现行设计和交付说明

**Files:**
- Modify: `AGENTS.md`
- Modify: `README.md`
- Modify: `docs/design/product_protocol_csv/codedesign.md`
- Modify: `docs/design/product_protocol_csv/产品端-上位机通讯协议 V1.1（简版）.md`
- Modify: `docs/design/product_protocol_csv/实际测试项(1).md`
- Modify: `docs/design/hardware-layer-architecture.md`
- Modify: `docs/guides/demo-usage.md`

- [ ] **Step 1: 记录板级写入语义**

说明 `07/02` 是单次板级测试、PWM 位映射 `0/peak`、方向独立、四路 enable、AD7606 回读、与活动旧舵控任务互斥、成功后不自动恢复输出；旧 `07/01/10/11` 继续存在但不在 PC 当前测试入口或执行全部中。

- [ ] **Step 2: 最终静态校验和构建**

```powershell
& $Py .\tools\generate_product_protocol.py --check .\docs\design\product_protocol_csv
& $Py -m pytest -q .\test_pyqt\tests
powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_test_debug
powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests
git diff --check
```

预期：命令退出码均为 0；C++ 测试二进制只交叉编译，实际执行仍需部署到隔离目标板。

- [ ] **Step 3: 按用户要求提交**

```powershell
git add -- AGENTS.md README.md tools/generate_product_protocol.py `
  docs/design/product_protocol_csv docs/design/hardware-layer-architecture.md `
  docs/guides/demo-usage.md src/MB_DDF_HW_Test test_pyqt tests/hw_unit `
  docs/superpowers/plans/2026-07-18-helm-board-test.md
git commit -m "feat(hw-test): 增加舵控板级测试"
```
