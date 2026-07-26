# 测试规范

> 适用项目：多产品通用硬件测试软件（Qt 5.15 兼容、Qt 6 Core/Network/SerialPort/Widgets/WebSockets fallback / C++17 / Windows）。
>
> 本文是仓库全局唯一的测试规则、测试准入、运行方式和当前测试清单。HAL 专属覆盖快照见 [HAL 测试设计报告](hal-test-design-report.md)；其他设计文档不得复制或替代本文的公共规则。
>
> 当前实现事实以公共 API、CMake 目标和测试注册为准。本文的“源级定义数”不是已执行、已通过或已验证的 CTest 结果。

## 1. 当前清单与统计口径

根 CMake 在 BUILD_TESTING 为真时加入 tests/；tests/CMakeLists.txt 当前加入 HAL、日志、BIZ、算法和应用五个目录，共生成七个 GoogleTest 可执行目标。七个目标均用 gtest_discover_tests 在构建后发现 CTest 条目；应用目录另直接注册 GUI offscreen 启动、runner/TUI/Web 的帮助或 smoke、根脚本帮助/非法 UI/选择 TUI、一个 TUI stdin 会话和一个 runner 异步错误，共十个进程测试。`front/` 另有 Vitest，不进入 CTest 清单。

| 目录 | 测试目标 | 测试源文件 | 源级 GoogleTest 定义 | 当前范围 |
| --- | --- | ---: | ---: | --- |
| tests/hal/ | hwtest_hal_tests | 9 | 31 | HAL 接口、资源、安全、Mock、Loader、宿主串口枚举、Qt 控制 Provider |
| tests/log/ | hwtest_log_tests | 3 | 7 | 日志服务、JSONL sink、HAL 日志桥接 |
| tests/biz/ | hwtest_biz_tests | 6 | 41 | 配置、计划、单次/PC 周期/设备流调度、Qt 工作线程、样本、报告和架构边界 |
| tests/algorithm/ | hwtest_algorithm_tests | 2 | 22 | MB_DDF CSV、流式控制传输、SYSTEM_STATUS、ELEC_HEALTH_STATUS 和设备流能力判定 |
| tests/app/ | hwtest_app_tests / hwtest_gui_tests / hwtest_web_tests | 8 | 81 | 共享启动/控制器、TUI/GUI/WebSocket、连续运行样本、异步停止与关闭、跨前端等价性、架构边界及经 HAL/Qt UDP 的两个独立测试闭环 |
| 合计 | 7 个目标 | 28 | 182 | 当前源级 GoogleTest 清单 |

182 是当前测试源码中的 GoogleTest 定义数。完整构建后的 CTest 清单为 192 条：182 条动态发现的 GoogleTest 加 10 条应用入口/脚本进程测试。2026-07-26 的 Debug 构建后 `ctest -N` 实际列出 192 条；清单数量不表示已经执行或通过，只有实际运行 CTest 并报告零失败才能作通过结论。

28 个测试定义源文件使用 `*_test.cpp` 命名。两个 HAL DLL fixture、GUI/Web 自定义 GoogleTest 入口、应用测试支持库和测试 helper 不包含测试定义，不计入该数量。

浏览器前端当前有 3 个 `*.test.ts` 文件、9 条 Vitest，用于协议解析/请求、50,000 点有界缓冲、时间窗、min/max 降采样和同图/分图/自定义分组。它们由 `npm test` 独立运行，不计入上述 192 条 CTest。

## 2. 当前覆盖与条件资产

