#pragma once

#include "hal_error_mapper.h"
#include "resource_mapper.h"

namespace hwtest::hal {

class SafetyGuard {
public:
    explicit SafetyGuard(ResourceMapper* mapper = nullptr);

    void setMapper(ResourceMapper* mapper);

    HalStatus validateAnalogWrite(const ResourceBinding& binding,
                                  double value,
                                  const AnalogWriteOptions& options,
                                  double* effectiveValue) const;
    HalStatus validateDigitalWrite(const ResourceBinding& binding,
                                   DigitalLevel level,
                                   const DigitalWriteOptions& options) const;
    HalStatus validateSerialConfig(const ResourceBinding& binding,
                                   const SerialConfig& config,
                                   const OperationOptions& options) const;
    HalStatus validateCanFrame(const ResourceBinding& binding,
                              const CanFdFrame& frame,
                              const OperationOptions& options) const;

private:
    ResourceMapper* m_mapper = nullptr;
};

} // namespace hwtest::hal
