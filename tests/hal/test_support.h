#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace hwtest::hal::testsupport {

inline QVariantMap makeDevice(const QString& alias)
{
    QVariantMap device;
    device.insert(QStringLiteral("alias"), alias);
    device.insert(QStringLiteral("adapterId"), QStringLiteral("mock.adapter.v1"));
    device.insert(QStringLiteral("vendor"), QStringLiteral("MockVendor"));
    device.insert(QStringLiteral("model"), QStringLiteral("MockDevice"));
    device.insert(QStringLiteral("serialNumber"), QStringLiteral("DAQ-001"));
    device.insert(QStringLiteral("location"), QStringLiteral("rack-1"));
    device.insert(QStringLiteral("firmwareVersion"), QStringLiteral("1.0.0"));

    QVariantMap match;
    match.insert(QStringLiteral("serialNumber"), QStringLiteral("DAQ-001"));
    device.insert(QStringLiteral("match"), match);
    return device;
}

inline QVariantMap makeResource(const QString& device,
                                const QString& module,
                                const QString& direction,
                                int physicalIndex)
{
    QVariantMap resource;
    resource.insert(QStringLiteral("device"), device);
    resource.insert(QStringLiteral("module"), module);
    resource.insert(QStringLiteral("direction"), direction);
    resource.insert(QStringLiteral("physicalIndex"), physicalIndex);
    return resource;
}

inline QVariantMap defaultHalConfig()
{
    QVariantMap config;
    QVariantMap hardware;

    QVariantList devices;
    devices.push_back(makeDevice(QStringLiteral("main_daq")));
    hardware.insert(QStringLiteral("devices"), devices);

    QVariantMap resources;
    resources.insert(QStringLiteral("AD_MAIN_0"),
                     makeResource(QStringLiteral("main_daq"),
                                  QStringLiteral("analog"),
                                  QStringLiteral("input"),
                                  0));
    resources.insert(QStringLiteral("DA_MAIN_0"),
                     makeResource(QStringLiteral("main_daq"),
                                  QStringLiteral("analog"),
                                  QStringLiteral("output"),
                                  0));
    resources.insert(QStringLiteral("DI_POWER_OK"),
                     makeResource(QStringLiteral("main_daq"),
                                  QStringLiteral("digital"),
                                  QStringLiteral("input"),
                                  0));
    resources.insert(QStringLiteral("DO_POWER_EN"),
                     makeResource(QStringLiteral("main_daq"),
                                  QStringLiteral("digital"),
                                  QStringLiteral("output"),
                                  0));
    resources.insert(QStringLiteral("SERIAL_A"),
                     makeResource(QStringLiteral("main_daq"),
                                  QStringLiteral("serial"),
                                  QStringLiteral("bidirectional"),
                                  0));
    resources.insert(QStringLiteral("CANFD_A"),
                     makeResource(QStringLiteral("main_daq"),
                                  QStringLiteral("canfd"),
                                  QStringLiteral("bidirectional"),
                                  0));
    hardware.insert(QStringLiteral("resources"), resources);

    config.insert(QStringLiteral("hardware"), hardware);
    return config;
}

inline QVariantMap safeStateHalConfig()
{
    QVariantMap config = defaultHalConfig();
    QVariantMap safeState;
    safeState.insert(QStringLiteral("DA_MAIN_0"), 0.0);
    safeState.insert(QStringLiteral("DO_POWER_EN"), QStringLiteral("Low"));
    config.insert(QStringLiteral("safeState"), safeState);
    return config;
}

} // namespace hwtest::hal::testsupport
