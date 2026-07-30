#include <gtest/gtest.h>

#include "hal_board_test_fixture.h"
#include "mbddf_algorithm_registry.h"

#include <hal/i_analog_io.h>
#include <hal/i_digital_io.h>
#include <hal/i_hal_device.h>
#include <hal/i_sample_task_io.h>

#include <limits>

namespace hwtest::app {
namespace {

TEST(MbdDfAlgorithmRegistryTest, RegistersDoWriteAndHelmBoardAsDedicatedAlgorithms)
{
    EXPECT_EQ(mbddfAlgorithmRegistry().size(), 14);
    const auto* doWrite = findMbdDfAlgorithm(QStringLiteral("mbddf.do_write"));
    const auto* helmBoard = findMbdDfAlgorithm(
        QStringLiteral("mbddf.helm_board_test"));
    ASSERT_NE(doWrite, nullptr);
    ASSERT_NE(helmBoard, nullptr);
    EXPECT_EQ(doWrite->requestProfileId, QStringLiteral("do_write_request"));
    EXPECT_EQ(helmBoard->responseProfileId,
              QStringLiteral("helm_board_test_response"));
}

TEST(BoardFixtureRequirementTest, OpensAuxiliaryBoardsOnlyForPhysicalModes)
{
    const BoardFixtureRequirement doWrite = boardFixtureRequirement(
        QStringLiteral("mbddf.do_write"), {});
    EXPECT_TRUE(doWrite.pxi6259);
    EXPECT_FALSE(doWrite.pxi6733);

    const BoardFixtureRequirement manual = boardFixtureRequirement(
        QStringLiteral("mbddf.helm_board_test"),
        {{QStringLiteral("test_mode"), 1}});
    EXPECT_FALSE(manual.pxi6259);
    EXPECT_FALSE(manual.pxi6733);

    const BoardFixtureRequirement automatic = boardFixtureRequirement(
        QStringLiteral("mbddf.helm_board_test"),
        {{QStringLiteral("test_mode"), 0}});
    EXPECT_TRUE(automatic.pxi6259);
    EXPECT_TRUE(automatic.pxi6733);
}

class FakeDigitalIo final : public hwtest::hal::IDigitalIo {
public:
    hwtest::hal::HalResult<hwtest::hal::DigitalSample> readDi(
        const hwtest::hal::ResourceId& channel,
        const hwtest::hal::OperationOptions&) override
    {
        hwtest::hal::HalResult<hwtest::hal::DigitalSample> result;
        result.value.channel = channel;
        result.value.level = hwtest::hal::DigitalLevel::Low;
        return result;
    }

    hwtest::hal::HalResult<QVector<hwtest::hal::DigitalSample>> readDiBatch(
        const QVector<hwtest::hal::ResourceId>& channels,
        const hwtest::hal::OperationOptions&) override
    {
        ++readBatchCount;
        hwtest::hal::HalResult<QVector<hwtest::hal::DigitalSample>> result;
        for (int index = 0; index < channels.size(); ++index) {
            hwtest::hal::DigitalSample sample;
            sample.channel = channels.at(index);
            sample.level = index == 0 ? hwtest::hal::DigitalLevel::High
                                      : hwtest::hal::DigitalLevel::Low;
            result.value.push_back(sample);
        }
        return result;
    }

    hwtest::hal::HalStatus writeDo(
        const hwtest::hal::ResourceId&, hwtest::hal::DigitalLevel,
        const hwtest::hal::DigitalWriteOptions&) override { return {}; }
    hwtest::hal::HalStatus writeDoBatch(
        const QMap<hwtest::hal::ResourceId, hwtest::hal::DigitalLevel>&,
        const hwtest::hal::DigitalWriteOptions&) override { return {}; }
    hwtest::hal::HalResult<hwtest::hal::DigitalSample> waitEdge(
        const hwtest::hal::ResourceId& channel, hwtest::hal::DigitalLevel,
        const hwtest::hal::OperationOptions& options) override
    {
        return readDi(channel, options);
    }

