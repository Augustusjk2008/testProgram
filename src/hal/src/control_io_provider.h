#pragma once

#include "hal/hal_types.h"

namespace hwtest::hal {

class ControlIoProvider {
public:
    virtual ~ControlIoProvider() = default;

    virtual HalStatus open(const QVariantMap& properties,
                           const OperationOptions& options) = 0;
    virtual HalStatus close(const OperationOptions& options) = 0;
    virtual HalStatus write(const QByteArray& data,
                            const OperationOptions& options) = 0;
    virtual HalResult<QByteArray> read(int maxBytes,
                                       const OperationOptions& options) = 0;
};

} // namespace hwtest::hal
