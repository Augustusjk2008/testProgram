#pragma once

#include "resource_mapper.h"

namespace hwtest::hal {

struct AdapterDeviceOpenSpec {
    int version = 1;
    DeviceDescriptor device;
    QString physicalDeviceId;
    QVector<ResourceBinding> channels;
    QVariantMap safeState;
    QVariantList taskProfiles;

    static AdapterDeviceOpenSpec fromResourceMapper(const ResourceMapper& mapper,
                                                     const DeviceId& deviceId);

    QVariantMap toVariantMap() const;
};

} // namespace hwtest::hal