| 目标 | 当前已覆盖的行为 | 证据边界 |
| --- | --- | --- |
| HAL | 错误映射、资源映射、安全校验、会话、Mock AD/DA、DI/DO、宿主串口枚举、串口 echo、CANFD loopback、AdapterLoader fixture、控制资源路由、Qt UDP 回环和 timeout | 自动化测试中的串口枚举不打开设备；Qt UDP 仅是本机 Provider 证据；COM3 实机结果属于下述独立手工 smoke，不是默认 CTest 或厂家 SDK 证据 |
| 日志 | LogService、JsonLineFileSink、HalLogEvent 到 LogEvent 桥接 | 不覆盖 UI 或真实设备日志链 |
| BIZ | FakeAlgorithmExecutor 下的配置、计划、调度、重试、三种运行模式、专用 QThread 的 event dispatcher/计时器注册、可中断轮间等待、轮次/样本标记、状态、报告和架构扫描 | BIZ 不构造 HAL 假对象、Socket、codec 或硬件执行对象；Qt 线程回归只证明同步任务具备 Qt dispatcher，不证明算法调用期间持续泵送事件、HAL actor 或跨线程取消；设备流测试只证明 BIZ 单次调用边界，不证明某产品支持主动回告 |
| 算法 | 帧编解码、CSV 无效输入、流式短读/粘包/噪声/超时、SYSTEM_STATUS 模拟器、两个固定命令执行器的 Qt UDP/脚本传输路径、ELEC_HEALTH_STATUS 字段判定和两个算法拒绝设备流 | 当前只实现两个已知的独立单步算法；本机 UDP 模拟目标不等同于真实板端通讯；拒绝设备流不证明其他算法已实现设备持续回告 |
| 应用/TUI/GUI/WebSocket | 共享启动参数与覆盖顺序、控制资源与会话串口选择、线程亲和和运行代次隔离、同步/异步停止门禁、GUI/Web 非阻塞关闭、Web JSON/Origin/单客户端/16 KiB/关闭码、完整快照/样本投影、PC 周期两轮 UDP 指令—反馈闭环、TUI/GUI 与 TUI/Web 的配置/通过/超时/停止等价性、GUI/Web 源码和链接架构扫描、runner/TUI/GUI/Web/根脚本入口 | 默认自动化中的串口选择只证明配置覆盖；下述 COM3 实机 smoke 独立于 CTest，不能外推到真实网口、其他 DUT 或长期稳定性 |
| 浏览器前端 | Vitest 下的类型化协议、有界数据结构、字段发现、时间窗、降采样和图组；TypeScript/Vite 生产构建 | 不替代 WebSocket/UDP C++ 集成测试，也不证明真实硬件、长期浏览器稳定性或设备流算法 |

下列测试依赖条件资产，缺失时可调用 GTEST_SKIP。跳过只表示该次没有执行断言，不能证明任何协议、配置迁移、SYSTEM_STATUS、HAL 或硬件能力。

- 5 个 MB_DDF 协议测试和 33 个 MB_DDF 跨层/集成测试依赖 MB_DDF_PROTOCOL_CSV_DIR 指向的外部 CSV 资产目录；后者包含 25 个应用/TUI/GUI/Web UDP 运行、连续采样、停止、关闭或等价性测试。
- BIZ 的导入附件样例测试依赖 tmp/hwtest_BIZ/configs/sample_product.testcfg；tmp 不是仓库实现事实。
- 算法测试中有 9 个自包含的帧、传输、能力判定或临时 CSV 用例；其余 12 个依赖外部 MB_DDF CSV。

