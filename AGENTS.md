# 仓库规范

最终答复使用中文。

## 适用范围与事实源

- 本文件适用于整个仓库；`src/hal/AGENTS.md` 对 `src/hal/` 子树具有更高优先级。
- 架构总览：`docs/design/overview/five-layer-architecture.md`。
- BIZ 契约：`docs/design/contracts/business-scheduling-layer.md`。
- HAL 契约：`docs/design/contracts/hal-interface-protocol.md`。
- 设备通讯契约：`docs/design/contracts/device-communication-protocol.md`。
- WebSocket 前端契约：`docs/design/contracts/websocket-frontend-protocol.md`。
- 日志契约：`docs/design/contracts/log-interface-protocol.md`。
- 测试规范：`docs/design/testing/testing-specification.md`。
- `[当前实现]` 只陈述公共 API、CMake 目标、测试注册和已核对源码事实；`[目标契约-未实现]` 是已批准的边界，不得写成已落地行为。
- 公共 API、CMake 目标和测试注册是“当前已实现行为”的代码事实。代码与文档冲突时，先判断代码是否违反分层、安全或公共契约；代码合理则同步文档，代码明显有缺陷则修代码并补回归测试。
- `docs/plan/`、`docs/superpowers/plans/` 是历史执行记录，不是现行接口事实源。
- 审查、搜索和统计默认忽略 `tmp/`、`build*/`、`cmake-build*/`、`out/` 和 `.git/`；不得把附件、生成物或旧构建结果当作仓库实现事实。
- `dut/` 是随仓库导入的嵌入式 MB_DDF_v2 被测对象（DUT）快照；该目录内的 `AGENTS.md` 对其子树具有更高优先级。DUT 的 C++20/AArch64 工程、协议 CSV、目标板测试和部署脚本与宿主 Qt/C++17 工程分开维护。
- DUT 快照来源为 `H:/Resources/RTLinux/Demos/MB_DDF_v2`；首次导入提交为 `32d961fbaccc3411378241dd1fa850d662354e4c`，当前快照来源提交为 `982b3f5bbce222aea061e9ce1523ba926c801658`。

## 当前实现

