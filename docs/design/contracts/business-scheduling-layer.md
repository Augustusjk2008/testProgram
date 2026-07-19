# 业务调度层接口契约

> 本文只定义 `hwtest::biz` 的对上服务、纯业务模型、计划调度和 BIZ 到算法层的端口。生产硬件/通讯 I/O 边界见 `../overview/five-layer-architecture.md`，协议 CSV 与物理帧见 `device-communication-protocol.md`，日志字段和映射见 `log-interface-protocol.md`。
>
> `[当前实现]` 以 `src/biz/include/biz/`、`src/biz/src/` 和 `hwtest_biz` 为准。`[目标契约-未实现]` 的 HAL Provider 路由、Qt 串口/TCP/UDP 和真实厂家链不属于本契约。

## 1. 边界

```text
调用方
  -> ITestRunService
  -> TestConfig / TestPlan / TestContext
  -> IAlgorithmExecutor
```

BIZ 负责：

- 配置读取、迁移、校验和新格式写出。
- 计划生成、稳定依赖排序、重试、任务状态和结果编排。
- 报告编排，以及 `LogEvent` 的生产和转发。
- 向算法端口传递 `TestPlan`、`TestContext` 和不透明的 `executionConfig`。

BIZ 不解释产品协议字段，不执行单步判定，也不持有或操作测试设备/DUT 的硬件、通讯、连接、deadline 或物理安全态。它的公开头、目标链接和运行期对象不得直接出现 HAL、Adapter、Socket、codec、测量工厂或安全输出执行接口。

## 2. 公共类型和配置模型

`[当前实现]` 完整声明以 `src/biz/include/biz/biz_types.h` 为准；命名空间为 `hwtest::biz`，对上使用 Qt Core 类型。

| 类型 | BIZ 语义 |
| --- | --- |
| `Status`、`Result<T>`、`ErrorInfo` | 所有同步操作的成功/失败和错误上下文；BIZ 不定义或映射 HAL 错误码 |
| `TestConfig` | 根配置：身份、步骤、硬件需求、协议 profile、`executionConfig`、安全/运行时策略和报告字段 |
| `TestStep` | 可调度步骤：标识、算法 ID、参数、超时、重试、启用状态、依赖和判据 |
| `TestPlan` | 由已规范化配置生成的有序步骤和相关业务模型；不包含 `executionConfig` |
| `TestContext` | 一次任务的 `runId`、`requestId`、产品、操作者、工位和 tags；不得扩展为设备句柄或通讯对象 |
| `TestResult`、`MeasurementRecord`、`RawSample` | 单步结果、测量记录和算法回传样本；BIZ 聚合但不改变产品判定语义 |
| `ProtocolProfile`、`HardwareRequirement`、`SafetyPolicy` | 兼容和透传模型；BIZ 保存/校验结构，不解释协议或实施安全动作 |
| `RuntimeConfig`、`ReportOptions` | 业务调度与报告选项；文件 I/O 不属于生产硬件/通讯 I/O 边界 |

`TestState`、`TestVerdict`、`SkipReason`、`RunControl`、`CmpOp`、`Permission` 和 `ErrorCode` 的枚举值是公共兼容面。结构体应优先尾部扩展，不改变既有枚举数值或语义。

配置规则：

- `TestConfigManager` 严格拒绝已建模对象中的未知字段，校验身份、步骤标识、超时、重试、依赖和枚举范围，并无损读写已建模字段。
- `executionConfig` 是传给 `IAlgorithmExecutor::prepare()` 的不透明 `QVariantMap`。BIZ 不验证其协议、设备、通讯或安全内部字段。
- 新写出的根配置只能使用 `executionConfig`。旧根字段 `halConfig` 只能在读取迁移时映射为 `executionConfig`；新配置不得再写出 `halConfig`。
- `HardwareRequirement.adapterId` 是当前兼容的、不透明需求元数据。BIZ 不用它选择后端，它也不等同于 HAL 部署配置中的目标 `providerId`。
- `dependsOn` 的新格式使用 `StepId`；读取迁移可接受唯一的 `testItemId`，计划生成时统一为 `StepId`。

