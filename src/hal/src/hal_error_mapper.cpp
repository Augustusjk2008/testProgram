#include "hal_error_mapper.h"

#include <QStringLiteral>

namespace hwtest::hal {

HalStatusCode mapAdapterStatus(int adapterStatusCode)
{
    switch (adapterStatusCode) {
    case HAL_ADAPTER_OK: return HalStatusCode::Ok;
    case HAL_ADAPTER_INVALID_ARGUMENT: return HalStatusCode::InvalidArgument;
    case HAL_ADAPTER_NOT_FOUND: return HalStatusCode::NotFound;
    case HAL_ADAPTER_NOT_SUPPORTED: return HalStatusCode::NotSupported;
    case HAL_ADAPTER_BUSY: return HalStatusCode::Busy;
    case HAL_ADAPTER_TIMEOUT: return HalStatusCode::Timeout;
    case HAL_ADAPTER_IO_ERROR: return HalStatusCode::IoError;
    case HAL_ADAPTER_PROTOCOL_ERROR: return HalStatusCode::ProtocolError;
    case HAL_ADAPTER_DEVICE_DISCONNECTED: return HalStatusCode::DeviceDisconnected;
    case HAL_ADAPTER_BUFFER_TOO_SMALL: return HalStatusCode::BufferTooSmall;
    case HAL_ADAPTER_INTERNAL_ERROR: return HalStatusCode::InternalError;
    default: return HalStatusCode::AdapterError;
    }
}

HalStatus makeError(HalStatusCode code,
                    const QString& operation,
                    const QString& message,
                    const DeviceId& deviceId,
                    const ResourceId& resourceId,
                    const QString& adapterCode,
                    const QVariantMap& detail)
{
    HalStatus status;
    status.code = code;
    status.error.code = code;
    status.error.operation = operation;
    status.error.message = message;
    status.error.deviceId = deviceId;
    status.error.resourceId = resourceId;
    status.error.adapterCode = adapterCode;
    status.error.detail = detail;
    return status;
}

HalStatus fromAdapterStatus(const HalAdapterStatus& status,
                            const QString& operation,
                            const DeviceId& deviceId,
                            const ResourceId& resourceId,
                            const QVariantMap& detail)
{
    const HalStatusCode code = mapAdapterStatus(status.code);
    return makeError(code,
                     operation,
                     QString::fromUtf8(status.message),
                     deviceId,
                     resourceId,
                     QString::number(status.vendorCode),
                     detail);
}

} // namespace hwtest::hal
