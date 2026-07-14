# 多产品通用硬件测试软件五层架构

> 技术栈：Qt 5.15 / C++17 / Qt Widgets / Windows 7+
> 当前范围：AD、DA、DI、DO、串口、CANFD。
> 本文定位：总览、分层边界、跨层依赖。业务调度细节见 `../contracts/business-scheduling-layer.md`，HAL 细节见 `../contracts/hal-interface-protocol.md`，设备通讯协议见 `../contracts/device-communication-protocol.md`，日志细节见 `../contracts/log-interface-protocol.md`，测试规范见 `../testing/testing-specification.md`。

---

## 1. 事实来源

| 文档 | 主定义 |
| --- | --- |
| `overview/five-layer-architecture.md` | 五层关系、模块边界、跨层流程 |
| `contracts/business-scheduling-layer.md` | 业务调度层契约 |
| `contracts/hal-interface-protocol.md` | HAL 对上接口、Adapter ABI、资源和错误映射 |
| `contracts/device-communication-protocol.md` | 测试设备与被测件底层通讯协议、CSV 建模和字段布局 |
| `contracts/log-interface-protocol.md` | `LogEvent`、`LogService`、`logProduced`、source 约定 |
| `testing/testing-specification.md` | 测试目录、分层边界、用例范围、运行方式 |

冲突处理：

- HAL 术语以 `../contracts/hal-interface-protocol.md` 和 `src/hal/include/hal/*.h` 为准。
- 测试设备与被测件的底层通讯帧以 `../contracts/device-communication-protocol.md` 为准。
- 日志术语以 `../contracts/log-interface-protocol.md` 为准。
- 业务入口以 `biz::ITestRunService` 为准；`SchedulerAPI` 只作迁移期兼容名。
- BIZ 到算法层的唯一执行端口是 `biz::IAlgorithmExecutor`；算法层内部实现名不构成跨层契约。

---

## 2. 总体架构

```text
UI
  -> hwtest_biz
  -> biz::IAlgorithmExecutor  (implemented by the algorithm layer)
  -> hwtest_hal
  -> Adapter
  -> vendor DLL / SDK / driver

UI / BIZ / algorithm
  -> hwtest::logging::LogEvent  (provided by hwtest_log_types)
  -> logging service or sink
```

边界：

- `hwtest_biz` 是配置和调度库，不是硬件执行层。
- BIZ 只直接依赖 Qt Core 和无 HAL 依赖的 `hwtest_log_types`；不得 include、link、call 或 own `hwtest_hal`、`IHalService`、`IHalDevice`。
- `biz::IAlgorithmExecutor` 由算法层实现，是 BIZ 唯一的执行出口；算法实现拥有 HAL 生命周期、单步执行和硬件安全收尾。
- Adapter 是本软件维护的最下层；厂家 DLL、SDK、驱动是外部依赖。
- 日志模块为旁路基础模块，不属于五层任一层。

---

## 3. 分层职责

| 层级 | 职责 | 不做 |
| --- | --- | --- |
| UI 交互层 | 登录、产品选择、配置编辑、测试启停、进度和结果展示 | 不直接访问算法实现、HAL、Adapter |
| 业务调度层 | 配置、计划、依赖排序、重试、运行状态、结果编排、报告 | 不执行单步算法；不直接使用 HAL、Socket、codec、测量工厂或安全输出 |
| 核心测试算法层 | 实现 `IAlgorithmExecutor`、单步算法、测量工厂、协议 codec/Socket、HAL 生命周期和安全输出执行 | 不操作 UI，不实现 BIZ 流程或报告编排 |
| HAL 层 | 设备发现、资源映射、参数归一化、安全校验、统一错误、调用 Adapter | 不做业务流程，不做测试判定 |
| Adapter 层 | 加载/链接厂家库，封装设备调用，转换数据和错误 | 不接触 UI、业务流程、测试判定 |

依赖规则：

| 层级 | 可以依赖 | 禁止依赖 |
| --- | --- | --- |
| UI | `hwtest_biz` 对上服务、日志服务 | 算法实现、HAL、Adapter、厂家 SDK |
| BIZ | Qt Core、`hwtest_log_types`、业务数据模型、`IAlgorithmExecutor` | HAL、Adapter、厂家 SDK、Socket、codec、测量与安全执行接口 |
| 算法 | BIZ 数据模型和执行端口、HAL 对上接口、日志事件模型 | UI、BIZ 流程实现、厂家 SDK |
| HAL | Adapter ABI、资源配置 | UI、业务调度、测试判定、日志服务 |
| Adapter | 厂家 DLL/lib/SDK/Win32 API | UI、业务、算法判定 |

---

## 4. 配置驱动

`.testcfg` 是产品测试方案输入，描述：

- 产品信息。
- 硬件设备匹配规则。
- 逻辑资源映射。
- 测试项启用状态和顺序。
- 测试参数、阈值、超时、重试。
- 算法执行配置 `executionConfig`。
- 协议 CSV 资产引用；由算法层解释和执行。
- 业务报告字段。

典型流向：

```text
.testcfg
  -> hwtest_biz 校验并标准化配置
  -> hwtest_biz 生成依赖有序 TestPlan
  -> hwtest_biz 创建 TestContext
  -> IAlgorithmExecutor.prepare(plan, context, executionConfig)
  -> hwtest_biz 按计划调用 executeStep 并编排重试/结果
  -> 算法层调用 HAL 逻辑资源
  -> HAL 映射真实设备和通道
```

