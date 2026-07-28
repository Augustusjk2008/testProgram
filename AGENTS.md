# 仓库规范

最终答复使用中文。

## 项目定位与事实源

- 本仓库同时维护 Windows/Qt 宿主测试程序与 `dut/` 中的 MB_DDF_v2 嵌入式快照。根规则覆盖宿主工程，`src/hal/AGENTS.md` 与 `dut/AGENTS.md` 分别覆盖对应子树。
- 架构总览以 `docs/design/overview/five-layer-architecture.md` 为准；BIZ、HAL、设备通讯、WebSocket 前端和日志契约分别位于 `docs/design/contracts/`；测试清单、统计和验证证据统一记录在 `docs/design/testing/testing-specification.md`。
- 公共 API、CMake 目标、测试注册和已核对源码共同描述当前实现；运行记录统一引用测试规范。
- 代码与文档出现差异时，先按分层、安全和公共契约判断；实现合理时同步事实源，实现偏离契约时修正代码并补回归测试。
- `docs/plan/` 与 `docs/superpowers/plans/` 保存历史执行记录。历史方案保留原文，并在顶部链接现行事实源。
- 审查、搜索和统计聚焦源码与事实源，默认过滤一般 `tmp/` 内容、`build*/`、`cmake-build*/`、`out/` 和 `.git/`；`tmp/helm_control/` 是需保留使用的例外目录，相关任务中按需纳入，不得随其他 `tmp/` 内容清理。
- DUT 快照来源与导入提交记录统一维护在 `docs/design/README.md`。

## 当前项目

- 当前宿主开发与验证以 `hwtest_web` 和 `front/` 组成的 Web 链路为主基线。新增功能和行为变更默认优先在 Web 入口实现并完成验证；待 Web 基线开发验证完毕后，再按实际需求完善 TUI 和 Qt GUI，除非当前任务明确要求同步修改。
- 根入口 `CMakeLists.txt` 使用 C++17，先探测 Qt 5.15 Core/Network/SerialPort/Widgets，再要求 Qt 5.15 WebSockets；基础 Qt 5 选择失败时进入同一组件集的 Qt 6 fallback。
- HAL 位于 `src/hal/`，目标为 `hwtest_hal`。控制资源通过 `qt.serial` 或 `qt.udp` Provider 路由；其他设备通过 `AdapterRouter` 路由到 `MockAdapter` 或 C ABI v1 Adapter。HAL 以版本化 `AdapterDeviceOpenSpec` 向厂家 Adapter 投影单设备身份、资源、任务档案和安全态。
- `src/adapters/ni_daqmx/` 在 `HWTEST_ENABLE_NI_DAQMX=ON` 时构建 PXI-6259 NI-DAQmx Adapter。核心 ABI v1 保持稳定，可选 task ABI v1 与 `ISampleTaskIo` 提供 AI/AO/DI/DO、有限/连续采样、时钟、start/reference/pause 触发、边沿计数和频率脉冲输出；Fake NI-DAQmx 覆盖软件路径。
- 日志模块位于 `src/logging/`：`hwtest_log_types` 提供仅依赖 Qt Core 的值类型，`hwtest_log` 提供日志服务、文件 sink 和 HAL 日志桥接。
- BIZ 位于 `src/biz/`，目标为 `hwtest_biz`，负责配置、拓扑排序、运行模式、重试、状态、结果和报告。工作任务运行于带 Qt event dispatcher 的 `QThread`。
- 算法位于 `src/algorithm/`，目标为 `hwtest_algorithm_mbddf`，提供 MB_DDF CSV 编解码和九项已注册算法：`mbddf.system_status`、`mbddf.elec_health_status`、`mbddf.memperf`、`mbddf.spi_flash`、`mbddf.dh_pulse_config`、`mbddf.timer_jitter`、`mbddf.di_read`、`mbddf.imu_stream`、`mbddf.helm_stream`。
- 九份 `configs/mbddf_*.testcfg.json` 中，前七项声明 `single`，系统状态、电气健康、内存、定时器和 DI 还声明 `pc_periodic`；惯测与舵机连续实测只声明 `device_stream`。BIZ 提供 `single`、`pc_periodic` 和 `device_stream` 三种通用运行语义，配置能力以各 JSON 的 `reportFields.supportedRunModes` 为准；同一配置不得同时声明 `pc_periodic` 与 `device_stream`，字段缺失时只安全回退到 `single`，应用控制器拒绝启动未声明的模式。
- 电气健康以设备 `status`/`err_code` 判定；SPI Flash 写入固定隔离测试区并保留写入结果；`TIMER_JITTER` 在结束阶段发送 STOP 清理。
- `DiStimulusController` 通过 HAL 批量数字输出维护 16 路逻辑掩码、revision 和安全复位；PXI-6259 配置中的物理 safe state 与 DI descriptor 的 inactive level 保持一致。
- 应用层位于 `src/app/`，共享 `hwtest_app_core`、`hwtest_tui_support`、`hwtest_gui_support` 和 `hwtest_web_support`，产出 `hwtest_pc_runner`、`hwtest_tui`、`hwtest_gui` 与 `hwtest_web`。四个入口统一使用 `TestApplicationController` 和 MB_DDF 注册表；启动选项动态扫描并校验 `configs/*.testcfg.json`。`ContinuousDataRecorder` 在显式启用的连续任务中按固定元数据和 descriptor 测量列增量保存 UTF-8-SIG/TSV TXT，`single` 不保存。
- `hwtest_web` 提供回环 WebSocket v1 服务；`front/` 提供独立的 React/Vite 遥测控制台，支持配置选择、运行控制、两种连续模式、算法运行参数编辑、完整 descriptor 测量列保存、动态测量字段和 16 路 DI 刺激/回读。运行期覆盖只作用于本次启动，浏览器按 `configId + runParameterSchemaVersion` 保存编辑值；保存目录只由后端配置，曲线选择不改变保存列。
- 舵机连续实测由 DUT 以 1 ms 周期经 DDS 连接用户独立启停的 `MB_DDF_v2_HelmControl`；它与 `HELM_BOARD_TEST 07/02` 不建立生命周期、互斥、忙状态或进程控制绑定。DUT/Web 不设置舵角业务范围，舵控程序保留内部限幅；当前保存原始样本，不计算性能或伯德图。
- `tests/` 按 HAL、日志、BIZ、算法和应用组织 GoogleTest 目标；NI Fake Adapter 使用独立 CTest 入口；`front/` 的 Vitest 独立运行。测试清单与统计以测试规范为准。
- `dut/docs/design/product_protocol_csv/` 保存当前 MB_DDF 协议 CSV 快照；`hwtest.ps1` 默认使用该目录，显式传入 `MB_DDF_PROTOCOL_CSV_DIR` 时可改用另一受控资产目录。