- 根构建入口：`CMakeLists.txt`，先查找 Qt 5.15 Core/Network/SerialPort/Widgets/WebSockets，失败后使用同一 Qt 6 组件集；若已选中 Qt 5 但缺少 WebSockets，则配置明确失败，不混用 Qt 6 模块。
- HAL：`src/hal/`，产物 `hwtest_hal`；公共头位于 `src/hal/include/hal/`，内部实现位于 `src/hal/src/`。
- 日志类型：`src/logging/` 中的 `hwtest_log_types`，只包含 `LogEvent` 等值类型和元类型函数，仅依赖 Qt Core，不得依赖 HAL。
- 日志服务：`src/logging/` 中的 `hwtest_log`，包含日志服务、文件 sink 和 HAL 日志桥接，可以依赖 `hwtest_hal`。
- 业务调度：`src/biz/`，产物 `hwtest_biz`；公共头位于 `src/biz/include/biz/`，实现位于 `src/biz/src/`。
- 算法：`src/algorithm/` 已提供 `hwtest_algorithm_mbddf`、MB_DDF CSV 协议编解码、`SYSTEM_STATUS`/`ELEC_HEALTH_STATUS` 固定执行器，以及可由配置驱动的单步交换执行器（当前接入 `MEMPERF_TEST`、`SPI_FLASH_TEST`、`DH_PULSE_CONFIG`、带 STOP 清理的 `TIMER_JITTER` 和 `DI_READ`）；`DiStimulusController` 通过 HAL 批量数字输出维护逻辑掩码、revision 和安全复位，命名空间为 `hwtest::algorithm::mbddf`。
- 应用：`src/app/` 提供共享 `hwtest_app_core`、`hwtest_tui_support`、`hwtest_gui_support`、`hwtest_web_support`，以及独立的 `hwtest_pc_runner`、`hwtest_tui`、`hwtest_gui`、`hwtest_web`；四个入口均通过 `TestApplicationController` 和统一 MB_DDF 注册表组装一个已知的独立单步测试（当前支持 `mbddf.system_status`、`mbddf.elec_health_status`、`mbddf.memperf`、`mbddf.spi_flash`、`mbddf.dh_pulse_config`、`mbddf.timer_jitter`、`mbddf.di_read`）。
- 测试：`tests/hal/`、`tests/log/`、`tests/biz/`、`tests/algorithm/`、`tests/app/` 使用 GoogleTest 并通过 CTest 注册；当前清单和统计只以 `docs/design/testing/testing-specification.md` 为准。
- 当前已有行式 TUI、Qt Widgets GUI、仅监听 `127.0.0.1` 的 `hwtest_web` WebSocket 后端，以及 `front/` 中独立构建的 React/Vite 浏览器遥测控制台。BIZ 已实现 `single`、`pc_periodic`、`device_stream` 三种运行语义；其任务工作线程已由原生 `std::thread` 迁移为 `QThread`，并以自动化回归锁定 Qt event dispatcher 和计时器注册能力。七个当前 MB_DDF 单步配置均支持单次，系统状态、电气健康、内存、定时器和 DI 配置还支持 PC 周期；设备持续回告模式仍按各算法能力校验，五个后续接入项目不声明 `device_stream`。应用层从已验证配置投影测试 descriptor，经 WebSocket 快照传给前端；WebSocket 还按后端固定 `configs/` 白名单提供测试摘要和 `configId` 切换，并以追加式 v1 动作提供带 revision 的 DI 刺激设置/安全复位。React 前端据此显示测试名称、支持的运行模式、首页主指标、测量字段及 16 路 DI 刺激/回读状态，并继续自动发现样本新增字段。HAL 已实现多 Adapter 懒加载、真实 C ABI v1 调用链和默认关闭的可选 NI-DAQmx Adapter；该 NI 路径已有 Fake API 自动化证据，但 USB-6259 真机尚未验收。TCP Provider 和全面真实硬件验收仍未实现。HAL 控制通道已实现 `qt.serial` 和 `qt.udp`：UDP 已有经应用控制器/BIZ/算法/HAL 的本机模拟目标闭环；`qt.serial` 于 2026-07-26 在授权隔离条件下完成 PC COM3 到 MB_DDF_v2 目标板的两个既有测试项单次及三轮 PC 周期 smoke。后续五项尚未形成真实板端验收证据。电气健康只按设备 `status`/`err_code` 判定，不包含未批准的电压阈值；SPI Flash 配置明确写入固定隔离测试区且不恢复。`SystemStatusSimulator` 仍只作为纯协议替身。
- `dut/docs/design/product_protocol_csv/` 是已导入的 MB_DDF 协议 CSV 快照，当前快照清单为 32 个 CSV；源事实目录仍为 `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv`。仓库尚未实现协议 manifest、内容哈希和不可变快照的自动机制。

## 分层与 I/O 边界

```text
浏览器 Web UI -> 回环 WebSocket 后端；TUI / Qt GUI / batch CLI
  -> hwtest_app_core::TestApplicationController
  -> hwtest_biz
  -> biz::IAlgorithmExecutor（算法层实现）
  -> hwtest_hal
  -> Provider / Adapter
```

