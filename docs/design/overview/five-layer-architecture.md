# 多产品通用硬件测试软件五层架构

> 本文只定义分层边界和依赖方向。BIZ API 见 `../contracts/business-scheduling-layer.md`，HAL API 见 `../contracts/hal-interface-protocol.md`，协议 CSV 和帧规则见 `../contracts/device-communication-protocol.md`，WebSocket 前端消息见 `../contracts/websocket-frontend-protocol.md`，日志模型与映射见 `../contracts/log-interface-protocol.md`，测试边界见 `../testing/testing-specification.md`。
>
> `[当前实现]` 是已由代码、CMake 或测试注册核对的事实；`[目标契约-未实现]` 是批准的目标，不能作为已落地能力或验收结论。

## 1. 事实来源

| 文档 | 主定义 |
| --- | --- |
| `overview/five-layer-architecture.md` | 分层边界、依赖方向和生产 I/O 归属 |
| `contracts/business-scheduling-layer.md` | BIZ 服务、模型、调度与算法端口 |
| `contracts/hal-interface-protocol.md` | HAL 对上接口、资源和 Adapter ABI |
| `contracts/device-communication-protocol.md` | 测试设备与 DUT 的协议 CSV、字段和物理帧 |
| `contracts/websocket-frontend-protocol.md` | `hwtest_web` 的 JSON、动作、错误、顺序和关闭语义 |
| `contracts/log-interface-protocol.md` | `LogEvent`、来源、追踪和 HAL/Adapter 日志映射 |
| `testing/testing-specification.md` | 测试分层、范围和运行方式 |

代码、公共头、CMake 目标和测试注册优先于设计文本。总览不重述各契约的字段、错误码或日志映射。

## 2. 边界模型

`[当前实现]` 已有 BIZ、HAL、日志、MB_DDF 算法、共享应用控制器、一次性 runner、行式 TUI、Qt Widgets GUI、回环 WebSocket 后端和独立 React/Vite 浏览器遥测控制台。BIZ 已支持单次、PC 周期和设备持续回告三种调度语义；当前 `SYSTEM_STATUS` 支持前两者，并明确拒绝缺少产品协议命令的设备持续模式。Qt 串口已有一次受控 COM3 真实板端 smoke；TCP Provider、真实厂家链和全面真实硬件验收仍未实现。

```text
front/ 浏览器控制台 -> ws://127.0.0.1:<port>/ws -> hwtest_web
hwtest_tui / hwtest_gui / hwtest_pc_runner -----------------+
                                                           |
  -> hwtest_app_core::TestApplicationController
  -> hwtest_biz
  -> biz::IAlgorithmExecutor
  -> hwtest_algorithm_mbddf
       -> HalControlTransport
       -> hwtest_hal::IControlChannel
       -> ControlChannelManager
            -> qt.serial -> QSerialPort
            -> qt.udp -> QUdpSocket
```

控制资源按 HAL 部署配置中的 `control.resourceId` 和资源 `providerId` 选择串口或 UDP；Qt 标准接口不经过 Vendor Adapter。AD/DA、DI/DO、旧 `ISerialBus` 和 CANFD 等既有资源仍走 `CAbiAdapter -> MockAdapter`。纯协议 golden 测试仍可注入 `SystemStatusSimulator`。

`[目标契约-未实现]` 完整依赖方向如下。通用 Router、Vendor Provider 和控制通道 Mock Provider 不代表当前已实现。

```text
TUI / Qt GUI / hwtest_web / 浏览器 Web UI
  -> hwtest_app_core
  -> hwtest_biz
  -> biz::IAlgorithmExecutor
  -> 算法层
  -> hwtest_hal
  -> providerId 路由
       -> Qt 标准 API Provider
       -> Vendor Adapter
       -> Mock Provider
```

日志是旁路基础模块，不属于上述任一层；其来源、`requestId` 和 HAL/Adapter 映射只以 `../contracts/log-interface-protocol.md` 为主定义。

## 3. 职责边界

| 层或边界 | 负责 | 不负责 |
| --- | --- | --- |
| UI / 应用组合 | `[当前实现]` `hwtest_app_core` 统一组装 HAL、算法、BIZ 和日志；TUI、Qt GUI 与 `hwtest_web` 支持配置加载、控制口/串口选择、准备、启停和结果查看；浏览器控制台通过 WebSocket 消费快照/样本并提供全局连续运行条、总览、可配置曲线和诊断 | 解释产品协议、持有 DUT/生产 I/O Socket 或串口、绕过应用控制器或 BIZ 直接执行测试判定；`hwtest_web` 的 `QWebSocketServer/QWebSocket` 仅作前端传输 |
| BIZ | 配置、计划、稳定拓扑排序、重试、通用运行模式、运行状态、样本/结果编排和报告 | 解释协议字段、执行单步判定、持有硬件或通讯对象、执行安全动作 |
| 算法 | `[当前实现]` MB_DDF CSV、编解码、流式分帧、命令/序号匹配和 `SYSTEM_STATUS` 判定 | BIZ 流程、UI、具体 Qt/厂家连接、物理 safe state |
| HAL | `[当前实现]` 提供资源、会话、安全 API 和控制通道原始 I/O；控制资源持有 Qt 串口/UDP 对象并执行操作 timeout | 业务调度、产品协议字段解释和测试判定 |
| Provider / Adapter | `[当前实现]` 控制资源按 `providerId` 选择 `qt.serial` 或 `qt.udp`；既有硬件资源仍走 Mock/C ABI 兼容链；`[目标契约-未实现]` 通用 Qt/Vendor/Mock Router | UI、业务流程和产品判定 |