## 分层与 I/O 归属

```text
浏览器 Web UI -> 回环 WebSocket 后端；TUI / Qt GUI / batch CLI
  -> hwtest_app_core::TestApplicationController
  -> hwtest_biz
  -> biz::IAlgorithmExecutor（算法层实现）
  -> hwtest_hal
  -> Provider / Adapter
```

- `hwtest_app_core` 是组合根，统一管理配置、HAL 会话、算法执行器、BIZ 服务、DI 安全态和关闭顺序。
- TUI、Qt GUI、WebSocket 后端和浏览器 UI 消费应用层 DTO、动作结果、快照和样本事件。控制器动作在其 QObject 亲和线程执行；GUI 与 WebSocket 通过异步事件、`stopAsync()` 和 `stopCompleted` 观察停止过程。
- BIZ 保持硬件无关，仅依赖 Qt Core、`hwtest_log_types` 和自身公共模型，并通过 `biz::IAlgorithmExecutor` 调用算法。
- 算法层负责产品协议 CSV、编解码、序列、流程、判定及可编辑运行参数的 Schema/语义校验，并通过 HAL 请求设备生命周期与 I/O。应用层只投影 Schema、合并本次覆盖并传递规范化结果；BIZ 不解释参数。
- HAL 持有具体连接，执行原始 I/O、deadline、错误归一化和物理 safe state。`module = "control"` 的资源按 `providerId` 使用 `qt.serial` 或 `qt.udp`；其他资源按设备 `adapterId` 使用 Adapter 路径。
- 日志作为旁路基础模块；BIZ 使用 `hwtest::logging::LogEvent`，事件来源和 HAL/Adapter 映射由日志契约统一定义。
- 配置、日志和报告文件沿各自服务边界访问。纯协议与 golden 测试可直接使用 Simulator；产品模拟和集成验证沿 HAL 路径使用 HAL Mock 或隔离模拟目标。

## 配置与兼容

