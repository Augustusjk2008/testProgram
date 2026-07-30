# 测试规范

> 适用项目：多产品通用硬件测试软件（Qt 5.15 兼容、Qt 6 Core/Network/SerialPort/Widgets/WebSockets fallback / C++17 / Windows）。
>
> 本文是仓库全局唯一的测试规则、测试准入、运行方式和当前测试清单。旧 HAL 测试报告已移入 `../history/`，现行设计文档不得复制或替代本文的公共规则。
>
> 当前实现事实以公共 API、CMake 目标和测试注册为准。本文的“源级定义数”不是已执行、已通过或已验证的 CTest 结果。

## 1. 当前清单与统计口径

根 CMake 在 BUILD_TESTING 为真时加入 tests/；tests/CMakeLists.txt 当前加入 HAL、日志、BIZ、算法和应用五个目录，共生成七个 GoogleTest 可执行目标。七个目标均用 gtest_discover_tests 在构建后发现 CTest 条目；应用目录另直接注册 GUI offscreen 启动、runner/TUI/Web 的帮助或 smoke、根脚本帮助/非法 UI/选择 TUI、一个 TUI stdin 会话和一个 runner 异步错误，共十个进程测试。`src/adapters/ni_daqmx/tests` 还无条件注册一个不依赖真实 SDK 的 Fake NI-DAQmx CTest。`front/` 另有 Vitest，不进入 CTest 清单。

| 目录 | 测试目标 | 测试源文件 | 源级 GoogleTest 定义 | 当前范围 |
| --- | --- | ---: | ---: | --- |
| tests/hal/ | hwtest_hal_tests | 10 | 56 | HAL 接口、资源、安全、Mock、多 Adapter 路由、真实 C ABI 调用、宿主串口枚举、Qt 控制 Provider、多控制资源独立会话与串口占用、版本化设备投影及可选任务 ABI |
| tests/log/ | hwtest_log_tests | 3 | 7 | 日志服务、JSONL sink、HAL 日志桥接 |
| tests/biz/ | hwtest_biz_tests | 6 | 44 | 配置、计划、单次/PC 周期/设备流调度、零间隔严格串行及暂停/恢复/停止门禁、设备流禁用步骤重试、不透明运行参数透传、Qt 工作线程、样本、报告和架构边界 |
| tests/algorithm/ | hwtest_algorithm_tests | 13 | 104 | MB_DDF CSV、1..255 payload、F32 来源类型、流式控制传输、BUS LOOP/ECHO 链路与双通道短读回写、运行参数 Schema/归一化、任务范围控制连接与同 worker 停止收尾、固定命令、惯测/舵机设备流、IMU 宿主时间戳间隔、DI stimulus；以及后处理公共端口、版本化捕获、五种舵机波形、扫频局部同步估计、状态归约、取消、有限 JSON 和可追溯 fixture |
| tests/app/ | hwtest_app_tests / hwtest_gui_tests / hwtest_web_tests | 13 | 156 | 共享启动/控制器、配置目录十一项发现、BUS 模式/辅助资源/紧凑字段契约、设备流本地时间配置、TUI/GUI/WebSocket、DI 与运行能力、零间隔参数、连续 TXT 保存、惯测/舵机 Qt UDP；以及分析资源配置、STOP 双栅栏和尾样本、完整舵机捕获—分析—查询链、Web v1 capability/批量遥测协商、全局安全序号、FIFO/背压和出站边界 |
| src/adapters/ni_daqmx/tests/ | hwtest_ni_daqmx_adapter_fake_tests | 1 | 0 | 原生 NI-DAQmx Adapter 与 Fake NIDAQmx API 的自定义 main/CTest |
| 合计 | 7 个 GoogleTest + 1 个 Fake CTest | 46 | 367 | 45 个含 GoogleTest 定义的源文件及一个非 GoogleTest 测试源 |

367 是当前测试源码中的 GoogleTest 定义数。Windows 完整构建并完成测试发现后的当前 CTest 清单为 378 条：367 条动态发现的 GoogleTest 加 10 条应用入口/脚本进程测试和 1 条 `NiDaqmxAdapterFakeTest`。清单数量本身不表示通过，执行证据见下述按时间记录。

