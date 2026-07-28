# 连续测试与遥测 Web 前端设计

> **历史设计稿（2026-07-26）**：本文保留原始设计判断，不代表当前八项配置、WebSocket v1 或浏览器实现。现行事实见 `../../design/overview/five-layer-architecture.md`、`../../design/contracts/business-scheduling-layer.md`、`../../design/contracts/websocket-frontend-protocol.md` 和 `../../design/testing/testing-specification.md`。

## 1. 目标

在现有 `TestApplicationController -> BIZ -> 算法 -> HAL` 链路上增加 PC 主动定时连续测试，并建立从算法 `RawSample` 到浏览器的实时样本通道。浏览器前端采用深色遥测控制舱风格，为当前唯一的 `mbddf.system_status` 测试提供可运行的小范围演示，同时让未来测试项目复用同一运行控制条、曲线配置器和诊断视图。

## 2. 运行模式

运行模式是追加式兼容面：

| 模式 | 含义 | BIZ 行为 | 当前 SYSTEM_STATUS |
| --- | --- | --- | --- |
| `single` | 单次测试 | 执行测试计划一次 | 支持 |
| `pc_periodic` | PC 主动连续测试 | 每轮完成后等待 `intervalMs`，再重新发送命令并采集响应，直到停止或达到 `maxCycles` | 支持，作为本次演示 |
| `device_stream` | 设备主动持续回告 | BIZ 只调用一次算法步骤；算法发送一次启动命令并持续读取设备回告，停止时发送/执行算法定义的停止流程 | 类型和边界保留；SYSTEM_STATUS 无对应启动/停止流命令，明确返回 `CapabilityUnsupported` |

`pc_periodic` 的间隔采用“上一轮完成到下一轮开始”的延迟，不允许同一测试会话内请求重叠。`maxCycles == 0` 表示无限运行，正数表示完成指定轮数后进入 `Finished`。暂停作用于当前算法检查点和轮间等待；停止必须同时中断轮间等待和正在执行的传输。

设备持续回告不是 PC 周期循环的别名。未来 DH 等算法可在一次 `executeStep()` 内多次调用 `observer.onSample()`，现有 `IRunControl` 与 `requestStop()` 继续承担停止门控。

## 3. 分层与数据流

```text
浏览器 RunControlBar
  -> WebSocket start { mode, intervalMs, maxCycles }
  -> TestApplicationController::start(TestRunOptions)
  -> ITestRunService::startTestWithOptions(RunOptions)
  -> BIZ 周期调度（仅 pc_periodic）
  -> IAlgorithmExecutor::executeStep()
  -> IAlgorithmObserver::onSample(RawSample)
  -> ITestRunService::sampleProduced
  -> TestApplicationController::sampleReceived(ApplicationSample)
  -> WebSocket sample 事件
  -> 前端有界样本缓冲与 uPlot Canvas 曲线
```

- BIZ 只解释通用运行模式、轮次、间隔和停止语义，不解释协议字段。
- 算法层决定某产品是否支持 `device_stream`，并负责设备流命令和样本解码。
- 应用层只把 BIZ 类型投影成前端 DTO，并维持快照中的运行模式、当前轮次和样本计数。
- Web 层只校验 JSON 边界、排队调用控制器和广播完整快照/样本，不持有 HAL 或算法对象。
- 浏览器不得用自身定时器重复发送 `start`；浏览器定时器只用于批量绘图和重连退避。

## 4. 协议扩展

`start` 保持空参数兼容，新增可选字段：

```json
{
  "v": 1,
  "type": "request",
  "id": "run-1",
  "action": "start",
  "params": {
    "mode": "pc_periodic",
    "intervalMs": 500,
    "maxCycles": 0
  }
}
```

新增异步样本消息：

```json
{
  "v": 1,
  "type": "sample",
  "seq": 42,
  "sample": {
    "taskId": "...",
    "stepId": "SYSTEM_STATUS",
    "channelId": "SYSTEM_STATUS",
    "timestampUs": 1780000000000000,
    "cycleIndex": 7,
    "values": {"cpu_usage": 12.5, "cpu_temp": 44.2},
    "tags": {}
  }
}
```

快照追加 `runMode`、`intervalMs`、`maxCycles`、`cycleIndex` 和 `sampleCount`。所有字段均为尾部扩展；旧客户端忽略未知字段仍可继续使用单次模式。

## 5. 前端信息架构

前端使用一个持久化应用壳，运行控制条在所有视图顶部保持可见：

1. **任务总览**：连接、控制资源、当前阶段、轮次、进度、PASS/FAIL、SYSTEM_STATUS 最新数值。
2. **曲线工作台**：动态发现数值字段，支持字段勾选、全部同图、每项一图和自定义分组，横轴固定为采样时间。
3. **报文与诊断**：最近样本 JSON、请求/响应帧、错误与前端事件记录。

图表配置保存在浏览器 `localStorage`，按测试通道隔离。当前 SYSTEM_STATUS 默认选择 CPU/内存占用率和温度字段；若字段尚未出现，页面显示空状态而不是伪造数据。

## 6. 图表性能预算

- 使用 uPlot 的 Canvas 2D 渲染，不使用 SVG 节点表示每个点。
- WebSocket 到达路径只追加到有界环形缓冲；React 不为每个样本触发全树渲染。
- 样本以 `requestAnimationFrame` 和最多 10 Hz 的提交频率批量刷新。
- 每个通道最多保留 50,000 个原始点；诊断事件最多保留 500 条。
- 绘制前根据画布像素宽度执行 min/max 桶降采样，保留尖峰；单图目标不超过约 `2 * width` 个绘制点。
- 每个图表组件使用命令式 `setData()` 更新并独立销毁，切换页面后不保留不可见图表实例。
- 尊重 `prefers-reduced-motion`；状态呼吸动画不超过每秒一次，图表本身不使用装饰动画。

## 7. 错误与安全

- 运行参数在 Web 边界和 BIZ 再次校验；`pc_periodic` 的 `intervalMs` 范围为 `10..3600000`，`maxCycles` 范围为 `0..1000000000`。
- `device_stream` 对 SYSTEM_STATUS 返回能力不支持，不发送虚构的启动帧。
- 客户端掉线沿用现有 `stopAsync -> stopCompleted -> shutdown`，因此 PC 周期循环和未来设备流都会停止。
- 连续运行期间硬件/协议执行错误结束会话并进入 `Error`；普通判定失败可完成当前轮，是否跳过同轮后续步骤仍遵循 `stopOnFirstFailure`。
- 原有 `start {}`、TUI、GUI、batch runner 和单次测试语义保持不变。

## 8. 验证边界

- BIZ 单测覆盖单次、PC 无限/有限周期、可中断轮间等待、轮次标记、设备流只执行一次。
- 算法测试覆盖 SYSTEM_STATUS 拒绝 `device_stream` 且 PC 周期仍逐轮发送请求。
- WebSocket 集成测试通过 UDP 隔离目标验证多轮双向交互、sample 消息和停止后不再发包。
- 前端 Vitest 覆盖协议解析、字段扁平化、有界缓冲、时间窗和同图/分图/自定义分组。
- 前端构建、C++ Debug/Release 和现有架构扫描全部保持通过。
