# 业务调度层接口契约

> 适用项目：多产品通用硬件测试软件（Qt 5.15 / C++17 / Windows）
> 本文定位：业务调度层对上接口、内部执行核心、配置/计划/结果模型、资源与安全边界。
> 当前范围：测试生命周期调度、配置解析、计划生成、资源管理、安全护栏、日志、报告；硬件能力对接 AD、DA、DI、DO、串口、CANFD；测试总线和被测总线支持串口、CANFD、网口；统一采用 `EXCHANGE` 测试模式和 JSON 协议配置。
> 事实来源：`docs/design/overview/five-layer-architecture.md`、`docs/design/contracts/hal-interface-protocol.md`、`docs/design/contracts/log-interface-protocol.md`、`src/hal/include/hal/*.h`、附件《业务调度层接口契约文档_V3.1.md》。
> 冲突处理：HAL 术语以 HAL 文档和头文件为准；日志术语以 `log-interface-protocol.md` 为准；业务入口以 `TestRunService` 为准，`SchedulerAPI` 只作兼容名；执行核心以 `TestFlowEngine` 为准，`TestEngine` 只作历史/内部别名。

---

## 1. 边界

```text
UI / 交互层
  -> TestRunService / SchedulerAPI
  -> 业务调度层
  -> MeasurementBase / TestFlowEngine / MeasurementFactory
  -> HAL IHalService / IHalDevice
  -> Adapter
  -> 厂家 DLL / lib / SDK / Win32 API
```

业务调度层负责：

- 配置加载、保存、校验。
- 测试计划生成和执行编排。
- 测试启动、停止、暂停、继续、重启。
- 资源申请、释放、并行控制。
- 安全策略下发和停机安全收尾。
- 结果收集、日志桥接、报告生成。
- 为测量层提供 `TestContext`、`IHalDevice*`、`ISocketTransport*`、`RunControl`。

业务调度层不负责：

- UI 展示和控件状态。
- 测试判定细节的硬编码。
- HAL 类型定义和 Adapter ABI。
- 厂家 SDK 直连。
- 日志存储底层实现。

---

## 2. 术语和兼容名

| 概念 | 主名 | 兼容/历史名 |
| --- | --- | --- |
| 对外业务入口 | `TestRunService` | `SchedulerAPI` |
| 执行核心 | `TestFlowEngine` | `TestEngine` |
| 配置管理 | `TestConfigManager` | `IConfigParser`、`SchedulerAPI` 内部旧名 |
| 计划生成 | `TestPlanBuilder` | - |
| 测量基类 | `MeasurementBase` | - |
| 测量工厂 | `MeasurementFactory` | - |
| 资源管理 | `IResourceManager` / `ResourceManager` | - |
| 安全护栏 | `ISafetyGuard` / `SafetyGuard` | - |
| 报告生成 | `IReportGenerator` / `ReportService` | - |
| 日志模型 | `LogEvent` | `ILogger`、`logGenerated` |

规则：

- `SchedulerAPI`、`TestEngine`、`ILogger`、`logGenerated` 只允许出现在兼容层或历史说明里。
- 新代码只认 `TestRunService`、`TestFlowEngine`、`LogEvent`、`logProduced`。
- `TestConfig` 在本文中指根配置；单项配置由 `TestStep` 表达，不再用同名双义。

---

## 3. 对上协议原则

- 命名空间建议：`hwtest::biz`。
- 对上统一使用 Qt 友好类型：`QString`、`QStringList`、`QVector`、`QVariantMap`、`QObject`。
- 所有同步 API 统一返回 `Status` 或 `Result<T>`。
- 硬件调用必须带超时，逻辑资源只传 ID，不传厂家句柄。
- `requestId` 必须贯穿 UI、业务、算法、HAL、Adapter 同一条链路。
- 信号在释放锁之后发出；跨线程连接使用 `Qt::QueuedConnection`。
- `OperationOptions`、`HalStatus`、`HalResult<T>` 直接复用 HAL 文档，不在本文重定义。

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
- `ErrorCode` 是业务层码，不等于 `HalStatusCode`。