- BIZ 负责配置、计划、稳定拓扑排序、运行状态、重试、结果编排和报告，保持硬件无关。它只能直接依赖 Qt Core、`hwtest_log_types` 和自身公共模型；禁止 include、link、call 或持有 HAL、Adapter、Socket、codec、测量基类/工厂或安全输出执行对象。
- TUI、Qt GUI、WebSocket 后端和浏览器 Web UI 只能消费应用层 DTO、动作结果、快照和样本事件；HAL 会话、算法执行器、BIZ 服务及其收尾顺序统一由 `hwtest_app_core` 组合，不得在各前端复制。控制器动作和快照只能在其 QObject 亲和线程调用，其他线程必须排队投递；GUI 和 WebSocket 后端不调用 `waitForTerminal()`，只通过异步事件、`stopAsync()` 和 `stopCompleted` 观察运行、停止和终态。PC 周期调度属于 BIZ，浏览器不得定时重复发送 `start`。这里禁止的是前端持有 DUT/生产 I/O Socket；`QWebSocketServer`/`QWebSocket` 只属于前端传输边界。
- `[目标契约-未实现]` 算法层负责产品协议 CSV、编解码、序列/流程和判定，并通过 HAL 请求设备生命周期；它不持有具体连接对象，也不直接执行生产原始 I/O、deadline 或物理 safe state。
- `[目标契约-未实现]` 面向测试设备或 DUT 的所有生产态硬件和通讯 I/O 必须统一经 HAL。HAL 持有具体连接对象，执行原始 I/O 和 deadline，归一化错误，并执行物理 safe state。
- `[当前实现]` `module = "control"` 的资源按显式 `providerId` 路由 `qt.serial` 或 `qt.udp`，直接使用 Qt 标准 API 并绕过 Vendor Adapter；其他资源按设备 `adapterId` 经 `AdapterRouter` 懒路由到 `MockAdapter` 或真实 `CAbiAdapter`，可选 `ni.daqmx` DLL 通过后者进入 NI-DAQmx。通用 Provider Router、TCP、控制通道 Mock Provider 和其他厂家 Provider 尚未实现。
- 配置、日志和报告文件 I/O 不属于上述生产硬件/通讯 I/O 规则。
- 纯协议或 golden 单测可以直接使用 Simulator；产品模拟和集成测试必须经过 HAL，可使用 HAL Mock，或使用标准 Provider 连接隔离模拟目标。不得以直连 Simulator 作为 HAL 集成证据。
- `[当前实现]` 现有“配置 -> BIZ -> 算法 -> `SystemStatusSimulator`”用例是遗留的非 HAL 跨层替身回归，可以保留为 golden-frame 证据，但不是产品模拟或集成验收范式。
- 日志是旁路基础模块；BIZ 使用 `hwtest::logging::LogEvent`，不得为了日志而传递依赖完整 `hwtest_log`。事件来源和 HAL/Adapter 映射仅由 `docs/design/contracts/log-interface-protocol.md` 主定义。

## 配置与兼容

- 新写出的 BIZ 测试配置仅使用 `executionConfig` 作为交给算法层的不透明执行配置；旧根字段 `halConfig` 仅允许在读取迁移时出现。该规则不改变当前 HAL 公共函数既有的参数/API 兼容面。
- BIZ 可以保存和透传 `ProtocolProfile`、`SafetyPolicy` 与硬件需求，但不得解释协议字段或执行硬件安全动作。
- 公共 HAL 和 BIZ 头文件均视为兼容面；结构体优先尾部扩展，避免改变既有枚举数值和语义。
- Adapter ABI 变化时必须同步 `src/hal/include/hal/hal_adapter_abi.h`、HAL 契约和 ABI 测试。

## 代码与文档约束

- 使用 C++17 和 Qt 5.15 兼容 API；Qt 6 fallback 必须保持可编译。
- 命名空间分别使用 `hwtest::hal`、`hwtest::logging`、`hwtest::biz`、`hwtest::algorithm::mbddf`、`hwtest::app`。
- 协议解释、产品判定留在算法层；具体连接、原始 I/O、deadline 和物理安全态按目标边界留在 HAL。不要把它们重新塞回 BIZ。
- 修改公共 API、配置字段、状态语义、错误码、构建依赖或测试边界时，同步相应事实源文档和契约测试。
- 跨文档概念只保留一个主定义：总览写职责和依赖，契约写接口和语义，实现报告写当前落地细节，测试规范写验证边界；其他文档使用链接引用，不复制大段定义。
- 历史计划若与当前实现不一致，应保留历史内容并在顶部标注“已被替代”及现行事实源，不把旧计划改写成当前设计。

## 本地前后端服务管理

