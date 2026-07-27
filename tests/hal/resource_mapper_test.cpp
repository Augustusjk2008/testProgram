#include "adapter_device_open_spec.h"
#include "resource_mapper.h"

#include "test_support.h"

#include <gtest/gtest.h>

using namespace hwtest::hal;

namespace {

QVariantMap pxiProjectionConfig()
{
    QVariantMap config;
    QVariantMap hardware;

    QVariantMap niProperties;
    niProperties.insert(QStringLiteral("deviceName"), QStringLiteral("PXI1Slot2"));
    QVariantMap vendorProperties;
    vendorProperties.insert(QStringLiteral("ni"), niProperties);
    QVariantList taskProfiles;
    taskProfiles.push_back(QVariantMap{{QStringLiteral("id"), QStringLiteral("ai-finite")},
                                       {QStringLiteral("kind"), QStringLiteral("analogInput")},
                                       {QStringLiteral("mode"), QStringLiteral("finite")},
                                       {QStringLiteral("sampleRateHz"), 50000.0},
                                       {QStringLiteral("samplesPerChannel"), 1024}});

    QVariantMap pxiDevice{{QStringLiteral("alias"), QStringLiteral("pxi_6259")},
                          {QStringLiteral("adapterId"), QStringLiteral("ni.daqmx")},
                          {QStringLiteral("vendor"), QStringLiteral("NI")},
                          {QStringLiteral("model"), QStringLiteral("PXI-6259")},
                          {QStringLiteral("serialNumber"), QStringLiteral("CONFIGURE_ME")},
                          {QStringLiteral("properties"),
                           QVariantMap{{QStringLiteral("vendor"), vendorProperties},
                                       {QStringLiteral("taskProfiles"), taskProfiles}}}};
    QVariantMap unrelatedDevice{{QStringLiteral("alias"), QStringLiteral("other_daq")},
                                {QStringLiteral("adapterId"), QStringLiteral("mock.adapter.v1")}};
    hardware.insert(QStringLiteral("devices"), QVariantList{pxiDevice, unrelatedDevice});

    QVariantMap resources;
    QVariantMap ai0 = testsupport::makeResource(QStringLiteral("pxi_6259"),
                                                 QStringLiteral("analog"),
                                                 QStringLiteral("input"),
                                                 0);
    ai0.insert(QStringLiteral("properties"),
               QVariantMap{{QStringLiteral("terminalConfig"), QStringLiteral("Differential")}});
    resources.insert(QStringLiteral("AI_0"), ai0);
    resources.insert(QStringLiteral("AO_0"),
                     testsupport::makeResource(QStringLiteral("pxi_6259"),
                                               QStringLiteral("analog"),
                                               QStringLiteral("output"),
                                               0));
    resources.insert(QStringLiteral("DIO_0"),
                     testsupport::makeResource(QStringLiteral("pxi_6259"),
                                               QStringLiteral("digital"),
                                               QStringLiteral("output"),
                                               0));
    resources.insert(QStringLiteral("CTR_0"),
                     testsupport::makeResource(QStringLiteral("pxi_6259"),
                                               QStringLiteral("counter"),
                                               QStringLiteral("input"),
                                               0));
    resources.insert(QStringLiteral("UNRELATED_DO"),
                     testsupport::makeResource(QStringLiteral("other_daq"),
                                               QStringLiteral("digital"),
                                               QStringLiteral("output"),
                                               0));
    hardware.insert(QStringLiteral("resources"), resources);
    config.insert(QStringLiteral("hardware"), hardware);
    config.insert(QStringLiteral("safeState"),
                  QVariantMap{{QStringLiteral("AO_0"), 0.0},
                              {QStringLiteral("DIO_0"), QStringLiteral("Low")},
                              {QStringLiteral("UNRELATED_DO"), QStringLiteral("High")}});
    return config;
}

QVariantMap channelForResource(const QVariantList& channels, const QString& resourceId)
{
    for (const QVariant& value : channels) {
        const QVariantMap channel = value.toMap();
        if (channel.value(QStringLiteral("resourceId")).toString() == resourceId) {
            return channel;
        }
    }
    return {};
}

} // namespace

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

