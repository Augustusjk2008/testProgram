# 测试规范

> 适用项目：多产品通用硬件测试软件（Qt 5.15 兼容、Qt 6 Core/Network/SerialPort/Widgets fallback / C++17 / Windows）。
>
> 本文是仓库全局唯一的测试规则、测试准入、运行方式和当前测试清单。HAL 专属覆盖快照见 [HAL 测试设计报告](hal-test-design-report.md)；其他设计文档不得复制或替代本文的公共规则。
>
> 当前实现事实以公共 API、CMake 目标和测试注册为准。本文的“源级定义数”不是已执行、已通过或已验证的 CTest 结果。

## 1. 当前清单与统计口径

根 CMake 在 BUILD_TESTING 为真时加入 tests/；tests/CMakeLists.txt 当前加入 HAL、日志、BIZ、算法和应用五个目录，共生成六个 GoogleTest 可执行目标。六个目标均用 gtest_discover_tests 在构建后发现 CTest 条目；应用目录另直接注册 GUI offscreen 启动、runner/TUI 的两个 `--help`、根脚本帮助/非法 UI/选择 TUI、一个 TUI stdin 会话和一个 runner 异步错误，共八个进程测试。

| 目录 | 测试目标 | 测试源文件 | 源级 GoogleTest 定义 | 当前范围 |
| --- | --- | ---: | ---: | --- |
| tests/hal/ | hwtest_hal_tests | 9 | 31 | HAL 接口、资源、安全、Mock、Loader、宿主串口枚举、Qt 控制 Provider |
| tests/log/ | hwtest_log_tests | 3 | 7 | 日志服务、JSONL sink、HAL 日志桥接 |
| tests/biz/ | hwtest_biz_tests | 6 | 35 | 配置、计划、调度、报告和架构边界 |
| tests/algorithm/ | hwtest_algorithm_tests | 2 | 20 | MB_DDF CSV、流式控制传输和 SYSTEM_STATUS |
| tests/app/ | hwtest_app_tests / hwtest_gui_tests | 4 | 35 | 共享启动/控制器、TUI/GUI、异步停止与关闭、跨前端等价性、架构边界及经 HAL/Qt UDP 的闭环 |
| 合计 | 6 个目标 | 24 | 128 | 当前源级测试清单 |

128 是当前测试源码中的 GoogleTest 定义数。完整构建后的预期 CTest 清单为 136 条：128 条动态发现的 GoogleTest 加 8 条应用入口/脚本进程测试。2026-07-20 的 Debug 构建后 `ctest -N` 实际列出 136 条；清单数量不表示已经执行或通过，只有实际运行 CTest 并报告零失败才能作通过结论。

24 个测试定义源文件使用 `*_test.cpp` 命名。两个 HAL DLL fixture、GUI 自定义 GoogleTest 入口和应用测试支持库不包含测试定义，不计入该数量。

## 2. 当前覆盖与条件资产

| 目标 | 当前已覆盖的行为 | 证据边界 |
| --- | --- | --- |
| HAL | 错误映射、资源映射、安全校验、会话、Mock AD/DA、DI/DO、宿主串口枚举、串口 echo、CANFD loopback、AdapterLoader fixture、控制资源路由、Qt UDP 回环和 timeout | 串口枚举不打开设备；Qt UDP 仅是本机 Provider 证据；没有真实串口、真实网口或厂家 SDK 证据 |
| 日志 | LogService、JsonLineFileSink、HalLogEvent 到 LogEvent 桥接 | 不覆盖 UI 或真实设备日志链 |
| BIZ | FakeAlgorithmExecutor 下的配置、计划、调度、重试、状态、报告和架构扫描 | BIZ 不构造 HAL 假对象、Socket、codec 或硬件执行对象 |
| 算法 | 帧编解码、CSV 无效输入、流式短读/粘包/噪声/超时、SYSTEM_STATUS 模拟器及 Qt UDP 成功/超时/坏 CRC 路径 | 当前只实现 mbddf.system_status；本机 UDP 模拟目标不等同于真实板端通讯 |
| 应用/TUI/GUI | 共享启动参数与覆盖顺序、控制资源与会话串口选择、线程亲和和运行代次隔离、同步/异步停止门禁、GUI 非阻塞关闭、快照/结果投影、TUI/GUI 配置/通过/超时/停止等价性、GUI 源码和链接架构扫描、runner/TUI/GUI/根脚本入口，以及经 BIZ/算法/HAL/Qt UDP 到协议模拟目标的闭环 | 串口选择只证明配置覆盖，不证明端口可打开；offscreen 与 UDP 模拟目标不证明 Web UI、真实串口、真实网口或真实 DUT |

下列测试依赖条件资产，缺失时可调用 GTEST_SKIP。跳过只表示该次没有执行断言，不能证明任何协议、配置迁移、SYSTEM_STATUS、HAL 或硬件能力。

