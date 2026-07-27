# 测试规范

> 适用项目：多产品通用硬件测试软件（Qt 5.15 兼容、Qt 6 Core/Network/SerialPort/Widgets/WebSockets fallback / C++17 / Windows）。
>
> 本文是仓库全局唯一的测试规则、测试准入、运行方式和当前测试清单。[HAL 测试设计报告](hal-test-design-report.md) 仅保留历史快照；其他设计文档不得复制或替代本文的公共规则。
>
> 当前实现事实以公共 API、CMake 目标和测试注册为准。本文的“源级定义数”不是已执行、已通过或已验证的 CTest 结果。

## 1. 当前清单与统计口径

根 CMake 在 BUILD_TESTING 为真时加入 tests/；tests/CMakeLists.txt 当前加入 HAL、日志、BIZ、算法和应用五个目录，共生成七个 GoogleTest 可执行目标。七个目标均用 gtest_discover_tests 在构建后发现 CTest 条目；应用目录另直接注册 GUI offscreen 启动、runner/TUI/Web 的帮助或 smoke、根脚本帮助/非法 UI/选择 TUI、一个 TUI stdin 会话和一个 runner 异步错误，共十个进程测试。`src/adapters/ni_daqmx/tests` 还无条件注册一个不依赖真实 SDK 的 Fake NI-DAQmx CTest。`front/` 另有 Vitest，不进入 CTest 清单。

| 目录 | 测试目标 | 测试源文件 | 源级 GoogleTest 定义 | 当前范围 |
| --- | --- | ---: | ---: | --- |
| tests/hal/ | hwtest_hal_tests | 10 | 51 | HAL 接口、资源、安全、Mock、多 Adapter 路由、真实 C ABI 调用、宿主串口枚举、Qt 控制 Provider、版本化设备投影与可选任务 ABI |
| tests/log/ | hwtest_log_tests | 3 | 7 | 日志服务、JSONL sink、HAL 日志桥接 |
| tests/biz/ | hwtest_biz_tests | 6 | 41 | 配置、计划、单次/PC 周期/设备流调度、Qt 工作线程、样本、报告和架构边界 |
| tests/algorithm/ | hwtest_algorithm_tests | 3 | 31 | MB_DDF CSV、流式控制传输、任务范围控制连接与同 worker 停止收尾、陈旧 RX bank 错误的精确重发、固定命令、配置驱动单步交换、DI stimulus、定时器 START/STOP 清理和设备流能力判定 |
| tests/app/ | hwtest_app_tests / hwtest_gui_tests / hwtest_web_tests | 8 | 93 | 共享启动/控制器、TUI/GUI/WebSocket、DI 双设备准备/安全态校验/收尾、连续运行样本及后端 TXT 保存、单次禁止保存、异步停止与关闭、跨前端等价性、架构边界、配置 descriptor、测试配置白名单选择及既有经 HAL/Qt UDP 的两个独立测试闭环 |
| src/adapters/ni_daqmx/tests/ | hwtest_ni_daqmx_adapter_fake_tests | 1 | 0 | 原生 NI-DAQmx Adapter 与 Fake NIDAQmx API 的自定义 main/CTest |
| 合计 | 7 个 GoogleTest + 1 个 Fake CTest | 31 | 223 | 当前源级 GoogleTest 清单及一个非 GoogleTest 测试源 |

223 是当前测试源码中的 GoogleTest 定义数。Windows 完整构建后的 CTest 清单为 234 条：223 条动态发现的 GoogleTest 加 10 条应用入口/脚本进程测试和 1 条 `NiDaqmxAdapterFakeTest`。清单数量不表示已经执行或通过，只有实际运行 CTest 并报告零失败才能作通过结论。

2026-07-27 在 Windows/Visual Studio 2022 x64 构建树中设置已批准的 `MB_DDF_PROTOCOL_CSV_DIR` 后，Debug 与 Release 完整构建均成功，两个配置的 CTest 均为 232/232 通过。该结果包含仓库 Fake NIDAQmx、动态 Adapter fixture 和自退出的应用进程测试；证据等级仍按下文分类。

