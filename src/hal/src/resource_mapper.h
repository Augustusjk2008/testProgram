#pragma once

#include "hal/hal_types.h"

#include <QHash>

namespace hwtest::hal {

struct ResourceBinding {
    ResourceId resourceId;
    DeviceId deviceId;
    AdapterId adapterId;
    QString module;
    QString direction;
    int physicalIndex = -1;
    QVariantMap properties;
};

class ResourceMapper {
public:
    bool load(const QVariantMap& halConfig);

    QVector<DeviceDescriptor> devices() const;
    QVector<ResourceBinding> bindingsForDevice(const DeviceId& deviceId) const;
    ResourceBinding binding(const ResourceId& resourceId) const;
    bool hasResource(const ResourceId& resourceId) const;
    DeviceDescriptor deviceDescriptor(const DeviceId& deviceId) const;
    DeviceCapabilities capabilities(const DeviceId& deviceId) const;
    QVariantMap safeState() const;

private:
    static DeviceDescriptor parseDeviceDescriptor(const QVariantMap& deviceMap,
                                                  int index);
    static ResourceBinding parseResourceBinding(const QString& resourceId,
                                                const QVariantMap& resourceMap,
                                                const QHash<QString, QVariantMap>& deviceLookup);
    static ChannelDescriptor channelFromBinding(const ResourceBinding& binding);
    static QString moduleMaskKey(const QString& module);

    QVector<DeviceDescriptor> m_devices;
    QVector<ResourceBinding> m_bindings;
    QHash<QString, int> m_deviceIndexById;
    QVariantMap m_safeState;
};

} // namespace hwtest::hal