配置和计划的公开接口为：

```cpp
class TestConfigManager {
public:
    Result<TestConfig> load(const ConfigPath& filePath) const;
    Status save(const ConfigPath& filePath, const TestConfig& config) const;
    Result<QVector<QString>> validate(const TestConfig& config) const;
};

class TestPlanBuilder {
public:
    Result<TestPlan> build(const TestConfig& config) const;
};
```

## 3. 对上服务

`[当前实现]` `ITestRunService` 是当前唯一的对上业务服务接口；公共头中不存在 `SchedulerAPI`，历史文档中的该名称不构成兼容面。

```cpp
class ITestRunService : public QObject {
public:
    virtual Status initialize() = 0;
    virtual Status shutdown() = 0;
    virtual Status loadConfiguration(const ConfigPath& configPath) = 0;
    virtual Result<TaskId> startTest(const QStringList& testItems = {},
                                     int priority = -1) = 0;
    virtual Status pauseTest() = 0;
    virtual Status resumeTest() = 0;
    virtual Status stopTest(int timeoutMs = 5000) = 0;
    virtual Status resetHardware() = 0;
    virtual Result<TestState> getCurrentState() const = 0;
    virtual Result<ReportPath> generateReport(const ReportOptions& options = {}) = 0;
    virtual Result<SystemResource> getResourceStatus() const = 0;

signals:
    void testProgress(const TaskId&, const TestItemId&, int progress, const QString& step);
    void stateChanged(const TaskId&, TestState);
    void resultProduced(const TaskId&, const TestResult&);
    void logProduced(const hwtest::logging::LogEvent&);
    void hardwareError(const TaskId&, const TestItemId&, ErrorCode, const QString&);
};
```

- `resetHardware()` 是保留的对上方法名；BIZ 的实现只能委托算法端口的 `reset()`，不保有设备对象。
- 空 `testItems` 表示全部启用步骤；`priority == -1` 使用 `RuntimeConfig::taskPriorityDefault`，其他值只接受 1 到 3。
- `loadConfiguration()` 仅允许 `Idle` 或 `Finished`；`stopTest()` 在 `Idle` 幂等成功。
- `generateReport()` 只读取已编排的结果和日志摘要，不触发算法、日志存储或硬件操作。

报告和工厂的公开接口为：

```cpp
class IReportGenerator {
public:
    virtual ~IReportGenerator() = default;
    virtual Result<ReportPath> createReport(const QVector<TestResult>& results,
                                            const ReportOptions& options) = 0;
};

ITestRunService* createTestRunService(IAlgorithmExecutor* executor,
                                      QObject* parent = nullptr);
void destroyTestRunService(ITestRunService* service);
IReportGenerator* createReportGenerator();
void destroyReportGenerator(IReportGenerator* generator);
```

`createTestRunService()` 接收非拥有的执行器指针；服务不会删除执行器，执行器必须晚于服务销毁。`IReportGenerator` 只读取结果快照。

状态约定：

| 当前状态 | 操作 | 目标状态 |
| --- | --- | --- |
| `Uninitialized` | `initialize()` | `Idle` |
| `Idle` / `Finished` | `loadConfiguration()` / `startTest()` | `Idle` / `Running` |
| `Running` | `pauseTest()` | `Paused` |
| `Paused` | `resumeTest()` | `Running` |
| `Running` / `Paused` | `stopTest()` | `Stopping` 后收敛到 `Idle` |
| 活动状态 | 算法端口返回不可恢复业务错误 | `Error` |
| `Idle` / `Finished` / `Error` | `shutdown()` | `Uninitialized` |

## 4. BIZ 到算法层端口

