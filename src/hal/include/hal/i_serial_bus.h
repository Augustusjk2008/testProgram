#pragma once

#include "hal_global.h"
#include "hal_types.h"

namespace hwtest::hal {

class HWTEST_HAL_EXPORT ISerialBus {
public:
    virtual ~ISerialBus() = default;

    virtual HalStatus openSerial(const ResourceId& port,
                                 const SerialConfig& config,
                                 const OperationOptions& options) = 0;
    virtual HalStatus closeSerial(const ResourceId& port,
                                  const OperationOptions& options) = 0;
    virtual HalStatus flushSerial(const ResourceId& port,
                                  const OperationOptions& options) = 0;

    virtual HalStatus writeSerial(const ResourceId& port,
                                  const QByteArray& data,
                                  const OperationOptions& options) = 0;
    virtual HalResult<QByteArray> readSerial(const ResourceId& port,
                                             int maxBytes,
                                             const OperationOptions& options) = 0;
    virtual HalResult<SerialTransactionResult> transactSerial(const ResourceId& port,
                                                              const SerialTransaction& transaction) = 0;
};

} // namespace hwtest::hal
