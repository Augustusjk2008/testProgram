#pragma once

#include "biz_global.h"

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

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

struct HWTEST_BIZ_EXPORT ErrorInfo {
    ErrorCode code = ErrorCode::Ok;
    QString message;
    QString operation;
    TestItemId testItemId;
    QVariantMap detail;
};

struct HWTEST_BIZ_EXPORT Status {
    ErrorCode code = ErrorCode::Ok;
    ErrorInfo error;

    bool ok() const { return code == ErrorCode::Ok; }
};

template <typename T>
struct Result {
    Status status;
    T value{};

    bool ok() const { return status.ok(); }
};

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

struct HWTEST_BIZ_EXPORT MeasurementRecord {
    QString name;
    QVariant expected;
    QVariant actual;
    QVariant tolerance;
    QString unit;
    QVariantMap metadata;
};

struct HWTEST_BIZ_EXPORT Criterion {
    QString metric;
    CmpOp op = CmpOp::GreaterThan;
    QVariant ref = 0.0;
    double lo = 0.0;
    double hi = 0.0;
    double tol = 0.0;
    bool passIfMatched = true;
};

struct HWTEST_BIZ_EXPORT ExchangeAction {
    QString source;
    QString busType;
    QString channelId;
    QString operation;
    QVariantMap options;
    QString protocolProfileId;
    QVariantMap protocolProfile;
};

struct HWTEST_BIZ_EXPORT ExchangeConfig {
    ExchangeAction stimulus;
    ExchangeAction acquisition;
    int settlingTimeMs = 0;
    QList<Criterion> criteria;
};

struct HWTEST_BIZ_EXPORT ProtocolProfile {
    QString id;
    QString busType;
    QString payloadEncoding;
    QVariantMap frameFormat;
    QVariantMap timing;
    QVariantMap responseRules;
    QVariantMap fieldMapping;
};

struct HWTEST_BIZ_EXPORT TestStep {
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
    QList<StepId> dependsOn;
    QList<Criterion> criteria;
};

struct HWTEST_BIZ_EXPORT HardwareRequirement {
    QString requirementId;
    QString deviceId;
    QString adapterId;
    QList<QString> requiredModules;
    QList<QString> requiredResources;
    int priority = 0;
    QVariantMap match;
};

struct HWTEST_BIZ_EXPORT SafetyPolicy {
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

struct HWTEST_BIZ_EXPORT RuntimeConfig {
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

struct HWTEST_BIZ_EXPORT TestConfig {
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

struct HWTEST_BIZ_EXPORT TestPlan {
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

struct HWTEST_BIZ_EXPORT TestContext {
    TaskId runId;
    RequestId requestId;
    QString productModel;
    UserId operatorId;
    StationId stationId;
    QVariantMap tags;
};

struct HWTEST_BIZ_EXPORT RawSample {
    qint64 timestampUs = 0;
    QString channelId;
    QVariantMap values;
    QVariantMap tags;
};

struct HWTEST_BIZ_EXPORT SystemResource {
    double cpuUsage = 0.0;
    qint64 memoryUsedMB = 0;
    qint64 diskFreeMB = 0;
    int idleThreadNum = 0;
    QList<QString> occupiedResources;
};

struct HWTEST_BIZ_EXPORT TestResult {
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

struct HWTEST_BIZ_EXPORT ReportOptions {
    QString outDir;
    QString title;
    TaskId taskId;
    QList<QString> itemFilter;
    bool html = true;
    bool csv = false;
    bool txt = false;
    bool xml = false;
};

HWTEST_BIZ_EXPORT QString errorCodeToString(ErrorCode code);
HWTEST_BIZ_EXPORT QString testStateToString(TestState state);
HWTEST_BIZ_EXPORT QString testVerdictToString(TestVerdict verdict);
HWTEST_BIZ_EXPORT void registerBizMetaTypes();

} // namespace hwtest::biz

Q_DECLARE_METATYPE(hwtest::biz::ErrorCode)
Q_DECLARE_METATYPE(hwtest::biz::TestState)
Q_DECLARE_METATYPE(hwtest::biz::TestVerdict)
Q_DECLARE_METATYPE(hwtest::biz::TestResult)
Q_DECLARE_METATYPE(QVector<hwtest::biz::TestResult>)
Q_DECLARE_METATYPE(hwtest::biz::SystemResource)
Q_DECLARE_METATYPE(hwtest::biz::RawSample)
