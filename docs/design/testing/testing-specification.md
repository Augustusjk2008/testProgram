# 测试规范

> 本文是宿主工程当前唯一的测试事实源：定义测试清单、可复现验证方式、证据边界与已知门禁限制。它不记录按日期堆叠的历史执行流水，也不以静态数量代替一次实际测试结果。

## 1. 事实源与阅读边界

- 当前实现以公共 API、CMake 目标、CTest 注册和已核对的测试源码为准；本文件规定如何把它们转化为可审计证据。
- 架构与分层见[五层架构](../overview/five-layer-architecture.md)；BIZ、HAL、设备通信、日志和 WebSocket 语义分别以[业务调度契约](../contracts/business-scheduling-layer.md)、[HAL 契约](../contracts/hal-interface-protocol.md)、[设备通信契约](../contracts/device-communication-protocol.md)、[日志契约](../contracts/log-interface-protocol.md)和[WebSocket 前端契约](../contracts/websocket-frontend-protocol.md)为准。本文不重复其字段和时序细节。
- `dut/docs/design/product_protocol_csv/` 是唯一批准的协议资产。任何仓库外协议目录或产品端软件副本均无当前协议验证效力，不能替代或解释 `dut/`；工具链与真实硬件证据仍按本文第 5 节独立记录。
- 测试输出必须记录：提交或工作树状态、构建目录、配置（Debug/Release）、实际命令、协议资产目录、通过/失败/跳过数量和已知告警。未执行的测试不得写成通过。

## 2. 当前测试清单

根 CMake 在 `BUILD_TESTING=ON` 时加入 `tests/`。当前有七个 GoogleTest 可执行目标，构建后由 `gtest_discover_tests` 发现；应用目录另注册十个自退出的进程测试；NI-DAQmx Fake 另有一个 CTest。`front/` 的 Vitest 独立运行，不进入 CTest。

| 范围 | CMake/测试目标 | 测试源文件 | 源级 GoogleTest 定义 | 当前验证重点 |
| --- | --- | ---: | ---: | --- |
| `tests/hal/` | `hwtest_hal_tests` | 11 | 62 | HAL 会话、资源、安全态、Provider、Mock、C ABI 和 Adapter 路由 |
| `tests/log/` | `hwtest_log_tests` | 3 | 7 | 日志服务、JSONL sink、HAL 日志桥接 |
| `tests/biz/` | `hwtest_biz_tests` | 6 | 45 | 配置、计划、三种运行模式、调度、线程、状态和报告 |
| `tests/algorithm/` | `hwtest_algorithm_tests` | 16 | 135 | 协议 CSV、编解码、运行参数、串口、DH、DI、板级流程、流式算法和后处理 |
| `tests/app/` | `hwtest_app_tests`、`hwtest_gui_tests`、`hwtest_web_tests` | 18 | 225 | 控制器、配置发现与持久化、进程级日志文件、TUI/GUI/WebSocket、连续保存、fixture、分析与 Web 出站边界 |
| `src/adapters/ni_daqmx/tests/` | `hwtest_ni_daqmx_adapter_fake_tests` | 1 | 0 | 生产 Adapter 源码配合 Fake NIDAQmx 的软件路径 |

- 合计为 55 个测试源文件：54 个含 GoogleTest 定义的源文件和 1 个 NI Fake 自定义 main；GoogleTest 定义数为 474。
- 在一个已完成发现的构建树中，当前应有 485 条 CTest：474 条动态 GoogleTest、10 条应用进程测试和 1 条 NI Fake CTest。`ctest -N` 只核对该清单，不证明通过。
- 浏览器前端当前有 35 个 `*.test.ts`/`*.test.tsx` 文件和 150 条 Vitest；它们不计入 485 条 CTest。

### 2.1 当前配置与运行模式门禁

当前配置目录有 13 份 `mbddf_*.testcfg.json`，每份恰有一个启用步骤。配置和 descriptor 必须满足下表；应用控制器必须拒绝未声明的模式。

| 运行模式 | 当前配置 |
| --- | --- |
| `single` | `system_status`、`elec_health_status`、`serial_test`、`memperf`、`spi_flash`、`dh_pulse_config`、`timer_jitter`、`di_read`、`do_write`、`helm_board_test` |
| `pc_periodic`（同时支持 `single`） | `system_status`、`elec_health_status`、`memperf`、`timer_jitter`、`di_read` |
| `device_stream` | `dh_ignite_stream`、`imu_stream`、`helm_stream` |

