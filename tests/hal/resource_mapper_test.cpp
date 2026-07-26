#include "resource_mapper.h"

#include "test_support.h"

#include <gtest/gtest.h>

using namespace hwtest::hal;

TEST(ResourceMapperTest, LoadsDefaultMockDeviceWhenConfigIsEmpty)
{
    ResourceMapper mapper;

    EXPECT_TRUE(mapper.load(QVariantMap{}));
    EXPECT_EQ(mapper.devices().size(), 1);
    EXPECT_EQ(mapper.devices().first().deviceId, QStringLiteral("mock_device_0"));
    EXPECT_TRUE(mapper.hasResource(QStringLiteral("AD_MAIN_0")));
    EXPECT_TRUE(mapper.hasResource(QStringLiteral("CANFD_A")));
}

TEST(ResourceMapperTest, LoadsCustomDeviceResourcesAndSafeState)
{
    QVariantMap config = testsupport::defaultHalConfig();
    QVariantMap safeState;
    safeState.insert(QStringLiteral("DA_MAIN_0"), 1.25);
    config.insert(QStringLiteral("safeState"), safeState);

    ResourceMapper mapper;
    EXPECT_TRUE(mapper.load(config));

    const DeviceDescriptor device = mapper.deviceDescriptor(QStringLiteral("main_daq"));
    EXPECT_EQ(device.deviceId, QStringLiteral("main_daq"));
    EXPECT_EQ(device.adapterId, QStringLiteral("mock.adapter.v1"));
    EXPECT_EQ(device.vendor, QStringLiteral("MockVendor"));
    EXPECT_EQ(device.properties.value(QStringLiteral("match")).toMap().value(QStringLiteral("serialNumber")).toString(),
              QStringLiteral("DAQ-001"));

    const ResourceBinding binding = mapper.binding(QStringLiteral("DA_MAIN_0"));
    EXPECT_EQ(binding.deviceId, QStringLiteral("main_daq"));
    EXPECT_EQ(binding.module, QStringLiteral("analog"));
    EXPECT_EQ(binding.direction, QStringLiteral("output"));
    EXPECT_EQ(binding.physicalIndex, 0);
}

TEST(ResourceMapperTest, BuildsCapabilitiesForDevice)
{
    ResourceMapper mapper;
    EXPECT_TRUE(mapper.load(testsupport::defaultHalConfig()));

    const DeviceCapabilities capabilities = mapper.capabilities(QStringLiteral("main_daq"));
    EXPECT_EQ(capabilities.channels.size(), 6);
    EXPECT_TRUE(capabilities.supportedModules.contains(QStringLiteral("analog")));
    EXPECT_TRUE(capabilities.supportedModules.contains(QStringLiteral("digital")));
}

TEST(ResourceMapperTest, ReturnsSafeState)
{
    QVariantMap config = testsupport::defaultHalConfig();
    QVariantMap safeState;
    safeState.insert(QStringLiteral("DA_MAIN_0"), 1.25);
    config.insert(QStringLiteral("safeState"), safeState);

    ResourceMapper mapper;
    EXPECT_TRUE(mapper.load(config));

    EXPECT_EQ(mapper.safeState().value(QStringLiteral("DA_MAIN_0")).toDouble(), 1.25);
}

TEST(ResourceMapperTest, InheritsDeviceAdapterForResources)
{
    QVariantMap config = testsupport::defaultHalConfig();
    QVariantMap hardware = config.value(QStringLiteral("hardware")).toMap();
    QVariantList devices = hardware.value(QStringLiteral("devices")).toList();
    QVariantMap device = devices.first().toMap();
    device.insert(QStringLiteral("adapterId"), QStringLiteral("vendor.fixture"));
    devices[0] = device;
    hardware.insert(QStringLiteral("devices"), devices);
    config.insert(QStringLiteral("hardware"), hardware);

    ResourceMapper mapper;
    ASSERT_TRUE(mapper.load(config));
    EXPECT_EQ(mapper.binding(QStringLiteral("DO_POWER_EN")).adapterId,
              QStringLiteral("vendor.fixture"));
}

TEST(ResourceMapperTest, RejectsUnknownDeviceAdapterMismatchAndDuplicatePhysicalChannel)
{
    QVariantMap unknown = testsupport::defaultHalConfig();
    QVariantMap hardware = unknown.value(QStringLiteral("hardware")).toMap();
    QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    QVariantMap output = resources.value(QStringLiteral("DO_POWER_EN")).toMap();
    output.insert(QStringLiteral("device"), QStringLiteral("missing-device"));
    resources.insert(QStringLiteral("DO_POWER_EN"), output);
    hardware.insert(QStringLiteral("resources"), resources);
    unknown.insert(QStringLiteral("hardware"), hardware);
    ResourceMapper mapper;
    EXPECT_FALSE(mapper.load(unknown));

    QVariantMap mismatch = testsupport::defaultHalConfig();
    hardware = mismatch.value(QStringLiteral("hardware")).toMap();
    resources = hardware.value(QStringLiteral("resources")).toMap();
    output = resources.value(QStringLiteral("DO_POWER_EN")).toMap();
    output.insert(QStringLiteral("adapterId"), QStringLiteral("wrong.adapter"));
    resources.insert(QStringLiteral("DO_POWER_EN"), output);
    hardware.insert(QStringLiteral("resources"), resources);
    mismatch.insert(QStringLiteral("hardware"), hardware);
    EXPECT_FALSE(mapper.load(mismatch));

    QVariantMap duplicate = testsupport::defaultHalConfig();
    hardware = duplicate.value(QStringLiteral("hardware")).toMap();
    resources = hardware.value(QStringLiteral("resources")).toMap();
    resources.insert(QStringLiteral("DO_POWER_EN_COPY"),
                     resources.value(QStringLiteral("DO_POWER_EN")));
    hardware.insert(QStringLiteral("resources"), resources);
    duplicate.insert(QStringLiteral("hardware"), hardware);
    EXPECT_FALSE(mapper.load(duplicate));
}
