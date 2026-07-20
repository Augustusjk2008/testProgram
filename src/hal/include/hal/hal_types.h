#pragma once

#include "hal_global.h"

#include <QByteArray>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace hwtest::hal {

using DeviceId = QString;
using AdapterId = QString;
using ResourceId = QString;
using RequestId = QString;
using SessionId = QString;

enum class HalStatusCode {
    Ok = 0,
    InvalidArgument,
    InvalidState,
    NotInitialized,
    NotFound,
    NotSupported,
    PermissionDenied,
    Busy,
    Timeout,
    Cancelled,
    SafetyLimitExceeded,
    DeviceDisconnected,
    AdapterLoadFailed,
    AdapterSymbolMissing,
    AdapterError,
    IoError,
    ProtocolError,
    CrcMismatch,
    DataMismatch,
    BufferTooSmall,
    InternalError
};

struct HalError {
    HalStatusCode code = HalStatusCode::Ok;
    QString message;
    QString adapterCode;
    QString deviceId;
    QString resourceId;
    QString operation;
    QVariantMap detail;
};

struct HalStatus {
    HalStatusCode code = HalStatusCode::Ok;
    HalError error;

    bool ok() const noexcept
    {
        return code == HalStatusCode::Ok;
    }
};

template <typename T>
struct HalResult {
    HalStatus status;
    T value {};

    bool ok() const noexcept
    {
        return status.ok();
    }
};

struct OperationOptions {
    int timeoutMs = 1000;
    int retryCount = 0;
    int retryIntervalMs = 50;
    RequestId requestId;
    QVariantMap tags;
};

struct HalLogEvent {
    qint64 timestampUs = 0;
    QString level;
    QString source;
    QString category;
    QString message;
    RequestId requestId;
    qint64 durationMs = -1;
    QString status;
    QString adapterCode;
    DeviceId deviceId;
    ResourceId resourceId;
    QString operation;
    QVariantMap context;
};

struct DeviceDescriptor {
    DeviceId deviceId;
    AdapterId adapterId;
    QString vendor;
    QString model;
    QString serialNumber;
    QString location;
    QString firmwareVersion;
    QVariantMap properties;
};

struct ChannelDescriptor {
    ResourceId resourceId;
    QString module;
    QString direction;
    int physicalIndex = -1;
    QVariantMap properties;
};

struct DeviceCapabilities {
    DeviceDescriptor device;
    QVector<ChannelDescriptor> channels;
    QStringList supportedModules;
    QVariantMap limits;
};

enum class AnalogUnit {
    Volt,
    MilliVolt,
    Ampere,
    MilliAmpere,
    RawCount
};

struct AnalogRange {
    double minValue = 0.0;
    double maxValue = 0.0;
    AnalogUnit unit = AnalogUnit::Volt;
};

struct AnalogSample {
    ResourceId channel;
    double value = 0.0;
    AnalogUnit unit = AnalogUnit::Volt;
    qint64 timestampUs = 0;
    QVariantMap metadata;
};

struct AnalogReadOptions {
    OperationOptions op;
    AnalogRange range;
    int sampleCount = 1;
    int sampleRateHz = 0;
    bool returnRaw = false;
};

struct AnalogWriteOptions {
    OperationOptions op;
    AnalogRange range;
    bool safeClamp = true;
};

enum class DigitalLevel {
    Low = 0,
    High = 1,
    Unknown = 2
};

struct DigitalSample {
    ResourceId channel;
    DigitalLevel level = DigitalLevel::Unknown;
    qint64 timestampUs = 0;
    QVariantMap metadata;
};

struct DigitalWriteOptions {
    OperationOptions op;
    bool verifyAfterWrite = true;
};

enum class SerialParity { None, Odd, Even, Mark, Space };
enum class SerialStopBits { One, OneAndHalf, Two };
enum class SerialFlowControl { None, Hardware, Software };

struct SerialConfig {
    int baudRate = 115200;
    int dataBits = 8;
    SerialParity parity = SerialParity::None;
    SerialStopBits stopBits = SerialStopBits::One;
    SerialFlowControl flowControl = SerialFlowControl::None;
    QVariantMap vendorOptions;
};

struct SerialPortDescriptor {
    QString portName;
    QString description;
    QString manufacturer;
    QString serialNumber;
    QString systemLocation;
};

