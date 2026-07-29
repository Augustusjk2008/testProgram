#include "control_channel_manager.h"

#include "control_io_provider.h"
#include "hal_error_mapper.h"
#include "qt_serial_control_provider.h"
#include "qt_udp_control_provider.h"

#include <utility>

namespace hwtest::hal {

namespace {

QVariantMap providerDetail(const QString& providerId)
{
    QVariantMap detail;
    detail.insert(QStringLiteral("providerId"), providerId);
    return detail;
}

QString normalizedSerialPortName(const QVariantMap& properties)
{
    const QString portName = properties.value(QStringLiteral("portName")).toString().trimmed();
#ifdef Q_OS_WIN
    return portName.toCaseFolded();
#else
    return portName;
#endif
}

} // namespace

ControlChannelManager::ControlChannelManager() = default;

ControlChannelManager::ControlChannelManager(ProviderFactory providerFactory)
    : m_providerFactory(std::move(providerFactory))
{
}

ControlChannelManager::~ControlChannelManager() = default;

HalStatus ControlChannelManager::open(const ResourceBinding& binding,
                                      const OperationOptions& options)
{
    const QString operation = QStringLiteral("control.openControl");
    if (m_openSessions.find(binding.resourceId) != m_openSessions.cend()) {
        return makeError(HalStatusCode::Busy,
                         operation,
                         QStringLiteral("Control resource is already open"),
                         binding.deviceId,
                         binding.resourceId,
                         {},
                         providerDetail(binding.providerId));
    }

    const QString providerId = binding.providerId.trimmed();
    if (providerId.isEmpty()) {
        return makeError(HalStatusCode::InvalidArgument,
                         operation,
                         QStringLiteral("Control resource requires an explicit providerId"),
                         binding.deviceId,
                         binding.resourceId,
                         {},
                         providerDetail(binding.providerId));
    }

    if (providerId != QStringLiteral("qt.serial") && providerId != QStringLiteral("qt.udp")) {
        return makeError(HalStatusCode::NotSupported,
                         operation,
                         QStringLiteral("Control resource providerId is not supported"),
                         binding.deviceId,
                         binding.resourceId,
                         {},
                         providerDetail(binding.providerId));
    }

    const ResourceId conflictingResourceId = openSerialPortOwner(binding);
    if (!conflictingResourceId.isEmpty()) {
        QVariantMap detail = providerDetail(binding.providerId);
        detail.insert(QStringLiteral("portName"),
                      binding.properties.value(QStringLiteral("portName")).toString().trimmed());
        detail.insert(QStringLiteral("conflictingResourceId"), conflictingResourceId);
        return makeError(HalStatusCode::Busy,
                         operation,
                         QStringLiteral("A control resource already owns the requested serial port"),
                         binding.deviceId,
                         binding.resourceId,
                         {},
                         detail);
    }

    std::unique_ptr<ControlIoProvider> provider;
    if (m_providerFactory) {
        provider = m_providerFactory(binding);
    } else if (providerId == QStringLiteral("qt.serial")) {
        provider = std::make_unique<QtSerialControlProvider>();
    } else {
        provider = std::make_unique<QtUdpControlProvider>();
    }
    if (provider == nullptr) {
        return makeError(HalStatusCode::InternalError,
                         operation,
                         QStringLiteral("Control provider factory did not create a provider"),
                         binding.deviceId,
                         binding.resourceId,
                         {},
                         providerDetail(binding.providerId));
    }

    HalStatus status = withBindingContext(provider->open(binding.properties, options),
                                          binding,
                                          operation);
    if (!status.ok()) {
        return status;
    }

    OpenControlSession session;
    session.binding = binding;
    session.provider = std::move(provider);
    m_openSessions.emplace(binding.resourceId, std::move(session));
    return {};
}

HalStatus ControlChannelManager::close(const ResourceBinding& binding,
                                       const OperationOptions& options)
{
    const QString operation = QStringLiteral("control.closeControl");
    const HalStatus openStatus = ensureOpenFor(binding, operation);
    if (!openStatus.ok()) {
        return openStatus;
    }

    const auto sessionIt = m_openSessions.find(binding.resourceId);
    HalStatus status = withBindingContext(sessionIt->second.provider->close(options), binding, operation);
    m_openSessions.erase(sessionIt);
    return status;
}

HalStatus ControlChannelManager::write(const ResourceBinding& binding,
                                       const QByteArray& data,
                                       const OperationOptions& options)
{
    const QString operation = QStringLiteral("control.writeControl");
    const HalStatus openStatus = ensureOpenFor(binding, operation);
    if (!openStatus.ok()) {
        return openStatus;
    }
    const auto sessionIt = m_openSessions.find(binding.resourceId);
    return withBindingContext(sessionIt->second.provider->write(data, options), binding, operation);
}

HalResult<QByteArray> ControlChannelManager::read(const ResourceBinding& binding,
                                                   int maxBytes,
                                                   const OperationOptions& options)
{
    const QString operation = QStringLiteral("control.readControl");
    HalResult<QByteArray> result;
    const HalStatus openStatus = ensureOpenFor(binding, operation);
    if (!openStatus.ok()) {
        result.status = openStatus;
        return result;
    }
    const auto sessionIt = m_openSessions.find(binding.resourceId);
    result = sessionIt->second.provider->read(maxBytes, options);
    result.status = withBindingContext(result.status, binding, operation);
    return result;
}

HalStatus ControlChannelManager::closeAll(const OperationOptions& options)
{
    HalStatus firstFailure;
    for (auto sessionIt = m_openSessions.begin(); sessionIt != m_openSessions.end();) {
        const ResourceBinding binding = sessionIt->second.binding;
        const HalStatus status = withBindingContext(sessionIt->second.provider->close(options),
                                                     binding,
                                                     QStringLiteral("control.closeControl"));
        sessionIt = m_openSessions.erase(sessionIt);
        if (firstFailure.ok() && !status.ok()) {
            firstFailure = status;
        }
    }
    return firstFailure;
}

HalStatus ControlChannelManager::ensureOpenFor(const ResourceBinding& binding,
                                                const QString& operation) const
{
    if (m_openSessions.find(binding.resourceId) != m_openSessions.cend()) {
        return {};
    }
    return makeError(HalStatusCode::InvalidState,
                     operation,
                     QStringLiteral("Control resource is not open"),
                     binding.deviceId,
                     binding.resourceId,
                     {},
                     providerDetail(binding.providerId));
}

ResourceId ControlChannelManager::openSerialPortOwner(const ResourceBinding& binding) const
{
    if (binding.providerId.trimmed() != QStringLiteral("qt.serial")) {
        return {};
    }

    const QString requestedPort = normalizedSerialPortName(binding.properties);
    if (requestedPort.isEmpty()) {
        return {};
    }

    for (const auto& entry : m_openSessions) {
        const ResourceBinding& openBinding = entry.second.binding;
        if (openBinding.providerId.trimmed() != QStringLiteral("qt.serial")) {
            continue;
        }
        const QString openPort = normalizedSerialPortName(openBinding.properties);
        if (!openPort.isEmpty() && openPort == requestedPort) {
            return openBinding.resourceId;
        }
    }
    return {};
}

HalStatus ControlChannelManager::withBindingContext(HalStatus status,
                                                     const ResourceBinding& binding,
                                                     const QString& fallbackOperation)
{
    if (status.ok()) {
        return status;
    }
    status.error.code = status.code;
    if (status.error.operation.isEmpty()) {
        status.error.operation = fallbackOperation;
    }
    if (status.error.deviceId.isEmpty()) {
        status.error.deviceId = binding.deviceId;
    }
    if (status.error.resourceId.isEmpty()) {
        status.error.resourceId = binding.resourceId;
    }
    if (!status.error.detail.contains(QStringLiteral("providerId"))) {
        status.error.detail.insert(QStringLiteral("providerId"), binding.providerId);
    }
    return status;
}

} // namespace hwtest::hal
