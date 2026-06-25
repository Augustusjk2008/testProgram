#include "hal_device.h"

#include "hal_error_mapper.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <utility>

namespace hwtest::hal {

namespace {

static QVariantMap contextForBinding(const ResourceBinding& binding,
                                     const QString& operation)
{
    QVariantMap context;
    context.insert(QStringLiteral("deviceId"), binding.deviceId);
    context.insert(QStringLiteral("resourceId"), binding.resourceId);
    context.insert(QStringLiteral("module"), binding.module);
    context.insert(QStringLiteral("direction"), binding.direction);
    context.insert(QStringLiteral("operation"), operation);
    context.insert(QStringLiteral("physicalIndex"), binding.physicalIndex);
    return context;
}

static QString normalizeText(const QVariant& value)
{
    return value.toString().trimmed().toLower();
}

static qint64 nowUs()
{
    return static_cast<qint64>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) * 1000;
}

static HalStatus notOpenStatus(const QString& operation)
{
    return makeError(HalStatusCode::InvalidState,
                     operation,
                     QStringLiteral("Device session is already closed"));
}

} // namespace

HalDevice::HalDevice(HardwareAdapter* backend,
                     SessionId sessionId,
                     DeviceDescriptor descriptor,
                     DeviceCapabilities capabilities,
                     QVector<ResourceBinding> bindings,
                     QVariantMap safeState,
                     LogCallback logCallback)
    : m_backend(backend)
    , m_sessionId(std::move(sessionId))
    , m_descriptor(std::move(descriptor))
    , m_capabilities(std::move(capabilities))
    , m_safeState(std::move(safeState))
    , m_safetyGuard(nullptr)
    , m_logCallback(std::move(logCallback))
{
    for (const ResourceBinding& binding : bindings) {
        m_bindingsByResourceId.insert(binding.resourceId, binding);
        if (binding.physicalIndex >= 0) {
            m_resourceIdByPhysicalIndex.insert(binding.physicalIndex, binding.resourceId);
        }
    }
    for (const ResourceBinding& binding : bindings) {
        if (binding.module == QStringLiteral("analog")) {
            if (binding.direction == QStringLiteral("input")) {
                m_analogInputRanges.insert(binding.resourceId, AnalogRange{0.0, 0.0, AnalogUnit::Volt});
            } else {
                m_analogOutputRanges.insert(binding.resourceId, AnalogRange{0.0, 0.0, AnalogUnit::Volt});
            }
        }
    }
}

DeviceDescriptor HalDevice::descriptor() const
{
    return m_descriptor;
}

DeviceCapabilities HalDevice::capabilities() const
{
    return m_capabilities;
}

IAnalogIo* HalDevice::analogIo()
{
    return static_cast<IAnalogIo*>(this);
}

IDigitalIo* HalDevice::digitalIo()
{
    return static_cast<IDigitalIo*>(this);
}

ISerialBus* HalDevice::serialBus()
{
    return static_cast<ISerialBus*>(this);
}

ICanFdBus* HalDevice::canFdBus()
{
    return static_cast<ICanFdBus*>(this);
}