同日增加 PC 周期后端 TXT 保存后重新配置 `BUILD_TESTING=ON`，Debug 完整构建成功，更新后的 CTest 为 234/234 通过、无跳过；新增覆盖电气健康不限轮次收到两轮后由用户停止并完成 UTF-8-SIG/TSV 文件、单次即使请求保存也不创建文件、WebSocket `saveData` 类型边界、保存状态/路径快照和经真实 WebSocket/Qt UDP 的有限周期自然完成保存链。随后将电气健康用例强化为上述手动停止路径后，单项重新构建执行通过。该次未重复 Release，不能替代上一段的 Release 历史证据。

30 个含 GoogleTest 定义的源文件使用 `*_test.cpp` 命名；另有 1 个 NI Fake 自定义 main 测试源。四个 HAL DLL fixture、Fake NIDAQmx 库/头、GUI/Web 自定义 GoogleTest 入口、应用测试支持库和测试 helper 不包含 GoogleTest 定义，不计入 223。

浏览器前端当前有 8 个 `*.test.ts` 文件、36 条 Vitest，用于协议解析/请求、`saveData` 周期透传与单次/设备持续强制关闭、配置 descriptor 与测试配置白名单目录校验、默认配置选择、配置测量标签/单位、50,000 点有界缓冲、时间窗、min/max 降采样和同图/分图/自定义分组，以及 DI WebSocket payload 严格解析、命令队列合并、revision、失败回滚、复位串行、active-low、回读位和 settling。它们由 `npm test` 独立运行，不计入上述 234 条 CTest。

## 2. 当前覆盖与条件资产

| 目标 | 当前已覆盖的行为 | 证据边界 |
| --- | --- | --- |
| HAL | 错误映射、严格多设备资源映射、会话、Mock AD/DA/DI/DO、宿主串口枚举、串口 echo、CANFD loopback、控制资源路由、Qt UDP 回环和 timeout；动态 Fake ABI v1 的实际装载/数字批写/状态映射/缺符号、AdapterRouter 惰性多后端路由、逻辑 alias 映射、driver-only 初始化、版本化单设备 open projection、`HalService -> AdapterRouter -> CAbiAdapter -> NI parser/Fake DAQmx` 组合链、核心 ABI v1 的可选 task API 兼容、C ABI I/O/close 串行化、短读任务块、任务先 stop/close 再 safe state，以及 close 错误消费 handle | 自动化测试中的串口枚举不打开设备；动态 DLL 是 `tests/hal/fixtures` 的 Fake，Qt UDP 仅是本机 Provider 证据；COM3 实机结果属于下述独立手工 smoke，不是默认 CTest、NI SDK 或 PXI-6259 证据 |
| 原生 NI-DAQmx Adapter | 同一生产源码和独立 `ni_daqmx_config.*` 的 Fake 构建；PXI-6259 identity/open projection/占位 serial/topology/速率边界，按需 AI/AO/DI/DO，有限/连续任务路径，内部/外部时钟、start/reference/pause 触发、边沿计数/频率脉冲、短读、overflow/underflow、错误注入、任务状态、安全态/stop/clear 顺序；Vendor C ABI driver-only 初始化与单设备 open projection 交接；默认关闭的真实 SDK 构建路径 | `NiDaqmxAdapterFakeTest` 链接仓库 Fake `NIDAQmx`；独立可选 DLL 构建也使用 Fake 头/导入库。两者均不代表已安装 NI SDK、实际 PXI-6259、MAX 身份、接线、电平兼容、物理输出、触发时序或安全态验收 |
| 日志 | LogService、JsonLineFileSink、HalLogEvent 到 LogEvent 桥接 | 不覆盖 UI 或真实设备日志链 |
| BIZ | FakeAlgorithmExecutor 下的配置、计划、调度、重试、三种运行模式、专用 QThread 的 event dispatcher/计时器注册、同线程 `finishRun()`、可中断轮间等待、轮次/样本标记、状态、报告和架构扫描 | BIZ 不构造 HAL 假对象、Socket、codec 或硬件执行对象；Qt 线程回归只证明同步任务具备 Qt dispatcher，不证明算法调用期间持续泵送事件、HAL actor 或跨线程取消；设备流测试只证明 BIZ 单次调用边界，不证明某产品支持主动回告 |
| 算法 | 帧编解码、CSV 无效输入、流式短读/粘包/噪声/超时、任务内控制通道复用及停止时由 worker 收尾、陈旧 RX bank 错误精确重发及当前序号错误不重发、SYSTEM_STATUS 模拟器、固定命令执行器、配置驱动单步交换、DI_READ golden、DI stimulus 配置/全量批写/active-low/revision/白名单/错误回退、定时器 START/STOP 清理、ELEC_HEALTH_STATUS 字段判定和设备流拒绝 | DI stimulus 使用 `IHalDevice` Fake；其余五项目前只有协议/脚本化执行、Fake 或本机模拟证据，尚无真实板端、NI 或 PXI-6259 验收；本机 UDP 模拟目标不等同于真实板端通讯 |
| 应用/TUI/GUI/WebSocket | 共享启动参数与覆盖顺序、测试配置目录发现及白名单 `configId` 切换、控制资源与会话串口选择、DI descriptor/双设备准备/复位/revision/异步停止安全收尾、线程亲和和运行代次隔离、同步/异步停止门禁、GUI/Web 非阻塞关闭、Web JSON/Origin/单客户端/16 KiB/关闭码、完整快照/样本投影（含配置 descriptor、数字刺激 DTO 和保存状态）、DI 动作严格参数白名单、PC 周期两轮 UDP 指令—反馈闭环及完整 TXT 保存、单次禁止保存、TUI/GUI 与 TUI/Web 的配置/通过/超时/停止等价性、GUI/Web 源码和链接架构扫描、runner/TUI/GUI/Web/根脚本入口 | DI 应用测试将 `ni.daqmx.libraryPath` 指向动态 Fake DLL，并使用本机 UDP peer；连续保存自动化使用临时目录和 Qt UDP 隔离目标，只证明宿主文件格式与应用链，不证明真实板端长期采集或掉电恢复。Web 集成当前覆盖参数白名单与权威快照，不覆盖成功 DI 写、revision 冲突或断开安全态的 Web 端到端链。默认自动化不能外推到真实网口、NI/PXI-6259、其他 DUT 或长期稳定性 |
| 浏览器前端 | Vitest 下的类型化协议、测试目录与 descriptor 校验、默认配置选择、`saveData` 周期透传和非周期强制关闭、字段标签/单位、有界数据结构、字段发现、时间窗、降采样和图组；DI WebSocket snapshot/reply 严格解析，以及 16 路面板所复用的 32 ms 合并、revision、失败回滚、reset 串行、active-low、`di_state[0]`/`di_state[1]` 回读和 settling；TypeScript/Vite 单文件生产构建 | SessionProvider/总览页面板已由源码接线，但没有独立浏览器 DOM 组件或真实 WebSocket/PXI-6259 端到端验收；Vitest 不证明真实硬件、长期浏览器稳定性或设备流算法 |

