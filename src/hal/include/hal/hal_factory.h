#pragma once

#include "hal_global.h"
#include "hal_types.h"

#include <QObject>

namespace hwtest::hal {

class IHalService;

HWTEST_HAL_EXPORT IHalService* createHalService(QObject* parent = nullptr);
HWTEST_HAL_EXPORT void destroyHalService(IHalService* service);
HWTEST_HAL_EXPORT QVector<SerialPortDescriptor> availableSerialPorts();

} // namespace hwtest::hal