2026-07-27 在 Windows/Visual Studio 2022 x64 构建树中设置已批准的 `MB_DDF_PROTOCOL_CSV_DIR` 后，Debug 与 Release 完整构建均成功，两个配置的 CTest 均为 232/232 通过。该结果包含仓库 Fake NIDAQmx、动态 Adapter fixture 和自退出的应用进程测试；证据等级仍按下文分类。

同日增加 PC 周期后端 TXT 保存后重新配置 `BUILD_TESTING=ON`，Debug 完整构建成功，更新后的 CTest 为 234/234 通过、无跳过；新增覆盖电气健康不限轮次收到两轮后由用户停止并完成 UTF-8-SIG/TSV 文件、单次即使请求保存也不创建文件、WebSocket `saveData` 类型边界、保存状态/路径快照和经真实 WebSocket/Qt UDP 的有限周期自然完成保存链。随后将电气健康用例强化为上述手动停止路径后，单项重新构建执行通过。该次未重复 Release，不能替代上一段的 Release 历史证据。

同日补充三种运行模式边界后，新增用例先在旧实现上全部失败，再在修正后通过：配置缺失能力字段只回退 `single`，`pc_periodic`/`device_stream` 冲突配置被加载和目录发现共同拒绝，未声明模式在启动前返回 `CapabilityUnsupported`，设备持续模式忽略 PC 周期参数但可生成带 `run_mode=device_stream` 和周期字段 `NA` 的数据文件。使用外部 32 份协议 CSV 完成 Debug 与 Release 全量构建和 CTest，两个配置均为 239/239 通过、无跳过；浏览器前端 8 个测试文件共 37/37 Vitest 通过，TypeScript/Vite 单文件生产构建成功。Release 构建仍有 Qt 5 `QVector` 与当前 MSVC 标准库组合产生的既有弃用警告，不影响构建和测试结论。

2026-07-28 新增 IMU 设备持续流后，外部来源和 `dut/` 的协议生成器测试均为 19/19，通过 37 份 CSV 严格校验；两套 DUT `hw_tests` 均完成 AArch64 Debug 交叉构建，但未部署或运行目标板。宿主使用外部 37 份 CSV 顺序完成 Debug 与 Release 全量构建，两个配置均为 247/247 CTest 通过、无失败、无跳过；覆盖一次 START、完整主动反馈、至少一帧通过、零帧失败、异常 STOP 不重发和固定 23 列 TXT。浏览器前端仍为 8 个文件、37/37 Vitest 通过，TypeScript/Vite 单文件构建成功。Release 仅保留 Qt 5 `QVector<QString>` 与当前 MSVC/GoogleTest 组合产生的既有 `C4996/STL4043` 弃用警告。该自动化证明协议、宿主和交叉构建软件路径，不证明 COM4、921600、400 Hz 吞吐或真机停止收尾。

2026-07-28 完成设计—代码一致性收敛后，新增 requestId 全操作传播、默认帧日志脱敏/显式完整帧调试、WebSocket v1 高位 DI 隐藏和按配置隔离图表工作区回归。使用外部 37 份 CSV 完成 Debug 与 Release 全量构建，两个配置均为 249/249 CTest 通过、无失败、无跳过；浏览器前端为 9 个文件、39/39 Vitest 通过，TypeScript/Vite 单文件生产构建成功；`dut/` 的 37 份协议 CSV 校验通过。两个宿主配置仍只出现既有 Qt 5/MSVC `C4996/STL4043` 弃用警告。本轮未执行 DUT AArch64 交叉构建或目标板测试，不能据此增加板端证据等级。

2026-07-28 新增舵机远端连续实测与算法运行参数 Schema 后，宿主使用仓库内 37 份协议快照完成 Debug、Release 全量构建；两个配置的 CTest 都是 263 项中 262 项执行通过、1 项条件跳过、0 失败。跳过项仍是已清理 `tmp/hwtest_BIZ/configs/sample_product.testcfg` 的导入附件样例，不计为通过证据。浏览器前端 10 个文件、44/44 Vitest 通过，TypeScript/Vite 单文件生产构建成功。DUT 协议生成器 19/19、37 份 CSV 校验通过；Windows PyQt 主机侧 199/199 通过。AArch64 Debug/Release 均交叉构建 `MB_DDF_v2` 和独立 `MB_DDF_v2_HelmControl`，`MB_DDF_HW_Tests` 目标也交叉构建通过，但未部署或在目标板运行，因此不构成 DDS/舵机实测证据。