下列测试依赖条件资产，缺失时可调用 GTEST_SKIP。跳过只表示该次没有执行断言，不能证明任何协议、配置迁移、SYSTEM_STATUS、HAL 或硬件能力。

- 5 个 MB_DDF 协议测试和 38 个 MB_DDF 跨层/集成测试依赖 `MB_DDF_PROTOCOL_CSV_DIR` 指向的外部 CSV 资产目录；后者包含应用/TUI/GUI/Web UDP 运行、连续采样/TXT 保存、停止、关闭或等价性测试。
- BIZ 的导入附件样例测试依赖 tmp/hwtest_BIZ/configs/sample_product.testcfg；tmp 不是仓库实现事实。
- 算法测试中有 12 个自包含的帧、传输、DI stimulus、能力判定或临时 CSV 用例；其余 19 个依赖外部 MB_DDF CSV。

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
- 使用外部 32 份协议 CSV 完成 Debug 全量构建和 195 条 CTest，结果为 195/195 通过；随后 Release 全量构建和 CTest 同样为 195/195 通过。
- 自退出的 `hwtest_pc_runner` 随后使用 `CONTROL_SERIAL`、COM3、`614400 / 8E1 / 无流控` 发起一次 SYSTEM_STATUS；调用覆盖串口打开、写入、等待读取和关闭，未再观察到 `QObject::startTimer` 警告，但因板端产品协议服务未响应而以 `BusTimeout` 结束。目标板 SSH 22 端口探测也未在 15 秒内成功。
- 随后在用户再次明确授权、板端可达且无遗留服务的条件下，使用来源提交 `982b3f5bbce222aea061e9ce1523ba926c801658` 的 `HW_TEST Release` 画像启动产品协议服务。宿主只启动 Debug `hwtest_web`，显式选择 `CONTROL_SERIAL`、COM3 和外部 32 份协议 CSV；未启动浏览器/Vite 前端，WebSocket 客户端直接发送 `load -> prepare -> single -> pc_periodic -> quit`。
- 单次运行收到一个 `SYSTEM_STATUS` 样本并到达 `finished/Pass/Ok`，`status=0`、`err_code=0`。随后 `pc_periodic` 以 `intervalMs=100`、`maxCycles=3` 完成轮次 `1,2,3`，样本事件序号为 `2,3,4`，最终快照为 `cycleIndex=3`、`sampleCount=3` 和 `finished/Pass/Ok`；三轮产品协议序号依次为 `4660,4661,4662`。
- 在同一授权板端和 COM3 上切换到独立 `ELEC_HEALTH_STATUS` 配置后，单次运行收到一个 `ELEC_HEALTH_STATUS` 样本并到达 `finished/Pass/Ok`，`status=0`、`err_code=0`；观测电压和辅助量作为样本输出保留。随后以 `intervalMs=100`、`maxCycles=3` 完成轮次 `1,2,3`，样本事件序号为 `2,3,4`，最终快照为 `cycleIndex=3`、`sampleCount=3` 和 `finished/Pass/Ok`；三轮产品协议序号依次为 `4660,4661,4662`。
- 两个测试项的 WebSocket 客户端都执行了 `load -> prepare -> single -> pc_periodic -> quit`，未启动浏览器/Vite。健康测试后端完整诊断中 `QObject::startTimer` 和 `Timers can only be used with threads started with QThread` 均为 0；`quit` 以关闭码 `1000` 完成，随后核对板端身份后发送 `SIGTERM`，18765 无监听、远端无 `MB_DDF_v2`、COM3 可重新独占打开并关闭。
- 从后端启动到 `quit` 的完整进程诊断输出只有 WebSocket 就绪行，`QObject::startTimer: Timers can only be used with threads started with QThread` 出现次数为 `0`。`quit` 回复成功并以 WebSocket 关闭码 `1000` 结束；随后对身份核对为 `MB_DDF_v2` 的板端进程发送 `SIGTERM`。收尾复核确认 18765 无监听、本地无后端或调试进程、远端无 `MB_DDF_v2`，COM3 可重新独占打开并关闭。
- 这次复测把 QThread 修复后的短时真实串口单次/三周期成功链补齐，但仍不是默认 CTest 或生产验收；没有覆盖长时稳定性、通信超时、拔插、运行中停止、板端异常退出或物理安全状态恢复。`HW_TEST` 画像本身也不提供状态恢复。