- 5 个 MB_DDF 协议测试和 18 个 SYSTEM_STATUS 跨层/集成测试依赖 MB_DDF_PROTOCOL_CSV_DIR 指向的外部 CSV 资产目录；后者包含 11 个应用/TUI/GUI UDP 运行、停止、关闭或等价性测试。
- BIZ 的导入附件样例测试依赖 tmp/hwtest_BIZ/configs/sample_product.testcfg；tmp 不是仓库实现事实。
- 算法测试中有 8 个自包含的帧、传输或临时 CSV 用例；其余 12 个依赖外部 MB_DDF CSV。

协议 CSV 是运行期资产。用户已批准 `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容作为 MB_DDF 基线，当前为 32 个 CSV；协议测试按一文件一定义及当前实际字段校验。仓库不保存这些 CSV fixture，因此测试仍可能因目录缺失而跳过；基线已批准不等于 manifest/hash 可复现快照已经实现。

## 3. 五级证据模型

下表按证据强度递增。低级证据不能替代高级证据，也不得把未实现级别写成已验证能力。

| 级别 | 证据对象 | 当前状态 | 可证明与不可证明的范围 |
| --- | --- | --- | --- |
| 1. 协议 Simulator | 配置 -> BIZ -> SystemStatusAlgorithmExecutor -> SystemStatusSimulator -> golden request frame | 已有；遗留的非 HAL 跨层替身回归，依赖外部 CSV | 仅证明当前 SYSTEM_STATUS 的模拟器闭环、CRC 和超时处理；不是产品模拟或集成验收范式 |
| 2. HAL Mock 集成 | 算法 -> IControlChannel -> HAL Mock Provider | SYSTEM_STATUS 正向闭环未实现 | 现有 MockAdapter 回环不是控制通道 Mock Provider；算法 fake 只作传输契约测试 |
| 3. Qt Provider | `qt.serial`/`qt.udp` 标准 Qt 通讯 Provider | 部分实现 | Qt UDP 已有原始回环及经 BIZ/算法/HAL 的本机模拟目标闭环；Qt 串口只有配置错误契约和可编译证据，无实机联调；TCP 未实现 |
| 4. Vendor Adapter | 真实厂家 Adapter DLL/SDK 经 C ABI 接入 | 未实现 | AdapterLoader fixture 仅证明最小加载器行为；CAbiAdapter 当前仍委托 MockAdapter |
| 5. 真实硬件 | 隔离台架上的真实目标和测试设备 | 未实现 | 当前没有真实硬件测试目标，也没有 CTest hardware label |

新增五级中的任何目标时，文档、CMake 和测试必须同时标明其级别、依赖资产、隔离条件和通过证据。在代码与测试落地前，一律标记为“未实现”。

## 4. 分层与集成规则

| 范围 | 单元测试允许依赖 | 禁止或不作为通过证据 |
| --- | --- | --- |
| BIZ | FakeAlgorithmExecutor、配置样本、结果和报告样本 | HAL、Adapter、Socket、codec、测量对象、硬件安全执行 |
| 应用/TUI/GUI | `TestApplicationController`、前端支持库、TUI 命令解析、Qt offscreen、本机 UDP 隔离目标 | UI 直接持有 HAL/Socket/算法对象；GUI 调用阻塞等待；真实硬件结论；ANSI 屏幕文本作为业务状态源 |
| 算法 | 协议 CSV 样本、Simulator、脚本化传输、IHalDevice 测试替身 | UI、业务调度实现、厂家 SDK |
| HAL | MockAdapter、最小 ABI fixture、资源配置 | 业务判定、产品协议字段解释、真实厂家硬件结论 |
| Vendor Adapter | 厂家 SDK 假对象或隔离仿真 DLL | UI、BIZ、算法判定 |
| 真实硬件 | 经授权的隔离设备、独立报告和显式硬件目标 | 默认 CI 或开发机自动执行 |

跨层验收必须明确是契约测试、协议测试、HAL Mock 集成、Provider 集成、Vendor Adapter 集成或真实硬件验收。不得把串口 echo、CANFD loopback 或 Simulator 结果描述为真实通讯证据。

SYSTEM_STATUS 当前同时有 Simulator golden 链和“BIZ -> 算法 -> HalControlTransport -> HAL -> qt.udp -> 本机模拟目标”成功链。HAL Mock Provider 正向链、真实串口/目标板链和 `ProtocolProfile -> CSV -> HAL ResourceId` 一致性校验仍是未实现验收项。

## 5. 测试准入

- 公共 HAL 或 BIZ 头文件、配置字段、状态语义、错误码、资源类型或 Adapter ABI 变化时，必须同步相应契约文档和回归测试。
- 修改 BIZ 时，必须运行 hwtest_biz_tests 和 BIZ 架构扫描；BIZ 测试不得引入硬件执行依赖。
- 修改协议 CSV 规则、解析器或资产引用时，必须同步 device-communication-protocol.md 和协议契约测试，并记录基线路径、观测时间与清单；manifest/hash 机制落地后再记录固定版本和内容哈希。
- 修改 Mock 行为时，必须说明证据级别；可配置超时/错误注入和 SYSTEM_STATUS 控制通道 Mock Provider 集成仍未实现，不得作为既有能力验收。
- 修改 Qt Provider、Vendor Adapter 或真实硬件路径时，必须新增相应级别的隔离测试；当前 Qt UDP 有本机隔离测试，Qt 串口实机、真实 Adapter、CTest hardware label 和真实硬件验收仍未实现。
- 修改共享应用控制器、runner 或 TUI 命令时，必须运行 `hwtest_app_tests`；修改 Qt GUI 时还必须运行 `hwtest_gui_tests`。Qt GUI/Web UI 必须复用控制器 DTO/事件，不得以新增前端为由复制组合根。
- 修复行为缺陷时，先补能复现问题的回归测试，再修改实现。

### 5.1 前端使用习惯兼容准入

面向操作人员的基础生命周期固定为 `load -> select -> prepare -> run -> terminal -> result -> disconnect`；TUI 中的 `use/port`、`wait/status` 分别承担 select 和 terminal 观察动作。用户操作说明见 [TUI 使用指南](../../user/tui-usage-guide.md)。

当前 `TestApplicationController` 仍只接受 `mbddf.system_status`，并要求恰好一个启用的 SYSTEM_STATUS 步骤。以下规则是新增测试项目的合入门禁，不得据此宣称多项目执行器已经实现：

- 新项目必须通过现有 `-TestConfig`、`-HalConfig` 选择；不得用产品专用入口复制一套加载、准备、运行和收尾生命周期。
- 既有 TUI 命令的前置状态、硬件副作用和语义不得改变：`load/use/port` 不打开设备，`prepare` 才建立硬件会话，`disconnect/quit` 执行有序收尾。
- 既有机器可读前缀和退出语义属于兼容面，包括 `ok`、`error`、`phase=`、`verdict=`、`rawData=` 以及成功/启动或收尾失败的既有进程退出码。输出可以尾部追加字段，不得静默改变现有字段含义或顺序。
- 新能力优先通过配置和现有控制器 DTO 表达。确需新增命令或字段时只做追加式扩展；废弃项必须保留兼容别名、迁移说明和明确的移除周期。
- `TuiShellTest` 继续锁定命令解析、前置状态和会话覆盖；`HwtestTuiScriptedSession` 继续锁定真实进程命令流；`FrontendEquivalenceTest` 继续锁定 TUI/GUI 控制器阶段、终态和结果等价性。
- 每个新增项目至少增加一个从加载、选择、准备、运行、终态观察、结果读取到断开的前端工作流用例，并证明原有项目的上述回归未退化。涉及新硬件路径时还必须满足本文的证据级别和隔离测试要求。
- 项目文档只新增配置组合、资源选择和项目特有结果字段；通用命令含义统一引用使用指南，避免不同项目形成互相冲突的操作手册。

## 6. 构建与验证

根目录脚本是 Windows 下的推荐入口；它检查协议资产，使用 Visual Studio 2022 x64，并在 `test` 动作中启用完整测试。首次测试会把固定版本和哈希校验后的 GoogleTest 源码放入已忽略的 `tmp/deps/` 缓存：

    .\hwtest.ps1 build
    .\hwtest.ps1 test
    .\hwtest.ps1 test -Configuration Release
    .\hwtest.ps1 test -TestRegex "^(HalTypesTest|TuiShellTest)\."

以下是脚本对应的底层通用命令。完整构建是动态发现六个测试目标的前提。

    cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
    cmake --build build_vs --config Debug --parallel
    ctest --test-dir build_vs -C Debug -N
    ctest --test-dir build_vs -C Debug --output-on-failure
    cmake --build build_vs --config Release --parallel
    ctest --test-dir build_vs -C Release -N
    ctest --test-dir build_vs -C Release --output-on-failure

ctest -N 只确认构建后动态发现的 CTest 清单；它不执行测试，也不证明通过。执行结果中的 GTEST_SKIP 必须在报告中单列为“未验证”，不能合并为通过。

要执行已批准 MB_DDF 基线的相关测试，而不是接受跳过，先在同一 PowerShell 会话显式设置并检查资产目录。该目录是当前协议事实源，但不是不可变的仓库内 fixture；每次结果应记录观测时间与实际文件清单：

    $env:MB_DDF_PROTOCOL_CSV_DIR = "H:\Resources\RTLinux\Demos\MB_DDF_v2\docs\design\product_protocol_csv"
    if (-not (Test-Path $env:MB_DDF_PROTOCOL_CSV_DIR)) { throw "MB_DDF CSV assets are required" }
    ctest --test-dir build_vs -C Debug -R "^(MbddfProtocolTest|SystemStatusExecutorTest|HalControlTransportTest|SystemStatusUdpIntegrationTest|TestApplicationControllerTest|TuiShellTest|GuiMainWindowTest|FrontendEquivalenceTest)\." --output-on-failure

HAL 专属覆盖快照和缺口见 [HAL 测试设计报告](hal-test-design-report.md)。本文以外的历史计划不是现行测试规则或通过证据。