2026-07-28 完成本轮审阅修复与连续流双时间轴收敛后，宿主使用仓库内 37 份协议快照分别完成 Debug、Release 全量构建；两个配置的 272 条 CTest 注册项均为 271 项执行通过、1 项条件跳过、0 失败。跳过项仍是缺少外部同事导入附件的 `TestConfigManagerTest.ImportedColleagueSampleLoadsAndBuildsThroughMigrationBoundary`，不计为通过证据。新增自动化覆盖 IMU 已发布样本固定 `2500 us` 相对轴、流执行器重入重新锚定、HELM DDS 实际相对轴/倒退/JSON 安全整数上限、U16 重复/反向/半环/自然回绕、`device_stream` 启动前拒绝步骤重试、可选相对时间 Web 投影及非法时间服务端拒绝、TXT `sample_time_us=0/2500` 与负哨兵回退、F32 最短可往返文本和必填运行参数前端拦截。浏览器前端 10 个文件、46/46 Vitest 通过，TypeScript/Vite 单文件生产构建成功；构建仅出现既有 Qt 5/MSVC `C4996/STL4043` 弃用警告。本轮未运行 DUT 协议生成器、PyQt 主机侧套件、AArch64 交叉构建或目标板测试，不能据此增加 COM、DDS、舵机或板端证据等级。

2026-07-29 落地舵机停止后性能 sidecar 后，使用仓库内 37 份协议快照执行 `hwtest.ps1 test` Debug 及显式 Release 全量构建/CTest；两个配置各有 339 条注册项，均为 338 项执行通过、1 项条件跳过、0 失败。跳过项仍是缺少外部同事导入附件的 `TestConfigManagerTest.ImportedColleagueSampleLoadsAndBuildsThroughMigrationBoundary`，不计为通过。Release 首轮发现 Origin 白名单测试把客户端断开误当作异步 `DropCleanup` 已结束的竞态；测试按独立服务实例隔离后在 Release 连续 20 次通过，随后 Debug/Release 全量均通过。新增证据覆盖 DDS bool 写入契约、版本化流式捕获、五种波形、正反/截断/纯时延扫频、资源限制、尾样本双栅栏、分析身份、协作取消与 join 超时、诊断保留、TXT SHA-256、确定性关键点投影、抽样前全序列有限值校验、最终发布摘要的完整 8 KiB 预检、完整出站 16 KiB、真实 Qt UDP 舵机捕获—分析—结果查询链，以及分析结果不修改采集 verdict/error。浏览器前端为 15 个文件、61/61 Vitest 通过，TypeScript/Vite 构建生成仅含 `dist/index.html` 的 520,792 字节单文件。DUT 主机侧 `dut/tools/tests` 为 20/20 pytest 通过，AArch64 Debug `hw_tests` 画像交叉构建 `MB_DDF_HW_Tests`/`MB_DDF_HW_Smoke` 等目标成功；未部署或运行目标板，也未启动真实舵机台架，不能据此增加真机性能证据等级。

2026-07-29 舵机 DDS 解锁与 PWM 门控收敛后，先在新增生命周期测试缺少 `HelmPwmLifecycle`
时观察到 AArch64 `hw_tests` 交叉编译按预期失败，实现后重新交叉构建成功。新增测试源覆盖
DDS B27 `helm_unlock`、解锁首帧、运行帧、STOP 回零尾帧、发布失败可观测性，以及启动强制
关 PWM、DO0 解锁、29/30 ms 边界、重复请求不重置、不反向和三类首次 I/O 失败。
`hw_test_debug` 与 `hw_test_release` 均交叉构建 `MB_DDF_v2` 和独立 `MB_DDF_v2_HelmControl`
成功，37 份产品 CSV 校验通过，`dut/tools/tests` 为 20/20 pytest 通过。由于当前主机无 AArch64/QEMU
执行环境且未获得目标板/真实舵机台架运行授权，新增 GoogleTest 只能证明已进入并通过交叉编译，
不记为运行通过，也不构成 DO0 电平、30 ms 真实时序、PWM 输出或机械回零证据。

