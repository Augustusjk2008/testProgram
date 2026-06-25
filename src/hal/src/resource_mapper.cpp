#include "resource_mapper.h"

namespace hwtest::hal {

static QString readString(const QVariantMap& map, const QString& key, const QString& fallback = {})
{
    const QVariant value = map.value(key);
    return value.isValid() ? value.toString() : fallback;
}

static int readInt(const QVariantMap& map, const QString& key, int fallback = -1)
{
    const QVariant value = map.value(key);
    return value.isValid() ? value.toInt() : fallback;
}

static QVariantMap readMap(const QVariantMap& map, const QString& key)
{
    return map.value(key).toMap();
}

bool ResourceMapper::load(const QVariantMap& halConfig)
{
    m_devices.clear();
    m_bindings.clear();
    m_deviceIndexById.clear();
    m_safeState.clear();

    const QVariantMap hardwareMap = halConfig.value(QStringLiteral("hardware")).toMap();
    const QVariantList deviceList = hardwareMap.value(QStringLiteral("devices")).toList();
    const QVariantMap resourceMap = hardwareMap.value(QStringLiteral("resources")).toMap();

    QHash<QString, QVariantMap> deviceLookup;
    for (int index = 0; index < deviceList.size(); ++index) {
        const QVariantMap deviceMap = deviceList.at(index).toMap();
        DeviceDescriptor descriptor = parseDeviceDescriptor(deviceMap, index);
        m_deviceIndexById.insert(descriptor.deviceId, m_devices.size());
        m_devices.push_back(descriptor);
        deviceLookup.insert(descriptor.deviceId, deviceMap);
    }

    for (auto it = resourceMap.constBegin(); it != resourceMap.constEnd(); ++it) {
        const ResourceBinding binding = parseResourceBinding(it.key(), it.value().toMap(), deviceLookup);
        m_bindings.push_back(binding);
    }

    m_safeState = halConfig.value(QStringLiteral("safeState")).toMap();

    if (m_devices.isEmpty()) {
        DeviceDescriptor descriptor;
        descriptor.deviceId = QStringLiteral("mock_device_0");
        descriptor.adapterId = QStringLiteral("mock.adapter.v1");
        descriptor.vendor = QStringLiteral("MockVendor");
        descriptor.model = QStringLiteral("MockDevice");
        m_deviceIndexById.insert(descriptor.deviceId, m_devices.size());
        m_devices.push_back(descriptor);
    }

    if (m_bindings.isEmpty()) {
        const DeviceDescriptor& device = m_devices.first();
        const QString deviceId = device.deviceId;
        const QString adapterId = device.adapterId.isEmpty() ? QStringLiteral("mock.adapter.v1") : device.adapterId;
        const QVector<ResourceBinding> defaults = {
            {QStringLiteral("AD_MAIN_0"), deviceId, adapterId, QStringLiteral("analog"), QStringLiteral("input"), 0, {}},
            {QStringLiteral("DA_MAIN_0"), deviceId, adapterId, QStringLiteral("analog"), QStringLiteral("output"), 0, {}},
            {QStringLiteral("DI_POWER_OK"), deviceId, adapterId, QStringLiteral("digital"), QStringLiteral("input"), 0, {}},
            {QStringLiteral("DO_POWER_EN"), deviceId, adapterId, QStringLiteral("digital"), QStringLiteral("output"), 0, {}},
            {QStringLiteral("SERIAL_A"), deviceId, adapterId, QStringLiteral("serial"), QStringLiteral("bidirectional"), 0, {}},
            {QStringLiteral("CANFD_A"), deviceId, adapterId, QStringLiteral("canfd"), QStringLiteral("bidirectional"), 0, {}}
        };
        for (const ResourceBinding& binding : defaults) {
            m_bindings.push_back(binding);
        }
    }

    return true;
}

QVector<DeviceDescriptor> ResourceMapper::devices() const
{
    return m_devices;
}

QVector<ResourceBinding> ResourceMapper::bindingsForDevice(const DeviceId& deviceId) const
{
    QVector<ResourceBinding> result;
    for (const ResourceBinding& binding : m_bindings) {
        if (binding.deviceId == deviceId) {
            result.push_back(binding);
        }
    }
    return result;
}

ResourceBinding ResourceMapper::binding(const ResourceId& resourceId) const
{
    for (const ResourceBinding& binding : m_bindings) {
        if (binding.resourceId == resourceId) {
            return binding;
        }
    }
    return {};
}

bool ResourceMapper::hasResource(const ResourceId& resourceId) const
{
    for (const ResourceBinding& binding : m_bindings) {
        if (binding.resourceId == resourceId) {
            return true;
        }
    }
    return false;
}

DeviceDescriptor ResourceMapper::deviceDescriptor(const DeviceId& deviceId) const
{
    const auto index = m_deviceIndexById.value(deviceId, -1);
    if (index >= 0 && index < m_devices.size()) {
        return m_devices.at(index);
    }
    return {};
}

DeviceCapabilities ResourceMapper::capabilities(const DeviceId& deviceId) const
{
    DeviceCapabilities capabilities;
    capabilities.device = deviceDescriptor(deviceId);
    const QVector<ResourceBinding> bindings = bindingsForDevice(deviceId);
    QStringList modules;
    for (const ResourceBinding& binding : bindings) {
        capabilities.channels.push_back(channelFromBinding(binding));
        if (!modules.contains(binding.module)) {
            modules.push_back(binding.module);
        }
    }
    capabilities.supportedModules = modules;
    capabilities.limits.insert(QStringLiteral("analog.maxSampleRateHz"), 100000);
    capabilities.limits.insert(QStringLiteral("canfd.maxPayloadBytes"), 64);
    return capabilities;
}

QVariantMap ResourceMapper::safeState() const
{
    return m_safeState;
}

DeviceDescriptor ResourceMapper::parseDeviceDescriptor(const QVariantMap& deviceMap, int index)
{
    DeviceDescriptor descriptor;
    descriptor.deviceId = readString(deviceMap, QStringLiteral("alias"), QStringLiteral("mock_device_%1").arg(index));
    descriptor.adapterId = readString(deviceMap, QStringLiteral("adapterId"), QStringLiteral("mock.adapter.v1"));
    descriptor.vendor = readString(deviceMap, QStringLiteral("vendor"), QStringLiteral("MockVendor"));
    descriptor.model = readString(deviceMap, QStringLiteral("model"), QStringLiteral("MockDevice"));
    descriptor.serialNumber = readString(deviceMap, QStringLiteral("serialNumber"));
    descriptor.location = readString(deviceMap, QStringLiteral("location"));
    descriptor.firmwareVersion = readString(deviceMap, QStringLiteral("firmwareVersion"));
    descriptor.properties = deviceMap.value(QStringLiteral("properties")).toMap();
    const QVariantMap match = readMap(deviceMap, QStringLiteral("match"));
    if (!match.isEmpty()) {
        descriptor.properties.insert(QStringLiteral("match"), match);
    }
    return descriptor;
}

ResourceBinding ResourceMapper::parseResourceBinding(const QString& resourceId,
                                                     const QVariantMap& resourceMap,
                                                     const QHash<QString, QVariantMap>& deviceLookup)
{
    ResourceBinding binding;
    binding.resourceId = resourceId;
    binding.deviceId = readString(resourceMap, QStringLiteral("device"));
    binding.adapterId = readString(resourceMap, QStringLiteral("adapterId"), QStringLiteral("mock.adapter.v1"));
    binding.module = readString(resourceMap, QStringLiteral("module"));
    binding.direction = readString(resourceMap, QStringLiteral("direction"), QStringLiteral("bidirectional"));
    binding.physicalIndex = readInt(resourceMap, QStringLiteral("physicalIndex"), 0);
    binding.properties = resourceMap.value(QStringLiteral("properties")).toMap();

    if (!deviceLookup.contains(binding.deviceId) && !deviceLookup.isEmpty()) {
        binding.deviceId = deviceLookup.constBegin().key();
    }

    if (binding.adapterId.isEmpty()) {
        binding.adapterId = QStringLiteral("mock.adapter.v1");
    }

    return binding;
}

ChannelDescriptor ResourceMapper::channelFromBinding(const ResourceBinding& binding)
{
    ChannelDescriptor channel;
    channel.resourceId = binding.resourceId;
    channel.module = binding.module;
    channel.direction = binding.direction;
    channel.physicalIndex = binding.physicalIndex;
    channel.properties = binding.properties;
    return channel;
}

QString ResourceMapper::moduleMaskKey(const QString& module)
{
    return module.trimmed().toLower();
}

} // namespace hwtest::hal