    int readBatchCount = 0;
};

class FakeAnalogIo final : public hwtest::hal::IAnalogIo {
public:
    hwtest::hal::HalStatus configureAd(
        const hwtest::hal::ResourceId&, const hwtest::hal::AnalogRange&,
        const hwtest::hal::OperationOptions&) override { return {}; }
    hwtest::hal::HalResult<hwtest::hal::AnalogSample> readAd(
        const hwtest::hal::ResourceId&, const hwtest::hal::AnalogReadOptions&) override
    { return {}; }
    hwtest::hal::HalResult<QVector<hwtest::hal::AnalogSample>> readAdBatch(
        const QVector<hwtest::hal::ResourceId>&,
        const hwtest::hal::AnalogReadOptions&) override { return {}; }
    hwtest::hal::HalStatus configureDa(
        const hwtest::hal::ResourceId&, const hwtest::hal::AnalogRange&,
        const hwtest::hal::OperationOptions&) override { return {}; }
    hwtest::hal::HalStatus writeDa(
        const hwtest::hal::ResourceId& channel, double,
        const hwtest::hal::AnalogWriteOptions&) override
    {
        writeChannels.push_back(channel);
        if (channel == failWriteChannel) {
            hwtest::hal::HalStatus status;
            status.code = hwtest::hal::HalStatusCode::IoError;
            status.error.code = status.code;
            status.error.message = QStringLiteral("injected AO failure");
            return status;
        }
        return {};
    }
    hwtest::hal::HalStatus writeDaBatch(
        const QMap<hwtest::hal::ResourceId, double>& values,
        const hwtest::hal::AnalogWriteOptions& options) override
    {
        ++writeBatchCount;
        lastValues = values;
        lastOptions = options;
        return writeStatus;
    }

    int writeBatchCount = 0;
    QMap<hwtest::hal::ResourceId, double> lastValues;
    hwtest::hal::AnalogWriteOptions lastOptions;
    hwtest::hal::HalStatus writeStatus;
    QVector<hwtest::hal::ResourceId> writeChannels;
    hwtest::hal::ResourceId failWriteChannel;
};

class FakeSampleTasks final : public hwtest::hal::ISampleTaskIo {
public:
    hwtest::hal::HalResult<hwtest::hal::SampleTaskId> createTask(
        const hwtest::hal::SampleTaskConfig& config,
        const hwtest::hal::OperationOptions&) override
    {
        lastConfig = config;
        return {{}, QStringLiteral("fixture-task")};
    }
    hwtest::hal::HalStatus startTask(
        const hwtest::hal::SampleTaskId&,
        const hwtest::hal::OperationOptions&) override
    {
        ++startCount;
        return {};
    }
    hwtest::hal::HalResult<hwtest::hal::SampleTaskBlock> readTask(
        const hwtest::hal::SampleTaskId&, int,
        const hwtest::hal::OperationOptions&) override
    {
        ++readCount;
        return {{}, block};
    }
    hwtest::hal::HalStatus writeTask(
        const hwtest::hal::SampleTaskId&,
        const hwtest::hal::SampleTaskBlock&,
        const hwtest::hal::OperationOptions&) override { return {}; }
    hwtest::hal::HalResult<hwtest::hal::SampleTaskStatus> taskStatus(
        const hwtest::hal::SampleTaskId&,
        const hwtest::hal::OperationOptions&) override { return {}; }
    hwtest::hal::HalStatus stopTask(
        const hwtest::hal::SampleTaskId&,
        const hwtest::hal::OperationOptions&) override
    {
        ++stopCount;
        return {};
    }
    hwtest::hal::HalStatus closeTask(
        const hwtest::hal::SampleTaskId&,
        const hwtest::hal::OperationOptions&) override
    {
        ++closeCount;
        return {};
    }

    hwtest::hal::SampleTaskConfig lastConfig;
    hwtest::hal::SampleTaskBlock block;
    int startCount = 0;
    int readCount = 0;
    int stopCount = 0;
    int closeCount = 0;
};

class FakeDevice final : public hwtest::hal::IHalDevice {
public:
    hwtest::hal::DeviceDescriptor descriptor() const override { return {}; }
    hwtest::hal::DeviceCapabilities capabilities() const override { return {}; }
    hwtest::hal::IAnalogIo* analogIo() override { return analog; }
    hwtest::hal::IDigitalIo* digitalIo() override { return digital; }
    hwtest::hal::ISerialBus* serialBus() override { return nullptr; }
    hwtest::hal::ICanFdBus* canFdBus() override { return nullptr; }
    hwtest::hal::ISampleTaskIo* sampleTasks() override { return tasks; }