逻辑资源示例：

```text
AD_MAIN_0
DA_MAIN_0
DI_POWER_OK
DO_POWER_EN
SERIAL_A
CANFD_A
```

---

## 5. 运行流程

### 5.1 启动

```text
启动程序
  -> 初始化日志模块
  -> 加载全局设置
  -> 初始化业务服务
  -> 初始化 UI
  -> 等待用户选择产品配置
```

### 5.2 加载配置

```text
UI 选择 .testcfg
  -> TestConfigManager 解析和校验
  -> TestPlanBuilder 生成 TestPlan
  -> UI 展示测试项列表
```

### 5.3 执行测试

```text
UI 请求开始
  -> hwtest_biz 创建 TestContext、冻结 TestPlan
  -> hwtest_biz 调 IAlgorithmExecutor.prepare
  -> hwtest_biz 依据依赖顺序调用 IAlgorithmExecutor.executeStep 并处理重试
  -> 算法层按协议 CSV 编解码测试设备与被测件通讯帧
  -> 算法层管理 HAL 初始化、调用和安全输出
  -> HAL 调 Adapter -> 厂家库
  -> 结果和日志回到 hwtest_biz；报告只读取编排后的快照
```

### 5.4 停止

```text
UI 请求停止
  -> hwtest_biz 更新 IRunControl 并调用 IAlgorithmExecutor.requestStop(timeoutMs)
  -> 算法层使当前步骤在安全点结束，并负责 HAL 安全态和设备生命周期
  -> hwtest_biz 停止后续调度、汇总已产生结果、更新状态并通知 UI
```

---

## 6. 日志与追踪

- 五层均通过 `logProduced` 语义生产日志事件。
- 统一日志模型为 `LogEvent`。
- HAL 内部事件为 `HalLogEvent`，进入日志模块时映射为 `LogEvent`。
- Adapter 日志先进入 HAL，再由 HAL 产生 `source = "adapter"` 的 `HalLogEvent`。
- 业务层为每次测试任务生成一个非空 `requestId`，任务内各步骤以及 UI、业务、算法、HAL、Adapter 同链路复用。
- 报告模块读取日志摘要，不负责收集日志。

详见 `../contracts/log-interface-protocol.md`。

---

## 7. 安全与稳定性

- 所有硬件调用都必须有超时；该调用和错误细化由算法/HAL 层处理。
- 输出类安全范围校验、safe state、设备关闭和 HAL reset/shutdown 属于算法/HAL 层。
- BIZ 只保存和透传 `executionConfig`，并把算法端口返回的业务 `Status`、结果和日志编排给上层。
- 停止、取消、设备断开、严重 IO 错误、应用退出时，算法层负责硬件安全收尾，BIZ 负责状态和结果收尾。
- 低配 Win7 机器优先保证稳定、简洁、可维护。

安全策略主定义：

- 配置迁移和 BIZ 的业务边界见 `../contracts/business-scheduling-layer.md`。
- HAL 输出校验和安全态执行见 `../contracts/hal-interface-protocol.md`。

---

## 8. 推荐源码结构

当前仓库已落地 HAL、日志和可运行的 BIZ 配置/调度/报告实现。完整系统推荐结构：

```text
src/
  ui/
  biz/
    include/biz/
    src/
  algorithms/
    analog/
    digital/
    serial/
    canfd/
  hal/
    include/hal/
    src/
  logging/
    include/logging/
    src/
  adapters/

configs/products/
logs/
reports/
data/
```

本仓库当前落地范围：

```text
src/hal/include/hal/   公共 HAL 头文件
src/hal/src/           HAL 内部实现和 Mock/C ABI Adapter 包装
src/logging/include/   公共日志头文件
src/logging/src/       日志服务、JSONL sink、HAL 日志桥接
src/biz/include/biz/   BIZ 数据模型、配置/计划、对上服务、报告和算法端口契约
src/biz/src/           配置迁移与校验、稳定拓扑计划、串行调度/重试、状态控制和报告实现
src/biz/               hwtest_biz 静态库；仅直接依赖 Qt Core 和 hwtest_log_types
tests/biz/             35 个 BIZ 用例；仅使用 FakeAlgorithmExecutor、配置与结果样本
docs/design/           架构与接口协议
```

算法层具体实现仍由完整系统提供：它实现 `IAlgorithmExecutor` 并持有 HAL。当前 BIZ 按稳定拓扑顺序串行执行步骤；并行配置字段已保留，但并行调度尚未启用。

---

## 9. 验收标准

- UI 不直接调用算法实现、HAL 或 Adapter。
- BIZ API 和源码不得出现 HAL、`IHal*`、Socket、`MeasurementBase`/`MeasurementFactory`、codec 或安全输出执行接口；允许出现“禁止直接使用”的架构说明。
- 算法层实现 `IAlgorithmExecutor`，并承担单步执行、协议/通讯、HAL 生命周期和安全输出执行。
- HAL 不包含具体测试判定逻辑。
- Adapter 不包含 UI、业务流程、测试判定逻辑。
- 新产品优先新增 `.testcfg`。
- 新板卡优先新增或替换 Adapter。
- 新写出的配置只使用 `executionConfig`；旧 `halConfig` 仅可作为迁移读取输入。
- 所有硬件操作有超时、统一错误、日志和 `requestId`。
- Mock Adapter 能支撑无真实硬件联调。
