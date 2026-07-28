# 多产品通用硬件测试软件五层架构

> 本文只定义分层、依赖方向、生产 I/O 归属和组合边界。公共字段、协议语义、实现证据与运行命令均由下列事实源定义，不在总览重复。

## 1. 事实源

| 范围 | 主定义 |
| --- | --- |
| BIZ 服务、模型与调度 | [业务调度层接口契约](../contracts/business-scheduling-layer.md) |
| HAL、Provider、Adapter 与安全态 | [HAL 层接口协议](../contracts/hal-interface-protocol.md) |
| 产品协议、CSV、帧与算法语义 | [设备通讯协议契约](../contracts/device-communication-protocol.md) |
| WebSocket v1 | [WebSocket 前端协议契约](../contracts/websocket-frontend-protocol.md) |
| 日志模型与追踪 | [日志模块接口协议](../contracts/log-interface-protocol.md) |
| 测试清单、证据与运行方式 | [测试规范](../testing/testing-specification.md) |
| 操作说明 | [TUI 使用指南](../../user/tui-usage-guide.md) 与 [浏览器前端说明](../../../front/README.md) |

公共 API、CMake 目标、测试注册和已核对源码共同描述当前实现。历史快照位于 `../history/`，不作为现行事实源。

## 2. 分层与依赖

```text
浏览器 Web UI -> 回环 WebSocket 后端；TUI / Qt GUI / batch CLI
  -> hwtest_app_core::TestApplicationController
  -> hwtest_biz
  -> biz::IAlgorithmExecutor（算法层实现）
  -> hwtest_hal
  -> Provider / Adapter

日志模块 <- 各层结构化事件（旁路，不反向控制业务）
```

依赖只能沿箭头向下：

- 应用层可以组合 BIZ、算法、HAL 和日志，但前端入口不得绕过 `TestApplicationController`。
- BIZ 只依赖 Qt Core、`hwtest_log_types` 和自身公共模型；硬件执行出口只有 `biz::IAlgorithmExecutor`。
- 算法层解释产品协议、声明并校验可编辑运行参数、组织交互流程并通过 HAL 请求 I/O。
- HAL 不解释产品字段或测试判据，只处理连接、原始 I/O、deadline、错误归一化和物理安全态。
- 日志模块接收事件，不持有 UI、BIZ、算法、HAL 或 Adapter 的控制权。

## 3. 职责与禁止项

| 层 | 负责 | 不负责 |
| --- | --- | --- |
| UI / 应用 | 配置选择、算法参数 Schema 投影和本次覆盖合并、组合生命周期、DTO/动作/快照/样本适配、有序关闭 | 自行定义产品参数语义或范围、产品协议解释、直接打开 DUT 或测试设备、绕过 BIZ 执行判定 |
| BIZ | 配置、计划、拓扑顺序、运行模式、重试、状态、结果和报告编排 | Socket/串口/设备句柄、产品帧编解码、物理安全动作 |
| 算法 | CSV 协议、编解码、序号、可编辑运行参数 Schema/语义校验、交互流程、样本和判定 | Provider/Adapter 选择、厂家 SDK、资源生命周期所有权 |
| HAL | 逻辑资源映射、会话、Provider/Adapter 路由、原始 I/O、deadline、错误和关闭安全态 | 产品命令、产品阈值和业务报告 |
| Provider / Adapter | Qt 串口/UDP或厂家驱动调用、物理通道和设备级 I/O | BIZ 调度与产品判定 |
| 日志 | 结构化事件、缓存、sink 和 HAL 映射 | 驱动业务状态或设备操作 |

## 4. I/O 与文件边界

- 测试设备或 DUT 的生产硬件/通讯 I/O 必须经过 HAL。
- `module = "control"` 的资源按 `providerId` 进入标准控制 Provider；其他设备按 `adapterId` 进入 Adapter 路径。
- 纯协议/golden 测试可以直接使用 Simulator；产品模拟和集成验证必须沿 HAL Mock 或标准 Provider 路径。
- 配置、日志、报告和连续数据文件由各自服务边界访问，不属于生产硬件/通讯 I/O。
- 浏览器只通过回环 WebSocket 使用应用 DTO；WebSocket 后端不持有产品协议或底层设备对象。

## 5. 生命周期与安全边界

- `hwtest_app_core` 是组合根，负责 HAL 会话、算法执行器、BIZ 服务、DI 控制器和关闭顺序。
- BIZ 停止是协作式流程；应用入口通过同步或异步控制器动作观察完成状态。
- `SafetyPolicy.enterSafeStateOnStop` 和 `enterSafeStateOnError` 当前仅为兼容字段，不驱动运行期分支；实际停止复位和 HAL 关闭安全态以对应契约的 `[当前实现]` 描述为准。
- HAL 会话关闭前尽力停止/释放已跟踪任务并应用已配置 safe state。该软件行为不等于真实设备、接线、供电或台架的物理安全验收。

## 6. 构建边界

- 根工程使用 C++17，并保持 Qt 5.15 与 Qt 6 fallback 的同组件集兼容。
- `hwtest_biz` 不链接 HAL；算法目标链接 BIZ 与 HAL；`hwtest_app_core` 作为组合根链接各实现层。
- 公共 HAL/BIZ 头、Adapter ABI、配置字段和错误语义属于兼容面；变更时同步契约与回归测试。
- 当前目标、入口和测试数量只在 CMake与[测试规范](../testing/testing-specification.md)中维护，本总览不复制统计或执行记录。
