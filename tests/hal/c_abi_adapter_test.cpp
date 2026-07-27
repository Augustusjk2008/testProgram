#include "c_abi_adapter.h"
#include "hal/i_sample_task_io.h"

#include <gtest/gtest.h>

#include <QLibrary>
#include <QThread>

#include <thread>

using namespace hwtest::hal;

namespace {

QVariantMap adapterConfig(const QString& libraryPath)
{
    QVariantMap config;
    config.insert(QStringLiteral("adapterId"), QStringLiteral("fixture.digital.v1"));
    config.insert(QStringLiteral("libraryPath"), libraryPath);
    config.insert(QStringLiteral("settings"),
                  QVariantMap{{QStringLiteral("deviceName"), QStringLiteral("fixture_device")}});
    return config;
}

QVariantMap niAdapterConfig(const QString& libraryPath)
{
    return QVariantMap{{QStringLiteral("adapterId"), QStringLiteral("ni.daqmx")},
                       {QStringLiteral("libraryPath"), libraryPath},
                       {QStringLiteral("settings"),
                        QVariantMap{{QStringLiteral("timeoutSeconds"), 0.25}}}};
}

QVariantMap niDeviceOpenSpec()
{
    const QVariantMap out{{QStringLiteral("resourceId"), QStringLiteral("OUT0")},
        {QStringLiteral("module"), QStringLiteral("digital")},
        {QStringLiteral("direction"), QStringLiteral("output")},
        {QStringLiteral("physicalIndex"), 0},
        {QStringLiteral("properties"),
         QVariantMap{{QStringLiteral("portNumber"), 0},
                     {QStringLiteral("lineNumber"), 0}}}};
    const QVariantMap in{{QStringLiteral("resourceId"), QStringLiteral("IN0")},
        {QStringLiteral("module"), QStringLiteral("digital")},
        {QStringLiteral("direction"), QStringLiteral("input")},
        {QStringLiteral("physicalIndex"), 16},
        {QStringLiteral("properties"),
         QVariantMap{{QStringLiteral("portNumber"), 1},
                     {QStringLiteral("lineNumber"), 0}}}};
    return QVariantMap{
        {QStringLiteral("schema"), QStringLiteral("hwtest.adapter-device-open")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("physicalDeviceId"), QStringLiteral("PXI1Slot2")},
        {QStringLiteral("device"),
         QVariantMap{{QStringLiteral("deviceId"), QStringLiteral("ni6259_stimulus")},
                     {QStringLiteral("adapterId"), QStringLiteral("ni.daqmx")},
                     {QStringLiteral("model"), QStringLiteral("PXI-6259")},
                     {QStringLiteral("serialNumber"), QStringLiteral("62590002")}}},
        {QStringLiteral("channels"), QVariantList{out, in}},
        {QStringLiteral("taskProfiles"), QVariantList{}},
        {QStringLiteral("safeState"),
         QVariantMap{{QStringLiteral("OUT0"), QStringLiteral("Low")}}}};
}

} // namespace