### 2.3 2026-07-27 SYSTEM_STATUS 第二帧诊断

- 在用户明确授权且 DUT 上电连接的条件下，使用 PC 的 COM3 `614400 / 8E1 / 无流控` 直连正在运行的 `HW_TEST Release`。保持同一次串口打开并连续发送合法 `SYSTEM_STATUS` 后，实测响应严格交替为约 `106..125 ms` 的正常 `01/01` 响应和约 `11..28 ms` 的 `FF/00 error_response`；因此复现不依赖 PC 关闭、重开串口。
- 错误帧解码为原命令 `01/01`、原序号 `0x1031`、`err_code=0x0102`、`detail=0`，而当前请求序号为 `0x1235`。临时板端诊断记录的失败 payload 为固定陈旧数据，非当前请求；COM RX RAM 前后观测进一步显示两个乒乓 bank 中只有一路取得当前帧，另一路反复提供同一陈旧内容。当前证据将根因定位在 DUT/FPGA COM3 RX bank 路径，而不是宿主序号递增或串口重开。
- 宿主修复把控制连接生命周期改为“一次 BIZ 任务打开一次”，同一任务的步骤重试、PC 周期和跟随请求复用连接，并只对上述原命令匹配、原序号陈旧、`0x0102/detail=0` 的错误响应原帧重发一次。回归同时锁定普通 CRC 错误和当前序号错误响应不得触发该重发；临时 COM RAM 打印已从 DUT 源码移除，板端仅保留解析失败的错误码、detail 和 payload 十六进制诊断。
- 自动化覆盖任务内一次打开/关闭、停止等待运行收尾且由同一 worker 关闭传输、陈旧错误后成功以及当前请求错误不重发。首次正式镜像复核时，目标 event 2 一度不再增加，随后 PCIe 用户逻辑读回 `0xffffffff` 且 SSH 不再发送协议 banner；目标完整断电重上电后恢复为 PCIe `5.0 GT/s x4`、XDMA event 2 可用，正式 `HW_TEST Release` 镜像 SHA-256 为 `3eb9d2decdeb06c8f83aeb38aaf7bd4540e7d0170b0225b7857c0a69635eed7d`。
- 重上电后的首个独立 `SYSTEM_STATUS` 单次任务一次请求即为 `Pass/Ok`；第二个独立任务先在约 10 ms 收到陈旧 bank 的 `FF/00`，再于同一次串口打开内原帧重发并在约 110 ms 收到正常响应，结果仍为 `Pass/Ok`、`attempts=1`。宿主日志对应一次 `openControl`、两次 `writeControl/readControl` 和一次 `closeControl`，板端 event 2 从 1 增至 3，并记录一条 `0x0102/detail=0` 的陈旧 payload。
- 随后经真实 `hwtest_web` WebSocket 路径执行 `load -> prepare -> pc_periodic -> quit`，以 `intervalMs=150`、`maxCycles=6` 完成六轮，最终为 `finished/Pass/Ok`、`cycleIndex=6`、`sampleCount=6`、`attempts=1`，样本序号为 `4660..4665`。六轮共用一次串口打开和关闭，宿主记录 12 次写、12 次读和 6 个成功结果；板端 event 2 从 3 增至 15，并记录 6 次陈旧 bank 请求。再执行 4 个独立单次任务均为 `Pass/Ok`，event 2 从 15 增至 23。测试后 `quit` 正常关闭后端，板端进程经 `SIGTERM` 停止，18765 无监听且无遗留 `hwtest_web`/`MB_DDF_v2` 进程。
- 该证据确认宿主的任务范围连接复用和精确一次重发能够覆盖当前实机故障，但没有修复 FPGA COM3 双 RX bank 本身；若错误响应特征变化、连续两个 bank 都不可用或目标不产生中断，仍会按原有协议错误或超时失败，不能据此宣称底层硬件缺陷已消除。

