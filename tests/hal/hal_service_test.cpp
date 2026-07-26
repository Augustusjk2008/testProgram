#include "hal/hal_factory.h"

#include "hal_service.h"
#include "hal/i_hal_device.h"

#include "test_support.h"

#include <gtest/gtest.h>
#include <memory>

using namespace hwtest::hal;

namespace {

QVariantMap multiAdapterConfig(const QString& libraryPath)
{
    QVariantMap config;
    QVariantMap adapters;
    adapters.insert(QStringLiteral("fixture.digital.v1"),
                    QVariantMap{{QStringLiteral("providerId"), QStringLiteral("vendor.cabi")},
                                {QStringLiteral("libraryPath"), libraryPath},
                                {QStringLiteral("settings"),
                                 QVariantMap{{QStringLiteral("deviceName"),
                                              QStringLiteral("fixture_device")}}}});
    config.insert(QStringLiteral("adapters"), adapters);

    QVariantMap hardware;
    QVariantList devices;
    devices.push_back(QVariantMap{{QStringLiteral("alias"), QStringLiteral("mock_device")},
                                  {QStringLiteral("adapterId"), QStringLiteral("mock.adapter.v1")}});
    devices.push_back(QVariantMap{{QStringLiteral("alias"), QStringLiteral("fixture_device")},
                                  {QStringLiteral("adapterId"), QStringLiteral("fixture.digital.v1")}});
    hardware.insert(QStringLiteral("devices"), devices);
    QVariantMap resources;
    resources.insert(QStringLiteral("DO_MOCK"),
                     testsupport::makeResource(QStringLiteral("mock_device"),
                                               QStringLiteral("digital"),
                                               QStringLiteral("output"),
                                               0));
    QVariantMap vendorOutput = testsupport::makeResource(
        QStringLiteral("fixture_device"),
        QStringLiteral("digital"),
        QStringLiteral("output"),
        0);
    vendorOutput.insert(QStringLiteral("adapterId"), QStringLiteral("fixture.digital.v1"));
    resources.insert(QStringLiteral("DO_VENDOR"), vendorOutput);
    QVariantMap vendorInput = testsupport::makeResource(
        QStringLiteral("fixture_device"),
        QStringLiteral("digital"),
        QStringLiteral("input"),
        0);
    vendorInput.insert(QStringLiteral("adapterId"), QStringLiteral("fixture.digital.v1"));
    resources.insert(QStringLiteral("DI_VENDOR"), vendorInput);
    hardware.insert(QStringLiteral("resources"), resources);
    config.insert(QStringLiteral("hardware"), hardware);
    return config;
}

} // namespace

TEST(HalServiceTest, ScanDevicesRequiresInitialize)
{
    std::unique_ptr<IHalService> service(createHalService());

    const HalResult<QVector<DeviceDescriptor>> result = service->scanDevices(OperationOptions{});

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, HalStatusCode::NotInitialized);
}

