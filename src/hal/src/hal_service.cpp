#include "hal_service.h"

#include "hal_error_mapper.h"

#include <QDateTime>
#include <QElapsedTimer>

namespace hwtest::hal {

namespace {

static qint64 nowUs()
{
    return static_cast<qint64>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) * 1000;
}

} // namespace

HalService::HalService(QObject* parent)
    : IHalService(parent)
{
}

HalService::~HalService()
{
    shutdown();
}

void HalService::emitLog(const QString& level,
                         const QString& category,
                         const QString& message,
                         const QVariantMap& context)
{
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
    event.deviceId = context.value(QStringLiteral("deviceId")).toString();
    event.resourceId = context.value(QStringLiteral("resourceId")).toString();
    event.operation = context.value(QStringLiteral("operation")).toString();
    event.context = context;
    emitLogEvent(event);
}

void HalService::emitLogEvent(const HalLogEvent& event)
{
    HalLogEvent normalized = event;
    if (normalized.timestampUs <= 0) {
        normalized.timestampUs = nowUs();
    }
    if (normalized.source.isEmpty()) {
        normalized.source = QStringLiteral("hal");
    }

    QVariantMap context = normalized.context;
    if (!normalized.requestId.isEmpty()) {
        context.insert(QStringLiteral("requestId"), normalized.requestId);
    }
    if (normalized.durationMs >= 0) {
        context.insert(QStringLiteral("durationMs"), normalized.durationMs);
    }
    if (!normalized.status.isEmpty()) {
        context.insert(QStringLiteral("status"), normalized.status);
    }
    if (!normalized.adapterCode.isEmpty()) {
        context.insert(QStringLiteral("adapterCode"), normalized.adapterCode);
    }
    if (!normalized.deviceId.isEmpty()) {
        context.insert(QStringLiteral("deviceId"), normalized.deviceId);
    }
    if (!normalized.resourceId.isEmpty()) {
        context.insert(QStringLiteral("resourceId"), normalized.resourceId);
    }
    if (!normalized.operation.isEmpty()) {
        context.insert(QStringLiteral("operation"), normalized.operation);
    }

    normalized.context = context;
    emit logProduced(normalized);
    emit logMessage(normalized.level, normalized.category, normalized.message, context);
}

void HalService::emitOperationLog(const QString& operation,
                                  const OperationOptions& options,
                                  qint64 durationMs,
                                  const HalStatus& status,
                                  const DeviceId& deviceId,
                                  const SessionId& sessionId,
                                  const QVariantMap& context)
{
    QVariantMap payload = context;
    if (!sessionId.isEmpty()) {
        payload.insert(QStringLiteral("sessionId"), sessionId);
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
    event.deviceId = status.error.deviceId.isEmpty() ? deviceId : status.error.deviceId;
    event.resourceId = status.error.resourceId;
    event.operation = operation;
    event.context = payload;
    emitLogEvent(event);
}

HalDevice* HalService::sessionDevice(const SessionId& sessionId)
{
    const auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end()) {
        return nullptr;
    }
    return it.value().device.get();
}

const HalDevice* HalService::sessionDevice(const SessionId& sessionId) const
{
    const auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd()) {
        return nullptr;
    }
    return it.value().device.get();
}

HalStatus HalService::initialize(const QVariantMap& halConfig)
{
    shutdown();
    m_config = halConfig;
    if (!m_mapper.load(halConfig)) {
        m_config.clear();
        return makeError(HalStatusCode::InvalidArgument,
                         QStringLiteral("hal.initialize"),
                         m_mapper.errorString());
    }
    const HalStatus status = m_router.configure(halConfig, m_mapper);
    if (!status.ok()) {
        m_config.clear();
        return status;
    }

    m_initialized = true;
    emitLog(QStringLiteral("INFO"),
             QStringLiteral("hal.service"),
             QStringLiteral("HAL initialized"),
             {{QStringLiteral("timestampUs"), nowUs()}});
    return HalStatus{};
}

HalStatus HalService::shutdown()
{
    HalStatus firstError;
    while (!m_sessionOrder.isEmpty()) {
        const HalStatus status = closeDevice(m_sessionOrder.last(), OperationOptions{});
        if (!status.ok() && firstError.ok()) firstError = status;
    }
    m_sessions.clear();
    m_sessionOrder.clear();
    const HalStatus routerStatus = m_router.shutdown();
    if (!routerStatus.ok() && firstError.ok()) firstError = routerStatus;
    m_initialized = false;
    m_config.clear();
    return firstError;
}

HalResult<QVector<DeviceDescriptor>> HalService::scanDevices(const OperationOptions& options)
{
    HalResult<QVector<DeviceDescriptor>> result;
    Q_UNUSED(options)
    if (!m_initialized) {
        result.status = makeError(HalStatusCode::NotInitialized,
                                  QStringLiteral("hal.scanDevices"),
                                  QStringLiteral("HAL is not initialized"));
        return result;
    }
    result.value = m_mapper.devices();
    return result;
}

HalResult<DeviceCapabilities> HalService::queryCapabilities(const DeviceId& deviceId,
                                                            const OperationOptions& options)
{
    HalResult<DeviceCapabilities> result;
    Q_UNUSED(options)
    if (!m_initialized) {
        result.status = makeError(HalStatusCode::NotInitialized,
                                  QStringLiteral("hal.queryCapabilities"),
                                  QStringLiteral("HAL is not initialized"),
                                  deviceId);
        return result;
    }
    const DeviceDescriptor descriptor = m_mapper.deviceDescriptor(deviceId);
    if (descriptor.deviceId.isEmpty()) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("hal.queryCapabilities"),
                                  QStringLiteral("Device not found"),
                                  deviceId);
        return result;
    }
    result.value = m_mapper.capabilities(deviceId);
    return result;
}

