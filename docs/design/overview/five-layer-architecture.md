# 多产品通用硬件测试软件五层架构

> 技术栈：Qt 5.15 / C++17 / Qt Widgets / Windows 7+
> 当前范围：AD、DA、DI、DO、串口、CANFD。
> 本文定位：总览、分层边界、跨层依赖。业务调度细节见 `../contracts/business-scheduling-layer.md`，HAL 细节见 `../contracts/hal-interface-protocol.md`，日志细节见 `../contracts/log-interface-protocol.md`，测试规范见 `../testing/testing-specification.md`。

---

## 1. 事实来源

| 文档 | 主定义 |
| --- | --- |
| `overview/five-layer-architecture.md` | 五层关系、模块边界、跨层流程 |
| `contracts/business-scheduling-layer.md` | 业务调度层契约 |
| `contracts/hal-interface-protocol.md` | HAL 对上接口、Adapter ABI、资源和错误映射 |
| `contracts/log-interface-protocol.md` | `LogEvent`、`LogService`、`logProduced`、source 约定 |
| `testing/testing-specification.md` | 测试目录、分层边界、用例范围、运行方式 |

冲突处理：

- HAL 术语以 `../contracts/hal-interface-protocol.md` 和 `src/hal/include/hal/*.h` 为准。
- 日志术语以 `../contracts/log-interface-protocol.md` 为准。
- 业务入口以 `TestRunService` 为准；`SchedulerAPI` 只作附件术语或兼容别名。
- 执行核心以 `TestFlowEngine` 为准；`TestEngine` 只作内部实现名或历史别名。

---

## 2. 总体架构

```text
┌─────────────────────────────────────────────┐
│ 1. UI 交互层                                │
│ Qt Widgets 页面、配置编辑、测试启停、日志展示 │
└───────────────────────┬─────────────────────┘
                        │ UI 命令 / 展示模型
┌───────────────────────▼─────────────────────┐
│ 2. 业务调度层                                │
│ 配置加载、计划生成、流程编排、状态、权限、报告 │
└───────────────────────┬─────────────────────┘
                        │ TestPlan / TestContext
┌───────────────────────▼─────────────────────┐
│ 3. 核心测试算法层                            │
│ AD/DA、DI/DO、串口、CANFD 测试逻辑和判定      │
└───────────────────────┬─────────────────────┘
                        │ IHalService / IHalDevice
┌───────────────────────▼─────────────────────┐
│ 4. HAL 硬件抽象层                            │
│ 逻辑资源映射、统一错误、参数归一化、安全校验   │
└───────────────────────┬─────────────────────┘
                        │ HAL Adapter ABI / HardwareAdapter
┌───────────────────────▼─────────────────────┐
│ 5. 硬件适配器层 Adapter                      │
│ 封装厂家 DLL/lib/SDK/Win32 API               │
└─────────────────────────────────────────────┘

UI / 业务 / 算法
  -> emit logProduced(LogEvent)
  -> LogService

HAL / Adapter 日志
  -> emit IHalService::logProduced(HalLogEvent)
  -> hal_log_bridge
  -> LogService
```

边界：

- 前四层属于主程序核心模块。
- Adapter 是本软件维护的最下层；厂家 DLL、SDK、驱动是外部依赖。
- 算法层不得包含厂家 SDK 头文件，不直接调厂家函数。
- 更换板卡优先新增或替换 Adapter，HAL 以上少改或不改。
- 日志模块为旁路基础模块，不属于五层任一层。
- 当前日志模块源码位于 `src/logging/`，命名空间为 `hwtest::logging`，构建产物为 `hwtest_log`。

---

## 3. 分层职责