- 同一配置不得同时声明 `pc_periodic` 和 `device_stream`；能力字段缺失时只安全回退到 `single`。
- 配置编辑回归至少锁定目录启停、禁用项不会被后续 `load` 复活、排队选择不回退、损坏目录拒绝启动、失败加载不改变既有存储绑定、SHA-256 陈旧 revision 不覆盖、testcfg 只读身份、产品工程师表单描述及默认值、工位叶子覆盖不改基础 HAL、只读串口/NI 候选发现与手工输入降级、目录与三份配置源在 `prepare()` 前的漂移检查、重载失败时返回 `config_reload_failed` 并恢复原文件，以及准备/运行期间拒绝保存。revision 回归不作为多进程强 CAS 证据；这些主机测试也不构成串口或 PXI 真机验收。
- `HELM_BOARD_TEST` 的 `automatic`/`manual` 是算法运行参数，不是 BIZ run mode。
- `DO_WRITE` 是一次用户选择的完整掩码交互；测试完成、停止、断开和退出均发送 **0 次额外复位帧**。回归必须锁定“一次用户掩码、零额外复位帧”，不得描述为多步流程。

## 3. 协议资产与可复现性

所有协议、算法、应用和跨层测试在执行前都必须显式绑定仓库快照：

```powershell
$env:MB_DDF_PROTOCOL_CSV_DIR = (Resolve-Path ".\dut\docs\design\product_protocol_csv").Path
if (-not (Test-Path $env:MB_DDF_PROTOCOL_CSV_DIR)) {
    throw "The approved MB_DDF protocol CSV snapshot is required"
}
```

- 当前部分测试源码在环境变量缺失时仍存在非仓库回退风险。因此，未显式设置上述变量的 CTest 结果一律不作为通过、失败或跳过证据。
- 不得用其他目录验证“当前协议”，也不得将其结果写入测试结论。
- 若协议资产、CSV 解析、配置中的 `protocolAssetRoot` 或协议字段发生变化，必须以该快照运行协议契约、算法和受影响的应用测试，并记录实际资产目录。

## 4. 当前可复现验证命令

### 4.1 宿主正式证据：独立 Debug/Release 构建树

同一多配置 `build_vs` 树的 GoogleTest discovery 可能在后一次构建后让 `ctest -C Debug` 指向 Release 可执行文件。因此，正式 Debug 与 Release 证据必须使用独立构建树；同一树上的混合发现结果不得标记为纯 Debug 或纯 Release 证据。

```powershell
$env:MB_DDF_PROTOCOL_CSV_DIR = (Resolve-Path ".\dut\docs\design\product_protocol_csv").Path
if (-not (Test-Path $env:MB_DDF_PROTOCOL_CSV_DIR)) {
    throw "The approved MB_DDF protocol CSV snapshot is required"
}

cmake -S . -B build_test_debug -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build_test_debug --config Debug --parallel
ctest --test-dir build_test_debug -C Debug -N -V
ctest --test-dir build_test_debug -C Debug --output-on-failure

cmake -S . -B build_test_release -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build_test_release --config Release --parallel
ctest --test-dir build_test_release -C Release -N -V
ctest --test-dir build_test_release -C Release --output-on-failure
```

- `-N -V` 必须显示与所测配置一致的测试可执行文件路径；不一致时停止并修复/重新发现，不得把该次执行归入目标配置。
- `GTEST_SKIP` 必须单列为未验证，不能并入通过数。
- 不得在本文中预先声明上述全量命令的结果；每次执行另行记录实际输出。

### 4.2 定向宿主回归

在已绑定协议快照的目标构建树中，可按变更范围运行定向 CTest。例如：

```powershell
ctest --test-dir build_test_debug -C Debug -R "^(RunParameterSchemaTest|MbddfProtocolTest|SystemStatusExecutorTest|TestApplicationControllerTest|WebProtocolTest|WebSocket)\." --output-on-failure
ctest --test-dir build_test_debug -C Debug -L websocket --output-on-failure
ctest --test-dir build_test_debug -C Debug -L ni_daqmx --output-on-failure
```

标签 `ni_daqmx` 只证明 Fake NIDAQmx 软件路径；`websocket` 测试使用自退出的本机回环服务，不授权启动用户常驻服务。

### 4.3 浏览器前端与文档

修改 `front/` 后必须运行：