### 4.3 运行控制和判定

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
    double ref = 0.0;
    double lo = 0.0;
    double hi = 0.0;
    double tol = 0.0;
    bool passIfMatched = true;
};
```

### 4.5 配置和计划

```cpp
struct ExchangeAction {
    QString source;              // RemoteCommand / HalAnalog / HalDigital / HalSerial / HalCanFd
    QString busType;             // SERIAL / CANFD / ETHERNET
    QString channelId;
    QString operation;           // send / receive / read / write
    QVariantMap options;
    QString protocolProfileId;
    QVariantMap protocolProfile; // 内联协议，优先级高于 protocolProfileId
};

struct ExchangeConfig {
    ExchangeAction stimulus;
    ExchangeAction acquisition;
    int settlingTimeMs = 0;
    QList<Criterion> criteria;
};

struct ProtocolProfile {
    QString id;
    QString busType;
    QString payloadEncoding;     // hex / ascii / base64
    QVariantMap frameFormat;
    QVariantMap timing;
    QVariantMap responseRules;
    QVariantMap fieldMapping;
};

struct TestStep {
    StepId stepId;
    TestItemId testItemId;
    QString name;
    QString type = "EXCHANGE";
    QString board;
    QString algorithmId;
    QVariantMap parameters;
    int timeoutMs = 1000;
    int retryCount = 0;
    bool enabled = true;
    QStringList dependsOn;
    QList<Criterion> criteria;
};

struct HardwareRequirement {
    QString requirementId;
    QString deviceId;
    QString adapterId;
    QStringList requiredModules;
    QStringList requiredResources;
    int priority = 0;
    QVariantMap match;
};

struct SafetyPolicy {
    QVariantMap outputLimits;
    QVariantMap safeState;
    bool enterSafeStateOnStop = true;
    bool enterSafeStateOnError = true;
    double daMinVoltage = 0.0;
    double daMaxVoltage = 0.0;
    int doMinSwitchIntervalMs = 0;
    int canSendMaxHz = 0;
    int resourceLockTimeoutMs = 3000;
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
    QVariantMap halConfig;
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
    RuntimeConfig runtimeConfig;
};

struct TestContext {
    TaskId runId;
    hwtest::hal::RequestId requestId;
    QString productModel;
    UserId operatorId;
    StationId stationId;
    hwtest::hal::IHalService* hal = nullptr;
    class ISocketTransport* socket = nullptr;
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
    bool csv = true;
    bool txt = true;
    bool xml = false;
};
```

约定：

- `TestStep::type` 当前主值为 `"EXCHANGE"`。
- `board` 是逻辑设备名；`deviceId`、`adapterId`、`requiredResources` 只表达业务需求，不直接暴露厂家句柄。
- `protocolProfile` 内联优先于 `protocolProfileId`，再高于产品级默认协议。

---

## 5. `EXCHANGE` 统一模式

`EXCHANGE` 由 `stimulus` 和 `acquisition` 两个动作组成，动作来源可以是远程指令，也可以是 HAL 硬件操作。

| 场景 | stimulus | acquisition |
| --- | --- | --- |
| 远程指令 -> HAL 采集 | `RemoteCommand` | `HalAnalog` / `HalDigital` / `HalSerial` / `HalCanFd` |
| 远程指令 -> 通信总线反馈 | `RemoteCommand` | `RemoteCommand` |
| HAL 输出 -> 通信总线反馈 | `HalAnalog` / `HalDigital` / `HalSerial` / `HalCanFd` | `RemoteCommand` |

### 5.1 编解码接口

```cpp
class IFrameBuilder {
public:
    virtual ~IFrameBuilder() = default;
    virtual QByteArray build(const ExchangeAction& action,
                             const ProtocolProfile& profile) = 0;
};

