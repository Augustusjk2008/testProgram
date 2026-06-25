#pragma once

#include "hal_global.h"

#include <QObject>

namespace hwtest::hal {

class IHalService;

HWTEST_HAL_EXPORT IHalService* createHalService(QObject* parent = nullptr);
HWTEST_HAL_EXPORT void destroyHalService(IHalService* service);

} // namespace hwtest::hal