| 层级 | 职责 | 不做 |
| --- | --- | --- |
| UI 交互层 | 登录、产品选择、配置编辑、测试启停、进度和结果展示 | 不写硬件业务，不直接访问 HAL/Adapter |
| 业务调度层 | 配置管理、计划生成、流程执行、状态、权限、数据记录、报告 | 不调用厂家 SDK，不定义 HAL 类型 |
| 核心测试算法层 | 测试项逻辑、测量、阈值判定、结构化结果 | 不操作 UI，不写报告，不依赖厂家 |
| HAL 层 | 设备发现、资源映射、参数归一化、安全校验、统一错误、调用 Adapter | 不做业务流程，不做测试判定 |
| Adapter 层 | 加载/链接厂家库，封装设备调用，转换数据和错误 | 不接触 UI、业务流程、测试判定 |

依赖规则：

| 层级 | 可以依赖 | 禁止依赖 |
| --- | --- | --- |
| UI | 业务层 Service / ViewModel、日志服务 | HAL、Adapter、厂家 SDK |
| 业务 | 算法注册表、配置模型、报告、日志、HAL 服务句柄 | Adapter、厂家 SDK |
| 算法 | HAL 对上接口、测试上下文、日志事件模型 | UI、业务流程实现、厂家 SDK |
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
- 安全输出限制。
- 报告字段。

典型流向：

```text
.testcfg
  -> TestConfigManager 校验
  -> TestPlanBuilder 生成 TestPlan
  -> TestRunService 创建 TestContext
  -> TestFlowEngine 顺序执行 TestStep
  -> 算法调用 HAL 逻辑资源
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
  -> TestRunService 创建 TestContext
  -> IHalService.initialize(halConfig)
  -> HAL 扫描设备并建立资源映射
  -> TestFlowEngine 按 TestPlan 执行
  -> 算法调用 HAL
  -> HAL 调 Adapter
  -> Adapter 调厂家库
  -> 结果、日志、CSV、报告回到业务层
```

### 5.4 停止

```text
UI 请求停止
  -> TestRunService 标记停止
  -> 当前步骤结束后停止后续步骤
  -> HAL 输出进入安全状态
  -> 关闭设备
  -> 记录日志并更新 UI
```

---

## 6. 日志与追踪

- 五层均通过 `logProduced` 语义生产日志事件。
- 统一日志模型为 `LogEvent`。
- HAL 内部事件为 `HalLogEvent`，进入日志模块时映射为 `LogEvent`。
- Adapter 日志先进入 HAL，再由 HAL 产生 `source = "adapter"` 的 `HalLogEvent`。
- 业务层为每个测试步骤生成 `requestId`，UI、业务、算法、HAL、Adapter 同链路复用。
- 报告模块读取日志摘要，不负责收集日志。

详见 `../contracts/log-interface-protocol.md`。

---

## 7. 安全与稳定性

- 硬件调用必须有超时。
- 错误统一映射为 `HalStatusCode`。
- 输出类操作由 HAL 做安全范围校验。
- 停止、取消、设备断开、严重 IO 错误、应用退出时进入安全状态。
- 低配 Win7 机器优先保证稳定、简洁、可维护。

安全策略主定义：

- 业务层的 `SafetyPolicy` 见 `../contracts/business-scheduling-layer.md`。
- HAL 输出校验和 `safeState` 执行见 `../contracts/hal-interface-protocol.md`。

---

## 8. 推荐源码结构

当前仓库已落地 HAL 核心库和独立日志模块。完整系统推荐结构：

```text
src/
  ui/
  business/
    config/
    flow/
    report/
    permission/
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

本仓库现状：

```text
src/hal/include/hal/   公共 HAL 头文件
src/hal/src/           HAL 内部实现和 Mock/C ABI Adapter 包装
src/logging/include/   公共日志头文件
src/logging/src/       日志服务、JSONL sink、HAL 日志桥接
tests/log/             日志模块 GoogleTest
docs/design/           架构与接口协议
```

---

## 9. 验收标准

- UI 不直接调用 HAL 或 Adapter。
- 算法层不包含厂家 SDK 头文件。
- HAL 不包含具体测试判定逻辑。
- Adapter 不包含 UI、业务流程、测试判定逻辑。
- 新产品优先新增 `.testcfg`。
- 新板卡优先新增或替换 Adapter。
- 所有硬件操作有超时、统一错误、日志和 `requestId`。
- Mock Adapter 能支撑无真实硬件联调。