HalResult<SessionId> HalService::openDevice(const DeviceId& deviceId,
                                            const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("openDevice");
    HalResult<SessionId> result;
    if (!m_initialized) {
        result.status = makeError(HalStatusCode::NotInitialized,
                                  QStringLiteral("hal.openDevice"),
                                  QStringLiteral("HAL is not initialized"),
                                  deviceId);
        emitOperationLog(operation, options, timer.elapsed(), result.status, deviceId);
        return result;
    }
    const DeviceDescriptor descriptor = m_mapper.deviceDescriptor(deviceId);
    if (descriptor.deviceId.isEmpty()) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("hal.openDevice"),
                                  QStringLiteral("Device not found"),
                                  deviceId);
        emitOperationLog(operation, options, timer.elapsed(), result.status, deviceId);
        return result;
    }

    const HalResult<HardwareAdapter*> acquired = m_router.acquire(descriptor.adapterId);
    if (!acquired.ok() || acquired.value == nullptr) {
        result.status = acquired.status;
        emitOperationLog(operation, options, timer.elapsed(), result.status, deviceId);
        return result;
    }
    HardwareAdapter* const backend = acquired.value;
    QVariantMap openOptions = descriptor.properties;
    openOptions.insert(QStringLiteral("deviceId"), descriptor.deviceId);
    openOptions.insert(QStringLiteral("adapterId"), descriptor.adapterId);
    const HalResult<SessionId> backendSession = backend->openDevice(deviceId, openOptions, options);
    if (!backendSession.ok()) {
        m_router.release(descriptor.adapterId);
        result.status = backendSession.status;
        emitOperationLog(operation, options, timer.elapsed(), result.status, deviceId);
        return result;
    }

    SessionEntry entry;
    entry.descriptor = descriptor;
    entry.adapterId = descriptor.adapterId;
    entry.device = std::make_shared<HalDevice>(backend,
                                               backendSession.value,
                                               descriptor,
                                               m_mapper.capabilities(deviceId),
                                               m_mapper.bindingsForDevice(deviceId),
                                               m_mapper.safeState(),
                                               [this, deviceId](const HalLogEvent& event) {
                                                   HalLogEvent payload = event;
                                                   if (payload.deviceId.isEmpty()) {
                                                       payload.deviceId = deviceId;
                                                   }
                                                   emitLogEvent(payload);
                                               });
    const SessionId publicSession = QStringLiteral("hal-session-%1")
                                        .arg(++m_nextSessionId);
    m_sessions.insert(publicSession, std::move(entry));
    m_sessionOrder.push_back(publicSession);
    emit deviceChanged(descriptor, QStringLiteral("opened"));
    result.value = publicSession;
    emitOperationLog(operation,
                     options,
                     timer.elapsed(),
                     result.status,
                     deviceId,
                     result.value);
    return result;
}

HalStatus HalService::closeDevice(const SessionId& sessionId,
                                  const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("closeDevice");
    const auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end()) {
        const HalStatus status = makeError(HalStatusCode::NotFound,
                                           QStringLiteral("hal.closeDevice"),
                                           QStringLiteral("Session not found"),
                                           {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        emitOperationLog(operation, options, timer.elapsed(), status, {}, sessionId);
        return status;
    }
    const DeviceDescriptor descriptor = it.value().descriptor;
    const AdapterId adapterId = it.value().adapterId;
    const HalStatus status = it.value().device ? it.value().device->close(options) : HalStatus{};
    m_sessions.erase(it);
    m_sessionOrder.removeAll(sessionId);
    const HalStatus released = m_router.release(adapterId);
    emit deviceChanged(descriptor, QStringLiteral("closed"));
    return !status.ok() ? status : released;
}

HalStatus HalService::resetDevice(const SessionId& sessionId,
                                  const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("resetDevice");
    HalDevice* device = sessionDevice(sessionId);
    if (device == nullptr) {
        const HalStatus status = makeError(HalStatusCode::NotFound,
                                           QStringLiteral("hal.resetDevice"),
                                           QStringLiteral("Session not found"),
                                           {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        emitOperationLog(operation, options, timer.elapsed(), status, {}, sessionId);
        return status;
    }
    const HalStatus status = device->reset(options);
    if (status.ok()) {
        emit deviceChanged(device->descriptor(), QStringLiteral("reset"));
    }
    return status;
}

HalStatus HalService::healthCheck(const SessionId& sessionId,
                                  const OperationOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    const QString operation = QStringLiteral("healthCheck");
    HalDevice* device = sessionDevice(sessionId);
    if (device == nullptr) {
        const HalStatus status = makeError(HalStatusCode::NotFound,
                                           QStringLiteral("hal.healthCheck"),
                                           QStringLiteral("Session not found"),
                                           {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        emitOperationLog(operation, options, timer.elapsed(), status, {}, sessionId);
        return status;
    }
    return device->healthCheck(options);
}

HalResult<IHalDevice*> HalService::device(const SessionId& sessionId)
{
    HalResult<IHalDevice*> result;
    HalDevice* devicePtr = sessionDevice(sessionId);
    if (devicePtr == nullptr) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("hal.device"),
                                  QStringLiteral("Session not found"),
                                  {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        return result;
    }
    result.value = devicePtr;
    return result;
}

} // namespace hwtest::hal
