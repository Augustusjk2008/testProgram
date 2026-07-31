#pragma once

// 符号导出/导入宏
#include "biz_global.h"

// Qt 容器和基础类型
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

namespace hwtest::biz {

// --------------------- 类型别名 ---------------------
using TaskId     = QString;  // 测试任务ID
using PlanId     = QString;  // 测试计划ID
using StepId     = QString;  // 步骤ID
using TestItemId = QString;  // 测试项ID
using ConfigPath = QString;  // 配置文件路径
using ReportPath = QString;  // 报告路径
using RequestId  = QString;  // 请求ID
using UserId     = QString;  // 用户ID
using StationId  = QString;  // 站点ID

// --------------------- 枚举定义 ---------------------

// 测试运行状态
enum class TestState {
    Uninitialized = 0,  // 未初始化
    Idle,               // 空闲
    Running,            // 运行中
    Paused,             // 已暂停
    Stopping,           // 正在停止
    Finished,           // 已完成
    Error               // 错误
};

// 错误码枚举
enum class ErrorCode {
    Ok = 0,
    InvalidState,         // 无效状态
    NotInitialized,       // 未初始化
    ConfigNotLoaded,      // 配置未加载
    ConfigParseError,     // 配置解析错误
    ConfigSchemaError,    // 配置结构错误
    ItemNotFound,         // 项未找到
    DependencyCycle,      // 循环依赖
    ParameterRangeError,  // 参数范围错误
    PermissionDenied,     // 权限不足
    ResourceBusy,         // 资源忙
    ResourceTimeout,      // 资源超时
    CapabilityUnsupported,// 能力不支持
    BusTimeout,           // 总线超时
    ChannelOccupied,      // 通道被占用
    DriverMissing,        // 驱动缺失
    SampleFail,           // 采样失败
    RemoteCommandError,   // 远程命令错误
    ProtocolParseError,   // 协议解析错误
    DiskFull,             // 磁盘满
    Cancelled,            // 已取消
    FatalHardwareError,   // 致命硬件错误
    InternalError         // 内部错误
};

// --------------------- 基础数据结构 ---------------------

// 错误信息
struct HWTEST_BIZ_EXPORT ErrorInfo {
    ErrorCode   code       = ErrorCode::Ok;
    QString     message;          // 错误消息
    QString     operation;        // 出错操作名
    TestItemId  testItemId;       // 关联的测试项ID
    QVariantMap detail;           // 详细信息
};

// 通用状态封装，用于函数返回
struct HWTEST_BIZ_EXPORT Status {
    ErrorCode code  = ErrorCode::Ok;
    ErrorInfo error;              // 当 code != Ok 时有效

    bool ok() const { return code == ErrorCode::Ok; }
};

// 泛型结果封装（含状态和返回值）
template <typename T>
struct Result {
    Status status;
    T      value{};

