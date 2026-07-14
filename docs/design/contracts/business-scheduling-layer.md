# 业务调度层接口契约

> 适用项目：多产品通用硬件测试软件（Qt 5.15 / C++17 / Windows）
> 本文定位：BIZ 对上服务、纯业务数据模型、计划调度，以及 BIZ 到算法层的执行端口。
> 当前范围：配置、计划、依赖排序、重试、运行状态、结果编排和报告；权限枚举与并行参数仅为兼容/扩展面，硬件、通讯、协议和安全执行不属于 BIZ。
> 事实来源：`docs/design/overview/five-layer-architecture.md`、`docs/design/contracts/hal-interface-protocol.md`、`docs/design/contracts/device-communication-protocol.md`、`docs/design/contracts/log-interface-protocol.md`、`src/hal/include/hal/*.h`、附件《业务调度层接口契约文档_V3.1.md》。
> 冲突处理：测试设备与被测件的底层通讯帧以 `device-communication-protocol.md` 为准，由算法层实现；日志术语以 `log-interface-protocol.md` 为准；业务入口以 `ITestRunService` 为准，`SchedulerAPI` 只作迁移期兼容名；BIZ 的唯一执行端口为 `IAlgorithmExecutor`。

---

## 1. 边界

```text
UI
  -> hwtest_biz
  -> biz::IAlgorithmExecutor  (由算法层实现)
  -> hwtest_hal
  -> Adapter
```

BIZ 负责：

- 配置读取、迁移、校验和新格式写出。
- 计划生成、依赖拓扑排序、重试和任务状态编排。
- 结果聚合、日志转发和报告编排。
- 将不透明的 `executionConfig`、`TestPlan` 和 `TestContext` 交给算法端口。

BIZ 不负责：

- UI 展示和控件状态。
- 单步算法、测量对象、协议 codec 或通讯连接。
- HAL 生命周期、设备句柄、安全输出、safe state 或硬件 reset/shutdown。
- Adapter ABI、厂家 SDK 和日志存储实现。

---

## 2. 术语和兼容名

| 概念 | 主名 | 兼容/历史名 |
| --- | --- | --- |
| 对外业务入口 | `ITestRunService` | `SchedulerAPI` |
| BIZ 执行端口 | `IAlgorithmExecutor` | 无 |
| 控制和观察端口 | `IRunControl`、`IAlgorithmObserver` | 无 |
| 报告端口 | `IReportGenerator` | `ReportService` |
| 日志模型 | `hwtest::logging::LogEvent` | `ILogger`、`logGenerated` |

规则：

- `SchedulerAPI`、`ILogger`、`logGenerated` 只允许出现在兼容层或历史说明里。
- BIZ 新代码只认 `ITestRunService`、`IAlgorithmExecutor`、`LogEvent`、`logProduced`。
- `TestConfig` 在本文中指根配置；单项配置由 `TestStep` 表达，不再用同名双义。

---

## 3. 对上协议原则

- 命名空间为 `hwtest::biz`；日志类型直接使用 `hwtest::logging::LogEvent`。
- 对上统一使用 Qt 友好类型：`QString`、`QStringList`、`QVector`、`QVariantMap`、`QObject`。
- 所有同步 API 统一返回 `Status` 或 `Result<T>`。
- `RequestId` 是 `QString`，同一任务链在 UI、BIZ、算法和后续层之间保持一致。
- 信号在释放锁之后发出；跨线程连接使用 `Qt::QueuedConnection`。
- BIZ 的公共头、目标链接和运行期对象均不得直接出现 HAL、Socket、测量、codec 或安全执行接口。

---

## 4. 公共数据模型

### 4.1 基础类型

```cpp
namespace hwtest::biz {

using TaskId = QString;
using PlanId = QString;
using StepId = QString;
using TestItemId = QString;
using ConfigPath = QString;
using ReportPath = QString;
using RequestId = QString;
using UserId = QString;
using StationId = QString;

}
```

