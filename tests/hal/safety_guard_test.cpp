#include "safety_guard.h"

#include <gtest/gtest.h>

using namespace hwtest::hal;

TEST(SafetyGuardTest, ClampsAnalogWriteWhenAllowed)
{
    ResourceBinding binding;
    binding.deviceId = QStringLiteral("main_daq");
    binding.resourceId = QStringLiteral("DA_MAIN_0");

    SafetyGuard guard;
    double effectiveValue = 0.0;
    AnalogWriteOptions options;
    options.range = AnalogRange{0.0, 5.0, AnalogUnit::Volt};

    const HalStatus status = guard.validateAnalogWrite(binding, 7.0, options, &effectiveValue);

    EXPECT_TRUE(status.ok());
    EXPECT_DOUBLE_EQ(effectiveValue, 5.0);
}

TEST(SafetyGuardTest, RejectsAnalogWriteBeyondLimitWhenClampIsDisabled)
{
    ResourceBinding binding;
    binding.deviceId = QStringLiteral("main_daq");
    binding.resourceId = QStringLiteral("DA_MAIN_0");

    SafetyGuard guard;
    AnalogWriteOptions options;
    options.range = AnalogRange{0.0, 5.0, AnalogUnit::Volt};
    options.safeClamp = false;

    const HalStatus status = guard.validateAnalogWrite(binding, -1.0, options, nullptr);

    EXPECT_EQ(status.code, HalStatusCode::SafetyLimitExceeded);
}

TEST(SafetyGuardTest, RejectsInvalidDigitalSerialAndCanInputs)
{
    ResourceBinding digitalBinding;
    digitalBinding.deviceId = QStringLiteral("main_daq");
    digitalBinding.resourceId = QStringLiteral("DI_POWER_OK");
    digitalBinding.direction = QStringLiteral("input");

    ResourceBinding canBinding;
    canBinding.deviceId = QStringLiteral("main_daq");
    canBinding.resourceId = QStringLiteral("CANFD_A");

    SafetyGuard guard;

    EXPECT_EQ(guard.validateDigitalWrite(digitalBinding, DigitalLevel::High, DigitalWriteOptions{}).code,
              HalStatusCode::InvalidState);
    EXPECT_EQ(guard.validateDigitalWrite(ResourceBinding{}, DigitalLevel::Unknown, DigitalWriteOptions{}).code,
              HalStatusCode::InvalidArgument);

    SerialConfig serialConfig;
    serialConfig.baudRate = 0;
    EXPECT_EQ(guard.validateSerialConfig(ResourceBinding{}, serialConfig, OperationOptions{}).code,
              HalStatusCode::InvalidArgument);

    CanFdFrame frame;
    frame.payload = QByteArray(65, 'x');
    EXPECT_EQ(guard.validateCanFrame(canBinding, frame, OperationOptions{}).code,
              HalStatusCode::SafetyLimitExceeded);
}
