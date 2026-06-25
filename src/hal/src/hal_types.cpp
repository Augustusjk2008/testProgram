#include "hal/hal_types.h"

#include <QMetaType>

namespace hwtest::hal {

void registerHalMetaTypes()
{
    qRegisterMetaType<HalStatusCode>("hwtest::hal::HalStatusCode");
    qRegisterMetaType<HalError>("hwtest::hal::HalError");
    qRegisterMetaType<HalStatus>("hwtest::hal::HalStatus");
    qRegisterMetaType<OperationOptions>("hwtest::hal::OperationOptions");
    qRegisterMetaType<HalLogEvent>("hwtest::hal::HalLogEvent");
    qRegisterMetaType<DeviceDescriptor>("hwtest::hal::DeviceDescriptor");
    qRegisterMetaType<ChannelDescriptor>("hwtest::hal::ChannelDescriptor");
    qRegisterMetaType<DeviceCapabilities>("hwtest::hal::DeviceCapabilities");
    qRegisterMetaType<AnalogUnit>("hwtest::hal::AnalogUnit");
    qRegisterMetaType<AnalogRange>("hwtest::hal::AnalogRange");
    qRegisterMetaType<AnalogSample>("hwtest::hal::AnalogSample");
    qRegisterMetaType<AnalogReadOptions>("hwtest::hal::AnalogReadOptions");
    qRegisterMetaType<AnalogWriteOptions>("hwtest::hal::AnalogWriteOptions");
    qRegisterMetaType<DigitalLevel>("hwtest::hal::DigitalLevel");
    qRegisterMetaType<DigitalSample>("hwtest::hal::DigitalSample");
    qRegisterMetaType<DigitalWriteOptions>("hwtest::hal::DigitalWriteOptions");
    qRegisterMetaType<SerialParity>("hwtest::hal::SerialParity");
    qRegisterMetaType<SerialStopBits>("hwtest::hal::SerialStopBits");
    qRegisterMetaType<SerialFlowControl>("hwtest::hal::SerialFlowControl");
    qRegisterMetaType<SerialConfig>("hwtest::hal::SerialConfig");
    qRegisterMetaType<SerialTransaction>("hwtest::hal::SerialTransaction");
    qRegisterMetaType<SerialTransactionResult>("hwtest::hal::SerialTransactionResult");
    qRegisterMetaType<CanFdConfig>("hwtest::hal::CanFdConfig");
    qRegisterMetaType<CanFdFrame>("hwtest::hal::CanFdFrame");
    qRegisterMetaType<CanFdFilter>("hwtest::hal::CanFdFilter");
    qRegisterMetaType<QVector<DeviceDescriptor>>("QVector<hwtest::hal::DeviceDescriptor>");
    qRegisterMetaType<QVector<AnalogSample>>("QVector<hwtest::hal::AnalogSample>");
    qRegisterMetaType<QVector<DigitalSample>>("QVector<hwtest::hal::DigitalSample>");
    qRegisterMetaType<QVector<CanFdFrame>>("QVector<hwtest::hal::CanFdFrame>");
}

static QString toStringEnum(int value,
                            const char* const* names,
                            int count,
                            const char* fallback)
{
    if (value >= 0 && value < count) {
        return QString::fromLatin1(names[value]);
    }
    return QString::fromLatin1(fallback);
}

QString toString(HalStatusCode code)
{
    switch (code) {
    case HalStatusCode::Ok: return QStringLiteral("Ok");
    case HalStatusCode::InvalidArgument: return QStringLiteral("InvalidArgument");
    case HalStatusCode::InvalidState: return QStringLiteral("InvalidState");
    case HalStatusCode::NotInitialized: return QStringLiteral("NotInitialized");
    case HalStatusCode::NotFound: return QStringLiteral("NotFound");
    case HalStatusCode::NotSupported: return QStringLiteral("NotSupported");
    case HalStatusCode::PermissionDenied: return QStringLiteral("PermissionDenied");
    case HalStatusCode::Busy: return QStringLiteral("Busy");
    case HalStatusCode::Timeout: return QStringLiteral("Timeout");
    case HalStatusCode::Cancelled: return QStringLiteral("Cancelled");
    case HalStatusCode::SafetyLimitExceeded: return QStringLiteral("SafetyLimitExceeded");
    case HalStatusCode::DeviceDisconnected: return QStringLiteral("DeviceDisconnected");
    case HalStatusCode::AdapterLoadFailed: return QStringLiteral("AdapterLoadFailed");
    case HalStatusCode::AdapterSymbolMissing: return QStringLiteral("AdapterSymbolMissing");
    case HalStatusCode::AdapterError: return QStringLiteral("AdapterError");
    case HalStatusCode::IoError: return QStringLiteral("IoError");
    case HalStatusCode::ProtocolError: return QStringLiteral("ProtocolError");
    case HalStatusCode::CrcMismatch: return QStringLiteral("CrcMismatch");
    case HalStatusCode::DataMismatch: return QStringLiteral("DataMismatch");
    case HalStatusCode::BufferTooSmall: return QStringLiteral("BufferTooSmall");
    case HalStatusCode::InternalError: return QStringLiteral("InternalError");
    }
    return QStringLiteral("Unknown");
}

QString toString(AnalogUnit unit)
{
    switch (unit) {
    case AnalogUnit::Volt: return QStringLiteral("Volt");
    case AnalogUnit::MilliVolt: return QStringLiteral("MilliVolt");
    case AnalogUnit::Ampere: return QStringLiteral("Ampere");
    case AnalogUnit::MilliAmpere: return QStringLiteral("MilliAmpere");
    case AnalogUnit::RawCount: return QStringLiteral("RawCount");
    }
    return QStringLiteral("Unknown");
}

QString toString(DigitalLevel level)
{
    switch (level) {
    case DigitalLevel::Low: return QStringLiteral("Low");
    case DigitalLevel::High: return QStringLiteral("High");
    case DigitalLevel::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

QString toString(SerialParity parity)
{
    switch (parity) {
    case SerialParity::None: return QStringLiteral("None");
    case SerialParity::Odd: return QStringLiteral("Odd");
    case SerialParity::Even: return QStringLiteral("Even");
    case SerialParity::Mark: return QStringLiteral("Mark");
    case SerialParity::Space: return QStringLiteral("Space");
    }
    return QStringLiteral("Unknown");
}

QString toString(SerialStopBits stopBits)
{
    switch (stopBits) {
    case SerialStopBits::One: return QStringLiteral("One");
    case SerialStopBits::OneAndHalf: return QStringLiteral("OneAndHalf");
    case SerialStopBits::Two: return QStringLiteral("Two");
    }
    return QStringLiteral("Unknown");
}

QString toString(SerialFlowControl flowControl)
{
    switch (flowControl) {
    case SerialFlowControl::None: return QStringLiteral("None");
    case SerialFlowControl::Hardware: return QStringLiteral("Hardware");
    case SerialFlowControl::Software: return QStringLiteral("Software");
    }
    return QStringLiteral("Unknown");
}

} // namespace hwtest::hal