### 4.2 状态和错误

```cpp
enum class TestState {
    Uninitialized = 0,
    Idle,
    Running,
    Paused,
    Stopping,
    Finished,
    Error
};

enum class ErrorCode {
    Ok = 0,
    InvalidState,
    NotInitialized,
    ConfigNotLoaded,
    ConfigParseError,
    ConfigSchemaError,
    ItemNotFound,
    DependencyCycle,
    ParameterRangeError,
    PermissionDenied,
    ResourceBusy,
    ResourceTimeout,
    CapabilityUnsupported,
    BusTimeout,
    ChannelOccupied,
    DriverMissing,
    SampleFail,
    RemoteCommandError,
    ProtocolParseError,
    DiskFull,
    Cancelled,
    FatalHardwareError,
    InternalError
};

struct ErrorInfo {
    ErrorCode code = ErrorCode::Ok;
    QString message;
    QString operation;
    QString testItemId;
    QVariantMap detail;
};

struct Status {
    ErrorCode code = ErrorCode::Ok;
    ErrorInfo error;
    bool ok() const { return code == ErrorCode::Ok; }
};

template <typename T>
struct Result {
    Status status;
    T value {};
    bool ok() const { return status.ok(); }
};
```

约定：

- `Status` 只表达成功/失败和错误上下文。
- `Result<T>` 失败时 `value` 保持默认值。
- `ErrorCode` 是 BIZ 对上业务码；算法层负责将其执行结果归一化为该模型。

### 4.3 控制、判定和预留权限枚举

```cpp
enum class RunControl {
    Run = 0,
    Pause,
    Stop
};

enum class TestVerdict {
    Pass = 0,
    Fail,
    Error,
    Skipped
};

enum class SkipReason {
    None = 0,
    DependencyFailed,
    Disabled,
    ResourceBusy,
    SetupFailed,
    Cancelled
};

enum class Permission {
    LoadConfig,
    EditConfig,
    StartTest,
    StopTest,
    ExportReport,
    ManageHardware
};

enum class CmpOp {
    GreaterThan,
    GreaterOrEqual,
    LessThan,
    LessOrEqual,
    Equal,
    NotEqual,
    InRange
};
```

### 4.4 结果数据

```cpp
struct MeasurementRecord {
    QString name;
    QVariant expected;
    QVariant actual;
    QVariant tolerance;
    QString unit;
    QVariantMap metadata;
};

struct Criterion {
    QString metric;
    CmpOp op = CmpOp::GreaterThan;
    QVariant ref = 0.0;
    double lo = 0.0;
    double hi = 0.0;
    double tol = 0.0;
    bool passIfMatched = true;
};
```

### 4.5 配置、计划和执行上下文