TEST(HalServiceTest, EmitsOpenDeviceLogAndSupportsIoRoundTrip)
{
    QVector<HalLogEvent> events;
    {
        std::unique_ptr<IHalService> service(createHalService());
        QObject::connect(service.get(),
                         &IHalService::logProduced,
                         service.get(),
                         [&events](const HalLogEvent& event) {
                             events.push_back(event);
                         });

        EXPECT_TRUE(service->initialize(testsupport::defaultHalConfig()).ok());

        OperationOptions openOptions;
        openOptions.requestId = QStringLiteral("req-1");
        const HalResult<SessionId> session = service->openDevice(QStringLiteral("main_daq"), openOptions);
        ASSERT_TRUE(session.ok());
        ASSERT_FALSE(session.value.isEmpty());
        ASSERT_FALSE(events.isEmpty());

        const HalLogEvent openEvent = events.last();
        EXPECT_EQ(openEvent.operation, QStringLiteral("openDevice"));
        EXPECT_EQ(openEvent.requestId, QStringLiteral("req-1"));
        EXPECT_EQ(openEvent.status, QStringLiteral("Ok"));

        const HalResult<IHalDevice*> deviceResult = service->device(session.value);
        ASSERT_TRUE(deviceResult.ok());
        ASSERT_NE(deviceResult.value, nullptr);

        auto* analog = deviceResult.value->analogIo();
        auto* digital = deviceResult.value->digitalIo();
        auto* serial = deviceResult.value->serialBus();
        auto* can = deviceResult.value->canFdBus();
        ASSERT_NE(analog, nullptr);
        ASSERT_NE(digital, nullptr);
        ASSERT_NE(serial, nullptr);
        ASSERT_NE(can, nullptr);

        AnalogWriteOptions analogWriteOptions;
        analogWriteOptions.range = AnalogRange{0.0, 5.0, AnalogUnit::Volt};
        EXPECT_TRUE(analog->writeDa(QStringLiteral("DA_MAIN_0"), 2.75, analogWriteOptions).ok());
        const HalResult<AnalogSample> analogSample = analog->readAd(QStringLiteral("AD_MAIN_0"),
                                                                    AnalogReadOptions{});
        ASSERT_TRUE(analogSample.ok());
        EXPECT_DOUBLE_EQ(analogSample.value.value, 2.75);
        EXPECT_EQ(analogSample.value.channel, QStringLiteral("AD_MAIN_0"));

        EXPECT_TRUE(digital->writeDo(QStringLiteral("DO_POWER_EN"), DigitalLevel::High, DigitalWriteOptions{}).ok());
        const HalResult<DigitalSample> digitalSample = digital->readDi(QStringLiteral("DI_POWER_OK"), OperationOptions{});
        ASSERT_TRUE(digitalSample.ok());
        EXPECT_EQ(digitalSample.value.level, DigitalLevel::High);

        EXPECT_TRUE(serial->openSerial(QStringLiteral("SERIAL_A"), SerialConfig{}, OperationOptions{}).ok());
        EXPECT_TRUE(serial->writeSerial(QStringLiteral("SERIAL_A"), QByteArrayLiteral("ping"), OperationOptions{}).ok());
        const HalResult<QByteArray> serialRead = serial->readSerial(QStringLiteral("SERIAL_A"), 8, OperationOptions{});
        ASSERT_TRUE(serialRead.ok());
        EXPECT_EQ(serialRead.value, QByteArrayLiteral("ping"));

        CanFdFrame frame;
        frame.id = 0x123;
        frame.payload = QByteArrayLiteral("abc");
        EXPECT_TRUE(can->openCan(QStringLiteral("CANFD_A"), CanFdConfig{}, OperationOptions{}).ok());
        EXPECT_TRUE(can->sendCan(QStringLiteral("CANFD_A"), frame, OperationOptions{}).ok());
        const HalResult<CanFdFrame> received = can->receiveCan(QStringLiteral("CANFD_A"), OperationOptions{});
        ASSERT_TRUE(received.ok());
        EXPECT_EQ(received.value.id, frame.id);
        EXPECT_EQ(received.value.payload, frame.payload);
    }

    ASSERT_FALSE(events.isEmpty());
}

TEST(HalServiceTest, CloseAppliesSafeStateBeforeReopen)
{
    std::unique_ptr<IHalService> service(createHalService());
    EXPECT_TRUE(service->initialize(testsupport::safeStateHalConfig()).ok());

    const HalResult<SessionId> firstSession = service->openDevice(QStringLiteral("main_daq"), OperationOptions{});
    ASSERT_TRUE(firstSession.ok());

    const HalResult<IHalDevice*> deviceResult = service->device(firstSession.value);
    ASSERT_TRUE(deviceResult.ok());
    auto* analog = deviceResult.value->analogIo();
    auto* digital = deviceResult.value->digitalIo();
    ASSERT_NE(analog, nullptr);
    ASSERT_NE(digital, nullptr);

    AnalogWriteOptions analogWriteOptions;
    analogWriteOptions.range = AnalogRange{0.0, 5.0, AnalogUnit::Volt};
    EXPECT_TRUE(analog->writeDa(QStringLiteral("DA_MAIN_0"), 4.5, analogWriteOptions).ok());
    EXPECT_TRUE(digital->writeDo(QStringLiteral("DO_POWER_EN"), DigitalLevel::High, DigitalWriteOptions{}).ok());
    EXPECT_TRUE(service->closeDevice(firstSession.value, OperationOptions{}).ok());

    const HalResult<SessionId> secondSession = service->openDevice(QStringLiteral("main_daq"), OperationOptions{});
    ASSERT_TRUE(secondSession.ok());
    const HalResult<IHalDevice*> reopened = service->device(secondSession.value);
    ASSERT_TRUE(reopened.ok());

    const HalResult<AnalogSample> analogSample = reopened.value->analogIo()->readAd(QStringLiteral("AD_MAIN_0"),
                                                                                   AnalogReadOptions{});
    ASSERT_TRUE(analogSample.ok());
    EXPECT_DOUBLE_EQ(analogSample.value.value, 0.0);

    const HalResult<DigitalSample> digitalSample = reopened.value->digitalIo()->readDi(QStringLiteral("DI_POWER_OK"),
                                                                                       OperationOptions{});
    ASSERT_TRUE(digitalSample.ok());
    EXPECT_EQ(digitalSample.value.level, DigitalLevel::Low);
}