### 2.4 2026-07-27 COM1 双 RX bank 多轮回环门禁

- DUT Demo 的 COM1 全能力流程新增 16 轮内部回环。每轮使用不同的 32 字节 payload，轮次编码在偏移 13；同一次 `XdmaTransport` 打开、同一份 Level 中断/内部回环配置下依次执行发送和接收，不重试，并逐轮严格比较长度和全部字节。失败日志保留轮次、实际长度、首个差异和收发十六进制，任一失败都会使能力组失败，但测试仍跑完余下轮次以观察奇偶分布；通过条件固定为 `16/16`、奇数 `8/8`、偶数 `8/8`。
- AArch64 单元回归 `HwComLoopbackWorkflow*` 在 `root@192.168.1.29` 上执行两种内存端点模型且共 2 项通过。双 bank 都返回当前帧的模型为 `16/16`；一路固定返回首轮旧帧的模型稳定得到 `8/16`、奇数 `8/8`、偶数 `0/8`，工作流正确返回失败并继续完成全部 16 轮。该回归不访问 FPGA，只证明门禁能够识别交替陈旧 bank，不能作为真机缺陷证据。
- 同一板端随后执行完整 `MB_DDF_HW_Tests`：160 项中 157 项通过、2 项 SPI Flash 真机用例按条件跳过、1 项既有 `SystemTestProviderTest.ReadsTargetScmiCpuAndXdmaPcieSources` 失败；失败原因是当前目标不存在 `/sys/kernel/debug/clk/scmi_clk_cpul/clk_rate`，与 COM 回环改动无关。不得把该次完整目标记录为全通过。
- 随后按用户明确授权执行 `dut/debug.bat hw_run`，使用 Arm GNU `aarch64-none-linux-gnu 11.3 rel1` 构建 Release Demo 并部署到同一目标板。板端与本地二进制 SHA-256 均为 `31a6a2cbf9e23db199f770164484ec992790aa299449fbfc8486ba2589f099ae`。COM1 Adapter 预检通过，16 轮真实内部回环全部通过，摘要为 `16/16`、奇数 `8/8`、偶数 `8/8`，错误状态读取和原 COM 配置恢复也成功。
- 本次 COM1 结果没有复现双 bank 缺陷。它表明当前 bitstream 的 COM1 内部回环路径通过修复门禁；不能据此宣称 2.3 节已确认的 COM3 外部 RX 缺陷也已修复，因为通道和接收来源均不同。要关闭 COM3 缺陷，仍需在 COM3 外部线缆路径以逐轮唯一请求复测，并确认不再出现奇偶交替的旧序号或旧 payload。
- `hw_run` 整体退出码为 1，独立原因是 DH 主发动机 2 完成回告停留在 `0xFFFF` 并超时，以及 `/dev/spidev0.0` 的 Status/两个 Die Flag/JEDEC ID 均读到 `0xFF`；SPI 在地址读取和擦写前中止。该退出码不是 COM1 回环失败。运行结束后远端无 `MB_DDF_v2` 或 `gdbserver` 遗留进程。