TEST(CAbiAdapterTest, LoadsAbiAndRoundTripsDigitalBatch)
{
    CAbiAdapter adapter;
    ASSERT_TRUE(adapter.initialize(adapterConfig(
        QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH))).ok());
    EXPECT_EQ(adapter.adapterId(), QStringLiteral("fixture.digital.v1"));

    const auto devices = adapter.enumerateDevices(OperationOptions{});
    ASSERT_TRUE(devices.ok());
    ASSERT_EQ(devices.value.size(), 1);
    EXPECT_EQ(devices.value.first().deviceId, QStringLiteral("fixture_device"));

    const auto capabilities = adapter.queryCapabilities(
        QStringLiteral("fixture_device"), OperationOptions{});
    ASSERT_TRUE(capabilities.ok());
    EXPECT_TRUE(capabilities.value.supportedModules.contains(
        QStringLiteral("digital")));

    const auto session = adapter.openDevice(QStringLiteral("fixture_device"),
                                            QVariantMap{}, OperationOptions{});
    ASSERT_TRUE(session.ok());
    ASSERT_TRUE(adapter.writeDigitalBatch(
        session.value,
        QMap<int, DigitalLevel>{{0, DigitalLevel::High}, {1, DigitalLevel::Low}},
        DigitalWriteOptions{}).ok());
    const auto samples = adapter.readDigitalBatch(
        session.value, QVector<int>{0, 1}, OperationOptions{});
    ASSERT_TRUE(samples.ok());
    ASSERT_EQ(samples.value.size(), 2);
    EXPECT_EQ(samples.value.at(0).level, DigitalLevel::High);
    EXPECT_EQ(samples.value.at(1).level, DigitalLevel::Low);
    EXPECT_TRUE(adapter.closeDevice(session.value, OperationOptions{}).ok());
    EXPECT_TRUE(adapter.shutdown().ok());

    CAbiAdapter niAdapter;
    ASSERT_TRUE(niAdapter.initialize(niAdapterConfig(
        QString::fromLatin1(HAL_TEST_NI_DAQMX_ADAPTER_FIXTURE_PATH))).ok());
    const auto niSession = niAdapter.openDevice(
        QStringLiteral("ni6259_stimulus"), niDeviceOpenSpec(), OperationOptions{});
    ASSERT_TRUE(niSession.ok()) << niSession.status.error.message.toStdString();
    EXPECT_TRUE(niAdapter.writeDigital(
        niSession.value, 0, DigitalLevel::High, DigitalWriteOptions{}).ok());
    EXPECT_TRUE(niAdapter.closeDevice(niSession.value, OperationOptions{}).ok());
    EXPECT_TRUE(niAdapter.shutdown().ok());
}

TEST(CAbiAdapterTest, MapsVendorStatusAndRejectsMissingFunctions)
{
    CAbiAdapter adapter;
    ASSERT_TRUE(adapter.initialize(adapterConfig(
        QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH))).ok());
    const auto session = adapter.openDevice(QStringLiteral("fixture_device"),
                                            QVariantMap{}, OperationOptions{});
    ASSERT_TRUE(session.ok());
    const HalStatus timedOut = adapter.writeDigital(
        session.value, 63, DigitalLevel::High, DigitalWriteOptions{});
    EXPECT_EQ(timedOut.code, HalStatusCode::Timeout);
    EXPECT_EQ(timedOut.error.adapterCode, QStringLiteral("6259"));
    EXPECT_NE(timedOut.error.message.indexOf(QStringLiteral("fixture timeout")), -1);

    CAbiAdapter missingFunctions;
    const HalStatus unsupported = missingFunctions.initialize(adapterConfig(
        QString::fromLatin1(HAL_TEST_ADAPTER_FIXTURE_PATH)));
    EXPECT_EQ(unsupported.code, HalStatusCode::NotSupported);
}

TEST(CAbiAdapterTest, ReportsLibraryLoadFailure)
{
    CAbiAdapter adapter;
    const HalStatus status = adapter.initialize(adapterConfig(
        QStringLiteral("Z:/definitely/missing/hwtest_adapter.dll")));
    EXPECT_EQ(status.code, HalStatusCode::AdapterLoadFailed);
}

TEST(CAbiAdapterTest, MapsLogicalAliasWhenQueryingCapabilities)
{
    CAbiAdapter adapter;
    ASSERT_TRUE(adapter.initialize(adapterConfig(
        QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH))).ok());

    const auto capabilities = adapter.queryCapabilities(
        QStringLiteral("logical_fixture_alias"), OperationOptions{});

    ASSERT_TRUE(capabilities.ok()) << capabilities.status.error.message.toStdString();
    EXPECT_EQ(capabilities.value.device.deviceId,
              QStringLiteral("logical_fixture_alias"));
    EXPECT_TRUE(adapter.shutdown().ok());
}

TEST(CAbiAdapterTest, CloseFailureStillConsumesTheNativeHandle)
{
    CAbiAdapter adapter;
    ASSERT_TRUE(adapter.initialize(adapterConfig(
        QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH))).ok());
    const auto session = adapter.openDevice(
        QStringLiteral("fixture_device"),
        QVariantMap{{QStringLiteral("failClose"), true}},
        OperationOptions{});
    ASSERT_TRUE(session.ok());

    const HalStatus closed = adapter.closeDevice(session.value, OperationOptions{});

    EXPECT_EQ(closed.code, HalStatusCode::IoError);
    EXPECT_EQ(adapter.healthCheck(session.value, OperationOptions{}).code,
              HalStatusCode::NotFound);
    EXPECT_TRUE(adapter.shutdown().ok());
}

