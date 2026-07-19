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

} // namespace

ControlChannelManager::ControlChannelManager() = default;

ControlChannelManager::~ControlChannelManager() = default;

HalStatus ControlChannelManager::open(const ResourceBinding& binding,
                                      const OperationOptions& options)
{
    const QString operation = QStringLiteral("control.openControl");
    if (m_provider != nullptr) {
        return makeError(HalStatusCode::Busy,
                         operation,
                         QStringLiteral("A control channel is already open"),
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

    std::unique_ptr<ControlIoProvider> provider;
    if (providerId == QStringLiteral("qt.serial")) {
        provider = std::make_unique<QtSerialControlProvider>();
    } else if (providerId == QStringLiteral("qt.udp")) {
        provider = std::make_unique<QtUdpControlProvider>();
    } else {
        return makeError(HalStatusCode::NotSupported,
                         operation,
                         QStringLiteral("Control resource providerId is not supported"),
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

    m_openBinding = binding;
    m_provider = std::move(provider);
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

    HalStatus status = withBindingContext(m_provider->close(options), binding, operation);
    m_provider.reset();
    m_openBinding = {};
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
    return withBindingContext(m_provider->write(data, options), binding, operation);
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
    result = m_provider->read(maxBytes, options);
    result.status = withBindingContext(result.status, binding, operation);
    return result;
}

HalStatus ControlChannelManager::closeAll(const OperationOptions& options)
{
    if (m_provider == nullptr) {
        return {};
    }

    HalStatus status = withBindingContext(m_provider->close(options),
                                          m_openBinding,
                                          QStringLiteral("control.closeControl"));
    m_provider.reset();
    m_openBinding = {};
    return status;
}

HalStatus ControlChannelManager::ensureOpenFor(const ResourceBinding& binding,
                                                const QString& operation) const
{
    if (m_provider != nullptr && m_openBinding.resourceId == binding.resourceId) {
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
