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
