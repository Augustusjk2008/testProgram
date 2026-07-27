#include "adapter_device_open_spec.h"

#include <QSet>

namespace hwtest::hal {

namespace {

QString physicalDeviceIdFor(const DeviceDescriptor& device)
{
    const QVariantMap properties = device.properties;
    const QVariantMap vendor = properties.value(QStringLiteral("vendor")).toMap();
    const QVariantMap ni = vendor.value(QStringLiteral("ni")).toMap();
    const QString niDeviceName = ni.value(QStringLiteral("deviceName")).toString().trimmed();
    if (!niDeviceName.isEmpty()) {
        return niDeviceName;
    }

    const QString configuredDeviceId = properties.value(QStringLiteral("physicalDeviceId"))
                                           .toString()
                                           .trimmed();
    if (!configuredDeviceId.isEmpty()) {
        return configuredDeviceId;
    }

    const QString legacyDeviceName = properties.value(QStringLiteral("deviceName"))
                                         .toString()
                                         .trimmed();
    return legacyDeviceName.isEmpty() ? device.deviceId : legacyDeviceName;
}

QVariantMap deviceToVariantMap(const DeviceDescriptor& device)
{
    QVariantMap result;
    result.insert(QStringLiteral("deviceId"), device.deviceId);
    result.insert(QStringLiteral("adapterId"), device.adapterId);
    result.insert(QStringLiteral("vendor"), device.vendor);
    result.insert(QStringLiteral("model"), device.model);
    result.insert(QStringLiteral("serialNumber"), device.serialNumber);
    result.insert(QStringLiteral("location"), device.location);
    result.insert(QStringLiteral("firmwareVersion"), device.firmwareVersion);
    result.insert(QStringLiteral("properties"), device.properties);
    return result;
}

QVariantMap channelToVariantMap(const ResourceBinding& channel)
{
    QVariantMap result;
    result.insert(QStringLiteral("resourceId"), channel.resourceId);
    result.insert(QStringLiteral("deviceId"), channel.deviceId);
    result.insert(QStringLiteral("adapterId"), channel.adapterId);
    result.insert(QStringLiteral("module"), channel.module);
    result.insert(QStringLiteral("direction"), channel.direction);
    result.insert(QStringLiteral("physicalIndex"), channel.physicalIndex);
    result.insert(QStringLiteral("properties"), channel.properties);
    result.insert(QStringLiteral("providerId"), channel.providerId);
    return result;
}

} // namespace

AdapterDeviceOpenSpec AdapterDeviceOpenSpec::fromResourceMapper(const ResourceMapper& mapper,
                                                                 const DeviceId& deviceId)
{
    AdapterDeviceOpenSpec spec;
    spec.device = mapper.deviceDescriptor(deviceId);
    spec.physicalDeviceId = physicalDeviceIdFor(spec.device);
    spec.channels = mapper.bindingsForDevice(deviceId);
    spec.taskProfiles = spec.device.properties.value(QStringLiteral("taskProfiles")).toList();

    QSet<ResourceId> resourceIds;
    for (const ResourceBinding& channel : spec.channels) {
        resourceIds.insert(channel.resourceId);
    }
    const QVariantMap configuredSafeState = mapper.safeState();
    for (auto it = configuredSafeState.cbegin(); it != configuredSafeState.cend(); ++it) {
        if (resourceIds.contains(it.key())) {
            spec.safeState.insert(it.key(), it.value());
        }
    }
    return spec;
}

QVariantMap AdapterDeviceOpenSpec::toVariantMap() const
{
    QVariantMap result;
    result.insert(QStringLiteral("schema"), QStringLiteral("hwtest.adapter-device-open"));
    result.insert(QStringLiteral("version"), version);
    result.insert(QStringLiteral("device"), deviceToVariantMap(device));
    result.insert(QStringLiteral("physicalDeviceId"), physicalDeviceId);

    QVariantList channelList;
    channelList.reserve(channels.size());
    for (const ResourceBinding& channel : channels) {
        channelList.push_back(channelToVariantMap(channel));
    }
    result.insert(QStringLiteral("channels"), channelList);
    result.insert(QStringLiteral("safeState"), safeState);
    result.insert(QStringLiteral("taskProfiles"), taskProfiles);
    return result;
}

} // namespace hwtest::hal