`[当前实现]` `IAlgorithmExecutor` 是 BIZ 唯一的执行出口。该端口表达单步执行和取消语义，不定义产品协议、连接或 HAL API。

```cpp
class IRunControl {
public:
    virtual ~IRunControl() = default;
    virtual RunControl current() const = 0;
    virtual bool checkpoint() const = 0;
};

class IAlgorithmObserver {
public:
    virtual ~IAlgorithmObserver() = default;
    virtual void onProgress(const StepId&, const TestItemId&, int, const QString&) = 0;
    virtual void onSample(const StepId&, const RawSample&) = 0;
    virtual void onLog(const hwtest::logging::LogEvent&) = 0;
};

class IAlgorithmExecutor {
public:
    virtual ~IAlgorithmExecutor() = default;
    virtual Status prepare(const TestPlan&, const TestContext&,
                           const QVariantMap& executionConfig) = 0;
    virtual Result<TestResult> executeStep(const TestStep&, const IRunControl&,
                                           IAlgorithmObserver&) = 0;
    virtual Status requestStop(int timeoutMs) = 0;
    virtual Status reset() = 0;
    virtual Status shutdown(int timeoutMs) = 0;
};
```

- BIZ 先构建 `TestPlan` 和 `TestContext`，再调用 `prepare()`；`executeStep()` 只接收已排序的单步。
- 依赖失败、禁用、重试和任务级停止策略由 BIZ 编排；算法端口返回单步结果、样本、进度和日志。
- `IRunControl`、`IAlgorithmObserver` 的引用只在对应 `executeStep()` 调用期间有效，算法实现不得缓存。
- `requestStop(timeoutMs)` 必须使活动 `executeStep()` 在时限内观察取消并返回；其 `Status` 描述清理结果，不表示允许继续阻塞。`shutdown(timeoutMs)` 同样必须遵守时限。
- 工厂接收的 `IAlgorithmExecutor*` 为非拥有指针，执行器必须晚于服务销毁。

生产 I/O 的目标归属由总览定义：算法组织协议/流程/判定并请求 HAL，HAL 执行生产 I/O。BIZ 对此不增加设备、Provider 或网络接口。

## 5. 计划、调度、日志和报告

- `TestPlanBuilder` 过滤禁用步骤、合并默认超时和重试、拒绝缺失依赖和依赖环，并产生稳定拓扑顺序。
- `[当前实现]` 执行器按拓扑顺序串行调度。`parallelEnabled`、`maxParallel` 和 `Permission` 仅保留为配置/兼容扩展面，尚不形成并行组或授权服务。
- `getResourceStatus()` 当前返回 BIZ 只读快照；它不锁定、释放、复位或观察硬件资源。
- BIZ 只生产或转发 `logProduced(const hwtest::logging::LogEvent&)`。`LogEvent` 的来源、字段、追踪和 HAL/Adapter 映射以 `log-interface-protocol.md` 为唯一主定义。
- 报告只消费 BIZ 已编排的 `TestResult` 快照和摘要；报告文件 I/O 不触发执行器或设备操作。

## 6. 当前验证与扩展

- `[当前实现]` `hwtest_biz` 只直接依赖 Qt Core 与 HAL-free 的 `hwtest_log_types`；`hwtest_biz_tests` 使用 `FakeAlgorithmExecutor` 和配置样本，当前数量与统计口径见 `../testing/testing-specification.md`。
- 新算法通过实现 `IAlgorithmExecutor` 接入，不改变 BIZ 对上服务。产品协议或 CSV 规则留在算法层，BIZ 只透传 `executionConfig`。
- 产品模拟和集成验证必须经过 HAL，可使用 HAL Mock 或标准 Provider 连接隔离模拟目标；当前直连 Simulator 的算法闭环测试不构成该集成证据。具体测试边界见 `../overview/five-layer-architecture.md` 和 `../testing/testing-specification.md`。