class IFrameParser {
public:
    virtual ~IFrameParser() = default;
    virtual QVariantMap parse(const QByteArray& raw,
                              const ProtocolProfile& profile) = 0;
};
```

### 5.2 约定

- `busType == "SERIAL"` 时，串口由 `IHalDevice::serialBus()` 处理。
- `busType == "CANFD"` 时，CANFD 由 `IHalDevice::canFdBus()` 处理。
- `busType == "ETHERNET"` 时，网口由 `ISocketTransport` 处理。
- `HalAnalog` / `HalDigital` 只走 HAL 资源，不走 `ISocketTransport`。
- `settlingTimeMs` 只描述激励到采集之间的等待，不替代总线超时。

---

## 6. 对上接口 `TestRunService`

```cpp
class TestRunService : public QObject {
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
    virtual Result<ReportPath> generateReport(const ReportOptions& opt = {}) = 0;
    virtual Result<SystemResource> getResourceStatus() const = 0;

signals:
    void testProgress(const TaskId& taskId, const TestItemId& testItemId,
                      int progress, const QString& step);
    void stateChanged(const TaskId& taskId, TestState state);
    void resultProduced(const TaskId& taskId, const TestResult& result);
    void logProduced(const LogEvent& event);
    void hardwareError(const TaskId& taskId, const TestItemId& testItemId,
                       ErrorCode code, const QString& description);
};
```

### 6.1 方法语义

| 方法 | 输入 | 输出 | 语义 |
| --- | --- | --- | --- |
| `initialize` | - | `Status` | 注册元类型、创建内部模块、准备状态机 |
| `shutdown` | - | `Status` | 停止任务、释放资源、尽量进入安全态 |
| `loadConfiguration` | `configPath` | `Status` | 加载和校验 `.testcfg` |
| `startTest` | `testItems`、`priority` | `Result<TaskId>` | 创建任务并启动执行 |
| `pauseTest` | - | `Status` | 暂停当前任务 |
| `resumeTest` | - | `Status` | 恢复当前任务 |
| `stopTest` | `timeoutMs` | `Status` | 异步终止并等待收尾 |
| `resetHardware` | - | `Status` | 全局复位硬件 |
| `getCurrentState` | - | `Result<TestState>` | 获取当前状态 |
| `generateReport` | `opt` | `Result<ReportPath>` | 生成报告 |
| `getResourceStatus` | - | `Result<SystemResource>` | 获取资源快照 |

### 6.2 调用约定

- `startTest` 的 `testItems` 为空时，执行配置里全部启用项。
- `priority == -1` 时使用 `RuntimeConfig::taskPriorityDefault`。
- `priority` 只允许 1 到 3；越界返回 `ParameterRangeError`。
- `loadConfiguration` 只在 `Idle`、`Finished` 允许；其余状态返回 `InvalidState`。
- `stopTest` 在 `Idle` 可幂等返回 `Ok`。
- `shutdown` 应幂等。
- `generateReport` 只读结果和日志摘要，不触发硬件访问。

### 6.3 状态机

| 当前状态 | 允许操作 | 目标状态 |
| --- | --- | --- |
| `Uninitialized` | `initialize()` | `Idle` |
| `Idle` | `loadConfiguration()` / `startTest()` | `Idle` / `Running` |
| `Finished` | `loadConfiguration()` / `startTest()` | `Idle` / `Running` |
| `Running` | `pauseTest()` | `Paused` |
| `Paused` | `resumeTest()` | `Running` |
| `Running` / `Paused` | `stopTest()` | `Stopping` -> `Idle` |
| `Running` / `Paused` | 致命故障 | `Error` |
| `Idle` / `Finished` / `Error` | `shutdown()` | `Uninitialized` |

---

## 7. 执行核心

### 7.1 `ITestEngine`

```cpp
class ITestEngine {
public:
    virtual ~ITestEngine() = default;