协议 CSV 是运行期资产。用户已批准 `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容作为 MB_DDF 基线，当前为 32 个 CSV；协议测试按一文件一定义及当前实际字段校验。`dut/` 保存同步副本，但宿主测试未把它接成默认 fixture，仍可能因外部目录缺失而跳过；基线已批准和文件已复制都不等于 manifest/hash 自动机制已经实现。

### 2.1 2026-07-26 COM3 实机 smoke

本次是在用户明确授权、MB_DDF_v2 电路板已上电的条件下执行的独立手工证据，不进入默认 CTest：

- DUT 源码为 `H:/Resources/RTLinux/Demos/MB_DDF_v2` 的提交 `982b3f5bbce222aea061e9ce1523ba926c801658`；使用 `HW_TEST Release` 画像交叉构建并部署到 `root@192.168.1.29:/home/sast8/tmp`。目标为 AArch64 Linux，工具链为 Arm GNU `aarch64-none-linux-gnu 11.3 rel1`，sysroot 为 `D:/Program Files (x86)/Arm GNU Toolchain aarch64-none-linux-gnu/origin/armv8a-ucas-linux`。
- 板端通过 `/dev/xdma0` 的 COM3 窗口和 event 2 运行产品协议服务；PC 端使用 `COM3`、`614400 / 8E1 / 无流控`。宿主只启动 `hwtest_web`，未启动浏览器前端，并通过 WebSocket 手工发送 `load -> prepare -> start -> quit`。
- 单次 SYSTEM_STATUS 到达 `finished`，结果为 `Pass/Ok`，`status=0`、`err_code=0`，收到一个 `SYSTEM_STATUS` 样本。随后 `pc_periodic` 以 `intervalMs=100`、`maxCycles=3` 完成三轮，轮次为 `1,2,3`，快照和客户端均收到三个样本，最终仍为 `Pass/Ok`。
- 三轮最后一次解码值包括 CPU 小/大核频率 `1704/2016 MHz`、CPU/RK/K7 温度 `37/37/43 C`、内存占用约 `3.6435%`、PCIe `5 GT/s x4`、上电时间 `1158 s`。这些是一次运行观测值，不是产品阈值或稳定性基线。
- 当次风险：连接和 `prepare` 阶段无 Qt 警告，但三个实际串口周期在同一 BIZ 工作线程各产生一次 `QObject::startTimer: Timers can only be used with threads started with QThread`。当次字节交互仍成功，但线程模型当时尚未修正；当前修复与复核状态见 2.2。长时、超时、拔插、停止与物理安全收尾完成前，不得把该 smoke 写成生产验收。
- 同一来源提交的 `HW_TEST Release` 和完整 `MB_DDF_HW_Tests` 目标交叉构建通过；更新提交新增的 `HwXdmaTransport.WaitEventReportsReadinessWithoutReadingEventFd` 已部署到同一目标板并通过。测试结束后宿主后端和板端产品协议服务均已停止。

### 2.2 2026-07-26 QThread 修复后复核

- `TestRunService` 的任务 worker 已从原生 `std::thread` 迁移到 `QThread::create()`；`TestRunServiceTest.PcPeriodicWorkerProvidesQtEventDispatcher` 先在旧实现上因 dispatcher 为空、计时器 ID 为 `0` 失败，再在新实现上通过。该用例检查一次 `prepare()` 和两轮 `executeStep()` 位于同一非应用线程、存在 `QAbstractEventDispatcher` 且可注册 Qt 计时器。
- 使用外部 32 份协议 CSV 完成 Debug 全量构建和 192 条 CTest，结果为 192/192 通过；随后 Release 全量构建和 CTest 同样为 192/192 通过。
- 自退出的 `hwtest_pc_runner` 随后使用 `CONTROL_SERIAL`、COM3、`614400 / 8E1 / 无流控` 发起一次 SYSTEM_STATUS；调用覆盖串口打开、写入、等待读取和关闭，未再观察到 `QObject::startTimer` 警告，但因板端产品协议服务未响应而以 `BusTimeout` 结束。目标板 SSH 22 端口探测也未在 15 秒内成功。
- 随后在用户再次明确授权、板端可达且无遗留服务的条件下，使用来源提交 `982b3f5bbce222aea061e9ce1523ba926c801658` 的 `HW_TEST Release` 画像启动产品协议服务。宿主只启动 Debug `hwtest_web`，显式选择 `CONTROL_SERIAL`、COM3 和外部 32 份协议 CSV；未启动浏览器/Vite 前端，WebSocket 客户端直接发送 `load -> prepare -> single -> pc_periodic -> quit`。
- 单次运行收到一个 `SYSTEM_STATUS` 样本并到达 `finished/Pass/Ok`，`status=0`、`err_code=0`。随后 `pc_periodic` 以 `intervalMs=100`、`maxCycles=3` 完成轮次 `1,2,3`，样本事件序号为 `2,3,4`，最终快照为 `cycleIndex=3`、`sampleCount=3` 和 `finished/Pass/Ok`；三轮产品协议序号依次为 `4660,4661,4662`。
- 在同一授权板端和 COM3 上切换到独立 `ELEC_HEALTH_STATUS` 配置后，单次运行收到一个 `ELEC_HEALTH_STATUS` 样本并到达 `finished/Pass/Ok`，`status=0`、`err_code=0`；观测电压和辅助量作为样本输出保留。随后以 `intervalMs=100`、`maxCycles=3` 完成轮次 `1,2,3`，样本事件序号为 `2,3,4`，最终快照为 `cycleIndex=3`、`sampleCount=3` 和 `finished/Pass/Ok`；三轮产品协议序号依次为 `4660,4661,4662`。
- 两个测试项的 WebSocket 客户端都执行了 `load -> prepare -> single -> pc_periodic -> quit`，未启动浏览器/Vite。健康测试后端完整诊断中 `QObject::startTimer` 和 `Timers can only be used with threads started with QThread` 均为 0；`quit` 以关闭码 `1000` 完成，随后核对板端身份后发送 `SIGTERM`，18765 无监听、远端无 `MB_DDF_v2`、COM3 可重新独占打开并关闭。
- 从后端启动到 `quit` 的完整进程诊断输出只有 WebSocket 就绪行，`QObject::startTimer: Timers can only be used with threads started with QThread` 出现次数为 `0`。`quit` 回复成功并以 WebSocket 关闭码 `1000` 结束；随后对身份核对为 `MB_DDF_v2` 的板端进程发送 `SIGTERM`。收尾复核确认 18765 无监听、本地无后端或调试进程、远端无 `MB_DDF_v2`，COM3 可重新独占打开并关闭。
- 这次复测把 QThread 修复后的短时真实串口单次/三周期成功链补齐，但仍不是默认 CTest 或生产验收；没有覆盖长时稳定性、通信超时、拔插、运行中停止、板端异常退出或物理安全状态恢复。`HW_TEST` 画像本身也不提供状态恢复。

## 3. 五级证据模型

下表按证据强度递增。低级证据不能替代高级证据，也不得把未实现级别写成已验证能力。

| 级别 | 证据对象 | 当前状态 | 可证明与不可证明的范围 |
| --- | --- | --- | --- |
| 1. 协议 Simulator | 配置 -> BIZ -> SystemStatusAlgorithmExecutor -> SystemStatusSimulator -> golden request frame | 已有；遗留的非 HAL 跨层替身回归，依赖外部 CSV | 仅证明当前 SYSTEM_STATUS 的模拟器闭环、CRC 和超时处理；不是产品模拟或集成验收范式 |
| 2. HAL Mock 集成 | 算法 -> IControlChannel -> HAL Mock Provider | SYSTEM_STATUS/ELEC_HEALTH_STATUS 正向闭环未实现 | 现有 MockAdapter 回环不是控制通道 Mock Provider；算法 fake 只作传输契约测试 |
| 3. Qt Provider | `qt.serial`/`qt.udp` 标准 Qt 通讯 Provider | 部分实现 | Qt UDP 已有原始回环及经 BIZ/算法/HAL 的本机模拟目标闭环；Qt 串口已有带历史线程告警的早期 COM3/DUT smoke，以及 QThread 迁移后成功且无该警告的单次/三周期复测；自动化 hardware target 和异常路径仍缺失，TCP 未实现 |
| 4. Vendor Adapter | 真实厂家 Adapter DLL/SDK 经 C ABI 接入 | 未实现 | AdapterLoader fixture 仅证明最小加载器行为；CAbiAdapter 当前仍委托 MockAdapter |
| 5. 真实硬件 | 隔离台架上的真实目标和测试设备 | 部分手工证据 | 已有上述授权 COM3 SYSTEM_STATUS/ELEC_HEALTH_STATUS 历史及 QThread 迁移后复测；没有宿主 CTest hardware target、长期稳定性、异常收尾或完整物理安全验收 |

新增五级中的任何目标时，文档、CMake 和测试必须同时标明其级别、依赖资产、隔离条件和通过证据。在代码与测试落地前，一律标记为“未实现”。

## 4. 分层与集成规则

| 范围 | 单元测试允许依赖 | 禁止或不作为通过证据 |
| --- | --- | --- |
| BIZ | FakeAlgorithmExecutor、配置样本、结果和报告样本 | HAL、Adapter、Socket、codec、测量对象、硬件安全执行 |
| 应用/TUI/GUI/WebSocket/浏览器 | `TestApplicationController`、前端支持库、TUI 命令解析、Qt offscreen、Qt WebSockets 回环、本机 UDP 隔离目标、纯前端数据结构和浏览器 API | 前端直接持有 HAL、算法或 DUT/生产 I/O Socket；GUI/Web 调用阻塞等待；浏览器用定时器重复 `start`；真实硬件结论；ANSI 屏幕文本作为业务状态源。`QWebSocketServer/QWebSocket` 只属于前端传输，不属于 DUT I/O |
| 算法 | 协议 CSV 样本、Simulator、脚本化传输、IHalDevice 测试替身 | UI、业务调度实现、厂家 SDK |
| HAL | MockAdapter、最小 ABI fixture、资源配置 | 业务判定、产品协议字段解释、真实厂家硬件结论 |
| Vendor Adapter | 厂家 SDK 假对象或隔离仿真 DLL | UI、BIZ、算法判定 |
| 真实硬件 | 经授权的隔离设备、独立报告和显式硬件目标 | 默认 CI 或开发机自动执行 |

跨层验收必须明确是契约测试、协议测试、HAL Mock 集成、Provider 集成、Vendor Adapter 集成或真实硬件验收。不得把串口 echo、CANFD loopback 或 Simulator 结果描述为真实通讯证据。

`SYSTEM_STATUS` 与 `ELEC_HEALTH_STATUS` 当前都有“配置 -> BIZ -> 算法 -> HalControlTransport -> HAL -> qt.udp -> 本机模拟目标”自动化成功链，以及上述 `qt.serial -> COM3 -> MB_DDF_v2` 手工成功链。BIZ QThread 回归证明调度 worker 具备 Qt dispatcher，迁移后的真实串口短时成功复测进一步证明固定 COM3/DUT 链路未再产生目标计时器警告；两者仍不能替代长时和异常验收。HAL Mock Provider 正向链、真实串口自动化异常路径和 `ProtocolProfile -> CSV -> HAL ResourceId` 一致性校验仍是未实现验收项。

## 5. 测试准入

- 公共 HAL 或 BIZ 头文件、配置字段、状态语义、错误码、资源类型或 Adapter ABI 变化时，必须同步相应契约文档和回归测试。
- 修改 BIZ 时，必须运行 hwtest_biz_tests 和 BIZ 架构扫描；BIZ 测试不得引入硬件执行依赖。
- 修改协议 CSV 规则、解析器或资产引用时，必须同步 device-communication-protocol.md 和协议契约测试，并记录基线路径、观测时间与清单；manifest/hash 机制落地后再记录固定版本和内容哈希。
- 修改 Mock 行为时，必须说明证据级别；可配置超时/错误注入和 SYSTEM_STATUS/ELEC_HEALTH_STATUS 控制通道 Mock Provider 集成仍未实现，不得作为既有能力验收。
- 修改 Qt Provider、Vendor Adapter 或真实硬件路径时，必须新增相应级别的隔离测试；当前 Qt UDP 有本机隔离测试，BIZ 已有 Qt dispatcher/计时器注册回归，Qt 串口已有带历史告警的早期 smoke、无响应无告警诊断和迁移后成功无告警的手工复测；真实 Adapter、CTest hardware label、自动化异常路径和全面真实硬件验收仍未实现。
- 修改共享应用控制器、runner 或 TUI 命令时，必须运行 `hwtest_app_tests`；修改 Qt GUI 时还必须运行 `hwtest_gui_tests`；修改 WebSocket 协议、服务器、入口或脚本时必须运行 `hwtest_web_tests`、`ctest -L websocket` 和 `HwtestWeb*` 进程测试。修改 `front/` 时必须运行 `npm test` 和 `npm run build`。Qt GUI、WebSocket 后端和浏览器 Web UI 必须复用控制器 DTO/事件，不得以新增前端为由复制组合根。
- 修复行为缺陷时，先补能复现问题的回归测试，再修改实现。

### 5.1 前端使用习惯兼容准入

面向操作人员的基础生命周期固定为 `load -> select -> prepare -> run -> terminal -> result -> disconnect`；TUI 中的 `use/port`、`wait/status` 分别承担 select 和 terminal 观察动作。用户操作说明见 [TUI 使用指南](../../user/tui-usage-guide.md)。

当前 `TestApplicationController` 接受两个已知算法 ID（`mbddf.system_status`、`mbddf.elec_health_status`），并要求恰好一个启用的对应单步。以下规则是新增测试项目的合入门禁，不得据此宣称通用多项目执行器已经实现：

- 新项目必须通过现有 `-TestConfig`、`-HalConfig` 选择；不得用产品专用入口复制一套加载、准备、运行和收尾生命周期。
- 既有 TUI 命令的前置状态、硬件副作用和语义不得改变：`load/use/port` 不打开设备，`prepare` 才建立硬件会话，`disconnect/quit` 执行有序收尾。
- 既有机器可读前缀和退出语义属于兼容面，包括 `ok`、`error`、`phase=`、`verdict=`、`rawData=` 以及成功/启动或收尾失败的既有进程退出码。输出可以尾部追加字段，不得静默改变现有字段含义或顺序。
- 新能力优先通过配置和现有控制器 DTO 表达。确需新增命令或字段时只做追加式扩展；废弃项必须保留兼容别名、迁移说明和明确的移除周期。
- `TuiShellTest` 继续锁定命令解析、前置状态和会话覆盖；`HwtestTuiScriptedSession` 继续锁定真实进程命令流；`FrontendEquivalenceTest` 继续锁定 TUI/GUI 与 TUI/Web 的控制器阶段、终态和结果等价性。
- 每个新增项目至少增加一个从加载、选择、准备、运行、终态观察、结果读取到断开的前端工作流用例，并证明原有项目的上述回归未退化。涉及新硬件路径时还必须满足本文的证据级别和隔离测试要求。
- 项目文档只新增配置组合、资源选择和项目特有结果字段；通用命令含义统一引用使用指南，避免不同项目形成互相冲突的操作手册。

## 6. 构建与验证

根目录脚本是 Windows 下的推荐入口；它检查协议资产，使用 Visual Studio 2022 x64，并在 `test` 动作中启用完整测试。首次测试会把固定版本和哈希校验后的 GoogleTest 源码放入已忽略的 `tmp/deps/` 缓存：

    .\hwtest.ps1 build
    .\hwtest.ps1 test
    .\hwtest.ps1 test -Configuration Release
    .\hwtest.ps1 test -TestRegex "^(HalTypesTest|TuiShellTest)\."

浏览器前端单独验证：

    Set-Location front
    npm test
    npm run build
    Set-Location ..

以下是脚本对应的底层通用命令。完整构建是动态发现七个测试目标的前提。

    cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
    cmake --build build_vs --config Debug --parallel
    ctest --test-dir build_vs -C Debug -N
    ctest --test-dir build_vs -C Debug -L websocket --output-on-failure
    ctest --test-dir build_vs -C Debug --output-on-failure
    cmake --build build_vs --config Release --parallel
    ctest --test-dir build_vs -C Release -N
    ctest --test-dir build_vs -C Release --output-on-failure

ctest -N 只确认构建后动态发现的 CTest 清单；它不执行测试，也不证明通过。执行结果中的 GTEST_SKIP 必须在报告中单列为“未验证”，不能合并为通过。

`hwtest_web_tests` 动态发现的条目使用组合标签 `app_websocket`，两个 Web 进程测试使用 `app;websocket;process`；CTest 标签筛选按正则匹配，因此 `ctest -L websocket` 会同时选中全部 Web GTest 和进程测试。

要执行已批准 MB_DDF 基线的相关测试，而不是接受跳过，先在同一 PowerShell 会话显式设置并检查资产目录。该目录是当前协议事实源，但不是不可变的仓库内 fixture；每次结果应记录观测时间与实际文件清单：

    $env:MB_DDF_PROTOCOL_CSV_DIR = "H:\Resources\RTLinux\Demos\MB_DDF_v2\docs\design\product_protocol_csv"
    if (-not (Test-Path $env:MB_DDF_PROTOCOL_CSV_DIR)) { throw "MB_DDF CSV assets are required" }
    ctest --test-dir build_vs -C Debug -R "^(MbddfProtocolTest|SystemStatusExecutorTest|HalControlTransportTest|SystemStatusUdpIntegrationTest|TestApplicationControllerTest|TuiShellTest|GuiMainWindowTest|WebProtocolTest|WebSocket|FrontendEquivalenceTest)\." --output-on-failure

HAL 专属覆盖快照和缺口见 [HAL 测试设计报告](hal-test-design-report.md)。本文以外的历史计划不是现行测试规则或通过证据。