BIZ 只能直接依赖 Qt Core、`hwtest_log_types`、自身公共模型和 `biz::IAlgorithmExecutor`。它不得 include、link、call 或持有 HAL、Adapter、Socket、codec、测量工厂或安全输出执行对象。

## 4. 生产 I/O 与配置边界

- 面向测试设备或 DUT 的全部生产态硬件和通讯 I/O 必须统一经 HAL；当前控制通道已遵守该边界，算法不持有 `QSerialPort` 或 `QUdpSocket`。
- 各前端只允许调用 `hwtest_app_core` 的动作、快照和事件；不得分别复制 HAL 会话、算法执行器、BIZ 服务或日志收尾。控制器公共动作和 `snapshot()` 具有 QObject 线程亲和约束，Web 请求线程或 GUI worker 必须排队投递。`waitForTerminal()` 是 batch/TUI 的阻塞辅助方法，具有重入/运行代次保护；当前 Qt GUI 与 `hwtest_web` 都订阅异步事件，并通过 `stopAsync()`/`stopCompleted` 执行不阻塞亲和线程的停止，不调用该等待方法。浏览器只通过 WebSocket 契约消费 DTO；PC 周期循环在 BIZ，不由浏览器定时重复发送 `start`。
- 控制资源当前使用显式 `providerId` 路由 `qt.serial`/`qt.udp`；没有 `INetworkBus`，TCP、通用 Provider Router、Vendor Provider 和控制通道 Mock Provider 仍未实现。
- 配置、日志和报告文件 I/O 不属于上述生产硬件/通讯 I/O 规则。
- 新写出的 BIZ 配置只使用不透明的 `executionConfig`；旧根字段 `halConfig` 只允许在读取迁移阶段出现。BIZ 的字段和迁移语义见 `../contracts/business-scheduling-layer.md`。

边界流只表达责任归属：

```text
BIZ 编排 TestPlan / TestContext / executionConfig
  -> 算法解释产品协议与执行判定
  -> HAL 管理生命周期并执行生产 I/O
  -> 算法返回单步结果
  -> BIZ 编排结果、状态和报告
```

## 5. 测试边界

- 纯协议或 golden 单元测试可直接使用 Simulator；产品模拟和集成测试必须经过 HAL，可使用 HAL Mock 或标准 Provider 连接隔离模拟目标。
- `[当前实现]` `SystemStatusSimulator` 直接注入 `SystemStatusAlgorithmExecutor` 的闭环测试仍存在。这是过渡测试路径，不是“产品模拟已通过 HAL Mock”的证据。
- `[当前实现]` Qt UDP 本机模拟目标测试已经过 BIZ、算法、HAL 和 `qt.udp` Provider；它证明标准 Provider 闭环，不证明真实目标板或真实网口。
- 任何生产 I/O 或产品集成结论都不能由直连 Simulator 单独证明。

## 6. 当前落地范围

