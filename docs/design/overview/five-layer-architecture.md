# 多产品通用硬件测试软件五层架构

> 本文只定义分层边界和依赖方向。BIZ API 见 `../contracts/business-scheduling-layer.md`，HAL API 见 `../contracts/hal-interface-protocol.md`，协议 CSV 和帧规则见 `../contracts/device-communication-protocol.md`，日志模型与映射见 `../contracts/log-interface-protocol.md`，测试边界见 `../testing/testing-specification.md`。
>
> `[当前实现]` 是已由代码、CMake 或测试注册核对的事实；`[目标契约-未实现]` 是批准的目标，不能作为已落地能力或验收结论。

## 1. 事实来源

| 文档 | 主定义 |
| --- | --- |
| `overview/five-layer-architecture.md` | 分层边界、依赖方向和生产 I/O 归属 |
| `contracts/business-scheduling-layer.md` | BIZ 服务、模型、调度与算法端口 |
| `contracts/hal-interface-protocol.md` | HAL 对上接口、资源和 Adapter ABI |
| `contracts/device-communication-protocol.md` | 测试设备与 DUT 的协议 CSV、字段和物理帧 |
| `contracts/log-interface-protocol.md` | `LogEvent`、来源、追踪和 HAL/Adapter 日志映射 |
| `testing/testing-specification.md` | 测试分层、范围和运行方式 |

代码、公共头、CMake 目标和测试注册优先于设计文本。总览不重述各契约的字段、错误码或日志映射。

## 2. 边界模型

`[当前实现]` 已有 BIZ、HAL、日志、MB_DDF 算法、共享应用控制器、一次性 runner 和行式 TUI；仓库没有 Qt GUI、Web UI、TCP Provider、真实厂家链或真实硬件验收。

```text
hwtest_tui / hwtest_pc_runner
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

`[目标契约-未实现]` 完整依赖方向如下。Qt GUI、Web UI、通用 Router、Vendor Provider 和控制通道 Mock Provider 不代表当前已实现。

```text
TUI / Qt GUI / Web UI
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
| UI / 应用组合 | `[当前实现]` `hwtest_app_core` 统一组装 HAL、算法、BIZ 和日志；runner 一次运行，TUI 支持配置加载、控制口选择、准备、启停、等待和结果查看；`[目标契约-未实现]` Qt GUI 与 Web UI | 解释协议、持有 Socket/串口、绕过应用控制器或 BIZ 直接执行测试判定 |
| BIZ | 配置、计划、稳定拓扑排序、重试、运行状态、结果编排和报告 | 解释协议字段、执行单步判定、持有硬件或通讯对象、执行安全动作 |
| 算法 | `[当前实现]` MB_DDF CSV、编解码、流式分帧、命令/序号匹配和 `SYSTEM_STATUS` 判定 | BIZ 流程、UI、具体 Qt/厂家连接、物理 safe state |
| HAL | `[当前实现]` 提供资源、会话、安全 API 和控制通道原始 I/O；控制资源持有 Qt 串口/UDP 对象并执行操作 timeout | 业务调度、产品协议字段解释和测试判定 |
| Provider / Adapter | `[当前实现]` 控制资源按 `providerId` 选择 `qt.serial` 或 `qt.udp`；既有硬件资源仍走 Mock/C ABI 兼容链；`[目标契约-未实现]` 通用 Qt/Vendor/Mock Router | UI、业务流程和产品判定 |

BIZ 只能直接依赖 Qt Core、`hwtest_log_types`、自身公共模型和 `biz::IAlgorithmExecutor`。它不得 include、link、call 或持有 HAL、Adapter、Socket、codec、测量工厂或安全输出执行对象。

## 4. 生产 I/O 与配置边界

