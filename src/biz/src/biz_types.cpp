#include "biz/biz_types.h"

#include <QMetaType>

namespace hwtest::biz {

QString errorCodeToString(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Ok: return QStringLiteral("Ok");
    case ErrorCode::InvalidState: return QStringLiteral("InvalidState");
    case ErrorCode::NotInitialized: return QStringLiteral("NotInitialized");
    case ErrorCode::ConfigNotLoaded: return QStringLiteral("ConfigNotLoaded");
    case ErrorCode::ConfigParseError: return QStringLiteral("ConfigParseError");
    case ErrorCode::ConfigSchemaError: return QStringLiteral("ConfigSchemaError");
    case ErrorCode::ItemNotFound: return QStringLiteral("ItemNotFound");
    case ErrorCode::DependencyCycle: return QStringLiteral("DependencyCycle");
    case ErrorCode::ParameterRangeError: return QStringLiteral("ParameterRangeError");
    case ErrorCode::PermissionDenied: return QStringLiteral("PermissionDenied");
    case ErrorCode::ResourceBusy: return QStringLiteral("ResourceBusy");
    case ErrorCode::ResourceTimeout: return QStringLiteral("ResourceTimeout");
    case ErrorCode::CapabilityUnsupported: return QStringLiteral("CapabilityUnsupported");
    case ErrorCode::BusTimeout: return QStringLiteral("BusTimeout");
    case ErrorCode::ChannelOccupied: return QStringLiteral("ChannelOccupied");
    case ErrorCode::DriverMissing: return QStringLiteral("DriverMissing");
    case ErrorCode::SampleFail: return QStringLiteral("SampleFail");
    case ErrorCode::RemoteCommandError: return QStringLiteral("RemoteCommandError");
    case ErrorCode::ProtocolParseError: return QStringLiteral("ProtocolParseError");
    case ErrorCode::DiskFull: return QStringLiteral("DiskFull");
    case ErrorCode::Cancelled: return QStringLiteral("Cancelled");
    case ErrorCode::FatalHardwareError: return QStringLiteral("FatalHardwareError");
    case ErrorCode::InternalError: return QStringLiteral("InternalError");
    }
    return QStringLiteral("InternalError");
}

QString testStateToString(TestState state)
{
    switch (state) {
    case TestState::Uninitialized: return QStringLiteral("Uninitialized");
    case TestState::Idle: return QStringLiteral("Idle");
    case TestState::Running: return QStringLiteral("Running");
    case TestState::Paused: return QStringLiteral("Paused");
    case TestState::Stopping: return QStringLiteral("Stopping");
    case TestState::Finished: return QStringLiteral("Finished");
    case TestState::Error: return QStringLiteral("Error");
    }
    return QStringLiteral("Error");
}

QString testVerdictToString(TestVerdict verdict)
{
    switch (verdict) {
    case TestVerdict::Pass: return QStringLiteral("Pass");
    case TestVerdict::Fail: return QStringLiteral("Fail");
    case TestVerdict::Error: return QStringLiteral("Error");
    case TestVerdict::Skipped: return QStringLiteral("Skipped");
    }
    return QStringLiteral("Error");
}

QString runModeToString(RunMode mode)
{
    switch (mode) {
    case RunMode::Single: return QStringLiteral("single");
    case RunMode::PcPeriodic: return QStringLiteral("pc_periodic");
    case RunMode::DeviceStream: return QStringLiteral("device_stream");
    }
    return QStringLiteral("single");
}

bool runModeFromString(const QString& text, RunMode* mode)
{
    if (mode == nullptr) {
        return false;
    }
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("single")) {
        *mode = RunMode::Single;
        return true;
    }
    if (normalized == QStringLiteral("pc_periodic")) {
        *mode = RunMode::PcPeriodic;
        return true;
    }
    if (normalized == QStringLiteral("device_stream")) {
        *mode = RunMode::DeviceStream;
        return true;
    }
    return false;
}

void registerBizMetaTypes()
{
    // MOC records aliases used in ITestRunService signal signatures by their
    // unqualified spelling. Register both spellings for queued UI delivery.
    qRegisterMetaType<TaskId>("TaskId");
    qRegisterMetaType<StepId>("StepId");
    qRegisterMetaType<TestItemId>("TestItemId");
    qRegisterMetaType<ErrorCode>("ErrorCode");
    qRegisterMetaType<TestState>("TestState");
    qRegisterMetaType<TestResult>("TestResult");
    qRegisterMetaType<ErrorCode>("hwtest::biz::ErrorCode");
    qRegisterMetaType<TestState>("hwtest::biz::TestState");
    qRegisterMetaType<TestVerdict>("hwtest::biz::TestVerdict");
    qRegisterMetaType<RunMode>("hwtest::biz::RunMode");
    qRegisterMetaType<RunOptions>("hwtest::biz::RunOptions");
    qRegisterMetaType<TestResult>("hwtest::biz::TestResult");
    qRegisterMetaType<QVector<TestResult>>("QVector<hwtest::biz::TestResult>");
    qRegisterMetaType<SystemResource>("hwtest::biz::SystemResource");
    qRegisterMetaType<RawSample>("RawSample");
    qRegisterMetaType<RawSample>("hwtest::biz::RawSample");
}

} // namespace hwtest::biz