```cpp
struct TestStep {
    StepId stepId;
    TestItemId testItemId;
    QString name;
    QString type = QStringLiteral("EXCHANGE");
    QString board;
    QString algorithmId;
    QVariantMap parameters;
    int timeoutMs = 1000;
    int retryCount = 0;
    bool enabled = true;
    QStringList dependsOn;
    QList<Criterion> criteria;
};

struct RuntimeConfig {
    bool parallelEnabled = true;
    int maxParallel = 0;
    int defaultTimeoutMs = 1000;
    int defaultRetryCount = 0;
    int retryIntervalMs = 50;
    int taskPriorityDefault = 2;
    int pauseAutoReleaseMs = 0;
    bool stopOnFirstFailure = true;
    bool allowResume = false;
    QString reportDir;
    QString logDir;
    qint64 logRotateBytes = 10 * 1024 * 1024;
    int logKeepFiles = 10;
    QVariantMap tags;
};

struct TestConfig {
    QString schemaVersion;
    QString configId;
    QString productModel;
    QString productName;
    QString configVersion;
    QVector<TestStep> steps;
    QVector<HardwareRequirement> hardwareRequirements;
    QVector<ProtocolProfile> protocolProfiles;
    QVariantMap executionConfig;
    SafetyPolicy safetyPolicy;
    RuntimeConfig runtimeConfig;
    QVariantMap reportFields;
};

struct TestPlan {
    PlanId planId;
    QString configId;
    QString productModel;
    QString configVersion;
    QVector<TestStep> steps;
    QVector<HardwareRequirement> hardwareRequirements;
    QVector<ProtocolProfile> protocolProfiles;
    SafetyPolicy safetyPolicy;
    RuntimeConfig runtimeConfig;
};

struct TestContext {
    TaskId runId;
    RequestId requestId;
    QString productModel;
    UserId operatorId;
    StationId stationId;
    QVariantMap tags;
};

struct SystemResource {
    double cpuUsage = 0.0;
    qint64 memoryUsedMB = 0;
    qint64 diskFreeMB = 0;
    int idleThreadNum = 0;
    QStringList occupiedResources;
};

struct TestResult {
    StepId stepId;
    TestItemId testItemId;
    QString algorithmId;
    TestVerdict verdict = TestVerdict::Skipped;
    SkipReason skipReason = SkipReason::None;
    ErrorCode errorCode = ErrorCode::Ok;
    QString message;
    QVector<MeasurementRecord> measurements;
    QVariantMap rawData;
    int attempts = 1;
    qint64 startTimeUs = 0;
    qint64 endTimeUs = 0;
};

struct ReportOptions {
    QString outDir;
    QString title;
    TaskId taskId;
    QStringList itemFilter;
    bool html = true;
    bool csv = false;
    bool txt = false;
    bool xml = false;
};
```

约定：

- BIZ 仅解释 `TestStep` 的排序、启用、依赖、超时和重试语义；`algorithmId` 与 `parameters` 的执行含义属于算法层。
- `executionConfig` 是交给 `IAlgorithmExecutor::prepare` 的不透明配置。协议 CSV、codec、通讯连接、设备资源和安全限制由算法层解释。
- 迁移读取可以接受旧字段 `halConfig` 并标准化为 `executionConfig`；任何新写出的配置只能使用 `executionConfig`。
- 判定条件 `op` 新格式使用规范字符串；迁移读取兼容旧整数值。`ref` 是 JSON 标量，可保留数字、布尔或字符串语义，不得静默折算为 `0`。
- `TestContext` 的字段固定为以上六项，禁止扩展为设备句柄、通讯对象或执行对象。
- `ReportOptions` 一次必须且只能选择一种输出格式；默认只生成 HTML。`itemFilter` 可匹配 `stepId`、`testItemId` 或 `algorithmId`。

---

## 5. 对上接口 `ITestRunService`

```cpp
class ITestRunService : public QObject {
    Q_OBJECT
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
    void testProgress(const TaskId& taskId, const TestItemId& testItemId,
                      int progress, const QString& step);
    void stateChanged(const TaskId& taskId, TestState state);
    void resultProduced(const TaskId& taskId, const TestResult& result);
    void logProduced(const hwtest::logging::LogEvent& event);
    void hardwareError(const TaskId& taskId, const TestItemId& testItemId,
                       ErrorCode code, const QString& description);
};
```

`resetHardware` 是保留的对上方法名，其实现只能委托 `IAlgorithmExecutor::reset()`；`shutdown` 同理委托算法端口。BIZ 自身不持有设备对象，也不描述 HAL 初始化/关闭或安全态执行细节。

调用约定：

- 空 `testItems` 表示所有启用步骤；BIZ 先做依赖排序，再将单步交给算法端口。
- `priority == -1` 使用 `RuntimeConfig::taskPriorityDefault`；除 `-1` 外只接受 1 到 3，其余返回 `ParameterRangeError`。
- `loadConfiguration` 仅允许 `Idle` 或 `Finished`；`stopTest` 在 `Idle` 幂等返回成功。
- `generateReport` 只读取 BIZ 已编排的结果和日志摘要。