```powershell
Push-Location .\front
npm test
npm run build
Pop-Location
```

前端测试和构建不等价于真实浏览器吞吐、长期稳定性或硬件验收。说明性文档改动至少运行：

```powershell
git diff --check
```

### 4.4 DUT 侧

`dut/` 使用自己的局部规则、构建树和验证入口。DUT 的交叉构建、主机侧 Python/PyQt、部署和板端运行是独立证据，不能并入宿主 CTest。PyQt 套件即使断言全部通过，`FigureCanvasQTAgg` teardown 仍可能出现 `RuntimeError`；出现该告警的运行不属于完全干净的门禁结果，必须单独记录并处理。

## 5. 证据层级与边界

| 层级 | 允许的证据 | 不能证明 |
| --- | --- | --- |
| 纯单元/协议 | CSV golden、脚本化传输、Fake executor、纯前端状态和解析 | 真机时序、真实串口、物理电平或实际 SDK 安装 |
| HAL Mock / Provider 集成 | Mock Adapter、动态 Fake C ABI、本机 Qt UDP、隔离 WebSocket | DUT、PXI、DDS、真实串口或台架安全性 |
| Vendor Adapter Fake | 同一 Adapter 源码配合 Fake NIDAQmx | NI SDK、MAX 身份、PXI-6259/PXI-6733 接线、输出或安全清零 |
| DUT 主机侧/交叉构建 | Python/PyQt、AArch64 交叉编译、协议资产校验 | 已部署板端运行、真实硬件行为或机械性能 |
| 真实硬件 | 经明确授权的隔离台架、独立记录和实际接线/仪器数据 | 默认 CTest、CI 或未经授权的自动化 |

- 自动化中的串口回环/回显、Qt UDP、Fake SDK、Mock、浏览器 Vitest 和交叉构建均不构成真实 COM、DH 点火、DDS 舵机、PXI 或目标板验收。
- 板级端点、安全态、隔离和真机前置条件以 HAL 契约为准；协议字段与帧布局以设备通信契约为准；浏览器 DTO、顺序和关闭语义以 WebSocket 契约为准。
- 舵机后处理的算法、协议和前端结果仅证明软件链路。五种波形、扫频、提前 STOP、四通道差异和性能指标仍需授权隔离台架的原始 TXT、结果 JSON、前端截图和独立参考测量。

## 6. 已知门禁限制

- 协议目录未显式绑定时存在非仓库回退风险；该风险修复前，未绑定快照的测试无证据效力。
- 同一 `build_vs` 树可能产生 Debug/Release discovery 串线；该风险修复前，正式双配置证据只能来自独立构建树。
- `TestConfigManagerTest.ImportedColleagueSampleLoadsAndBuildsThroughMigrationBoundary` 所需 fixture 未纳入仓库，缺失时会 `GTEST_SKIP`；它是未验证项，不是通过项，也不能用仓库外文件补成当前证据。
- 当前配置矩阵、MEMPERF/SPI Flash 的配置驱动执行回归仍应持续加强；单纯“发现并加载配置”不能替代运行模式、请求编码和判据的回归。
- 正常 CTest 不包含真实硬件标签或台架执行。任何硬件结论必须附带授权、环境、设备身份、接线、隔离、命令、日志和收尾记录。
- 浏览器性能测试尚不能替代 1 kHz/2 kHz 长时压力或 Chrome Performance 验收；该类验证需用户明确授权后在独立运行环境执行。

## 7. 变更准入

- 修复行为缺陷时，先写能复现问题的回归，再修改实现。
- 公共 HAL/BIZ API、Adapter ABI、配置字段、运行模式、错误码、资源类型或状态语义变化时，同步对应契约和回归测试。
- 改动配置时，至少覆盖 13 项配置矩阵、descriptor 投影、未声明运行模式拒绝，以及受影响算法的配置驱动执行路径。
- 改动协议、CSV 或协议资产引用时，必须绑定批准快照运行受影响测试；不得使用仓库外结果替代。
- 改动 DI、DO_WRITE、板级 fixture、`rawData.boardTest`、运行参数持久化或 WebSocket 时，覆盖契约边界、错误路径和前端纯逻辑；DO_WRITE 回归始终锁定一次用户掩码和零额外复位帧。
- 改动 BIZ、应用、GUI、WebSocket 或前端时，按受影响目标运行本文件第 4 节的定向测试；前端改动额外运行 Vitest 与生产构建。
