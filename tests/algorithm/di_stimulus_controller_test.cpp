#include <algorithm/di_stimulus_controller.h>

#include <hal/i_digital_io.h>
#include <hal/i_hal_device.h>

#include <gtest/gtest.h>

namespace hwtest::algorithm::mbddf {
namespace {

class RecordingDigitalIo final : public hwtest::hal::IDigitalIo {
public:
    hwtest::hal::HalResult<hwtest::hal::DigitalSample> readDi(
        const hwtest::hal::ResourceId&,
        const hwtest::hal::OperationOptions&) override { return {}; }
    hwtest::hal::HalResult<QVector<hwtest::hal::DigitalSample>> readDiBatch(
        const QVector<hwtest::hal::ResourceId>&,
        const hwtest::hal::OperationOptions&) override { return {}; }
    hwtest::hal::HalStatus writeDo(const hwtest::hal::ResourceId&,
                                   hwtest::hal::DigitalLevel,
                                   const hwtest::hal::DigitalWriteOptions&) override
    {
        ++singleWrites;
        return {};
    }
    hwtest::hal::HalStatus writeDoBatch(
        const QMap<hwtest::hal::ResourceId, hwtest::hal::DigitalLevel>& values,
        const hwtest::hal::DigitalWriteOptions&) override
    {
        ++batchWrites;
        lastValues = values;
        return nextStatus;
    }
    hwtest::hal::HalResult<hwtest::hal::DigitalSample> waitEdge(
        const hwtest::hal::ResourceId&,
        hwtest::hal::DigitalLevel,
        const hwtest::hal::OperationOptions&) override { return {}; }

    int singleWrites = 0;
    int batchWrites = 0;
    QMap<hwtest::hal::ResourceId, hwtest::hal::DigitalLevel> lastValues;
    hwtest::hal::HalStatus nextStatus;
};

class DigitalDevice final : public hwtest::hal::IHalDevice {
public:
    hwtest::hal::DeviceDescriptor descriptor() const override { return {}; }
    hwtest::hal::DeviceCapabilities capabilities() const override { return {}; }
    hwtest::hal::IAnalogIo* analogIo() override { return nullptr; }
    hwtest::hal::IDigitalIo* digitalIo() override { return &digital; }
    hwtest::hal::ISerialBus* serialBus() override { return nullptr; }
    hwtest::hal::ICanFdBus* canFdBus() override { return nullptr; }

    RecordingDigitalIo digital;
};

QVariantMap executionConfig()
{
    QVariantList channels;
    channels.push_back(QVariantMap{{QStringLiteral("switchId"), QStringLiteral("di0")},
                                   {QStringLiteral("dutBit"), 0},
                                   {QStringLiteral("resourceId"), QStringLiteral("DO0")},
                                   {QStringLiteral("label"), QStringLiteral("DI0")},
                                   {QStringLiteral("activeLevel"), QStringLiteral("High")}});
    channels.push_back(QVariantMap{{QStringLiteral("switchId"), QStringLiteral("di1")},
                                   {QStringLiteral("dutBit"), 1},
                                   {QStringLiteral("resourceId"), QStringLiteral("DO1")},
                                   {QStringLiteral("label"), QStringLiteral("DI1")},
                                   {QStringLiteral("activeLevel"), QStringLiteral("Low")}});
    channels.push_back(QVariantMap{{QStringLiteral("switchId"), QStringLiteral("di2")},
                                   {QStringLiteral("dutBit"), 2},
                                   {QStringLiteral("resourceId"), QStringLiteral("DO2")},
                                   {QStringLiteral("label"), QStringLiteral("DI2")},
                                   {QStringLiteral("activeLevel"), QStringLiteral("High")}});
    return QVariantMap{{QStringLiteral("digitalStimulus"),
                        QVariantMap{{QStringLiteral("settlingMs"), 20},
                                    {QStringLiteral("channels"), channels}}}};
}

TEST(DiStimulusControllerTest, WritesEveryConfiguredLineAndMapsActiveLow)
{
    DigitalDevice device;
    DiStimulusController controller(&device);
    ASSERT_TRUE(controller.configure(executionConfig()).ok());
    ASSERT_TRUE(controller.resetDigitalStimulus().ok());
    EXPECT_EQ(device.digital.batchWrites, 1);
    EXPECT_EQ(device.digital.lastValues.value(QStringLiteral("DO0")),
              hwtest::hal::DigitalLevel::Low);
    EXPECT_EQ(device.digital.lastValues.value(QStringLiteral("DO1")),
              hwtest::hal::DigitalLevel::High);
    EXPECT_EQ(device.digital.lastValues.value(QStringLiteral("DO2")),
              hwtest::hal::DigitalLevel::Low);

    ASSERT_TRUE(controller.setDigitalStimulus(QStringLiteral("di1"), true, 1).ok());
    EXPECT_EQ(device.digital.batchWrites, 2);
    EXPECT_EQ(device.digital.singleWrites, 0);
    EXPECT_EQ(device.digital.lastValues.size(), 3);
    EXPECT_EQ(device.digital.lastValues.value(QStringLiteral("DO1")),
              hwtest::hal::DigitalLevel::Low);
    const DiStimulusState state = controller.state();
    EXPECT_EQ(state.appliedMask, 0x2u);
    EXPECT_EQ(state.revision, 2u);
    EXPECT_EQ(state.settlingMs, 20);
    EXPECT_GT(state.lastWriteTimestampUs, 0);
}

TEST(DiStimulusControllerTest, RejectsUnknownSwitchAndRevisionConflictWithoutWriting)
{
    DigitalDevice device;
    DiStimulusController controller(&device);
    ASSERT_TRUE(controller.configure(executionConfig()).ok());
    ASSERT_TRUE(controller.resetDigitalStimulus().ok());
    const int writes = device.digital.batchWrites;

    const auto unknown = controller.setDigitalStimulus(
        QStringLiteral("unconfigured"), true, controller.state().revision);
    EXPECT_EQ(unknown.code, hwtest::hal::HalStatusCode::NotFound);
    EXPECT_EQ(device.digital.batchWrites, writes);

    const auto conflict = controller.setDigitalStimulus(
        QStringLiteral("di0"), true, controller.state().revision + 1);
    EXPECT_EQ(conflict.code, hwtest::hal::HalStatusCode::DataMismatch);
    EXPECT_EQ(device.digital.batchWrites, writes);
}
} // namespace
} // namespace hwtest::algorithm::mbddf
