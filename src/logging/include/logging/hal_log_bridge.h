#pragma once

#include "i_log_service.h"
#include "log_global.h"
#include "log_types.h"

#include <QMetaObject>
#include <Qt>

#include "hal/hal_types.h"

namespace hwtest::hal {
class IHalService;
}

namespace hwtest::logging {

HWTEST_LOG_EXPORT LogEvent fromHalLogEvent(const hwtest::hal::HalLogEvent& event);

HWTEST_LOG_EXPORT QMetaObject::Connection connectHalLogs(
    hwtest::hal::IHalService* halService,
    ILogService* logService,
    Qt::ConnectionType connectionType = Qt::AutoConnection);

} // namespace hwtest::logging