- 新增 BIZ 测试配置使用 `executionConfig` 传递算法执行参数；读取器继续支持旧根字段 `halConfig` 的迁移。
- BIZ 保存并透传 `ProtocolProfile`、`SafetyPolicy` 与硬件需求，协议解释由算法层完成。`SafetyPolicy.enterSafeStateOnStop/Error` 当前仅为兼容字段，不驱动运行期分支；HAL 在会话关闭前按已配置 safe state 尽力收尾。
- 厂家 Adapter 初始化配置只承载驱动级设置；设备身份、资源映射和 safe state 通过 `hwtest.adapter-device-open` v1 传入 `openDevice()`。NI JSON 解析集中在 `ni_daqmx_config.*`。
- 公共 HAL 和 BIZ 头文件属于兼容面；结构体优先在尾部扩展，既有枚举值与语义保持稳定。
- Adapter ABI 变更同步更新 `hal_adapter_abi.h`、可选 `hal_adapter_task_abi.h`、HAL 契约和 ABI 测试。
- 命名空间统一使用 `hwtest::hal`、`hwtest::logging`、`hwtest::biz`、`hwtest::algorithm::mbddf` 和 `hwtest::app`。

## 代码与文档

- 宿主代码使用 C++17 和 Qt 5.15 兼容 API，同时保持 Qt 6 fallback 可编译。
- 修改公共 API、配置字段、状态语义、错误码、构建依赖或测试边界时，同步对应事实源与契约测试。
- 跨文档概念保留一个主定义：总览写职责与依赖，契约写接口与语义，实现报告写落地细节，测试规范写验证边界，其他文档使用链接引用。
- 行为缺陷先以回归测试复现，再实施修复。

## 本地服务与前端

- `hwtest_web`、Vite 和其他常驻服务状态由用户在当前对话明确授权；常规验证使用自行退出的单元测试、集成测试和 smoke test，已有服务按只读方式检查。
- WebSocket 后端默认监听 `127.0.0.1:18765`。用户手动运行时，在仓库根目录以前台方式执行：

```powershell
.\hwtest.ps1 -Ui web -WebPort 18765
```

- 已有最新构建产物时可追加 `-SkipBuild`。浏览器前端由用户在第二个终端以前台方式运行：

```powershell
Set-Location .\front
npm ci
npm run dev
```

- 浏览器访问 `http://127.0.0.1:5173/`，默认连接 `ws://127.0.0.1:18765/ws`。自定义后端端口通过 `front/.env.local` 中的 `VITE_HWTEST_WS_URL` 配置。两个前台服务均使用 `Ctrl+C` 退出。
- 每次修改 `front/` 源码后，在 `front/` 目录运行 `npm test` 和 `npm run build`。构建会内联 JS、CSS 与 Geist 字体，并生成可直接打开的 `front/dist/index.html`。
- 单文件页面通过用户手动运行的 `hwtest_web` 获取实时遥测；文件来源的 `Origin: null` 保持在回环 WebSocket 白名单和契约测试中。

## 构建与验证

- Windows 集成验证入口为 `.\hwtest.ps1 test`。直接使用 CMake 时显式开启测试：

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build_vs --config Debug --parallel
$env:MB_DDF_PROTOCOL_CSV_DIR = (Resolve-Path ".\dut\docs\design\product_protocol_csv").Path
ctest --test-dir build_vs -C Debug --output-on-failure
cmake --build build_vs --config Release --parallel
ctest --test-dir build_vs -C Release --output-on-failure
```

- 直接运行 CTest 时显式设置 `MB_DDF_PROTOCOL_CSV_DIR`，核对其他受控来源时替换该目录；`GTEST_SKIP` 记为待验证证据。
- 当前 NI 证据等级为 Fake NI-DAQmx 自动化。PXI-6259 真机验收记录共地、电平、隔离、接线映射、NI SDK 版本、MAX 设备身份、采样/触发参数、长时运行和安全收尾结果。
- 修改 BIZ 时运行 BIZ 契约与架构测试；修改算法层时运行 `tests/algorithm/`；产品模拟与集成测试沿 HAL 路径执行。
- 说明性文档改动至少运行链接/术语检查和 `git diff --check`。
- 宿主工程与 DUT 使用独立构建树和验证入口。进入 `dut/` 后按局部 `AGENTS.md` 与 `README.md` 使用 `build.ps1`、`debug.ps1`、`tests/test-dds-only.ps1`、`tests/test-all.ps1` 和 `tests/test-deploy.ps1`。
- DUT C++20 二进制面向 AArch64 目标板；Windows PyQt5/QtSerialPort 用例按 DUT 局部规则作为主机侧测试入口。
- DUT 板端结果记录 AArch64 工具链、sysroot、部署目标与运行结果，并作为独立的板端验证证据归档。DIDO、DH、舵控和 SPI 写入在隔离且明确授权的目标板环境执行。

## CodeGraph

- 跨模块修改、架构分析、重构、调用链或影响面分析先用可用的 CodeGraph 工具；仅对未覆盖或将要修改的具体细节读源码。
- CodeGraph 不可用时直接使用 `rg` 和源码阅读；变更后以测试、日志和 `git diff` 验证。
- 大改动后运行 `codegraph sync .` 刷新索引。
