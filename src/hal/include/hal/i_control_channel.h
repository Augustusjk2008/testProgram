#pragma once

#include "hal_global.h"
#include "hal_types.h"

#include <QByteArray>

namespace hwtest::hal {

class HWTEST_HAL_EXPORT IControlChannel {
public:
    virtual ~IControlChannel() = default;

    virtual HalStatus openControl(const ResourceId& resourceId,
                                  const OperationOptions& options) = 0;
    virtual HalStatus closeControl(const ResourceId& resourceId,
                                   const OperationOptions& options) = 0;
    virtual HalStatus writeControl(const ResourceId& resourceId,
                                   const QByteArray& data,
                                   const OperationOptions& options) = 0;
    virtual HalResult<QByteArray> readControl(const ResourceId& resourceId,
                                              int maxBytes,
                                              const OperationOptions& options) = 0;
};

} // namespace hwtest::hal