    virtual QObject* asQObject() = 0;
    virtual Result<bool> checkHardwareResource(const QStringList& items) = 0;
    virtual Result<TaskId> runTestTask(const QStringList& items, int priority) = 0;
    virtual Status pauseTask() = 0;
    virtual Status resumeTask() = 0;
    virtual Status requestStop(int timeoutMs) = 0;
    virtual Status waitForIdle(int timeoutMs) = 0;
    virtual TestState state() const = 0;
    virtual Result<QVector<TestResult>> allResults() const = 0;
};
```

说明：

- `checkHardwareResource` 只做预检，不锁资源。
- `runTestTask` 启动异步执行。
- `waitForIdle` 等待当前任务退出。
- `allResults` 返回最近一次任务快照。

### 7.2 `TestFlowEngine`

```cpp
class TestFlowEngine {
public:
    Result<QVector<TestResult>> run(const TestPlan& plan, TestContext& context);
    void requestStop();
    void requestPause();
    void requestResume();
};
```

职责：

- 按 `TestPlan.steps` 顺序执行。
- 跳过 `enabled == false` 的项。
- 处理依赖关系和重试。
- 调 `MeasurementFactory` 创建测试项实例。
- 通过 `RunControl` 响应暂停和停止。
- 收集结果交给 `TestResultCollector`。

### 7.3 错误处理

| 场景 | 处理 |
| --- | --- |
| `dependsOn` 失败 | 标记 `Skip(DependencyFailed)` |
| `enabled == false` | 标记 `Skip(Disabled)` |
| `setup` 失败 | 标记 `Skip(SetupFailed)` |
| `execute` 判据不通过 | 直接 `Fail` |
| `execute` 过程中可重试错误 | 按 `retryCount` 重试 |
| `checkpoint()` 返回 false | 立即收尾并停止后续步骤 |
| 致命硬件错误 | 终止整次任务，进入 `Error` |

---

## 8. 测量接入

### 8.1 `MeasurementBase`

```cpp
class MeasurementBase {
public:
    virtual ~MeasurementBase() = default;

    void setConfig(const TestStep& config);
    void setHalDevice(hwtest::hal::IHalDevice* device);
    void setSocketTransport(class ISocketTransport* socket);
    void setControlToken(const std::atomic<RunControl>* token);

    virtual Status setup() = 0;
    virtual Result<TestResult> execute() = 0;
    virtual Status cleanup() = 0;
    virtual QString getTestName() const = 0;
    virtual HardwareRequirement getRequiredResources() const = 0;

protected:
    bool checkpoint() const;
};
```

约定：

- `setConfig`、`setHalDevice`、`setSocketTransport`、`setControlToken` 先于 `setup()` 调用。
- `setup()` 失败时，引擎标记该项为 `Skip(SetupFailed)`。
- `cleanup()` 由引擎用 RAII 保证执行。
- `checkpoint()` 长流程里轮询 `RunControl`，`Pause` 时阻塞，`Stop` 时退出。

### 8.2 `MeasurementFactory`

```cpp
class MeasurementFactory {
public:
    using Creator = std::function<MeasurementBase*()>;