    bool ok() const { return status.ok(); }
};

// --------------------- 运行控制相关 ---------------------

// 运行控制命令
enum class RunControl {
    Run = 0,   // 运行
    Pause,     // 暂停
    Stop       // 停止
};

// 运行模式
enum class RunMode {
    Single = 0,    // 单次运行
    PcPeriodic,    // PC端周期性触发
    DeviceStream   // 设备端流式触发
};

// 运行选项
struct HWTEST_BIZ_EXPORT RunOptions {
    RunMode    mode        = RunMode::Single;
    int        intervalMs  = 1000;       // 周期间隔(毫秒)
    quint64    maxCycles   = 1;          // 最大循环次数
    QVariantMap parameters;              // 算法相关参数(透传，BIZ不解析)
};

// --------------------- 测试判定相关 ---------------------

// 测试结论
enum class TestVerdict {
    Pass = 0,   // 通过
    Fail,       // 失败
    Error,      // 错误
    Skipped     // 跳过
};

// 跳过原因
enum class SkipReason {
    None = 0,
    DependencyFailed,  // 依赖项失败
    Disabled,          // 已禁用
    ResourceBusy,      // 资源忙
    SetupFailed,       // 初始化失败
    Cancelled          // 已取消
};

// 权限类型
enum class Permission {
    LoadConfig,      // 加载配置
    EditConfig,      // 编辑配置
    StartTest,       // 启动测试
    StopTest,        // 停止测试
    ExportReport,    // 导出报告
    ManageHardware   // 管理硬件
};

// --------------------- 测量/判定相关结构 ---------------------

// 比较运算符
enum class CmpOp {
    GreaterThan,      // >
    GreaterOrEqual,   // >=
    LessThan,         // <
    LessOrEqual,      // <=
    Equal,            // ==
    NotEqual,         // !=
    InRange           // 在范围内
};

// 单条测量记录
struct HWTEST_BIZ_EXPORT MeasurementRecord {
    QString     name;        // 测量项名称
    QVariant    expected;    // 期望值
    QVariant    actual;      // 实际值
    QVariant    tolerance;   // 容差
    QString     unit;        // 单位
    QVariantMap metadata;    // 附加信息
};

// 判定准则
struct HWTEST_BIZ_EXPORT Criterion {
    QString metric;             // 指标名
    CmpOp   op       = CmpOp::GreaterThan;
    QVariant ref     = 0.0;    // 参考值
    double  lo       = 0.0;    // 下限(用于 InRange)
    double  hi       = 0.0;    // 上限(用于 InRange)
    double  tol      = 0.0;    // 容差
    bool    passIfMatched = true; // true=匹配则通过, false=匹配则失败
};

// --------------------- 总线交互相关结构 ---------------------

// 总线交互动作
struct HWTEST_BIZ_EXPORT ExchangeAction {
    QString     source;              // 数据源
    QString     busType;             // 总线类型
    QString     channelId;           // 通道ID
    QString     operation;           // 操作
    QVariantMap options;             // 操作选项
    QString     protocolProfileId;   // 协议模板ID
    QVariantMap protocolProfile;     // 协议模板内容(内联覆盖)
};

// 总线交互配置（一对激励/采集）
struct HWTEST_BIZ_EXPORT ExchangeConfig {
    ExchangeAction stimulus;           // 激励动作
    ExchangeAction acquisition;        // 采集动作
    int             settlingTimeMs = 0;// 稳定等待时间(毫秒)
    QList<Criterion> criteria;         // 判定准则列表
};

// 协议模板
struct HWTEST_BIZ_EXPORT ProtocolProfile {
    QString     id;              // 模板ID
    QString     busType;         // 总线类型
    QString     payloadEncoding; // 负载编码方式
    QVariantMap frameFormat;     // 帧格式
    QVariantMap timing;          // 时序参数
    QVariantMap responseRules;   // 响应解析规则
    QVariantMap fieldMapping;    // 字段映射
};

// --------------------- 测试步骤与配置 ---------------------

// 测试步骤定义
struct HWTEST_BIZ_EXPORT TestStep {
    StepId          stepId;                    // 步骤ID
    TestItemId      testItemId;                // 所属测试项ID
    QString         name;                      // 步骤名称
    QString         type      = QStringLiteral("EXCHANGE"); // 步骤类型
    QString         board;                     // 板卡标识
    QString         algorithmId;               // 算法ID
    QVariantMap     parameters;                // 步骤参数
    int             timeoutMs = 1000;          // 超时(毫秒)
    int             retryCount = 0;            // 重试次数
    bool            enabled   = true;          // 是否启用
    QList<StepId>   dependsOn;                 // 依赖步骤ID列表
    QList<Criterion> criteria;                 // 判定准则
};

// 硬件需求描述
struct HWTEST_BIZ_EXPORT HardwareRequirement {
    QString        requirementId;      // 需求ID
    QString        deviceId;           // 设备ID
    QString        adapterId;          // 适配器ID
    QList<QString> requiredModules;    // 需要的模块
    QList<QString> requiredResources;  // 需要的资源
    int            priority = 0;       // 优先级
    QVariantMap    match;              // 匹配条件
};

// 安全策略
struct HWTEST_BIZ_EXPORT SafetyPolicy {
    QVariantMap outputLimits;        // 输出限制
    QVariantMap safeState;           // 安全状态值
    // 以下为兼容字段，运行时保留但不做安全分支判断
    bool   enterSafeStateOnStop  = true;
    bool   enterSafeStateOnError = true;
    double daMinVoltage          = 0.0;
    double daMaxVoltage          = 0.0;
    int    doMinSwitchIntervalMs = 0;
    int    canSendMaxHz          = 0;
    int    resourceLockTimeoutMs = 3000;
};

// 运行时配置
struct HWTEST_BIZ_EXPORT RuntimeConfig {
    bool   parallelEnabled   = true;                       // 是否允许并行
    int    maxParallel       = 0;                          // 最大并行数(0=自动)
    int    defaultTimeoutMs  = 1000;                       // 默认超时
    int    defaultRetryCount = 0;                          // 默认重试次数
    int    retryIntervalMs   = 50;                         // 重试间隔
    int    taskPriorityDefault = 2;                        // 默认任务优先级
    int    pauseAutoReleaseMs = 0;                         // 暂停自动释放时间
    bool   stopOnFirstFailure = true;                      // 首次失败即停止
    bool   allowResume       = false;                      // 是否允许恢复运行
    QString reportDir;                                     // 报告输出目录
    QString logDir;                                        // 日志目录
    qint64 logRotateBytes    = 10 * 1024 * 1024;           // 日志轮转大小(默认10MB)
    int    logKeepFiles      = 10;                         // 保留日志文件数
    QVariantMap tags;                                      // 标签
};

// 完整测试配置（从配置文件加载）
struct HWTEST_BIZ_EXPORT TestConfig {
    QString                        schemaVersion;         // 配置结构版本
    QString                        configId;              // 配置ID
    QString                        productModel;          // 产品型号
    QString                        productName;           // 产品名称
    QString                        configVersion;         // 配置版本
    QVector<TestStep>              steps;                 // 步骤列表
    QVector<HardwareRequirement>   hardwareRequirements;  // 硬件需求
    QVector<ProtocolProfile>       protocolProfiles;      // 协议模板
    QVariantMap                    executionConfig;       // 执行配置(扩展)
    SafetyPolicy                   safetyPolicy;          // 安全策略
    RuntimeConfig                  runtimeConfig;         // 运行时配置
    QVariantMap                    reportFields;          // 报告字段
};

// 测试计划（运行时的可执行快照）
struct HWTEST_BIZ_EXPORT TestPlan {
    PlanId                      planId;                // 计划ID
    QString                     configId;              // 源配置ID
    QString                     productModel;          // 产品型号
    QString                     configVersion;         // 配置版本
    QVector<TestStep>           steps;                 // 步骤列表
    QVector<HardwareRequirement> hardwareRequirements; // 硬件需求
    QVector<ProtocolProfile>    protocolProfiles;      // 协议模板
    SafetyPolicy                safetyPolicy;          // 安全策略
    RuntimeConfig               runtimeConfig;         // 运行时配置
};

// --------------------- 运行时上下文与数据 ---------------------

// 测试上下文
struct HWTEST_BIZ_EXPORT TestContext {
    TaskId      runId;          // 本次运行ID
    RequestId   requestId;      // 请求ID
    QString     productModel;   // 产品型号
    UserId      operatorId;     // 操作员ID
    StationId   stationId;      // 站点ID
    QVariantMap tags;           // 标签
    QVariantMap runParameters;  // 运行参数
};

// 原始采样数据
struct HWTEST_BIZ_EXPORT RawSample {
    qint64     timestampUs    = 0;     // 时间戳(微秒)
    QString    channelId;             // 通道ID
    QVariantMap values;               // 采样值
    QVariantMap tags;                 // 标签
    quint64    cycleIndex     = 1;    // 周期序号
    qint64     streamElapsedUs = -1;  // 流已运行时间(微秒), -1表示不可用
};

// 系统资源状态
struct HWTEST_BIZ_EXPORT SystemResource {
    double        cpuUsage;            // CPU 使用率
    qint64        memoryUsedMB;        // 已用内存(MB)
    qint64        diskFreeMB;          // 剩余磁盘(MB)
    int           idleThreadNum;       // 空闲线程数
    QList<QString> occupiedResources;  // 被占用的资源列表
};

// 单个步骤的测试结果
struct HWTEST_BIZ_EXPORT TestResult {
    StepId                   stepId;             // 步骤ID
    TestItemId               testItemId;         // 测试项ID
    QString                  algorithmId;        // 算法ID
    TestVerdict              verdict     = TestVerdict::Skipped;
    SkipReason               skipReason  = SkipReason::None;
    ErrorCode                errorCode   = ErrorCode::Ok;
    QString                  message;            // 结果描述
    QVector<MeasurementRecord> measurements;     // 测量记录
    QVariantMap              rawData;            // 原始数据
    int                      attempts    = 1;    // 尝试次数
    qint64                   startTimeUs = 0;    // 开始时间(微秒)
    qint64                   endTimeUs   = 0;    // 结束时间(微秒)
    quint64                  cycleIndex  = 1;    // 周期序号
};

// 报告生成选项
struct HWTEST_BIZ_EXPORT ReportOptions {
    QString        outDir;        // 输出目录
    QString        title;         // 报告标题
    TaskId         taskId;        // 任务ID
    QList<QString> itemFilter;    // 测试项过滤器
    bool           html = true;   // 生成 HTML 报告
    bool           csv  = false;  // 生成 CSV 报告
    bool           txt  = false;  // 生成 TXT 报告
    bool           xml  = false;  // 生成 XML 报告
};

// --------------------- 工具函数 ---------------------

// 枚举转字符串
HWTEST_BIZ_EXPORT QString errorCodeToString(ErrorCode code);
HWTEST_BIZ_EXPORT QString testStateToString(TestState state);
HWTEST_BIZ_EXPORT QString testVerdictToString(TestVerdict verdict);
HWTEST_BIZ_EXPORT QString runModeToString(RunMode mode);

// 从字符串解析运行模式，成功返回 true
HWTEST_BIZ_EXPORT bool runModeFromString(const QString& text, RunMode* mode);

// 注册所有 biz 自定义类型到 Qt 元对象系统（支持信号槽/属性/QVariant）
HWTEST_BIZ_EXPORT void registerBizMetaTypes();

} // namespace hwtest::biz

// 将关键类型注册到 Qt 元类型系统
// 使它们可以在 QVariant、信号槽参数、属性系统中使用
Q_DECLARE_METATYPE(hwtest::biz::ErrorCode)
Q_DECLARE_METATYPE(hwtest::biz::TestState)
Q_DECLARE_METATYPE(hwtest::biz::TestVerdict)
Q_DECLARE_METATYPE(hwtest::biz::RunMode)
Q_DECLARE_METATYPE(hwtest::biz::RunOptions)
Q_DECLARE_METATYPE(hwtest::biz::TestResult)
Q_DECLARE_METATYPE(QVector<hwtest::biz::TestResult>)
Q_DECLARE_METATYPE(hwtest::biz::SystemResource)
Q_DECLARE_METATYPE(hwtest::biz::RawSample)