HalStatus HalDevice::close(const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("closeDevice");
    if (!m_open) {
        const HalStatus status;
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    const HalStatus safeStatus = applySafeState();
    if (m_backend != nullptr) {
        const HalStatus backendStatus = m_backend->closeDevice(m_sessionId, options);
        if (!backendStatus.ok()) {
            emitOperationLog(operation, options, timer.elapsed(), backendStatus);
            return backendStatus;
        }
    }
    m_open = false;
    const HalStatus status = safeStatus.ok() ? HalStatus{} : safeStatus;
    emitOperationLog(operation, options, timer.elapsed(), status);
    return status;
}

HalStatus HalDevice::reset(const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("resetDevice");
    if (!m_open) {
        const HalStatus status = notOpenStatus(QStringLiteral("device.reset"));
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    if (m_backend == nullptr) {
        const HalStatus status = makeError(HalStatusCode::InternalError,
                                           QStringLiteral("device.reset"),
                                           QStringLiteral("Backend adapter is missing"),
                                           m_descriptor.deviceId);
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    const HalStatus status = m_backend->resetDevice(m_sessionId, options);
    emitOperationLog(operation, options, timer.elapsed(), status);
    return status;
}

HalStatus HalDevice::healthCheck(const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("healthCheck");
    if (!m_open) {
        const HalStatus status = notOpenStatus(QStringLiteral("device.healthCheck"));
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    if (m_backend == nullptr) {
        const HalStatus status = makeError(HalStatusCode::InternalError,
                                           QStringLiteral("device.healthCheck"),
                                           QStringLiteral("Backend adapter is missing"),
                                           m_descriptor.deviceId);
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    const HalStatus status = m_backend->healthCheck(m_sessionId, options);
    emitOperationLog(operation, options, timer.elapsed(), status);
    return status;
}

bool HalDevice::isOpen() const
{
    return m_open;
}

const ResourceBinding* HalDevice::bindingFor(const ResourceId& resourceId) const
{
    const auto it = m_bindingsByResourceId.constFind(resourceId);
    if (it == m_bindingsByResourceId.constEnd()) {
        return nullptr;
    }
    return &it.value();
}

const ResourceBinding* HalDevice::bindingFor(const ResourceId& resourceId,
                                             const QString& module,
                                             const QString& expectedDirection,
                                             HalStatus* status) const
{
    if (!m_open) {
        if (status != nullptr) {
            *status = notOpenStatus(QStringLiteral("device.operation"));
        }
        return nullptr;
    }
    const ResourceBinding* binding = bindingFor(resourceId);
    if (binding == nullptr) {
        if (status != nullptr) {
            *status = makeError(HalStatusCode::NotFound,
                                QStringLiteral("device.operation"),
                                QStringLiteral("Resource not found"),
                                m_descriptor.deviceId,
                                resourceId);
        }
        return nullptr;
    }
    if (!moduleMatches(binding->module, module)) {
        if (status != nullptr) {
            *status = makeError(HalStatusCode::NotSupported,
                                QStringLiteral("device.operation"),
                                QStringLiteral("Resource module does not match requested interface"),
                                binding->deviceId,
                                binding->resourceId,
                                {},
                                contextForBinding(*binding, QStringLiteral("device.operation")));
        }
        return nullptr;
    }
    if (!directionMatches(effectiveDirection(*binding), expectedDirection)) {
        if (status != nullptr) {
            *status = makeError(HalStatusCode::InvalidState,
                                QStringLiteral("device.operation"),
                                QStringLiteral("Resource direction is not compatible with the operation"),
                                binding->deviceId,
                                binding->resourceId,
                                {},
                                contextForBinding(*binding, QStringLiteral("device.operation")));
        }
        return nullptr;
    }
    return binding;
}

HalStatus HalDevice::ensureOpen(const QString& operation) const
{
    if (!m_open) {
        return notOpenStatus(operation);
    }
    return HalStatus{};
}

HalStatus HalDevice::ensureModuleBinding(const ResourceId& resourceId,
                                         const QString& module,
                                         const QString& expectedDirection,
                                         const QString& operation,
                                         const ChannelInfo** channel) const
{
    Q_UNUSED(channel)
    HalStatus status;
    const ResourceBinding* binding = bindingFor(resourceId, module, expectedDirection, &status);
    if (binding == nullptr) {
        return status;
    }
    return HalStatus{};
}

HalStatus HalDevice::applySafeState()
{
    // Best-effort return all outputs to their configured safe values before shutdown.
    if (m_backend == nullptr) {
        return HalStatus{};
    }

    HalStatus firstError;
    for (auto it = m_safeState.constBegin(); it != m_safeState.constEnd(); ++it) {
        const ResourceId resourceId = it.key();
        const ResourceBinding* binding = bindingFor(resourceId);
        if (binding == nullptr) {
            continue;
        }
        if (binding->module == QStringLiteral("analog") && binding->direction == QStringLiteral("output")) {
            const double value = it.value().toDouble();
            const HalStatus status = m_backend->writeAnalog(m_sessionId,
                                                            binding->physicalIndex,
                                                            value,
                                                            AnalogWriteOptions{});
            if (!status.ok() && firstError.ok()) {
                firstError = status;
            }
        } else if (binding->module == QStringLiteral("digital") && binding->direction == QStringLiteral("output")) {
            const DigitalLevel level = digitalLevelFromVariant(it.value());
            const HalStatus status = m_backend->writeDigital(m_sessionId,
                                                             binding->physicalIndex,
                                                             level,
                                                             DigitalWriteOptions{});
            if (!status.ok() && firstError.ok()) {
                firstError = status;
            }
        } else if (binding->module == QStringLiteral("serial")) {
            const HalStatus status = m_backend->closeSerial(m_sessionId,
                                                            binding->physicalIndex,
                                                            OperationOptions{});
            if (!status.ok() && firstError.ok()) {
                firstError = status;
            }
        } else if (binding->module == QStringLiteral("canfd")) {
            const HalStatus status = m_backend->closeCan(m_sessionId,
                                                         binding->physicalIndex,
                                                         OperationOptions{});
            if (!status.ok() && firstError.ok()) {
                firstError = status;
            }
        }
    }
    return firstError;
}

void HalDevice::log(const QString& level,
                    const QString& category,
                    const QString& message,
                    const QVariantMap& context) const
{
    if (m_logCallback) {
        HalLogEvent event;
        event.timestampUs = nowUs();
        event.level = level;
        event.source = QStringLiteral("hal");
        event.category = category;
        event.message = message;
        event.requestId = context.value(QStringLiteral("requestId")).toString();
        event.durationMs = context.value(QStringLiteral("durationMs"), -1).toLongLong();
        event.status = context.value(QStringLiteral("status")).toString();
        event.adapterCode = context.value(QStringLiteral("adapterCode")).toString();
        event.deviceId = context.value(QStringLiteral("deviceId"), m_descriptor.deviceId).toString();
        event.resourceId = context.value(QStringLiteral("resourceId")).toString();
        event.operation = context.value(QStringLiteral("operation")).toString();
        event.context = context;
        m_logCallback(event);
    }
}

void HalDevice::emitOperationLog(const QString& operation,
                                 const OperationOptions& options,
                                 qint64 durationMs,
                                 const HalStatus& status,
                                 const ResourceBinding* binding,
                                 const QVariantMap& context) const
{
    if (!m_logCallback) {
        return;
    }

    QVariantMap payload = context;
    payload.insert(QStringLiteral("deviceId"),
                   binding != nullptr ? binding->deviceId : m_descriptor.deviceId);
    payload.insert(QStringLiteral("sessionId"), m_sessionId);
    if (binding != nullptr) {
        payload.insert(QStringLiteral("resourceId"), binding->resourceId);
        payload.insert(QStringLiteral("module"), binding->module);
        payload.insert(QStringLiteral("direction"), binding->direction);
        payload.insert(QStringLiteral("physicalIndex"), binding->physicalIndex);
    } else if (!status.error.resourceId.isEmpty()) {
        payload.insert(QStringLiteral("resourceId"), status.error.resourceId);
    }
    if (!options.tags.isEmpty()) {
        payload.insert(QStringLiteral("tags"), options.tags);
    }

    HalLogEvent event;
    event.timestampUs = nowUs();
    event.level = status.ok() ? QStringLiteral("INFO") : QStringLiteral("ERROR");
    event.source = QStringLiteral("hal");
    event.category = QStringLiteral("hal.") + operation;
    event.message = status.ok()
        ? QStringLiteral("HAL operation completed")
        : status.error.message;
    event.requestId = options.requestId;
    event.durationMs = durationMs;
    event.status = status.ok() ? QStringLiteral("Ok") : toString(status.code);
    event.adapterCode = status.error.adapterCode;
    event.deviceId = status.error.deviceId.isEmpty()
        ? payload.value(QStringLiteral("deviceId")).toString()
        : status.error.deviceId;
    event.resourceId = status.error.resourceId.isEmpty()
        ? payload.value(QStringLiteral("resourceId")).toString()
        : status.error.resourceId;
    event.operation = operation;
    event.context = payload;
    m_logCallback(event);
}

QString HalDevice::effectiveDirection(const ResourceBinding& binding)
{
    return binding.direction.isEmpty() ? QStringLiteral("bidirectional") : binding.direction;
}

bool HalDevice::directionMatches(const QString& actual, const QString& expected)
{
    if (expected.isEmpty() || expected == QStringLiteral("any")) {
        return true;
    }
    if (actual == expected) {
        return true;
    }
    return actual == QStringLiteral("bidirectional");
}

bool HalDevice::moduleMatches(const QString& actual, const QString& expected)
{
    return actual.trimmed().toLower() == expected.trimmed().toLower();
}

AnalogRange HalDevice::effectiveAnalogRange(const ResourceBinding& binding,
                                           const AnalogRange& requestedRange)
{
    if (requestedRange.minValue != requestedRange.maxValue) {
        return requestedRange;
    }
    AnalogRange range = requestedRange;
    const QVariantMap properties = binding.properties;
    const QVariant minValue = properties.value(QStringLiteral("safeMinValue"));
    const QVariant maxValue = properties.value(QStringLiteral("safeMaxValue"));
    if (minValue.isValid() && maxValue.isValid()) {
        range.minValue = minValue.toDouble();
        range.maxValue = maxValue.toDouble();
        return range;
    }
    range.minValue = 0.0;
    range.maxValue = 5.0;
    return range;
}

DigitalLevel HalDevice::digitalLevelFromVariant(const QVariant& value)
{
    const QString text = normalizeText(value);
    if (text == QStringLiteral("high") || text == QStringLiteral("1") || text == QStringLiteral("true")) {
        return DigitalLevel::High;
    }
    if (text == QStringLiteral("low") || text == QStringLiteral("0") || text == QStringLiteral("false")) {
        return DigitalLevel::Low;
    }
    return DigitalLevel::Unknown;
}

QVariantMap HalDevice::channelContext(const ResourceBinding& binding,
                                     const QString& operation)
{
    QVariantMap context = contextForBinding(binding, operation);
    context.insert(QStringLiteral("timestampUs"), nowUs());
    return context;
}

HalResult<AnalogSample> HalDevice::rewriteAnalogSample(const ResourceId& resourceId,
                                                       const HalResult<AnalogSample>& sample) const
{
    if (!sample.ok()) {
        return sample;
    }
    HalResult<AnalogSample> result = sample;
    result.value.channel = resourceId;
    return result;
}

HalResult<DigitalSample> HalDevice::rewriteDigitalSample(const ResourceId& resourceId,
                                                         const HalResult<DigitalSample>& sample) const
{
    if (!sample.ok()) {
        return sample;
    }
    HalResult<DigitalSample> result = sample;
    result.value.channel = resourceId;
    return result;
}

HalStatus HalDevice::configureAd(const ResourceId& channel,
                                 const AnalogRange& range,
                                 const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("analog.configureAd");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(channel, QStringLiteral("analog"), QStringLiteral("input"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    m_analogInputRanges.insert(channel, range);
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("analog.configureAd"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->configureAnalog(m_sessionId, binding->physicalIndex, range, false, options);
    emitOperationLog(operation, options, timer.elapsed(), status, binding);
    return status;
}

HalResult<AnalogSample> HalDevice::readAd(const ResourceId& channel,
                                          const AnalogReadOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("analog.readAd");
    HalResult<AnalogSample> result;
    HalStatus status;
    const ResourceBinding* binding = bindingFor(channel, QStringLiteral("analog"), QStringLiteral("input"), &status);
    if (binding == nullptr) {
        result.status = status;
        emitOperationLog(operation, options.op, timer.elapsed(), result.status);
        return result;
    }
    if (m_backend == nullptr) {
        result.status = makeError(HalStatusCode::InternalError,
                                  QStringLiteral("analog.readAd"),
                                  QStringLiteral("Backend adapter is missing"),
                                  binding->deviceId,
                                  binding->resourceId);
        emitOperationLog(operation, options.op, timer.elapsed(), result.status, binding);
        return result;
    }
    const HalResult<AnalogSample> sample = m_backend->readAnalog(m_sessionId, binding->physicalIndex, options);
    if (!sample.ok()) {
        result.status = sample.status;
        emitOperationLog(operation, options.op, timer.elapsed(), result.status, binding);
        return result;
    }
    result = rewriteAnalogSample(channel, sample);
    result.value.metadata.insert(QStringLiteral("resourceId"), channel);
    emitOperationLog(operation, options.op, timer.elapsed(), result.status, binding);
    return result;
}

HalResult<QVector<AnalogSample>> HalDevice::readAdBatch(const QVector<ResourceId>& channels,
                                                        const AnalogReadOptions& options)
{
    HalResult<QVector<AnalogSample>> result;
    for (const ResourceId& channel : channels) {
        const HalResult<AnalogSample> sample = readAd(channel, options);
        if (!sample.ok()) {
            result.status = sample.status;
            return result;
        }
        result.value.push_back(sample.value);
    }
    return result;
}

HalStatus HalDevice::configureDa(const ResourceId& channel,
                                 const AnalogRange& range,
                                 const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("analog.configureDa");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(channel, QStringLiteral("analog"), QStringLiteral("output"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    m_analogOutputRanges.insert(channel, range);
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("analog.configureDa"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->configureAnalog(m_sessionId, binding->physicalIndex, range, true, options);
    emitOperationLog(operation, options, timer.elapsed(), status, binding);
    return status;
}

HalStatus HalDevice::writeDa(const ResourceId& channel,
                             double value,
                             const AnalogWriteOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("analog.writeDa");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(channel, QStringLiteral("analog"), QStringLiteral("output"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options.op, timer.elapsed(), status);
        return status;
    }
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("analog.writeDa"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options.op, timer.elapsed(), status, binding);
        return status;
    }
    AnalogWriteOptions normalizedOptions = options;
    normalizedOptions.range = effectiveAnalogRange(*binding, options.range);
    double effectiveValue = value;
    status = m_safetyGuard.validateAnalogWrite(*binding, value, normalizedOptions, &effectiveValue);
    if (!status.ok()) {
        emitOperationLog(operation, options.op, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->writeAnalog(m_sessionId, binding->physicalIndex, effectiveValue, normalizedOptions);
    emitOperationLog(operation, options.op, timer.elapsed(), status, binding);
    return status;
}

HalStatus HalDevice::writeDaBatch(const QMap<ResourceId, double>& values,
                                  const AnalogWriteOptions& options)
{
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const HalStatus status = writeDa(it.key(), it.value(), options);
        if (!status.ok()) {
            return status;
        }
    }
    return HalStatus{};
}

HalResult<DigitalSample> HalDevice::readDi(const ResourceId& channel,
                                           const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("digital.readDi");
    HalResult<DigitalSample> result;
    HalStatus status;
    const ResourceBinding* binding = bindingFor(channel, QStringLiteral("digital"), QStringLiteral("input"), &status);
    if (binding == nullptr) {
        result.status = status;
        emitOperationLog(operation, options, timer.elapsed(), result.status);
        return result;
    }
    if (m_backend == nullptr) {
        result.status = makeError(HalStatusCode::InternalError,
                                  QStringLiteral("digital.readDi"),
                                  QStringLiteral("Backend adapter is missing"),
                                  binding->deviceId,
                                  binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
        return result;
    }
    const HalResult<DigitalSample> sample = m_backend->readDigital(m_sessionId, binding->physicalIndex, options);
    if (!sample.ok()) {
        result.status = sample.status;
        emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
        return result;
    }
    result = rewriteDigitalSample(channel, sample);
    result.value.metadata.insert(QStringLiteral("resourceId"), channel);
    emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
    return result;
}

HalResult<QVector<DigitalSample>> HalDevice::readDiBatch(const QVector<ResourceId>& channels,
                                                         const OperationOptions& options)
{
    HalResult<QVector<DigitalSample>> result;
    for (const ResourceId& channel : channels) {
        const HalResult<DigitalSample> sample = readDi(channel, options);
        if (!sample.ok()) {
            result.status = sample.status;
            return result;
        }
        result.value.push_back(sample.value);
    }
    return result;
}

HalStatus HalDevice::writeDo(const ResourceId& channel,
                             DigitalLevel level,
                             const DigitalWriteOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("digital.writeDo");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(channel, QStringLiteral("digital"), QStringLiteral("output"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options.op, timer.elapsed(), status);
        return status;
    }
    status = m_safetyGuard.validateDigitalWrite(*binding, level, options);
    if (!status.ok()) {
        emitOperationLog(operation, options.op, timer.elapsed(), status, binding);
        return status;
    }
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("digital.writeDo"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options.op, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->writeDigital(m_sessionId, binding->physicalIndex, level, options);
    emitOperationLog(operation, options.op, timer.elapsed(), status, binding);
    return status;
}

HalStatus HalDevice::writeDoBatch(const QMap<ResourceId, DigitalLevel>& values,
                                  const DigitalWriteOptions& options)
{
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const HalStatus status = writeDo(it.key(), it.value(), options);
        if (!status.ok()) {
            return status;
        }
    }
    return HalStatus{};
}

HalResult<DigitalSample> HalDevice::waitEdge(const ResourceId& channel,
                                             DigitalLevel targetLevel,
                                             const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("digital.waitEdge");
    HalResult<DigitalSample> result;
    HalStatus status;
    const ResourceBinding* binding = bindingFor(channel, QStringLiteral("digital"), QStringLiteral("input"), &status);
    if (binding == nullptr) {
        result.status = status;
        emitOperationLog(operation, options, timer.elapsed(), result.status);
        return result;
    }
    if (m_backend == nullptr) {
        result.status = makeError(HalStatusCode::InternalError,
                                  QStringLiteral("digital.waitEdge"),
                                  QStringLiteral("Backend adapter is missing"),
                                  binding->deviceId,
                                  binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
        return result;
    }
    const HalResult<DigitalSample> sample = m_backend->waitDigitalEdge(m_sessionId,
                                                                        binding->physicalIndex,
                                                                        targetLevel,
                                                                        options);
    if (!sample.ok()) {
        result.status = sample.status;
        emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
        return result;
    }
    result = rewriteDigitalSample(channel, sample);
    emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
    return result;
}

HalStatus HalDevice::openSerial(const ResourceId& port,
                                const SerialConfig& config,
                                const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("serial.openSerial");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(port, QStringLiteral("serial"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    m_openSerialPorts.insert(port, config);
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("serial.openSerial"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->openSerial(m_sessionId, binding->physicalIndex, config, options);
    emitOperationLog(operation, options, timer.elapsed(), status, binding);
    return status;
}

HalStatus HalDevice::closeSerial(const ResourceId& port,
                                 const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("serial.closeSerial");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(port, QStringLiteral("serial"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    m_openSerialPorts.remove(port);
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("serial.closeSerial"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->closeSerial(m_sessionId, binding->physicalIndex, options);
    emitOperationLog(operation, options, timer.elapsed(), status, binding);
    return status;
}

HalStatus HalDevice::flushSerial(const ResourceId& port,
                                 const OperationOptions& options)
{
    HalStatus status;
    const ResourceBinding* binding = bindingFor(port, QStringLiteral("serial"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        return status;
    }
    if (m_backend == nullptr) {
        return makeError(HalStatusCode::InternalError,
                         QStringLiteral("serial.flushSerial"),
                         QStringLiteral("Backend adapter is missing"),
                         binding->deviceId,
                         binding->resourceId);
    }
    return m_backend->flushSerial(m_sessionId, binding->physicalIndex, options);
}

HalStatus HalDevice::writeSerial(const ResourceId& port,
                                 const QByteArray& data,
                                 const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("serial.writeSerial");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(port, QStringLiteral("serial"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("serial.writeSerial"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->writeSerial(m_sessionId, binding->physicalIndex, data, options);
    emitOperationLog(operation, options, timer.elapsed(), status, binding);
    return status;
}

HalResult<QByteArray> HalDevice::readSerial(const ResourceId& port,
                                            int maxBytes,
                                            const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("serial.readSerial");
    HalResult<QByteArray> result;
    HalStatus status;
    const ResourceBinding* binding = bindingFor(port, QStringLiteral("serial"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        result.status = status;
        emitOperationLog(operation, options, timer.elapsed(), result.status);
        return result;
    }
    if (m_backend == nullptr) {
        result.status = makeError(HalStatusCode::InternalError,
                                  QStringLiteral("serial.readSerial"),
                                  QStringLiteral("Backend adapter is missing"),
                                  binding->deviceId,
                                  binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
        return result;
    }
    result = m_backend->readSerial(m_sessionId, binding->physicalIndex, maxBytes, options);
    emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
    return result;
}

HalResult<SerialTransactionResult> HalDevice::transactSerial(const ResourceId& port,
                                                             const SerialTransaction& transaction)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("serial.transactSerial");
    HalResult<SerialTransactionResult> result;
    HalStatus status;
    const ResourceBinding* binding = bindingFor(port, QStringLiteral("serial"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        result.status = status;
        emitOperationLog(operation, transaction.op, timer.elapsed(), result.status);
        return result;
    }
    if (m_backend == nullptr) {
        result.status = makeError(HalStatusCode::InternalError,
                                  QStringLiteral("serial.transactSerial"),
                                  QStringLiteral("Backend adapter is missing"),
                                  binding->deviceId,
                                  binding->resourceId);
        emitOperationLog(operation, transaction.op, timer.elapsed(), result.status, binding);
        return result;
    }
    const HalStatus writeStatus = m_backend->writeSerial(m_sessionId, binding->physicalIndex, transaction.tx, transaction.op);
    if (!writeStatus.ok()) {
        result.status = writeStatus;
        emitOperationLog(operation, transaction.op, timer.elapsed(), result.status, binding);
        return result;
    }
    const HalResult<QByteArray> readResult = m_backend->readSerial(m_sessionId,
                                                                   binding->physicalIndex,
                                                                   transaction.readMaxBytes,
                                                                   transaction.op);
    if (!readResult.ok()) {
        result.status = readResult.status;
        emitOperationLog(operation, transaction.op, timer.elapsed(), result.status, binding);
        return result;
    }
    result.value.rx = readResult.value;
    result.value.txTimestampUs = nowUs();
    result.value.rxTimestampUs = nowUs();
    result.value.metadata.insert(QStringLiteral("resourceId"), port);
    emitOperationLog(operation, transaction.op, timer.elapsed(), result.status, binding);
    return result;
}

HalStatus HalDevice::openCan(const ResourceId& bus,
                             const CanFdConfig& config,
                             const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("can.openCan");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(bus, QStringLiteral("canfd"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    m_openCanBuses.insert(bus, config);
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("can.openCan"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->openCan(m_sessionId, binding->physicalIndex, config, options);
    emitOperationLog(operation, options, timer.elapsed(), status, binding);
    return status;
}

HalStatus HalDevice::closeCan(const ResourceId& bus,
                              const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("can.closeCan");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(bus, QStringLiteral("canfd"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    m_openCanBuses.remove(bus);
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("can.closeCan"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->closeCan(m_sessionId, binding->physicalIndex, options);
    emitOperationLog(operation, options, timer.elapsed(), status, binding);
    return status;
}

HalStatus HalDevice::setCanFilters(const ResourceId& bus,
                                   const QVector<CanFdFilter>& filters,
                                   const OperationOptions& options)
{
    HalStatus status;
    const ResourceBinding* binding = bindingFor(bus, QStringLiteral("canfd"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        return status;
    }
    if (m_backend == nullptr) {
        return makeError(HalStatusCode::InternalError,
                         QStringLiteral("can.setCanFilters"),
                         QStringLiteral("Backend adapter is missing"),
                         binding->deviceId,
                         binding->resourceId);
    }
    return m_backend->setCanFilters(m_sessionId, binding->physicalIndex, filters, options);
}

HalStatus HalDevice::sendCan(const ResourceId& bus,
                             const CanFdFrame& frame,
                             const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("can.sendCan");
    HalStatus status;
    const ResourceBinding* binding = bindingFor(bus, QStringLiteral("canfd"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        emitOperationLog(operation, options, timer.elapsed(), status);
        return status;
    }
    status = m_safetyGuard.validateCanFrame(*binding, frame, options);
    if (!status.ok()) {
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    if (m_backend == nullptr) {
        status = makeError(HalStatusCode::InternalError,
                           QStringLiteral("can.sendCan"),
                           QStringLiteral("Backend adapter is missing"),
                           binding->deviceId,
                           binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), status, binding);
        return status;
    }
    status = m_backend->sendCan(m_sessionId, binding->physicalIndex, frame, options);
    emitOperationLog(operation, options, timer.elapsed(), status, binding);
    return status;
}

HalResult<CanFdFrame> HalDevice::receiveCan(const ResourceId& bus,
                                           const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("can.receiveCan");
    HalResult<CanFdFrame> result;
    HalStatus status;
    const ResourceBinding* binding = bindingFor(bus, QStringLiteral("canfd"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        result.status = status;
        emitOperationLog(operation, options, timer.elapsed(), result.status);
        return result;
    }
    if (m_backend == nullptr) {
        result.status = makeError(HalStatusCode::InternalError,
                                  QStringLiteral("can.receiveCan"),
                                  QStringLiteral("Backend adapter is missing"),
                                  binding->deviceId,
                                  binding->resourceId);
        emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
        return result;
    }
    result = m_backend->receiveCan(m_sessionId, binding->physicalIndex, options);
    emitOperationLog(operation, options, timer.elapsed(), result.status, binding);
    return result;
}

HalResult<QVector<CanFdFrame>> HalDevice::receiveCanBatch(const ResourceId& bus,
                                                          int maxFrames,
                                                          const OperationOptions& options)
{
    HalResult<QVector<CanFdFrame>> result;
    HalStatus status;
    const ResourceBinding* binding = bindingFor(bus, QStringLiteral("canfd"), QStringLiteral("bidirectional"), &status);
    if (binding == nullptr) {
        result.status = status;
        return result;
    }
    if (m_backend == nullptr) {
        result.status = makeError(HalStatusCode::InternalError,
                                  QStringLiteral("can.receiveCanBatch"),
                                  QStringLiteral("Backend adapter is missing"),
                                  binding->deviceId,
                                  binding->resourceId);
        return result;
    }
    result = m_backend->receiveCanBatch(m_sessionId, binding->physicalIndex, maxFrames, options);
    return result;
}

} // namespace hwtest::hal
