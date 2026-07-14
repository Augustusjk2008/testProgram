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

void registerBizMetaTypes()
{
    qRegisterMetaType<ErrorCode>("hwtest::biz::ErrorCode");
    qRegisterMetaType<TestState>("hwtest::biz::TestState");
    qRegisterMetaType<TestVerdict>("hwtest::biz::TestVerdict");
    qRegisterMetaType<TestResult>("hwtest::biz::TestResult");
    qRegisterMetaType<QVector<TestResult>>("QVector<hwtest::biz::TestResult>");
    qRegisterMetaType<SystemResource>("hwtest::biz::SystemResource");
    qRegisterMetaType<RawSample>("hwtest::biz::RawSample");
}

} // namespace hwtest::biz
