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

static bool hasLibraryPath(const QVariantMap& config)
{
    const QVariantMap adapterConfig = config.value(QStringLiteral("adapter")).toMap();
    if (adapterConfig.contains(QStringLiteral("libraryPath"))) {
        return !adapterConfig.value(QStringLiteral("libraryPath")).toString().isEmpty();
    }
    return !config.value(QStringLiteral("adapterLibraryPath")).toString().isEmpty();
}

static QVariantMap adapterConfig(const QVariantMap& config)
{
    QVariantMap result = config.value(QStringLiteral("adapter")).toMap();
    if (result.isEmpty()) {
        const QString libraryPath = config.value(QStringLiteral("adapterLibraryPath")).toString();
        if (!libraryPath.isEmpty()) {
            result.insert(QStringLiteral("libraryPath"), libraryPath);
        }
    }
    return result;
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

std::unique_ptr<HardwareAdapter> HalService::createBackend(const QVariantMap& halConfig)
{
    // Keep the default path in-process so the HAL remains usable without vendor binaries.
    Q_UNUSED(halConfig)
    return std::make_unique<CAbiAdapter>();
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
    m_mapper.load(halConfig);
    m_backend = createBackend(halConfig);
    if (m_backend == nullptr) {
        return makeError(HalStatusCode::InternalError,
                         QStringLiteral("hal.initialize"),
                         QStringLiteral("Unable to create backend adapter"));
    }

    const HalStatus status = m_backend->initialize(halConfig);
    if (!status.ok()) {
        m_backend.reset();
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
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it.value().device) {
            it.value().device->close();
        }
    }
    m_sessions.clear();
    if (m_backend != nullptr) {
        m_backend->shutdown();
        m_backend.reset();
    }
    m_initialized = false;
    m_config.clear();
    return HalStatus{};
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
    if (m_backend == nullptr) {
        result.status = makeError(HalStatusCode::InternalError,
                                  QStringLiteral("hal.openDevice"),
                                  QStringLiteral("Backend adapter is missing"),
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

    const HalResult<SessionId> backendSession = m_backend->openDevice(deviceId, m_config, options);
    if (!backendSession.ok()) {
        result.status = backendSession.status;
        emitOperationLog(operation, options, timer.elapsed(), result.status, deviceId);
        return result;
    }

    SessionEntry entry;
    entry.descriptor = descriptor;
    entry.device = std::make_unique<HalDevice>(m_backend.get(),
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
    m_sessions.insert(backendSession.value, std::move(entry));
    emit deviceChanged(descriptor, QStringLiteral("opened"));
    result.value = backendSession.value;
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
    const HalStatus status = it.value().device ? it.value().device->close(options) : HalStatus{};
    m_sessions.erase(it);
    emit deviceChanged(descriptor, QStringLiteral("closed"));
    return status;
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