2026-07-29 放宽 PC 周期零间隔并完成 Web 高频遥测收敛后，使用仓库内 37 份协议快照执行宿主 Debug 与 Release 全量构建/CTest；两个配置各有 356 条注册项，均为 355 项执行通过、1 项条件跳过、0 失败。跳过项仍是缺少外部同事导入附件的 `TestConfigManagerTest.ImportedColleagueSampleLoadsAndBuildsThroughMigrationBoundary`，不计为通过。C++ 定向调度/应用/Web 回归为 116/116，通过 WebSocket 标签集 72/72；新增证据覆盖 `intervalMs=0` 严格串行、暂停/恢复/停止检查与参数透传、STOP 请求前已排队尾样本投影、`single`/`batch-v1` 协商、20 ms/64 条/32 KiB 批门限、任务切换和关键事件前冲刷、全局安全序号、统一出站 FIFO、快照合并及背压硬上限。浏览器前端为 17 个测试文件、81/81 Vitest 通过，TypeScript/Vite 构建生成 531,657 字节单文件；覆盖连接 epoch、批消息兼容、序号缺口诊断、50,000 点有界缓存/淘汰统计、像素级 min/max 查询及遥测 store 与低频 Context 隔离。Release 只出现既有 Qt 5/MSVC `C4996/STL4043` 弃用警告。本轮未启动用户常驻的 `hwtest_web`/Vite 服务，未执行 1 kHz/2 kHz 持续 60 秒或 Chrome Performance 压力验收，因此不增加真实浏览器吞吐、长任务或 DUT/舵机性能证据等级。

2026-07-29 新增 BUS LOOP/ECHO 根宿主闭环并收敛 DUT 链路后，宿主使用仓库内 37 份协议快照完成 Debug 与 Release 全量构建/CTest；两个配置各有 374 条注册项，均为 373 项执行通过、1 项条件跳过、0 失败。跳过项仍是上述缺少外部导入附件的配置迁移用例，不计为通过。新增 18 个 GoogleTest 覆盖 HAL 多控制资源独立会话/确定性关闭/同物理串口冲突，BUS 0/1/3 参数白名单、COM3 拒绝、LOOP 运行期链路与次数判定，ECHO 固定 114 字节分段短读、原样回写、两个方向逐字节比较、5 秒超时、紧凑样本，以及十一项配置发现、运行模式和辅助资源不进入主控制口列表。浏览器前端仍为 17 个文件、81/81 Vitest 通过并完成单文件生产构建；DUT Windows PyQt 主机侧为 211/211、工具测试 20/20、37 份 CSV 校验通过，AArch64 Debug `hw_tests` 画像交叉构建 `MB_DDF_HW_Tests` 与 `MB_DDF_HW_Smoke` 等目标成功。所有验证均为 Fake、本机回环、主机侧或交叉构建；未打开真实 COM1/COM2/COM4、未部署或运行目标板，因此不构成 BUS LOOP/ECHO 真机验收。Release 仅出现既有 Qt 5/MSVC `C4996/STL4043` 弃用警告；PyQt 仅出现既有依赖弃用告警。

2026-07-30 将惯测设备流的宿主合成时间间隔改为测试配置后，新增测试先在旧实现上观察到配置 `4000 us` 仍产生 `2500 us` 时间增量、零值未拒绝，以及真实惯测配置缺少字段的预期失败；实现与配置补齐后均转为通过。宿主使用仓库内 37 份协议快照完成 Debug 与 Release 全量构建/CTest；两个配置各有 378 条注册项，均为 377 项执行通过、1 项条件跳过、0 失败。新增 4 个 GoogleTest 覆盖显式 `hostTimestampIntervalUs` 对 `timestampUs`/`streamElapsedUs` 的影响、缺省 2500 微秒兼容值、非法非正值在打开传输前拒绝、START/STOP 请求字节完全不变、惯测真实配置显式声明，以及舵机配置继续不声明该字段并保留 DDS 权威时间轴。跳过项仍是缺少外部导入附件的配置迁移用例，不计为通过。本轮未修改协议 CSV、DUT、HAL 或前端，也未运行目标板/真实惯测链路，自动化不构成产品 400 Hz 精度、丢帧或真机停止收尾证据。

