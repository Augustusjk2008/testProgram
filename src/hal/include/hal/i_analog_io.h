#pragma once

#include "hal_global.h"
#include "hal_types.h"

namespace hwtest::hal {

class HWTEST_HAL_EXPORT IAnalogIo {
public:
    virtual ~IAnalogIo() = default;

    virtual HalStatus configureAd(const ResourceId& channel,
                                  const AnalogRange& range,
                                  const OperationOptions& options) = 0;
    virtual HalResult<AnalogSample> readAd(const ResourceId& channel,
                                           const AnalogReadOptions& options) = 0;
    virtual HalResult<QVector<AnalogSample>> readAdBatch(const QVector<ResourceId>& channels,
                                                         const AnalogReadOptions& options) = 0;

    virtual HalStatus configureDa(const ResourceId& channel,
                                  const AnalogRange& range,
                                  const OperationOptions& options) = 0;
    virtual HalStatus writeDa(const ResourceId& channel,
                              double value,
                              const AnalogWriteOptions& options) = 0;
    virtual HalStatus writeDaBatch(const QMap<ResourceId, double>& values,
                                   const AnalogWriteOptions& options) = 0;
};

} // namespace hwtest::hal