- `src/algorithm/` 已有 `hwtest_algorithm_mbddf`、协议目录加载、payload/物理帧编解码、流式 `HalControlTransport`、`SystemStatusAlgorithmExecutor` 和 `SYSTEM_STATUS` 测试配置。
- `src/app/` 的 `hwtest_app_core` 提供 `TestApplicationController` 和共享启动配置；`hwtest_tui_support`、`hwtest_gui_support`、`hwtest_web_support` 只承载各自前端适配，`hwtest_pc_runner`、`hwtest_tui`、`hwtest_gui`、`hwtest_web` 是四个独立进程入口。TUI、GUI 和 WebSocket 请求中的会话覆盖都只修改内存中的控制资源或串口属性；PC 可在串口和 UDP 间选择，DUT 端无需切换模式。
- `front/` 是独立的 React 19、TypeScript、Vite、Tailwind CSS 和 uPlot 工程，不进入宿主 CMake，也不包含后端、数据库或 DUT I/O。它的三页共享同一个运行控制条；曲线使用有界缓冲、批量 Canvas 更新和 min/max 降采样。
- 根 CMake 构建 HAL、日志、BIZ、算法、共享应用核心、TUI/GUI/Web 支持库和四个应用入口，并查找同一 Qt 主版本的 Core、Network、SerialPort、Widgets、WebSockets。
- 根 `hwtest.ps1` 是 Windows 单命令入口，负责配置、构建、测试、通过 `-Ui tui|gui|web` 启动前端、启动 runner 和列出串口；`-WebPort` 只传给 WebSocket 后端。它不复制测试流程或绕过 `hwtest_app_core`。
- 当前测试目标、源码清单和统计口径以 `../testing/testing-specification.md` 为主定义，不以源级数量代替通过结果。
- `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容是已批准的 MB_DDF 协议 CSV 基线。`dut/` 保存同步副本，但宿主运行期仍显式依赖外部目录；当前清单与可复现性限制见 `../contracts/device-communication-protocol.md`。

## 7. 边界验收

- BIZ 保持硬件无关，只有 `IAlgorithmExecutor` 是其执行出口。
- 新生产硬件/通讯 I/O 必须进入 HAL；当前控制通道支持 Qt 串口和 UDP，Qt 串口只声明已记录的 COM3 SYSTEM_STATUS smoke，不据此声明长时/异常收尾、TCP、通用 Router、真实厂家链或全面硬件验收。
- 协议/golden 单测与产品模拟/集成测试按第 5 节隔离；后者以“实际经过 HAL”为证据边界，并继续区分 Mock、Qt Provider 和真实硬件等级。
- 新配置只写 `executionConfig`，旧 `halConfig` 仅作为迁移读入键。
- 日志映射不在本文复制，统一引用 `../contracts/log-interface-protocol.md`。

## 8. 当前 TUI、GUI 与浏览器操作

从仓库根目录用一条命令配置、构建并启动：

```powershell
.\hwtest.ps1 -Ui tui
.\hwtest.ps1 -Ui gui
.\hwtest.ps1 -Ui web -WebPort 18765
Set-Location front
npm install
npm run dev
```

`start -Ui tui|gui|web` 是等价的显式形式，三个短动作保留为兼容别名。脚本无参数时只显示帮助。浏览器开发服务器默认打开 `http://127.0.0.1:5173` 并连接 `ws://127.0.0.1:18765/ws`；它不替代 `hwtest_web`。GUI 和 Web 的停止、关闭与“断开”使用控制器异步停止完成信号和同一有序收尾路径；同步 `stop()` 继续服务 TUI/batch。

脚本默认从 `MB_DDF_PROTOCOL_CSV_DIR` 读取协议目录，未设置时使用当前已批准的 MB_DDF_v2 路径；也可通过 `-ProtocolCsvDir` 显式覆盖。当前板端基线只支持串口，建议先列出宿主端口，再分步选择：

```text
ports
load
controls
use CONTROL_SERIAL
port COM7
prepare
run
status                   # 可重复查看
wait 5000
result
disconnect
quit
```

也可在启动时覆盖本次会话的端口：`.\hwtest.ps1 -Ui tui -Port COM7` 或 `.\hwtest.ps1 -Ui gui -Port COM7`；一次性运行使用 `.\hwtest.ps1 run -Port COM7`。端口覆盖不回写 HAL JSON；长期部署值仍由 `hardware.resources.<ResourceId>.properties.portName` 配置。`CONTROL_NETWORK`/`qt.udp` 保留用于现有本机模拟和后续板端网口扩展，但不能描述为已可与当前仅支持串口的 MB_DDF_v2 板端交互。

`ports` 只枚举宿主串口，不打开设备；`load` 只读取并校验配置，不打开硬件；`use` 只在断开状态切换 PC 端逻辑控制资源；`port` 只在已加载且未连接时修改内存配置；`prepare` 才创建 HAL 会话、算法执行器和 BIZ 服务，并在 HAL 中尝试打开所选端口。`run` 当前只运行唯一启用的 `mbddf.system_status`。`pause`、`resume`、`stop [timeout-ms]` 也已接入 BIZ，成功停止后阶段为 `stopped`，可再次 `wait` 或 `run`；当前单项事务较短，暂停可能在到达检查点前已经结束。标准输入由独立线程读取，Qt 主事件循环持续处理 BIZ/HAL 的排队事件；交互模式在 `wait` 期间仍可接收 `status`/`stop`，管道模式则保持逐命令确认顺序。stdin/stdout 固定为 UTF-8，同一命令流可通过管道脚本执行。

`quit` 和 EOF 都会执行有序收尾；收尾失败会锁存在 `shutdown_failed` 快照中，TUI 以非零码退出。但现有 BIZ/算法停止仍是协作式的，`shutdown()` 最终会等待 worker；它不是进程级硬截止，强制杀进程也不能作为物理 safe state 已完成的证据。真实硬件使用前必须另行补进程监护和隔离安全验收。
