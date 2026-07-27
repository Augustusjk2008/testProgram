#include "adapter_router.h"

#include "c_abi_adapter.h"
#include "hal_error_mapper.h"
#include "mock_adapter.h"

#include <QSet>

namespace hwtest::hal {

namespace {

bool usesMockBackend(const AdapterId& adapterId)
{
    return adapterId.startsWith(QStringLiteral("mock.")) ||
        adapterId == QStringLiteral("mock.adapter.v1");
}

} // namespace

AdapterRouter::~AdapterRouter()
{
    shutdown();
}

QVariantMap AdapterRouter::configForAdapter(const AdapterId& adapterId,
                                            const ResourceMapper& mapper) const
{
    QVariantMap result = m_halConfig.value(QStringLiteral("adapters")).toMap()
                             .value(adapterId).toMap();
    if (result.isEmpty()) {
        const QVariantMap legacy = m_halConfig.value(QStringLiteral("adapter")).toMap();
        if (!legacy.isEmpty()) result = legacy;
    }
    result.insert(QStringLiteral("adapterId"), adapterId);
    if (!usesMockBackend(adapterId)) {
        return result;
    }
    if (m_halConfig.contains(QStringLiteral("mock"))) {
        result.insert(QStringLiteral("mock"), m_halConfig.value(QStringLiteral("mock")));
    }

    QVariantMap hardware;
    QVariantList devices;
    QSet<DeviceId> deviceIds;
    const QVariantList configuredDevices = m_halConfig.value(QStringLiteral("hardware")).toMap()
                                               .value(QStringLiteral("devices")).toList();
    for (const QVariant& value : configuredDevices) {
        const QVariantMap device = value.toMap();
        const DeviceId deviceId = device.value(QStringLiteral("alias")).toString();
        if (mapper.deviceDescriptor(deviceId).adapterId == adapterId) {
            devices.push_back(device);
            deviceIds.insert(deviceId);
        }
    }
    hardware.insert(QStringLiteral("devices"), devices);

    QVariantMap resources;
    QVariantMap safeState;
    const QVariantMap configuredResources = m_halConfig.value(QStringLiteral("hardware")).toMap()
                                                 .value(QStringLiteral("resources")).toMap();
    const QVariantMap configuredSafeState = m_halConfig.value(QStringLiteral("safeState")).toMap();
    for (auto it = configuredResources.cbegin(); it != configuredResources.cend(); ++it) {
        const ResourceBinding binding = mapper.binding(it.key());
        if (binding.adapterId != adapterId || !deviceIds.contains(binding.deviceId)) continue;
        resources.insert(it.key(), it.value());
        if (configuredSafeState.contains(it.key())) {
            safeState.insert(it.key(), configuredSafeState.value(it.key()));
        }
    }
    hardware.insert(QStringLiteral("resources"), resources);
    result.insert(QStringLiteral("hardware"), hardware);
    result.insert(QStringLiteral("safeState"), safeState);
    return result;
}

std::shared_ptr<HardwareAdapter> AdapterRouter::createBackend(
    const AdapterId& adapterId,
    const QVariantMap& config) const
{
    if (usesMockBackend(adapterId)) {
        return std::make_shared<MockAdapter>();
    }
    const QString providerId = config.value(QStringLiteral("providerId")).toString().trimmed();
    if (providerId == QStringLiteral("vendor.cabi") ||
        !config.value(QStringLiteral("libraryPath")).toString().trimmed().isEmpty()) {
        return std::make_shared<CAbiAdapter>();
    }
    return {};
}

HalStatus AdapterRouter::configure(const QVariantMap& halConfig,
                                   const ResourceMapper& mapper)
{
    shutdown();
    m_halConfig = halConfig;
    for (const DeviceDescriptor& device : mapper.devices()) {
        if (device.adapterId.trimmed().isEmpty()) {
            return makeError(HalStatusCode::InvalidArgument,
                             QStringLiteral("hal.adapterRouter.configure"),
                             QStringLiteral("Device adapterId must not be empty"),
                             device.deviceId);
        }
        if (!m_entries.contains(device.adapterId)) {
            Entry entry;
            entry.config = configForAdapter(device.adapterId, mapper);
            m_entries.insert(device.adapterId, entry);
        }
    }
    return {};
}

HalResult<HardwareAdapter*> AdapterRouter::acquire(const AdapterId& adapterId)
{
    HalResult<HardwareAdapter*> result;
    auto it = m_entries.find(adapterId);
    if (it == m_entries.end()) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("hal.adapterRouter.acquire"),
                                  QStringLiteral("Adapter is not configured"),
                                  {}, {}, adapterId);
        return result;
    }
    if (!it->backend) {
        it->backend = createBackend(adapterId, it->config);
        if (!it->backend) {
            result.status = makeError(HalStatusCode::NotSupported,
                                      QStringLiteral("hal.adapterRouter.acquire"),
                                      QStringLiteral("No backend is available for adapter '%1'")
                                          .arg(adapterId),
                                      {}, {}, adapterId);
            return result;
        }
        const HalStatus initialized = it->backend->initialize(it->config);
        if (!initialized.ok()) {
            it->backend.reset();
            result.status = initialized;
            return result;
        }
    }
    ++it->leases;
    result.value = it->backend.get();
    return result;
}

HalStatus AdapterRouter::release(const AdapterId& adapterId)
{
    auto it = m_entries.find(adapterId);
    if (it == m_entries.end()) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("hal.adapterRouter.release"),
                         QStringLiteral("Adapter is not configured"),
                         {}, {}, adapterId);
    }
    if (it->leases > 0) --it->leases;
    if (it->leases == 0 && it->backend) {
        const HalStatus status = it->backend->shutdown();
        it->backend.reset();
        return status;
    }
    return {};
}

HalStatus AdapterRouter::shutdown()
{
    HalStatus firstError;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->backend) {
            const HalStatus status = it->backend->shutdown();
            if (!status.ok() && firstError.ok()) firstError = status;
            it->backend.reset();
        }
        it->leases = 0;
    }
    m_entries.clear();
    m_halConfig.clear();
    return firstError;
}

} // namespace hwtest::hal
