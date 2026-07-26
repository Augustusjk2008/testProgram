#include "hal_device.h"
#include "mock_adapter.h"

#include "test_support.h"

#include <gtest/gtest.h>

using namespace hwtest::hal;

namespace {

class RecordingMockAdapter final : public MockAdapter {
public:
    HalStatus writeDigital(const SessionId& sessionId,
                           int physicalIndex,
                           DigitalLevel level,
                           const DigitalWriteOptions& options) override
    {
        ++singleWriteCount;
        return MockAdapter::writeDigital(sessionId, physicalIndex, level, options);
    }

    HalStatus writeDigitalBatch(const SessionId& sessionId,
                                const QMap<int, DigitalLevel>& values,
                                const DigitalWriteOptions& options) override
    {
        ++batchWriteCount;
        lastBatch = values;
        return MockAdapter::writeDigitalBatch(sessionId, values, options);
    }

    int singleWriteCount = 0;
    int batchWriteCount = 0;
    QMap<int, DigitalLevel> lastBatch;
};

class CloseFailingMockAdapter final : public MockAdapter {
public:
    HalStatus closeDevice(const SessionId& sessionId,
                          const OperationOptions& options) override
    {
        ++closeCalls;
        MockAdapter::closeDevice(sessionId, options);
        return makeError(HalStatusCode::IoError,
                         QStringLiteral("fixture.closeDevice"),
                         QStringLiteral("close cleanup failed after consuming the session"));
    }

    int closeCalls = 0;
};

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

class LegacyStyleHalDevice final : public IHalDevice {
public:
    DeviceDescriptor descriptor() const override { return {}; }
    DeviceCapabilities capabilities() const override { return {}; }
    IAnalogIo* analogIo() override { return nullptr; }
    IDigitalIo* digitalIo() override { return nullptr; }
    ISerialBus* serialBus() override { return nullptr; }
    ICanFdBus* canFdBus() override { return nullptr; }
};

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

TEST(HalDeviceTest, CloseFailureStillClosesTheConsumedBackendSession)
{
    CloseFailingMockAdapter backend;
    ASSERT_TRUE(backend.initialize(testsupport::defaultHalConfig()).ok());
    const SessionId sessionId = openSession(&backend);
    HalDevice device(&backend,
                     sessionId,
                     descriptor(),
                     capabilities(),
                     bindings(),
                     QVariantMap{});

    EXPECT_EQ(device.close(OperationOptions{}).code, HalStatusCode::IoError);
    EXPECT_FALSE(device.isOpen());
    EXPECT_TRUE(device.close(OperationOptions{}).ok());
    EXPECT_EQ(backend.closeCalls, 1);
}

TEST(HalDeviceTest, AppliesAllDigitalSafeLevelsWithOneBackendBatch)
{
    QVariantMap config = testsupport::defaultHalConfig();
    QVariantMap hardware = config.value(QStringLiteral("hardware")).toMap();
    QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    resources.insert(QStringLiteral("DO_SECOND"),
                     testsupport::makeResource(QStringLiteral("main_daq"),
                                               QStringLiteral("digital"),
                                               QStringLiteral("output"),
                                               1));
    hardware.insert(QStringLiteral("resources"), resources);
    config.insert(QStringLiteral("hardware"), hardware);
    config.insert(QStringLiteral("safeState"),
                  QVariantMap{{QStringLiteral("DO_POWER_EN"), QStringLiteral("Low")},
                              {QStringLiteral("DO_SECOND"), QStringLiteral("High")}});

    ResourceMapper mapper;
    ASSERT_TRUE(mapper.load(config));
    RecordingMockAdapter backend;
    ASSERT_TRUE(backend.initialize(config).ok());
    const HalResult<SessionId> session = backend.openDevice(
        QStringLiteral("main_daq"), QVariantMap{}, OperationOptions{});
    ASSERT_TRUE(session.ok());
    HalDevice device(&backend,
                     session.value,
                     mapper.deviceDescriptor(QStringLiteral("main_daq")),
                     mapper.capabilities(QStringLiteral("main_daq")),
                     mapper.bindingsForDevice(QStringLiteral("main_daq")),
                     config.value(QStringLiteral("safeState")).toMap());

    ASSERT_TRUE(device.close().ok());
    EXPECT_EQ(backend.batchWriteCount, 1);
    EXPECT_EQ(backend.singleWriteCount, 0);
    EXPECT_EQ(backend.lastBatch.size(), 2);
    EXPECT_EQ(backend.lastBatch.value(0), DigitalLevel::Low);
    EXPECT_EQ(backend.lastBatch.value(1), DigitalLevel::High);
}

TEST(HalDeviceTest, ControlChannelRejectsMissingControlResource)
{
    LegacyStyleHalDevice legacyDevice;
    EXPECT_EQ(legacyDevice.controlChannel(), nullptr);

    MockAdapter backend;
    EXPECT_TRUE(backend.initialize(testsupport::defaultHalConfig()).ok());
    const SessionId sessionId = openSession(&backend);

    HalDevice device(&backend,
                     sessionId,
                     descriptor(),
                     capabilities(),
                     bindings(),
                     QVariantMap{});

    ASSERT_NE(device.controlChannel(), nullptr);
    EXPECT_EQ(device.controlChannel()->openControl(QStringLiteral("CONTROL_MISSING"),
                                                    OperationOptions{})
                  .code,
              HalStatusCode::NotFound);
}