- 面向测试设备或 DUT 的全部生产态硬件和通讯 I/O 必须统一经 HAL；当前控制通道已遵守该边界，算法不持有 `QSerialPort` 或 `QUdpSocket`。
- 三种 UI 只允许调用 `hwtest_app_core` 的动作、快照和事件；不得分别复制 HAL 会话、算法执行器、BIZ 服务或日志收尾。控制器公共动作和 `snapshot()` 具有 QObject 线程亲和约束，Web 请求线程或 GUI worker 必须排队投递。`waitForTerminal()` 是 batch/TUI 的阻塞辅助方法，具有重入/运行代次保护；未来 Qt GUI/Web UI 应订阅 `snapshotChanged`，不得阻塞其事件线程。
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
- `src/app/` 的 `hwtest_app_core` 提供 `TestApplicationController`；`hwtest_pc_runner` 复用它执行一次测试，`hwtest_tui` 复用它完成分步操作。TUI 的 `use <ResourceId>` 只修改内存中的 `control.resourceId`，`ports` 经应用 DTO 列出宿主串口，`port <port-name>` 只覆盖本次会话中的串口资源属性；PC 可在串口和 UDP 间选择，DUT 端无需切换模式。
- 根 CMake 构建 HAL、日志、BIZ、算法、共享应用核心、runner 和 TUI，并查找同一 Qt 主版本的 Core、Network、SerialPort。
- 根 `hwtest.ps1` 是 Windows 单命令入口，负责配置、构建、测试、启动 TUI/runner 和列出串口；它不复制测试流程或绕过 `hwtest_app_core`。
- 当前测试目标、源码清单和统计口径以 `../testing/testing-specification.md` 为主定义，不以源级数量代替通过结果。
- `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容是已批准的 MB_DDF 协议 CSV 基线。它仍是仓库外依赖，当前清单与可复现性限制见 `../contracts/device-communication-protocol.md`。

## 7. 边界验收

- BIZ 保持硬件无关，只有 `IAlgorithmExecutor` 是其执行出口。
- 新生产硬件/通讯 I/O 必须进入 HAL；当前只声明控制通道支持 Qt 串口和 UDP，不声明 TCP、真实串口联调、通用 Router 或真实厂家链。
- 协议/golden 单测与产品模拟/集成测试按第 5 节隔离；后者以“实际经过 HAL”为证据边界，并继续区分 Mock、Qt Provider 和真实硬件等级。
- 新配置只写 `executionConfig`，旧 `halConfig` 仅作为迁移读入键。
- 日志映射不在本文复制，统一引用 `../contracts/log-interface-protocol.md`。

## 8. 当前 TUI 操作

从仓库根目录用一条命令配置、构建并启动：

```powershell
.\hwtest.ps1 tui
```

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

也可在启动时覆盖本次会话的端口：`.\hwtest.ps1 tui -Port COM7`；一次性运行使用 `.\hwtest.ps1 run -Port COM7`。端口覆盖不回写 HAL JSON；长期部署值仍由 `hardware.resources.<ResourceId>.properties.portName` 配置。`CONTROL_NETWORK`/`qt.udp` 保留用于现有本机模拟和后续板端网口扩展，但不能描述为已可与当前仅支持串口的 MB_DDF_v2 板端交互。

`ports` 只枚举宿主串口，不打开设备；`load` 只读取并校验配置，不打开硬件；`use` 只在断开状态切换 PC 端逻辑控制资源；`port` 只在已加载且未连接时修改内存配置；`prepare` 才创建 HAL 会话、算法执行器和 BIZ 服务，并在 HAL 中尝试打开所选端口。`run` 当前只运行唯一启用的 `mbddf.system_status`。`pause`、`resume`、`stop [timeout-ms]` 也已接入 BIZ，成功停止后阶段为 `stopped`，可再次 `wait` 或 `run`；当前单项事务较短，暂停可能在到达检查点前已经结束。标准输入由独立线程读取，Qt 主事件循环持续处理 BIZ/HAL 的排队事件；交互模式在 `wait` 期间仍可接收 `status`/`stop`，管道模式则保持逐命令确认顺序。stdin/stdout 固定为 UTF-8，同一命令流可通过管道脚本执行。

`quit` 和 EOF 都会执行有序收尾；收尾失败会锁存在 `shutdown_failed` 快照中，TUI 以非零码退出。但现有 BIZ/算法停止仍是协作式的，`shutdown()` 最终会等待 worker；它不是进程级硬截止，强制杀进程也不能作为物理 safe state 已完成的证据。真实硬件使用前必须另行补进程监护和隔离安全验收。