## 3. 五级证据模型

下表按证据强度递增。低级证据不能替代高级证据，也不得把未实现级别写成已验证能力。

| 级别 | 证据对象 | 当前状态 | 可证明与不可证明的范围 |
| --- | --- | --- | --- |
| 1. 协议 Simulator | 配置 -> BIZ -> SystemStatusAlgorithmExecutor -> SystemStatusSimulator -> golden request frame | 已有；遗留的非 HAL 跨层替身回归，依赖外部 CSV | 仅证明当前 SYSTEM_STATUS 的模拟器闭环、CRC 和超时处理；不是产品模拟或集成验收范式 |
| 2. HAL Mock 集成 | 算法 -> IControlChannel -> HAL Mock Provider | SYSTEM_STATUS/ELEC_HEALTH_STATUS 正向闭环未实现 | 现有 MockAdapter 回环不是控制通道 Mock Provider；算法 fake 只作传输契约测试 |
| 3. Qt Provider | `qt.serial`/`qt.udp` 标准 Qt 通讯 Provider | 部分实现 | Qt UDP 已有原始回环及经 BIZ/算法/HAL 的本机模拟目标闭环；Qt 串口已有带历史线程告警的早期 COM3/DUT smoke，以及 QThread 迁移后成功且无该警告的单次/三周期复测；自动化 hardware target 和异常路径仍缺失，TCP 未实现 |
| 4. Vendor Adapter | 厂家 Adapter DLL/SDK 经 C ABI 接入 | 通用 C ABI 与可选原生 NI-DAQmx Adapter 已实现，自动化仅有 Fake | `CAbiAdapter` 已实际装载/调用核心 ABI v1，并有惰性多 Adapter、数字批写和可选 task ABI 兼容回归；`hwtest_adapter_ni_daqmx` 具有默认关闭、待具备 SDK 后才可配置的构建路径，以及覆盖 PXI-6259 软件任务路径的 Fake NIDAQmx CTest。Fake 不能证明已安装 SDK 或真实厂家设备；没有 `hardware` 真机标签 |
| 5. 真实硬件 | 隔离台架上的真实目标和测试设备 | 部分手工证据 | 已有上述授权 COM3 SYSTEM_STATUS/ELEC_HEALTH_STATUS 历史及 QThread 迁移后复测；NI-DAQmx 路径已实现但没有 DI/NI/PXI-6259 真机、宿主 CTest hardware target、长期稳定性、异常收尾或完整物理安全验收 |

新增五级中的任何目标时，文档、CMake 和测试必须同时标明其级别、依赖资产、隔离条件和通过证据。在代码与测试落地前，一律标记为“未实现”。

## 4. 分层与集成规则