状态机：

| 当前状态 | 允许操作 | 目标状态 |
| --- | --- | --- |
| `Uninitialized` | `initialize()` | `Idle` |
| `Idle` / `Finished` | `loadConfiguration()` / `startTest()` | `Idle` / `Running` |
| `Running` | `pauseTest()` | `Paused` |
| `Paused` | `resumeTest()` | `Running` |
| `Running` / `Paused` | `stopTest()` | `Stopping` -> `Idle` |
| 任意活动状态 | 算法端口返回不可恢复业务错误 | `Error` |
| `Idle` / `Finished` / `Error` | `shutdown()` | `Uninitialized` |

---

## 6. BIZ 到算法层端口

`IAlgorithmExecutor` 由算法层实现。BIZ 只通过该端口提交计划和观察结果，既不包含也不管理具体算法、测量、codec、通讯连接或 HAL 对象。

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
    virtual void onProgress(const StepId& stepId,
                            const TestItemId& itemId,
                            int progress,
                            const QString& stage) = 0;
    virtual void onSample(const StepId& stepId,
                          const RawSample& sample) = 0;
    virtual void onLog(const hwtest::logging::LogEvent& event) = 0;
};

class IAlgorithmExecutor {
public:
    virtual ~IAlgorithmExecutor() = default;
    virtual Status prepare(const TestPlan& plan,
                           const TestContext& context,
                           const QVariantMap& executionConfig) = 0;
    virtual Result<TestResult> executeStep(const TestStep& step,
                                           const IRunControl& control,
                                           IAlgorithmObserver& observer) = 0;
    virtual Status requestStop(int timeoutMs) = 0;
    virtual Status reset() = 0;
    virtual Status shutdown(int timeoutMs) = 0;
};
```

端口约定：

- `prepare` 由算法层完成执行环境准备；BIZ 不解释 `executionConfig` 的设备、协议或安全字段。
- `executeStep` 只执行一个已由 BIZ 排序的步骤。依赖失败、禁用、重试次数和任务级停止策略均由 BIZ 编排。
- 算法层内部拥有 `MeasurementBase`/`MeasurementFactory`、codec、Socket、HAL 生命周期以及安全输出执行；它们不是 BIZ 公共 API。
- `requestStop`、`reset`、`shutdown` 的硬件语义由算法实现负责。BIZ 只更新状态、停止后续调度并发布结果/日志。
- 工厂接收的 `IAlgorithmExecutor*` 是非拥有指针，执行器必须晚于服务销毁；服务不会删除执行器。
- `IRunControl` 和 `IAlgorithmObserver` 引用只在对应 `executeStep()` 调用期间有效，算法实现不得缓存。
- `requestStop(timeoutMs)` 无论返回成功或失败，都必须让活动中的 `executeStep()` 观察取消并在该时限内返回；返回值描述清理结果，不表示可以继续阻塞。`shutdown(timeoutMs)` 也必须遵守时限。

---

## 7. 配置、计划和权限扩展点

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

配置和计划规则：

- `TestConfigManager` 严格拒绝已建模对象中的未知字段，校验 schema、产品标识、步骤标识唯一性以及超时、重试、运行时和安全范围，并无损读写已建模字段。
- `TestPlanBuilder` 过滤禁用步骤、合并默认超时和重试、拒绝缺失依赖和依赖环，并计算稳定的拓扑顺序。
- 新配置的 `dependsOn` 使用 `StepId`；迁移输入可引用唯一的 `testItemId`，计划生成时统一归一化为 `StepId`。
- 当前执行器按拓扑顺序串行调度。`parallelEnabled` 和 `maxParallel` 只做配置保留与范围校验，尚不形成并行组，也不启动并行步骤。
- `Permission` 目前只是公共兼容枚举，仓库尚未落地 `PermissionService` 或授权策略；接入权限时应位于 BIZ 对上入口，不得下沉为 HAL 权限判断。
- BIZ 不验证算法内部的协议、codec、通讯连接、设备能力或安全输出限制；这些由 `IAlgorithmExecutor::prepare` 返回业务 `Status`。
- 当前 `getResourceStatus` 返回默认的 BIZ 只读快照，作为后续资源观测扩展点；它不锁定、释放或复位硬件资源。

## 8. 日志、报告和工厂

- BIZ 只生产和转发 `logProduced(const hwtest::logging::LogEvent&)`；每次测试任务生成一个非空 `RequestId`，任务内各步骤沿用该值。
- `IReportGenerator` 只读 BIZ 编排后的 `TestResult` 快照，不触发算法、日志存储或硬件操作。
- `biz_factory` 只声明服务和报告对象的创建/销毁入口；服务可接受 `IAlgorithmExecutor`，不得提供设备、通讯或 HAL 注入重载。

```cpp
class IReportGenerator {
public:
    virtual ~IReportGenerator() = default;
    virtual Result<ReportPath> createReport(const QVector<TestResult>& results,
                                            const ReportOptions& options) = 0;
};
```

## 9. BIZ 运行流程

```text
loadConfiguration
  -> TestConfigManager.load / validate
  -> store the normalized configuration