struct SerialTransaction {
    QByteArray tx;
    QByteArray expectedPrefix;
    int readMinBytes = 0;
    int readMaxBytes = 4096;
    QByteArray terminator;
    OperationOptions op;
};

struct SerialTransactionResult {
    QByteArray rx;
    qint64 txTimestampUs = 0;
    qint64 rxTimestampUs = 0;
    QVariantMap metadata;
};

struct CanFdConfig {
    int nominalBitrate = 500000;
    int dataBitrate = 2000000;
    bool fdEnabled = true;
    bool bitrateSwitch = true;
    bool loopback = false;
    QVariantMap vendorOptions;
};

struct CanFdFrame {
    quint32 id = 0;
    bool extendedId = false;
    bool fd = true;
    bool bitrateSwitch = true;
    bool remoteRequest = false;
    QByteArray payload;
    qint64 timestampUs = 0;
    QVariantMap metadata;
};

struct CanFdFilter {
    quint32 id = 0;
    quint32 mask = 0xFFFFFFFFu;
    bool extendedId = false;
};

HWTEST_HAL_EXPORT void registerHalMetaTypes();
HWTEST_HAL_EXPORT QString toString(HalStatusCode code);
HWTEST_HAL_EXPORT QString toString(AnalogUnit unit);
HWTEST_HAL_EXPORT QString toString(DigitalLevel level);
HWTEST_HAL_EXPORT QString toString(SerialParity parity);
HWTEST_HAL_EXPORT QString toString(SerialStopBits stopBits);
HWTEST_HAL_EXPORT QString toString(SerialFlowControl flowControl);

} // namespace hwtest::hal

Q_DECLARE_METATYPE(hwtest::hal::HalStatusCode)
Q_DECLARE_METATYPE(hwtest::hal::HalError)
Q_DECLARE_METATYPE(hwtest::hal::HalStatus)
Q_DECLARE_METATYPE(hwtest::hal::OperationOptions)
Q_DECLARE_METATYPE(hwtest::hal::HalLogEvent)
Q_DECLARE_METATYPE(hwtest::hal::DeviceDescriptor)
Q_DECLARE_METATYPE(hwtest::hal::ChannelDescriptor)
Q_DECLARE_METATYPE(hwtest::hal::DeviceCapabilities)
Q_DECLARE_METATYPE(hwtest::hal::AnalogUnit)
Q_DECLARE_METATYPE(hwtest::hal::AnalogRange)
Q_DECLARE_METATYPE(hwtest::hal::AnalogSample)
Q_DECLARE_METATYPE(hwtest::hal::AnalogReadOptions)
Q_DECLARE_METATYPE(hwtest::hal::AnalogWriteOptions)
Q_DECLARE_METATYPE(hwtest::hal::DigitalLevel)
Q_DECLARE_METATYPE(hwtest::hal::DigitalSample)
Q_DECLARE_METATYPE(hwtest::hal::DigitalWriteOptions)
Q_DECLARE_METATYPE(hwtest::hal::SerialParity)
Q_DECLARE_METATYPE(hwtest::hal::SerialStopBits)
Q_DECLARE_METATYPE(hwtest::hal::SerialFlowControl)
Q_DECLARE_METATYPE(hwtest::hal::SerialConfig)
Q_DECLARE_METATYPE(hwtest::hal::SerialPortDescriptor)
Q_DECLARE_METATYPE(hwtest::hal::SerialTransaction)
Q_DECLARE_METATYPE(hwtest::hal::SerialTransactionResult)
Q_DECLARE_METATYPE(hwtest::hal::CanFdConfig)
Q_DECLARE_METATYPE(hwtest::hal::CanFdFrame)
Q_DECLARE_METATYPE(hwtest::hal::CanFdFilter)
Q_DECLARE_METATYPE(QVector<hwtest::hal::DeviceDescriptor>)
Q_DECLARE_METATYPE(QVector<hwtest::hal::SerialPortDescriptor>)
Q_DECLARE_METATYPE(QVector<hwtest::hal::AnalogSample>)
Q_DECLARE_METATYPE(QVector<hwtest::hal::DigitalSample>)
Q_DECLARE_METATYPE(QVector<hwtest::hal::CanFdFrame>)