| 范围 | 单元测试允许依赖 | 禁止或不作为通过证据 |
| --- | --- | --- |
| BIZ | FakeAlgorithmExecutor、配置样本、结果和报告样本 | HAL、Adapter、Socket、codec、测量对象、硬件安全执行 |
| 应用/TUI/GUI/WebSocket/浏览器 | `TestApplicationController`、前端支持库、TUI 命令解析、Qt offscreen、Qt WebSockets 回环、本机 UDP 隔离目标、动态 Fake Adapter DLL、纯前端数据结构和浏览器 API | 前端直接持有 HAL、算法或 DUT/生产 I/O Socket；GUI/Web 调用阻塞等待；浏览器用定时器重复 `start`；真实硬件结论；ANSI 屏幕文本作为业务状态源。`QWebSocketServer/QWebSocket` 只属于前端传输，不属于 DUT I/O |
| 算法 | 协议 CSV 样本、Simulator、脚本化传输、IHalDevice 测试替身 | UI、业务调度实现、厂家 SDK |
| HAL | MockAdapter、动态 Fake ABI v1 fixture、资源配置 | 业务判定、产品协议字段解释、真实厂家硬件结论 |
| Vendor Adapter | 厂家 SDK 假对象或隔离仿真 DLL；NI-DAQmx 可使用同一 Adapter 源码加 Fake NIDAQmx API | UI、BIZ、算法判定；Fake DLL/Fake SDK 不得写成已安装厂家 SDK 或真机证据 |
| 真实硬件 | 经授权的隔离设备、独立报告和显式硬件目标 | 默认 CI 或开发机自动执行 |

跨层验收必须明确是契约测试、协议测试、HAL Mock 集成、Provider 集成、Vendor Adapter 集成或真实硬件验收。不得把串口 echo、CANFD loopback 或 Simulator 结果描述为真实通讯证据。

`SYSTEM_STATUS` 与 `ELEC_HEALTH_STATUS` 当前都有“配置 -> BIZ -> 算法 -> HalControlTransport -> HAL -> qt.udp -> 本机模拟目标”自动化成功链，以及上述 `qt.serial -> COM3 -> MB_DDF_v2` 手工成功链。BIZ QThread 回归证明调度 worker 具备 Qt dispatcher，迁移后的真实串口短时成功复测进一步证明固定 COM3/DUT 链路未再产生目标计时器警告；两者仍不能替代长时和异常验收。DI 的算法、应用、通用 C ABI、NI Adapter 和前端证据分别是 Fake/Mock/纯前端层级，不能与上述 COM3 真机证据混合。HAL Mock Provider 正向链、真实串口自动化异常路径、PXI-6259 验收和 `ProtocolProfile -> CSV -> HAL ResourceId` 一致性校验仍是未实现验收项。

## 5. 测试准入

- 公共 HAL 或 BIZ 头文件、配置字段、状态语义、错误码、资源类型或 Adapter ABI 变化时，必须同步相应契约文档和回归测试。
- 修改 BIZ 时，必须运行 hwtest_biz_tests 和 BIZ 架构扫描；BIZ 测试不得引入硬件执行依赖。
- 修改协议 CSV 规则、解析器或资产引用时，必须同步 device-communication-protocol.md 和协议契约测试，并记录基线路径、观测时间与清单；manifest/hash 机制落地后再记录固定版本和内容哈希。
- 修改 Mock/Fake 行为时，必须说明证据级别；可配置超时/错误注入和 SYSTEM_STATUS/ELEC_HEALTH_STATUS 控制通道 Mock Provider 集成仍未实现，不得作为既有能力验收。动态 Fake C ABI fixture 只能验证 ABI/Adapter 路径，不能改写为厂家 SDK 或真机结果。
- 修改 Qt Provider、通用 C ABI、原生 Vendor Adapter 或真实硬件路径时，必须新增相应级别的隔离测试；当前 Qt UDP 有本机隔离测试，通用 C ABI 已有动态 Fake DLL、多 Adapter 懒加载、状态映射、数字批写和可选 task ABI 回归，PXI-6259 NI-DAQmx 已有可选 SDK 构建路径及 `NiDaqmxAdapterFakeTest`，BIZ 已有 Qt dispatcher/计时器注册回归，Qt 串口已有带历史告警的早期 smoke、无响应无告警诊断和迁移后成功无告警的手工复测；CTest `hardware` 标签、自动化异常路径和全面真实硬件验收仍未实现。
- 修改 `digitalStimulus` 配置、刺激 DTO/动作、revision 语义或安全收尾时，必须同步算法、应用、WebSocket 和前端纯逻辑回归；至少覆盖配置白名单、陈旧 revision 无写入、全量批写、写失败状态、动作参数拒绝和停止/收尾的 Fake/Mock 边界。成功 Web DI 写、revision 冲突和断开安全态的 Web 端到端覆盖仍待补齐。
- 修改共享应用控制器、runner 或 TUI 命令时，必须运行 `hwtest_app_tests`；修改 Qt GUI 时还必须运行 `hwtest_gui_tests`；修改 WebSocket 协议、服务器、入口或脚本时必须运行 `hwtest_web_tests`、`ctest -L websocket` 和 `HwtestWeb*` 进程测试。修改 `front/` 时必须运行 `npm test` 和 `npm run build`。Qt GUI、WebSocket 后端和浏览器 Web UI 必须复用控制器 DTO/事件，不得以新增前端为由复制组合根。
- 修复行为缺陷时，先补能复现问题的回归测试，再修改实现。

