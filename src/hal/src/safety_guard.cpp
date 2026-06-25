#include "safety_guard.h"

namespace hwtest::hal {

SafetyGuard::SafetyGuard(ResourceMapper* mapper)
    : m_mapper(mapper)
{
}

void SafetyGuard::setMapper(ResourceMapper* mapper)
{
    m_mapper = mapper;
}

HalStatus SafetyGuard::validateAnalogWrite(const ResourceBinding& binding,
                                           double value,
                                           const AnalogWriteOptions& options,
                                           double* effectiveValue) const
{
    const double minValue = options.range.minValue;
    const double maxValue = options.range.maxValue;
    double clampedValue = value;
    if (value < minValue) {
        if (!options.safeClamp) {
            return makeError(HalStatusCode::SafetyLimitExceeded,
                             QStringLiteral("analog.write"),
                             QStringLiteral("Analog output below safe minimum"),
                             binding.deviceId,
                             binding.resourceId,
                             {},
                             {{QStringLiteral("value"), value}, {QStringLiteral("minValue"), minValue}});
        }
        clampedValue = minValue;
    }
    if (value > maxValue) {
        if (!options.safeClamp) {
            return makeError(HalStatusCode::SafetyLimitExceeded,
                             QStringLiteral("analog.write"),
                             QStringLiteral("Analog output above safe maximum"),
                             binding.deviceId,
                             binding.resourceId,
                             {},
                             {{QStringLiteral("value"), value}, {QStringLiteral("maxValue"), maxValue}});
        }
        clampedValue = maxValue;
    }
    if (effectiveValue) {
        *effectiveValue = clampedValue;
    }
    return HalStatus{};
}

HalStatus SafetyGuard::validateDigitalWrite(const ResourceBinding& binding,
                                            DigitalLevel level,
                                            const DigitalWriteOptions& options) const
{
    Q_UNUSED(options)
    if (binding.direction == QStringLiteral("input")) {
        return makeError(HalStatusCode::InvalidState,
                         QStringLiteral("digital.write"),
                         QStringLiteral("Cannot write to input resource"),
                         binding.deviceId,
                         binding.resourceId);
    }
    if (level == DigitalLevel::Unknown) {
        return makeError(HalStatusCode::InvalidArgument,
                         QStringLiteral("digital.write"),
                         QStringLiteral("Digital level cannot be Unknown"),
                         binding.deviceId,
                         binding.resourceId);
    }
    return HalStatus{};
}

HalStatus SafetyGuard::validateSerialConfig(const ResourceBinding& binding,
                                            const SerialConfig& config,
                                            const OperationOptions& options) const
{
    Q_UNUSED(binding)
    Q_UNUSED(options)
    if (config.baudRate <= 0 || config.dataBits < 5 || config.dataBits > 8) {
        return makeError(HalStatusCode::InvalidArgument,
                         QStringLiteral("serial.open"),
                         QStringLiteral("Invalid serial configuration"));
    }
    return HalStatus{};
}

HalStatus SafetyGuard::validateCanFrame(const ResourceBinding& binding,
                                         const CanFdFrame& frame,
                                         const OperationOptions& options) const
{
    Q_UNUSED(options)
    const int payloadSize = frame.payload.size();
    const int maxPayload = frame.fd ? 64 : 8;
    if (payloadSize > maxPayload) {
        return makeError(HalStatusCode::SafetyLimitExceeded,
                         QStringLiteral("can.send"),
                         QStringLiteral("CAN payload exceeds maximum size"),
                         binding.deviceId,
                         binding.resourceId,
                         {},
                         {{QStringLiteral("payloadSize"), payloadSize}, {QStringLiteral("maxPayload"), maxPayload}});
    }
    return HalStatus{};
}

} // namespace hwtest::hal
