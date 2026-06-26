#include "mock_adapter.h"

#include "test_support.h"

#include <gtest/gtest.h>

using namespace hwtest::hal;

TEST(MockAdapterTest, EnforcesInitializationAndRoundTripsBasicState)
{
    MockAdapter adapter;

    EXPECT_FALSE(adapter.enumerateDevices(OperationOptions{}).ok());
    EXPECT_TRUE(adapter.initialize(testsupport::defaultHalConfig()).ok());

    const HalResult<QVector<DeviceDescriptor>> devices = adapter.enumerateDevices(OperationOptions{});
    ASSERT_TRUE(devices.ok());
    ASSERT_EQ(devices.value.size(), 1);

    const HalResult<SessionId> session = adapter.openDevice(QStringLiteral("main_daq"), QVariantMap{}, OperationOptions{});
    ASSERT_TRUE(session.ok());

    EXPECT_TRUE(adapter.writeAnalog(session.value, 0, 3.25, AnalogWriteOptions{}).ok());
    const HalResult<AnalogSample> analog = adapter.readAnalog(session.value, 0, AnalogReadOptions{});
    ASSERT_TRUE(analog.ok());
    EXPECT_DOUBLE_EQ(analog.value.value, 3.25);

    EXPECT_TRUE(adapter.writeDigital(session.value, 0, DigitalLevel::High, DigitalWriteOptions{}).ok());
    const HalResult<DigitalSample> digital = adapter.readDigital(session.value, 0, OperationOptions{});
    ASSERT_TRUE(digital.ok());
    EXPECT_EQ(digital.value.level, DigitalLevel::High);

    EXPECT_TRUE(adapter.writeSerial(session.value, 0, QByteArrayLiteral("abc"), OperationOptions{}).ok());
    const HalResult<QByteArray> serial = adapter.readSerial(session.value, 0, 16, OperationOptions{});
    ASSERT_TRUE(serial.ok());
    EXPECT_FALSE(serial.value.isEmpty());

    CanFdFrame frame;
    frame.id = 0x123;
    frame.payload = QByteArrayLiteral("payload");
    EXPECT_TRUE(adapter.sendCan(session.value, 0, frame, OperationOptions{}).ok());
    const HalResult<CanFdFrame> can = adapter.receiveCan(session.value, 0, OperationOptions{});
    ASSERT_TRUE(can.ok());
    EXPECT_EQ(can.value.id, frame.id);
    EXPECT_EQ(can.value.payload, frame.payload);
}
