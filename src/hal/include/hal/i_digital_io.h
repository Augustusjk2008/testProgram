#pragma once

#include "hal_global.h"
#include "hal_types.h"

namespace hwtest::hal {

class HWTEST_HAL_EXPORT IDigitalIo {
public:
    virtual ~IDigitalIo() = default;

    virtual HalResult<DigitalSample> readDi(const ResourceId& channel,
                                            const OperationOptions& options) = 0;
    virtual HalResult<QVector<DigitalSample>> readDiBatch(const QVector<ResourceId>& channels,
                                                          const OperationOptions& options) = 0;

    virtual HalStatus writeDo(const ResourceId& channel,
                              DigitalLevel level,
                              const DigitalWriteOptions& options) = 0;
    virtual HalStatus writeDoBatch(const QMap<ResourceId, DigitalLevel>& values,
                                   const DigitalWriteOptions& options) = 0;

    virtual HalResult<DigitalSample> waitEdge(const ResourceId& channel,
                                              DigitalLevel targetLevel,
                                              const OperationOptions& options) = 0;
};

} // namespace hwtest::hal