TEST(HalServiceTest, ReportsMissingSessionAndSupportsFactoryPair)
{
    std::unique_ptr<IHalService> service(createHalService());
    EXPECT_FALSE(service->device(QStringLiteral("missing")).ok());
    EXPECT_EQ(service->closeDevice(QStringLiteral("missing"), OperationOptions{}).code, HalStatusCode::NotFound);
    EXPECT_EQ(service->resetDevice(QStringLiteral("missing"), OperationOptions{}).code, HalStatusCode::NotFound);
    EXPECT_EQ(service->healthCheck(QStringLiteral("missing"), OperationOptions{}).code, HalStatusCode::NotFound);

    IHalService* raw = createHalService();
    ASSERT_NE(raw, nullptr);
    destroyHalService(raw);
}

TEST(HalServiceTest, LoadsVendorAdapterLazilyAndRoutesSessionsByDeviceAdapter)
{
    std::unique_ptr<IHalService> service(createHalService());
    QVariantMap lazyConfig = multiAdapterConfig(
        QStringLiteral("Z:/missing/vendor-adapter.dll"));
    ASSERT_TRUE(service->initialize(lazyConfig).ok());
    const auto mockOnly = service->openDevice(QStringLiteral("mock_device"),
                                              OperationOptions{});
    ASSERT_TRUE(mockOnly.ok());
    EXPECT_TRUE(service->closeDevice(mockOnly.value, OperationOptions{}).ok());
    const auto missingVendor = service->openDevice(QStringLiteral("fixture_device"),
                                                   OperationOptions{});
    EXPECT_FALSE(missingVendor.ok());
    EXPECT_EQ(missingVendor.status.code, HalStatusCode::AdapterLoadFailed);

    ASSERT_TRUE(service->shutdown().ok());
    ASSERT_TRUE(service->initialize(multiAdapterConfig(
        QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH))).ok());
    const auto mockSession = service->openDevice(QStringLiteral("mock_device"),
                                                 OperationOptions{});
    const auto vendorSession = service->openDevice(QStringLiteral("fixture_device"),
                                                   OperationOptions{});
    ASSERT_TRUE(mockSession.ok());
    ASSERT_TRUE(vendorSession.ok());
    EXPECT_NE(mockSession.value, vendorSession.value);

    const auto vendorDevice = service->device(vendorSession.value);
    ASSERT_TRUE(vendorDevice.ok());
    ASSERT_TRUE(vendorDevice.value->digitalIo()->writeDo(
        QStringLiteral("DO_VENDOR"), DigitalLevel::High, DigitalWriteOptions{}).ok());
    const auto readback = vendorDevice.value->digitalIo()->readDi(
        QStringLiteral("DI_VENDOR"), OperationOptions{});
    ASSERT_TRUE(readback.ok());
    EXPECT_EQ(readback.value.level, DigitalLevel::High);
    EXPECT_TRUE(service->closeDevice(vendorSession.value, OperationOptions{}).ok());
    EXPECT_TRUE(service->closeDevice(mockSession.value, OperationOptions{}).ok());
}