    FakeAnalogIo* analog = nullptr;
    FakeDigitalIo* digital = nullptr;
    FakeSampleTasks* tasks = nullptr;
};

TEST(HalBoardTestFixtureTest, RoutesDigitalAndFiniteAnalogAcquisitionToPxi6259)
{
    FakeDigitalIo digital;
    FakeSampleTasks tasks;
    tasks.block.sampleType = hwtest::hal::SampleValueType::Float64;
    tasks.block.channelCount = 1;
    tasks.block.samplesPerChannel = 7500;
    tasks.block.analogValues.fill(0.0, 7500);
    FakeDevice device;
    device.digital = &digital;
    device.tasks = &tasks;
    HalBoardTestFixture fixture;
    fixture.bind6259(&device);

    const auto levels = fixture.read6259Digital(
        {QStringLiteral("DUT_TX_ENABLE_SENSE"),
         QStringLiteral("DUT_ATTENUATOR_SENSE")}, 123);
    const auto captured = fixture.capture6259Analog(
        {QStringLiteral("HELM_PWM1_SENSE")}, 1250000.0, 7500, 456);

    ASSERT_TRUE(levels.ok());
    ASSERT_EQ(levels.value.size(), 2);
    EXPECT_EQ(levels.value.front().level, hwtest::hal::DigitalLevel::High);
    ASSERT_TRUE(captured.ok());
    EXPECT_EQ(captured.value.analogValues, tasks.block.analogValues);
    EXPECT_EQ(tasks.lastConfig.kind, hwtest::hal::SampleTaskKind::AnalogInput);
    EXPECT_EQ(tasks.lastConfig.mode, hwtest::hal::SampleTaskMode::Finite);
    EXPECT_DOUBLE_EQ(tasks.lastConfig.sampleRateHz, 1250000.0);
    EXPECT_EQ(tasks.lastConfig.samplesPerChannel, 7500);
    EXPECT_EQ(tasks.startCount, 1);
    EXPECT_EQ(tasks.readCount, 1);
    EXPECT_EQ(tasks.stopCount, 1);
    EXPECT_EQ(tasks.closeCount, 1);
}

TEST(HalBoardTestFixtureTest, RejectsShortAndNonFiniteFiniteAnalogCapture)
{
    FakeSampleTasks tasks;
    tasks.block.sampleType = hwtest::hal::SampleValueType::Float64;
    tasks.block.channelCount = 1;
    tasks.block.samplesPerChannel = 7499;
    tasks.block.analogValues.fill(0.0, 7499);
    FakeDevice device;
    device.tasks = &tasks;
    HalBoardTestFixture fixture;
    fixture.bind6259(&device);

    EXPECT_FALSE(fixture.capture6259Analog(
        {QStringLiteral("HELM_PWM1_SENSE")}, 1250000.0, 7500, 456).ok());

    tasks.block.samplesPerChannel = 7500;
    tasks.block.analogValues.fill(0.0, 7500);
    tasks.block.analogValues[123] = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(fixture.capture6259Analog(
        {QStringLiteral("HELM_PWM1_SENSE")}, 1250000.0, 7500, 456).ok());
}

TEST(HalBoardTestFixtureTest, RoutesAoBatchTo6733AndFailsClosedWhenUnbound)
{
    FakeAnalogIo analog;
    FakeDevice device;
    device.analog = &analog;
    HalBoardTestFixture fixture;
    fixture.bind6733(&device);
    const QMap<hwtest::hal::ResourceId, double> values{
        {QStringLiteral("HELM_FK1_STIM"), 2.5},
        {QStringLiteral("HELM_FK2_STIM"), 0.0},
    };

    const hwtest::hal::HalStatus written = fixture.write6733Analog(values, 789);

    ASSERT_TRUE(written.ok());
    EXPECT_EQ(analog.writeBatchCount, 1);
    EXPECT_EQ(analog.lastValues, values);
    EXPECT_EQ(analog.lastOptions.op.timeoutMs, 789);
    EXPECT_DOUBLE_EQ(analog.lastOptions.range.minValue, 0.0);
    EXPECT_DOUBLE_EQ(analog.lastOptions.range.maxValue, 5.0);

    fixture.clear();
    EXPECT_FALSE(fixture.write6733Analog(values, 789).ok());
    EXPECT_FALSE(fixture.read6259Digital({}, 10).ok());
}

TEST(HalBoardTestFixtureTest, ZeroAoIsBestEffortAcrossAll6733Channels)
{
    FakeAnalogIo analog;
    analog.failWriteChannel = QStringLiteral("HELM_FK2_STIM");
    FakeDevice device;
    device.analog = &analog;
    HalBoardTestFixture fixture;
    fixture.bind6733(&device);
    const QMap<hwtest::hal::ResourceId, double> values{
        {QStringLiteral("HELM_FK1_STIM"), 0.0},
        {QStringLiteral("HELM_FK2_STIM"), 0.0},
        {QStringLiteral("HELM_FK3_STIM"), 0.0},
        {QStringLiteral("HELM_FK4_STIM"), 0.0},
    };

    const hwtest::hal::HalStatus status = fixture.write6733Analog(values, 789);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(analog.writeBatchCount, 0);
    EXPECT_EQ(analog.writeChannels,
              QVector<hwtest::hal::ResourceId>({
                  QStringLiteral("HELM_FK1_STIM"),
                  QStringLiteral("HELM_FK2_STIM"),
                  QStringLiteral("HELM_FK3_STIM"),
                  QStringLiteral("HELM_FK4_STIM"),
              }));
}

} // namespace
} // namespace hwtest::app