TEST(CAbiAdapterTest, SerializesNativeIoAgainstClose)
{
    const QString libraryPath = QString::fromLatin1(
        HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH);
    QLibrary fixture(libraryPath);
    ASSERT_TRUE(fixture.load()) << fixture.errorString().toStdString();
    using WriteEntered = int (HAL_ADAPTER_CALL*)();
    const auto writeEntered = reinterpret_cast<WriteEntered>(
        fixture.resolve("hal_fixture_write_entered"));
    ASSERT_NE(writeEntered, nullptr);

    CAbiAdapter adapter;
    ASSERT_TRUE(adapter.initialize(adapterConfig(libraryPath)).ok());
    const auto session = adapter.openDevice(
        QStringLiteral("fixture_device"),
        QVariantMap{{QStringLiteral("slowWrite"), true}},
        OperationOptions{});
    ASSERT_TRUE(session.ok());

    HalStatus writeStatus;
    std::thread writer([&] {
        writeStatus = adapter.writeDigital(
            session.value, 0, DigitalLevel::High, DigitalWriteOptions{});
    });
    for (int attempt = 0; attempt < 1000 && writeEntered() == 0; ++attempt) {
        QThread::msleep(1);
    }
    const int entered = writeEntered();
    if (entered == 0) writer.join();
    ASSERT_EQ(entered, 1);

    const HalStatus closeStatus = adapter.closeDevice(
        session.value, OperationOptions{});
    writer.join();

    EXPECT_TRUE(writeStatus.ok());
    EXPECT_TRUE(closeStatus.ok()) << closeStatus.error.message.toStdString();
    EXPECT_TRUE(adapter.shutdown().ok());
    fixture.unload();
}

TEST(CAbiAdapterTest, PreservesEverySampleFromContinuousAnalogTask)
{
    CAbiAdapter adapter;
    ASSERT_TRUE(adapter.initialize(adapterConfig(
        QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH))).ok());
    const auto session = adapter.openDevice(QStringLiteral("fixture_device"),
                                            QVariantMap{},
                                            OperationOptions{});
    ASSERT_TRUE(session.ok());

    SampleTaskConfig config;
    config.kind = SampleTaskKind::AnalogInput;
    config.mode = SampleTaskMode::Continuous;
    config.sampleRateHz = 20000.0;
    config.samplesPerChannel = 4;
    config.bufferSamplesPerChannel = 16;
    const auto task = adapter.createSampleTask(session.value,
                                               QVector<int>{0, 1},
                                               config,
                                               OperationOptions{});
    ASSERT_TRUE(task.ok()) << task.status.error.message.toStdString();
    ASSERT_TRUE(adapter.startSampleTask(task.value, OperationOptions{}).ok());

    const auto block = adapter.readSampleTask(task.value, 4, OperationOptions{});

    ASSERT_TRUE(block.ok()) << block.status.error.message.toStdString();
    EXPECT_EQ(block.value.channelCount, 2);
    EXPECT_EQ(block.value.samplesPerChannel, 4);
    EXPECT_EQ(block.value.sampleType, SampleValueType::Float64);
    const double expected[] = {1.0, 2.0, 3.0, 4.0, 11.0, 12.0, 13.0, 14.0};
    ASSERT_EQ(block.value.analogValues.size(), 8);
    for (int index = 0; index < 8; ++index) {
        EXPECT_DOUBLE_EQ(block.value.analogValues.at(index), expected[index]);
    }
    ASSERT_TRUE(adapter.stopSampleTask(task.value, OperationOptions{}).ok());
    ASSERT_TRUE(adapter.closeSampleTask(task.value, OperationOptions{}).ok());
    ASSERT_TRUE(adapter.closeDevice(session.value, OperationOptions{}).ok());
    EXPECT_TRUE(adapter.shutdown().ok());
}