45 个含 GoogleTest 定义的源文件使用 `*_test.cpp` 命名；另有 1 个 NI Fake 自定义 main 测试源。四个 HAL DLL fixture、Fake NIDAQmx 库/头、GUI/Web 自定义 GoogleTest 入口、应用测试支持库和测试 helper 不包含 GoogleTest 定义，不计入 367。

浏览器前端当前有 17 个 `*.test.ts`/`*.test.tsx` 文件、81 条 Vitest。除既有协议、参数、遥测缓冲/图组、DI 和性能结果行为外，新增覆盖旧 `sample`/新 `sampleBatch` 兼容、全局序号重复/倒序/缺口诊断、50,000 点淘汰统计、像素级 min/max 抽样、连接 epoch 隔离、零间隔参数，以及高频遥测 store 与低频会话 Context 的渲染边界。它们由 `npm test` 独立运行，不计入上述 378 条 CTest。

## 2. 当前覆盖与条件资产

| 目标 | 当前已覆盖的行为 | 证据边界 |
| --- | --- | --- |
| HAL | 错误映射、严格多设备资源映射、会话、Mock AD/DA/DI/DO、宿主串口枚举、串口 echo、CANFD loopback、控制资源路由、Qt UDP 回环和 timeout；动态 Fake ABI v1 的实际装载/数字批写/状态映射/缺符号、AdapterRouter 惰性多后端路由、逻辑 alias 映射、driver-only 初始化、版本化单设备 open projection、`HalService -> AdapterRouter -> CAbiAdapter -> NI parser/Fake DAQmx` 组合链、核心 ABI v1 的可选 task API 兼容、C ABI I/O/close 串行化、短读任务块、任务先 stop/close 再 safe state，以及 close 错误消费 handle | 自动化测试中的串口枚举不打开设备；动态 DLL 是 `tests/hal/fixtures` 的 Fake，Qt UDP 仅是本机 Provider 证据；COM3 实机结果属于下述独立手工 smoke，不是默认 CTest、NI SDK 或 PXI-6259 证据 |
| 原生 NI-DAQmx Adapter | 同一生产源码和独立 `ni_daqmx_config.*` 的 Fake 构建；PXI-6259 identity/open projection/占位 serial/topology/速率边界，按需 AI/AO/DI/DO，有限/连续任务路径，内部/外部时钟、start/reference/pause 触发、边沿计数/频率脉冲、短读、overflow/underflow、错误注入、任务状态、安全态/stop/clear 顺序；Vendor C ABI driver-only 初始化与单设备 open projection 交接；默认关闭的真实 SDK 构建路径 | `NiDaqmxAdapterFakeTest` 链接仓库 Fake `NIDAQmx`；独立可选 DLL 构建也使用 Fake 头/导入库。两者均不代表已安装 NI SDK、实际 PXI-6259、MAX 身份、接线、电平兼容、物理输出、触发时序或安全态验收 |
| 日志 | LogService、JsonLineFileSink、HalLogEvent 到 LogEvent 桥接 | 不覆盖 UI 或真实设备日志链 |
| BIZ | FakeAlgorithmExecutor 下的配置、计划、调度、重试、三种运行模式、不透明运行参数从 `RunOptions` 到 `TestContext` 的原样透传、专用 QThread 的 event dispatcher/计时器注册、同线程 `finishRun()`、可中断轮间等待、零间隔让出调度与暂停/恢复/停止检查、严格非重入、轮次/样本标记、状态、报告和架构扫描 | BIZ 不构造 HAL 假对象、Socket、codec 或硬件执行对象，也不解释运行参数；Qt 线程回归只证明同步任务具备 Qt dispatcher，不证明算法调用期间持续泵送事件、HAL actor 或跨线程取消；设备流测试只证明 BIZ 单次调用边界，不证明某产品支持主动回告 |
| 算法 | 帧编解码、CSV 无效输入、控制传输、运行参数 Schema、固定命令与配置驱动交换、惯测/舵机设备流和 DI stimulus；后处理公共 API 不泄露产品字段，捕获按小端版本化格式保存 DDS 时间、数值与诊断，封存后不可变；独立 fixture/manifest 覆盖恒值、正弦、方波、三角波、一阶/二阶/纯时延扫频、正反向与提前停止，另覆盖激励阈值、相位符号、尾段、状态归约、取消和有限 JSON | fixture 由不调用 C++ 分析器的 Python 标准库脚本生成并记录 SHA-256；合成曲线只能证明数值和边界实现。IMU/舵机设备流仍以脚本化传输为主，本机模拟不能证明 COM3/COM4、DDS、真实舵机、921600、400 Hz/1 kHz、NI/PXI-6259 或目标板行为 |
| 应用/TUI/GUI/WebSocket | 既有共享启动、控制资源、DI、三种运行模式、零间隔参数、TXT 保存及 GUI/Web 非阻塞收尾；后处理增加 start 前存储预检、TXT→捕获→公开样本顺序、STOP 封存/硬件收尾双栅栏、queued 写门禁、工作线程进度/取消/join、资源/结果/摘要限制和当前身份只读查询；Web v1 覆盖 capability、按连接协商 `single`/`batch-v1`、批量时间/条数/字节门限、全局安全序号、任务切换与关键事件前冲刷、统一 FIFO、软背压和硬上限失败 | 完整控制器用例加载真实舵机配置和协议 CSV，经本机 Qt UDP 产生 HELM 样本并完成捕获—分析—文件—查询，证明宿主软件组合链；Web 自动化使用本机客户端，尚未构成 1 kHz/2 kHz、60 秒真实浏览器压力证据。默认自动化不能外推到真实网口、真实舵机、NI/PXI-6259、其他 DUT 或长期稳定性 |
| 浏览器前端 | 类型化协议、参数表单、旧 `sample`/新 `sampleBatch`、按连接 epoch 丢弃迟到事件、全局序号完整性诊断、50,000 点有界 store、累计接收/淘汰统计、像素级 min/max 查询、遥测 store 与低频 Context 隔离、约 10 Hz UI 提交，以及既有图组、DI 和性能结果行为；TypeScript/Vite 单文件生产构建 | 性能页只消费后端 `snapshot.analysis`/`analysisResult`，不以实时环形缓存计算；Vitest 和构建不证明真实 WebSocket/DDS/舵机/PXI-6259、视觉效果、长期浏览器稳定性或真机性能，1 kHz/2 kHz 压力和 Chrome Performance 仍需用户授权启动服务后执行 |