TEST(ResourceMapperTest, ProjectsPxiDeviceAsVersionedAdapterOpenSpec)
{
    ResourceMapper mapper;
    ASSERT_TRUE(mapper.load(pxiProjectionConfig()));

    const AdapterDeviceOpenSpec spec =
        AdapterDeviceOpenSpec::fromResourceMapper(mapper, QStringLiteral("pxi_6259"));

    EXPECT_EQ(spec.version, 1);
    EXPECT_EQ(spec.device.deviceId, QStringLiteral("pxi_6259"));
    EXPECT_EQ(spec.device.model, QStringLiteral("PXI-6259"));
    EXPECT_EQ(spec.physicalDeviceId, QStringLiteral("PXI1Slot2"));
    EXPECT_EQ(spec.channels.size(), 4);
    EXPECT_EQ(spec.safeState,
              QVariantMap({{QStringLiteral("AO_0"), 0.0},
                           {QStringLiteral("DIO_0"), QStringLiteral("Low")}}));
    ASSERT_EQ(spec.taskProfiles.size(), 1);
    EXPECT_EQ(spec.taskProfiles.first().toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("ai-finite"));

    const QVariantMap projection = spec.toVariantMap();
    EXPECT_EQ(projection.value(QStringLiteral("schema")).toString(),
              QStringLiteral("hwtest.adapter-device-open"));
    EXPECT_EQ(projection.value(QStringLiteral("version")).toInt(), 1);
    EXPECT_EQ(projection.value(QStringLiteral("physicalDeviceId")).toString(),
              QStringLiteral("PXI1Slot2"));

    const QVariantMap projectedDevice = projection.value(QStringLiteral("device")).toMap();
    EXPECT_EQ(projectedDevice.value(QStringLiteral("deviceId")).toString(),
              QStringLiteral("pxi_6259"));
    EXPECT_EQ(projectedDevice.value(QStringLiteral("adapterId")).toString(),
              QStringLiteral("ni.daqmx"));
    EXPECT_EQ(projectedDevice.value(QStringLiteral("model")).toString(),
              QStringLiteral("PXI-6259"));
    EXPECT_EQ(projectedDevice.value(QStringLiteral("properties")).toMap()
                  .value(QStringLiteral("vendor")).toMap()
                  .value(QStringLiteral("ni")).toMap()
                  .value(QStringLiteral("deviceName")).toString(),
              QStringLiteral("PXI1Slot2"));

    const QVariantList channels = projection.value(QStringLiteral("channels")).toList();
    ASSERT_EQ(channels.size(), 4);
    const QVariantMap ai0 = channelForResource(channels, QStringLiteral("AI_0"));
    EXPECT_EQ(ai0.value(QStringLiteral("module")).toString(), QStringLiteral("analog"));
    EXPECT_EQ(ai0.value(QStringLiteral("direction")).toString(), QStringLiteral("input"));
    EXPECT_EQ(ai0.value(QStringLiteral("physicalIndex")).toInt(), 0);
    EXPECT_EQ(ai0.value(QStringLiteral("properties")).toMap()
                  .value(QStringLiteral("terminalConfig")).toString(),
              QStringLiteral("Differential"));
    EXPECT_EQ(projection.value(QStringLiteral("safeState")).toMap(), spec.safeState);
    EXPECT_EQ(projection.value(QStringLiteral("taskProfiles")).toList(), spec.taskProfiles);
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
    EXPECT_TRUE(mapper.errorString().contains(QStringLiteral("unknown device"), Qt::CaseInsensitive));

    QVariantMap mismatch = testsupport::defaultHalConfig();
    hardware = mismatch.value(QStringLiteral("hardware")).toMap();
    resources = hardware.value(QStringLiteral("resources")).toMap();
    output = resources.value(QStringLiteral("DO_POWER_EN")).toMap();
    output.insert(QStringLiteral("adapterId"), QStringLiteral("wrong.adapter"));
    resources.insert(QStringLiteral("DO_POWER_EN"), output);
    hardware.insert(QStringLiteral("resources"), resources);
    mismatch.insert(QStringLiteral("hardware"), hardware);
    EXPECT_FALSE(mapper.load(mismatch));
    EXPECT_TRUE(mapper.errorString().contains(QStringLiteral("does not match")));

    QVariantMap duplicate = testsupport::defaultHalConfig();
    hardware = duplicate.value(QStringLiteral("hardware")).toMap();
    resources = hardware.value(QStringLiteral("resources")).toMap();
    resources.insert(QStringLiteral("DO_POWER_EN_COPY"),
                     resources.value(QStringLiteral("DO_POWER_EN")));
    hardware.insert(QStringLiteral("resources"), resources);
    duplicate.insert(QStringLiteral("hardware"), hardware);
    EXPECT_FALSE(mapper.load(duplicate));
    EXPECT_TRUE(mapper.errorString().contains(QStringLiteral("same physical channel")));
}
