#pragma once

#include "hal_global.h"
#include "hal_types.h"

namespace hwtest::hal {

class HWTEST_HAL_EXPORT ICanFdBus {
public:
    virtual ~ICanFdBus() = default;

    virtual HalStatus openCan(const ResourceId& bus,
                              const CanFdConfig& config,
                              const OperationOptions& options) = 0;
    virtual HalStatus closeCan(const ResourceId& bus,
                               const OperationOptions& options) = 0;
    virtual HalStatus setCanFilters(const ResourceId& bus,
                                    const QVector<CanFdFilter>& filters,
                                    const OperationOptions& options) = 0;
    virtual HalStatus sendCan(const ResourceId& bus,
                              const CanFdFrame& frame,
                              const OperationOptions& options) = 0;
    virtual HalResult<CanFdFrame> receiveCan(const ResourceId& bus,
                                             const OperationOptions& options) = 0;
    virtual HalResult<QVector<CanFdFrame>> receiveCanBatch(const ResourceId& bus,
                                                           int maxFrames,
                                                           const OperationOptions& options) = 0;
};

} // namespace hwtest::hal