下列测试依赖条件资产，缺失时可调用 GTEST_SKIP。跳过只表示该次没有执行断言，不能证明任何协议、配置迁移、SYSTEM_STATUS、HAL 或硬件能力。

- 直接运行 CTest 时，MB_DDF 协议/算法/跨层测试依赖 `MB_DDF_PROTOCOL_CSV_DIR` 指向受控 CSV 资产目录；本轮使用仓库内 `dut/docs/design/product_protocol_csv/`。`hwtest.ps1` 未显式指定参数或环境变量时会自动选择该快照。
- BIZ 的导入附件样例测试依赖 tmp/hwtest_BIZ/configs/sample_product.testcfg；tmp 不是仓库实现事实。
- 算法测试既包含自包含的帧、传输、参数 Schema、DI stimulus、能力判定和临时 CSV 用例，也包含加载受控 MB_DDF 目录的协议/执行器用例；缺失资产时只允许依赖项显式跳过。

协议 CSV 是运行期资产。仓库内 `dut/docs/design/product_protocol_csv/` 当前有 37 个定义，其中包括 5 份惯测流定义，并更新了舵机 START/反馈布局。协议测试按一文件一定义及当前实际字段校验；宿主脚本默认选择该快照，直接 CTest 仍需显式设置环境变量。可复现快照不等于 manifest/hash 自动机制已经实现。

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
- 修改设备流宿主合成时间轴配置时，必须覆盖显式间隔、缺省兼容值、非法非正值、样本 UTC/相对时间增量、START/STOP 请求字节不变，以及具有权威 DDS 时间的舵机配置不声明也不消费该字段；不得把宿主标注间隔写成 DUT 输出周期控制证据。
- 修改 Mock/Fake 行为时，必须说明证据级别；可配置超时/错误注入和 SYSTEM_STATUS/ELEC_HEALTH_STATUS 控制通道 Mock Provider 集成仍未实现，不得作为既有能力验收。动态 Fake C ABI fixture 只能验证 ABI/Adapter 路径，不能改写为厂家 SDK 或真机结果。
- 修改 Qt Provider、通用 C ABI、原生 Vendor Adapter 或真实硬件路径时，必须新增相应级别的隔离测试；当前 Qt UDP 有本机隔离测试，通用 C ABI 已有动态 Fake DLL、多 Adapter 懒加载、状态映射、数字批写和可选 task ABI 回归，PXI-6259 NI-DAQmx 已有可选 SDK 构建路径及 `NiDaqmxAdapterFakeTest`，BIZ 已有 Qt dispatcher/计时器注册回归，Qt 串口已有带历史告警的早期 smoke、无响应无告警诊断和迁移后成功无告警的手工复测；CTest `hardware` 标签、自动化异常路径和全面真实硬件验收仍未实现。
- 修改 `digitalStimulus` 配置、刺激 DTO/动作、revision 语义或安全收尾时，必须同步算法、应用、WebSocket 和前端纯逻辑回归；至少覆盖配置白名单、陈旧 revision 无写入、全量批写、写失败状态、动作参数拒绝和停止/收尾的 Fake/Mock 边界。成功 Web DI 写、revision 冲突和断开安全态的 Web 端到端覆盖仍待补齐。
- 修改共享应用控制器、runner 或 TUI 命令时，必须运行 `hwtest_app_tests`；修改 Qt GUI 时还必须运行 `hwtest_gui_tests`；修改 WebSocket 协议、服务器、入口或脚本时必须运行 `hwtest_web_tests`、`ctest -L websocket` 和 `HwtestWeb*` 进程测试。修改 `front/` 时必须运行 `npm test` 和 `npm run build`。Qt GUI、WebSocket 后端和浏览器 Web UI 必须复用控制器 DTO/事件，不得以新增前端为由复制组合根。
- 修复行为缺陷时，先补能复现问题的回归测试，再修改实现。

