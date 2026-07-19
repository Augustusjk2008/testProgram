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

`[当前实现]` 已有 BIZ、HAL、日志、MB_DDF 算法和命令行组合入口；仓库没有图形 UI、TCP Provider、真实厂家链或真实硬件验收。

```text
hwtest_pc_runner
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

`[目标契约-未实现]` 完整依赖方向如下。图形 UI、通用 Router、Vendor Provider 和控制通道 Mock Provider 不代表当前已实现。

```text
UI
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
| UI / 组合入口 | `[当前实现]` CLI 读取配置并组装当前单项测试；`[目标契约-未实现]` 图形 UI 的产品选择、测试启停、进度和结果展示 | 解释协议、绕过 BIZ 直接执行测试判定 |
| BIZ | 配置、计划、稳定拓扑排序、重试、运行状态、结果编排和报告 | 解释协议字段、执行单步判定、持有硬件或通讯对象、执行安全动作 |
| 算法 | `[当前实现]` MB_DDF CSV、编解码、流式分帧、命令/序号匹配和 `SYSTEM_STATUS` 判定 | BIZ 流程、UI、具体 Qt/厂家连接、物理 safe state |
| HAL | `[当前实现]` 提供资源、会话、安全 API 和控制通道原始 I/O；控制资源持有 Qt 串口/UDP 对象并执行操作 timeout | 业务调度、产品协议字段解释和测试判定 |
| Provider / Adapter | `[当前实现]` 控制资源按 `providerId` 选择 `qt.serial` 或 `qt.udp`；既有硬件资源仍走 Mock/C ABI 兼容链；`[目标契约-未实现]` 通用 Qt/Vendor/Mock Router | UI、业务流程和产品判定 |

BIZ 只能直接依赖 Qt Core、`hwtest_log_types`、自身公共模型和 `biz::IAlgorithmExecutor`。它不得 include、link、call 或持有 HAL、Adapter、Socket、codec、测量工厂或安全输出执行对象。

## 4. 生产 I/O 与配置边界

- 面向测试设备或 DUT 的全部生产态硬件和通讯 I/O 必须统一经 HAL；当前控制通道已遵守该边界，算法不持有 `QSerialPort` 或 `QUdpSocket`。
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
- `src/app/` 的 `hwtest_pc_runner` 读取 `configs/mbddf_system_status.testcfg.json` 和 HAL 部署配置；修改 `control.resourceId` 即在 PC 端选择串口或 UDP，DUT 端无需切换模式。
- 根 CMake 构建 HAL、日志、BIZ、算法和 CLI，并查找同一 Qt 主版本的 Core、Network、SerialPort。
- 当前测试目标、源码清单和统计口径以 `../testing/testing-specification.md` 为主定义，不以源级数量代替通过结果。
- `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容是已批准的 MB_DDF 协议 CSV 基线。它仍是仓库外依赖，当前清单与可复现性限制见 `../contracts/device-communication-protocol.md`。

## 7. 边界验收

- BIZ 保持硬件无关，只有 `IAlgorithmExecutor` 是其执行出口。
- 新生产硬件/通讯 I/O 必须进入 HAL；当前只声明控制通道支持 Qt 串口和 UDP，不声明 TCP、真实串口联调、通用 Router 或真实厂家链。
- 协议/golden 单测与产品模拟/集成测试按第 5 节隔离；后者以“实际经过 HAL”为证据边界，并继续区分 Mock、Qt Provider 和真实硬件等级。
- 新配置只写 `executionConfig`，旧 `halConfig` 仅作为迁移读入键。
- 日志映射不在本文复制，统一引用 `../contracts/log-interface-protocol.md`。
