#include "hal_device.h"
#include "mock_adapter.h"

#include "test_support.h"

#include <gtest/gtest.h>

using namespace hwtest::hal;

namespace {

QVector<ResourceBinding> bindings()
{
    ResourceMapper mapper;
    EXPECT_TRUE(mapper.load(testsupport::defaultHalConfig()));
    return mapper.bindingsForDevice(QStringLiteral("main_daq"));
}

DeviceCapabilities capabilities()
{
    ResourceMapper mapper;
    EXPECT_TRUE(mapper.load(testsupport::defaultHalConfig()));
    return mapper.capabilities(QStringLiteral("main_daq"));
}

DeviceDescriptor descriptor()
{
    ResourceMapper mapper;
    EXPECT_TRUE(mapper.load(testsupport::defaultHalConfig()));
    return mapper.deviceDescriptor(QStringLiteral("main_daq"));
}

SessionId openSession(MockAdapter* backend)
{
    const HalResult<SessionId> session = backend->openDevice(QStringLiteral("main_daq"), QVariantMap{}, OperationOptions{});
    EXPECT_TRUE(session.ok());
    return session.value;
}

} // namespace

TEST(HalDeviceTest, RejectsOperationsOnClosedSession)
{
    HalDevice device(nullptr,
                     QStringLiteral("session-1"),
                     descriptor(),
                     capabilities(),
                     bindings(),
                     testsupport::safeStateHalConfig().value(QStringLiteral("safeState")).toMap());

    device.close(OperationOptions{});

    EXPECT_EQ(device.analogIo()->readAd(QStringLiteral("AD_MAIN_0"), AnalogReadOptions{}).status.code,
              HalStatusCode::InvalidState);
    EXPECT_EQ(device.digitalIo()->writeDo(QStringLiteral("DO_POWER_EN"), DigitalLevel::High, DigitalWriteOptions{}).code,
              HalStatusCode::InvalidState);
    EXPECT_EQ(device.serialBus()->openSerial(QStringLiteral("SERIAL_A"), SerialConfig{}, OperationOptions{}).code,
              HalStatusCode::InvalidState);
}

TEST(HalDeviceTest, SupportsChannelTypeMismatchAndBatchOperations)
{
    MockAdapter backend;
    EXPECT_TRUE(backend.initialize(testsupport::defaultHalConfig()).ok());
    const SessionId sessionId = openSession(&backend);

    HalDevice device(&backend,
                     sessionId,
                     descriptor(),
                     capabilities(),
                     bindings(),
                     QVariantMap{});

    EXPECT_EQ(device.analogIo()->readAd(QStringLiteral("DO_POWER_EN"), AnalogReadOptions{}).status.code,
              HalStatusCode::NotSupported);
    EXPECT_EQ(device.digitalIo()->readDi(QStringLiteral("DA_MAIN_0"), OperationOptions{}).status.code,
              HalStatusCode::NotSupported);

    const HalResult<QVector<AnalogSample>> analogBatch = device.analogIo()->readAdBatch(QVector<ResourceId>{QStringLiteral("AD_MAIN_0"),
                                                                                                            QStringLiteral("AD_MAIN_0")},
                                                                                       AnalogReadOptions{});
    ASSERT_TRUE(analogBatch.ok());
    EXPECT_EQ(analogBatch.value.size(), 2);
    EXPECT_EQ(analogBatch.value.first().channel, QStringLiteral("AD_MAIN_0"));
}

TEST(HalDeviceTest, AppliesSafeStateOnClose)
{
    MockAdapter backend;
    EXPECT_TRUE(backend.initialize(testsupport::safeStateHalConfig()).ok());
    const SessionId sessionId = openSession(&backend);

    HalDevice device(&backend,
                     sessionId,
                     descriptor(),
                     capabilities(),
                     bindings(),
                     testsupport::safeStateHalConfig().value(QStringLiteral("safeState")).toMap());

    EXPECT_TRUE(device.analogIo()->writeDa(QStringLiteral("DA_MAIN_0"), 4.5, AnalogWriteOptions{}).ok());
    EXPECT_TRUE(device.digitalIo()->writeDo(QStringLiteral("DO_POWER_EN"), DigitalLevel::High, DigitalWriteOptions{}).ok());
    EXPECT_TRUE(device.serialBus()->openSerial(QStringLiteral("SERIAL_A"), SerialConfig{}, OperationOptions{}).ok());
    EXPECT_TRUE(device.canFdBus()->openCan(QStringLiteral("CANFD_A"), CanFdConfig{}, OperationOptions{}).ok());

    EXPECT_TRUE(device.close(OperationOptions{}).ok());
    EXPECT_FALSE(device.isOpen());
}
