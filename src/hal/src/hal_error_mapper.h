#pragma once

#include "hal/hal_adapter_abi.h"
#include "hal/hal_types.h"

namespace hwtest::hal {

HalStatusCode mapAdapterStatus(int adapterStatusCode);
HalStatus makeError(HalStatusCode code,
                    const QString& operation,
                    const QString& message,
                    const DeviceId& deviceId = {},
                    const ResourceId& resourceId = {},
                    const QString& adapterCode = {},
                    const QVariantMap& detail = {});
HalStatus fromAdapterStatus(const HalAdapterStatus& status,
                            const QString& operation,
                            const DeviceId& deviceId = {},
                            const ResourceId& resourceId = {},
                            const QVariantMap& detail = {});

} // namespace hwtest::hal