    static Status registerType(const QString& typeKey, Creator creator);
    static Result<MeasurementBase*> create(const QString& typeKey);
};
```

约定：

- 重复注册允许覆盖，返回 `Ok`。
- 类型缺失返回 `ItemNotFound`。
- `EXCHANGE` 是当前默认注册类型。

示例：

```cpp
static bool s_reg_exchange = [] {
    MeasurementFactory::registerType("EXCHANGE", [] { return new ExchangeMeasurement; });
    return true;
}();
```

### 8.3 `ISocketTransport`

```cpp
class ISocketTransport {
public:
    virtual ~ISocketTransport() = default;
    virtual Status send(const QByteArray& data) = 0;
    virtual Result<QByteArray> receive(int timeoutMs) = 0;
};
```

约定：

- 网口只通过该接口收发。
- 连接生命周期由业务调度层管理。
- `ExchangeAction::busType == "ETHERNET"` 时才使用。

---

## 9. 配置和计划

### 9.1 `TestConfigManager`

```cpp
class TestConfigManager {
public:
    Result<TestConfig> load(const QString& filePath);
    Status save(const QString& filePath, const TestConfig& config);
    Result<QVector<QString>> validate(const TestConfig& config) const;
};
```

校验项：

- schema 版本。
- 产品信息完整性。
- `stepId` / `testItemId` 唯一。
- `dependsOn` 无环。
- `type` 已注册。
- 超时、重试、并行数合法。
- `HardwareRequirement` 可匹配 HAL 资源。
- `ProtocolProfile` 和 `ExchangeAction` 结构合法。
- `SafetyPolicy` 不超 HAL 能力边界。

### 9.2 `TestPlanBuilder`

```cpp
class TestPlanBuilder {
public:
    Result<TestPlan> build(const TestConfig& config) const;
};
```

职责：

- 过滤禁用项。
- 合并默认超时和重试。
- 固化执行顺序。
- 解析 `protocolProfiles`。
- 抽取 `ExchangeConfig`。
- 生成依赖图和并行组。

### 9.3 `PermissionService`

```cpp
class PermissionService {
public:
    bool hasPermission(const QString& userId, Permission permission) const;
};
```

权限失败直接返回业务错误，不进入算法层或 HAL。

---

## 10. 资源和安全

### 10.1 `IResourceManager`

```cpp
class IResourceManager {
public:
    virtual ~IResourceManager() = default;

    virtual Status init() = 0;
    virtual Result<SystemResource> getSystemResourceInfo() = 0;
    virtual Result<int> maxParallel() const = 0;
    virtual Status tryAcquire(const HardwareRequirement& req, int timeoutMs) = 0;
    virtual Status release(const HardwareRequirement& req) = 0;
};
```

约定：

- `tryAcquire` 申请逻辑资源，失败返回 `ResourceBusy` 或 `ResourceTimeout`。
- `maxParallel == 0` 时由实现按资源和 CPU 推导。
- `occupiedResources` 只记逻辑资源 ID。

### 10.2 `ISafetyGuard`

```cpp
class ISafetyGuard {
public:
    virtual ~ISafetyGuard() = default;

    virtual Status loadPolicy(const SafetyPolicy& policy) = 0;
    virtual Result<double> clampDaVoltage(const QString& resourceId, double voltage) = 0;
    virtual Result<bool> isDoSwitchAllowed(const QString& resourceId, qint64 elapsedMs) = 0;
    virtual Result<bool> isCanSendAllowed(int hz) = 0;
    virtual Status lockResource(const QString& key, int timeoutMs) = 0;
    virtual Status unlockResource(const QString& key) = 0;
    virtual Status safeShutdown() = 0;
};
```

约定：

- `loadPolicy` 先于任何输出类动作。
- `safeShutdown` 进入配置的安全态。
- `clampDaVoltage` 返回钳位后数值。

---

## 11. 日志和报告

- 业务层只生产 `logProduced(const LogEvent&)`。
- `LogEvent` 字段和 `HalLogEvent` 映射规则按 `log-interface-protocol.md`。
- 业务层 `source` 建议使用 `flow`、`algorithm`、`system`。
- 每个步骤日志必须带同一个 `requestId`。
- `SchedulerAPI`、`ILogger`、`logGenerated` 不作为主契约。

报告：

- 报告只读 `TestResult` 和日志摘要。
- `IReportGenerator` 不直接接触 HAL。
- `ReportOptions` 只定义输出和过滤，不定义判定逻辑。

```cpp
class IReportGenerator {
public:
    virtual ~IReportGenerator() = default;
    virtual Result<ReportPath> createReport(const QVector<TestResult>& results,
                                            const ReportOptions& opt) = 0;
};
```

---

## 12. 运行流程

### 12.1 加载和启动

```text
UI 选择 .testcfg
  -> TestRunService.loadConfiguration
  -> TestConfigManager.load / validate
  -> TestPlanBuilder.build
  -> UI 展示计划