startTest
  -> TestPlanBuilder.build
  -> create TestContext
  -> IAlgorithmExecutor.prepare(plan, context, executionConfig)
  -> BIZ selects dependency-ready steps
  -> IAlgorithmExecutor.executeStep(step, control, observer)
  -> BIZ applies retry, skip and stop policy
  -> BIZ aggregates results and forwards logs
```

```text
pauseTest / resumeTest
  -> update IRunControl
  -> BIZ stops or resumes scheduling new steps
  -> algorithm checks the control object at its own safe checkpoints

stopTest / shutdown
  -> update IRunControl
  -> IAlgorithmExecutor.requestStop(timeoutMs)
  -> algorithm performs its execution and hardware lifecycle cleanup
  -> BIZ stops scheduling, records final results and changes state
```

## 10. 错误、并发和迁移

- BIZ 只处理 `Status`、`ErrorInfo`、`TestResult` 和 `LogEvent`，不定义或映射 HAL 错误码。
- 配置/参数错误优先使用 `ConfigParseError`、`ConfigSchemaError` 或 `ParameterRangeError`；依赖和调度错误使用 `DependencyCycle`、`ResourceBusy`、`ResourceTimeout` 或 `Cancelled`。
- BIZ 同步 API 使用互斥锁；所有对上信号在解锁后发出；`shutdown` 等待 BIZ 工作线程退出。
- 旧配置中的 `halConfig` 只能在读取迁移阶段映射到 `executionConfig`。BIZ 不把该旧字段传递为设备对象或执行接口，新写出一律使用 `executionConfig`。

## 11. 扩展和验收

- 新算法由算法层实现 `IAlgorithmExecutor`，不改变 BIZ 对上服务。
- 新协议或协议 CSV 字段规则按 `device-communication-protocol.md` 在算法层实现；BIZ 只透传 `executionConfig`。
- 新板卡优先在 HAL/Adapter 层扩展；BIZ 只处理由算法端口返回的业务结果。
- `hwtest_biz` 是静态库，只直接依赖 Qt Core 和 HAL-free `hwtest_log_types`。
- BIZ 单元测试使用 `FakeAlgorithmExecutor` 和配置样本；完整 BIZ -> 算法 -> HAL -> Mock Adapter 链属于系统集成测试。
- 当前 `hwtest_biz_tests` 注册 35 个 GoogleTest 用例，包含附件样例迁移加载、配置/计划/服务/报告和静态分层扫描。
- 架构扫描必须确认 BIZ 公共 API、源码和 BIZ 单测不出现 HAL、`IHal*`、Socket、`MeasurementBase`/`MeasurementFactory`、codec 或安全输出执行接口；文档中的禁止条款和算法层归属说明除外。