### 5.1 前端使用习惯兼容准入

面向操作人员的基础生命周期固定为 `load -> select -> prepare -> run -> terminal -> result -> disconnect`；TUI 中的 `use/port`、`wait/status` 分别承担 select 和 terminal 观察动作。用户操作说明见 [TUI 使用指南](../../user/tui-usage-guide.md)。

当前 `TestApplicationController` 通过统一注册表接受十一个已知算法 ID（`mbddf.system_status`、`mbddf.elec_health_status`、`mbddf.bus_loop`、`mbddf.bus_echo`、`mbddf.memperf`、`mbddf.spi_flash`、`mbddf.dh_pulse_config`、`mbddf.timer_jitter`、`mbddf.di_read`、`mbddf.imu_stream`、`mbddf.helm_stream`），并要求恰好一个启用步骤。BUS LOOP、SPI Flash 与 DH 只允许 `single`，BUS ECHO 只允许 `pc_periodic`，系统状态、电气健康、内存、定时器与 DI 按配置允许 `single`/`pc_periodic`，惯测与舵机连续实测只允许 `device_stream`。BUS 自动化必须覆盖 0/1/3 白名单、COM3 拒绝、内部 `loopback=true`、外部 114 字节短读/回写/双向一致性、5 秒超时和紧凑样本；不得把 Fake/主机侧结果记为 COM1/2/4 真机验收。以下规则是新增测试项目的合入门禁，不得据此宣称任意产品协议都已实现：

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

直接调用 CTest 时，先在同一 PowerShell 会话把协议资产指向仓库快照并检查目录；核对其他受控来源时可替换该值，并记录观测时间与实际文件清单：

    $env:MB_DDF_PROTOCOL_CSV_DIR = (Resolve-Path ".\dut\docs\design\product_protocol_csv").Path
    if (-not (Test-Path $env:MB_DDF_PROTOCOL_CSV_DIR)) { throw "MB_DDF CSV assets are required" }
    ctest --test-dir build_vs -C Debug -R "^(ImuStreamExecutorTest|HelmStreamExecutorTest|RunParameterSchemaTest|MbddfProtocolTest|SystemStatusExecutorTest|HalControlTransportTest|SystemStatusUdpIntegrationTest|TestApplicationControllerTest|TuiShellTest|GuiMainWindowTest|WebProtocolTest|WebSocket|FrontendEquivalenceTest)\." --output-on-failure

HAL 当前覆盖和缺口以第 2 节表格为准。`../history/` 和本文以外的历史计划不是现行测试规则或通过证据。
