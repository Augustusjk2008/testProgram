#pragma once

#include "hal_global.h"
#include "hal_types.h"

#include <QObject>

namespace hwtest::hal {

class IHalService;

HWTEST_HAL_EXPORT IHalService* createHalService(QObject* parent = nullptr);
HWTEST_HAL_EXPORT void destroyHalService(IHalService* service);
HWTEST_HAL_EXPORT QVector<SerialPortDescriptor> availableSerialPorts();
// Initializes a C ABI adapter only long enough to enumerate device identities.
// It never opens a device, queries capabilities, or creates I/O tasks.
HWTEST_HAL_EXPORT HalResult<QVector<DeviceDescriptor>> enumerateCAbiAdapterDevices(
    const QVariantMap& driverConfig,
    const OperationOptions& options = {});

} // namespace hwtest::hal