- 自动化代理不得主动启动、重启或在后台保留 `hwtest_web`、Vite 开发服务器或其他常驻前后端服务。只有用户在当前对话中明确要求启动时才允许启动；构建、测试、页面开发或浏览器验证需求本身不构成启动授权。
- 未得到明确授权时，验证应使用会自行退出的单元测试、集成测试或 smoke test。若发现已有常驻服务，默认只检查状态，不复用、不停止、不重启；需要变更其运行状态时先取得用户指令。
- 仓库 WebSocket 后端默认使用 `18765`，不得主动改用可能被其他程序占用的 `8765`。用户手动启动时，在仓库根目录打开第一个 PowerShell 终端并运行：

```powershell
.\hwtest.ps1 -Ui web -WebPort 18765
```

- 上述脚本会按需构建并以前台方式运行后端。已有最新构建产物时可运行 `.\hwtest.ps1 -Ui web -WebPort 18765 -SkipBuild` 跳过构建。
- 用户手动启动浏览器前端时，在仓库根目录打开第二个 PowerShell 终端；首次安装依赖或 `package-lock.json` 变化后先运行 `npm ci`，随后启动 Vite：

```powershell
Set-Location .\front
npm ci
npm run dev
```

- 日常依赖未变化时可省略 `npm ci`。浏览器访问 `http://127.0.0.1:5173/`，前端默认连接 `ws://127.0.0.1:18765/ws`。如用户手动改用其他后端端口，应在 `front/.env.local` 中设置 `VITE_HWTEST_WS_URL=ws://127.0.0.1:<端口>/ws`，再重新启动 Vite。
- 两个服务都以前台方式运行；用户在各自终端按 `Ctrl+C` 即可关闭。自动化代理不得为了方便将它们改为后台常驻启动。

### 前端单文件构建

- 每次修改 `front/` 源码后，必须在 `front/` 目录运行 `npm test` 和 `npm run build`；不得把未重新打包的 `dist/` 当作前端交付物。
- `npm run build` 在 TypeScript/Vite 构建后自动内联 JS、CSS 和 Geist 字体，并删除 `front/dist/assets/`，最终产物为可直接双击打开的 `front/dist/index.html`。
- 单文件 HTML 不提供 WebSocket 后端；需要实时遥测时仍由用户按上方流程手动启动 `hwtest_web`，自动化代理不得因此启动常驻服务。
- 单文件页面的浏览器 Origin 可能为 `null`；回环 WebSocket 契约应保留该文件来源白名单并由 WebSocket 回归测试锁定。

## 构建与验证

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Debug --parallel
ctest --test-dir build_vs -C Debug --output-on-failure
cmake --build build_vs --config Release --parallel
ctest --test-dir build_vs -C Release --output-on-failure
```

- `hwtest_hal` 必须保持可由宿主/根工程作为 Qt Core/Network/SerialPort 库目标构建，并保留无真实硬件的 Mock 路径。当前 `src/hal/` 没有独立 CMake 自举入口，不得宣称支持直接执行 `cmake -S src/hal`。
- 修改 BIZ 时必须运行 BIZ 契约/架构测试，确认 `src/biz/` 和 `tests/biz/` 没有越层依赖。
- 修改算法层时运行 `tests/algorithm/`；产品模拟或集成验证必须经过 HAL，而不是直连 Simulator。
- 修复行为缺陷时先补能够复现问题的回归测试；仅修改说明性文档时至少运行链接/术语检查和 `git diff --check`。

### DUT 构建与验证边界

- 宿主工程的 `CMakeLists.txt` 不加入 `dut/`；宿主 Debug/Release 构建和 CTest 不会隐式编译或运行 DUT。
- 进入 `dut/` 后，按其局部 `AGENTS.md` 和 `README.md` 使用 `build.ps1`、`debug.ps1`、`tests/test-dds-only.ps1`、`tests/test-all.ps1` 与 `tests/test-deploy.ps1`。
- DUT 的协议 CSV、板端单元测试、目标板 smoke/performance/stress 结果属于 DUT 证据；除非明确记录 AArch64 工具链、sysroot、部署目标和运行结果，不得把它们写成宿主应用或真实硬件验收证据。