### 5.1 前端使用习惯兼容准入

面向操作人员的基础生命周期固定为 `load -> select -> prepare -> run -> terminal -> result -> disconnect`；TUI 中的 `use/port`、`wait/status` 分别承担 select 和 terminal 观察动作。用户操作说明见 [TUI 使用指南](../../user/tui-usage-guide.md)。

当前 `TestApplicationController` 通过统一注册表接受七个已知算法 ID（`mbddf.system_status`、`mbddf.elec_health_status`、`mbddf.memperf`、`mbddf.spi_flash`、`mbddf.dh_pulse_config`、`mbddf.timer_jitter`、`mbddf.di_read`），并要求恰好一个启用的对应单步。以下规则是新增测试项目的合入门禁，不得据此宣称任意产品协议都已实现：

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

NI-DAQmx 的默认自动化是 Fake，不要求安装真实 SDK 或连接设备：

    ctest --test-dir build_vs -C Debug -L ni_daqmx --output-on-failure

它运行 `NiDaqmxAdapterFakeTest`（标签 `hal;adapter;ni_daqmx;fake`），不代表硬件验证。若要构建原生 DLL，应使用另一个构建目录；CMake 可自动搜索已安装 SDK，但为保证路径和 x64 导入库可复现，建议显式提供以下两个参数，缺任一可发现的头/库会在配置时失败：

    cmake -S . -B build_ni -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON -DHWTEST_ENABLE_NI_DAQMX=ON -DNI_DAQMX_INCLUDE_DIR="<NIDAQmx.h 所在目录>" -DNI_DAQMX_LIBRARY="<NIDAQmx.lib 路径>"
    cmake --build build_ni --config Debug --parallel

上述配置会在常规目标之外生成 `hwtest_adapter_ni_daqmx`；真实 PXI-6259 的部署、MAX 身份、接线、测试和安全验收必须在授权的隔离台架另行记录，不能由该命令或 Fake CTest 得出。

2026-07-27 另以仓库 Fake `NIDAQmx.h` 和 Debug Fake 导入库配置 `BUILD_TESTING=OFF`、`HWTEST_ENABLE_NI_DAQMX=ON` 的独立构建树，`hwtest_adapter_ni_daqmx` Debug DLL 构建成功；`dumpbin /exports` 同时确认 `hal_adapter_get_api_v1` 与 `hal_adapter_get_task_api_v1`。该记录只证明可选目标和 ABI 导出面可构建。

要执行已批准 MB_DDF 基线的相关测试，而不是接受跳过，先在同一 PowerShell 会话显式设置并检查资产目录。该目录是当前协议事实源，但不是不可变的仓库内 fixture；每次结果应记录观测时间与实际文件清单：

    $env:MB_DDF_PROTOCOL_CSV_DIR = "H:\Resources\RTLinux\Demos\MB_DDF_v2\docs\design\product_protocol_csv"
    if (-not (Test-Path $env:MB_DDF_PROTOCOL_CSV_DIR)) { throw "MB_DDF CSV assets are required" }
    ctest --test-dir build_vs -C Debug -R "^(MbddfProtocolTest|SystemStatusExecutorTest|HalControlTransportTest|SystemStatusUdpIntegrationTest|TestApplicationControllerTest|TuiShellTest|GuiMainWindowTest|WebProtocolTest|WebSocket|FrontendEquivalenceTest)\." --output-on-failure

HAL 专属覆盖快照和缺口见 [HAL 测试设计报告](hal-test-design-report.md)。本文以外的历史计划不是现行测试规则或通过证据。