UI 点击开始
  -> PermissionService 检查权限
  -> TestRunService.startTest
  -> TestRunService 创建 TestContext
  -> IHalService.initialize(halConfig)
  -> scanDevices / openDevice / healthCheck
  -> IResourceManager.tryAcquire
  -> ITestEngine.runTestTask
  -> TestFlowEngine.run
  -> 结果和日志回传
```

### 12.2 暂停和恢复

```text
pauseTest()
  -> RunControl = Pause
  -> MeasurementBase::checkpoint 阻塞等待
  -> 硬件资源保持占用

resumeTest()
  -> RunControl = Run
  -> 继续执行
```

### 12.3 停止和关闭

```text
stopTest()
  -> RunControl = Stop
  -> 当前步骤到安全点后返回
  -> cleanup()
  -> HAL 输出进入 safeState
  -> 关闭设备和释放资源

shutdown()
  -> 停止任务并等待空闲
  -> safeShutdown()
  -> release()
  -> 状态切回 Uninitialized
```

---

## 13. 错误映射

业务层不向 UI 暴露 `HalStatusCode`，只输出 `ErrorCode` 和 `LogEvent`。

| HAL 结果 | 业务结果 |
| --- | --- |
| `InvalidState` | `InvalidState` |
| `NotInitialized` | `NotInitialized` |
| `NotFound` | `ItemNotFound` / `ChannelOccupied` / `ResourceBusy`，按上下文定 |
| `Busy` | `ResourceBusy` |
| `Timeout` | `ResourceTimeout` / `BusTimeout`，按上下文定 |
| `NotSupported` | `CapabilityUnsupported` |
| `Cancelled` | `Cancelled` |
| `SafetyLimitExceeded` | `ParameterRangeError` 或 `FatalHardwareError` |
| `IoError` / `ProtocolError` / `AdapterError` / `DeviceDisconnected` | `FatalHardwareError` / `SampleFail` / `RemoteCommandError`，按阶段定 |

原则：

- 配置和参数错误优先归 `ParameterRangeError`。
- 资源占用错误优先归 `ResourceBusy` / `ResourceTimeout`。
- 通信类错误优先归 `BusTimeout` / `RemoteCommandError` / `ProtocolParseError`。
- 无法恢复的设备故障归 `FatalHardwareError`。

---

## 14. 并发和线程安全

1. `TestRunService` 的同步 API 需加互斥锁。
2. 信号在解锁后发出。
3. `ITestEngine` 和 `IResourceManager` 线程安全，且带超时。
4. `shutdown` 等待工作线程退出后再释放对象。
5. 暂停自动释放由 `RuntimeConfig::pauseAutoReleaseMs` 控制，`> 0` 时超时自动转 `Stop`。
6. HAL 资源互斥由 HAL 层保证，业务层只做逻辑资源调度。

---

## 15. 扩展原则

- 新测试项：继承 `MeasurementBase`，注册到 `MeasurementFactory`。
- 新协议：新增或内联 `ProtocolProfile`，优先不改 C++。
- 新板卡：优先新增或替换 Adapter，业务层少改。
- 新总线：先补 HAL 和 Adapter，再接入业务层资源模型。
- 新远程测试：优先复用 `EXCHANGE`，只扩展 `stimulus` / `acquisition`。

---

## 16. 验收标准

- UI 只依赖 `TestRunService` / `SchedulerAPI`。
- 算法层不含厂家 SDK 头文件。
- 业务层不直接依赖 Adapter。
- `logProduced` 是唯一主日志信号。
- `SchedulerAPI`、`TestEngine`、`ILogger` 只作兼容名。
- `EXCHANGE`、`ProtocolProfile`、`RunControl`、`MeasurementBase` 行为一致。
- Mock 环境可跑通配置、计划、执行、报告闭环。
