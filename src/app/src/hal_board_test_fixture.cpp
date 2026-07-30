#include "hal_board_test_fixture.h"

#include <hal/i_analog_io.h>
#include <hal/i_digital_io.h>
#include <hal/i_hal_device.h>
#include <hal/i_sample_task_io.h>

#include <QThread>

#include <algorithm>
#include <cmath>

namespace hwtest::app {

BoardFixtureRequirement boardFixtureRequirement(
    const QString& algorithmId,
    const QVariantMap& normalizedRunParameters)
{
    if (algorithmId == QStringLiteral("mbddf.do_write")) {
        return {true, false};
    }
    if (algorithmId == QStringLiteral("mbddf.helm_board_test") &&
        normalizedRunParameters.value(QStringLiteral("test_mode"), 0).toInt() == 0) {
        return {true, true};
    }
    return {};
}

namespace {

hwtest::hal::HalStatus failure(hwtest::hal::HalStatusCode code,
                               const QString& message,
                               const QString& operation)
{
    hwtest::hal::HalStatus status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    status.error.operation = operation;
    return status;
}

template <typename T>
hwtest::hal::HalResult<T> failedResult(hwtest::hal::HalStatusCode code,
                                      const QString& message,
                                      const QString& operation)
{
    hwtest::hal::HalResult<T> result;
    result.status = failure(code, message, operation);
    return result;
}

} // namespace

void HalBoardTestFixture::bind6259(hwtest::hal::IHalDevice* device) noexcept
{
    m_pxi6259 = device;
}

void HalBoardTestFixture::bind6733(hwtest::hal::IHalDevice* device) noexcept
{
    m_pxi6733 = device;
}

void HalBoardTestFixture::clear() noexcept
{
    m_pxi6259 = nullptr;
    m_pxi6733 = nullptr;
}

hwtest::hal::HalResult<QVector<hwtest::hal::DigitalSample>>
HalBoardTestFixture::read6259Digital(
    const QVector<hwtest::hal::ResourceId>& resources,
    int timeoutMs)
{
    if (m_pxi6259 == nullptr || m_pxi6259->digitalIo() == nullptr) {
        return failedResult<QVector<hwtest::hal::DigitalSample>>(
            hwtest::hal::HalStatusCode::NotInitialized,
            QStringLiteral("PXI-6259 digital input is not bound"),
            QStringLiteral("boardFixture.read6259Digital"));
    }
    if (resources.isEmpty() || timeoutMs <= 0) {
        return failedResult<QVector<hwtest::hal::DigitalSample>>(
            hwtest::hal::HalStatusCode::InvalidArgument,
            QStringLiteral("PXI-6259 digital resources and timeout are required"),
            QStringLiteral("boardFixture.read6259Digital"));
    }
    hwtest::hal::OperationOptions options;
    options.timeoutMs = timeoutMs;
    options.retryCount = 0;
    return m_pxi6259->digitalIo()->readDiBatch(resources, options);
}

hwtest::hal::HalResult<hwtest::hal::SampleTaskBlock>
HalBoardTestFixture::capture6259Analog(
    const QVector<hwtest::hal::ResourceId>& resources,
    double sampleRateHz,
    int samplesPerChannel,
    int timeoutMs)
{
    if (m_pxi6259 == nullptr || m_pxi6259->sampleTasks() == nullptr) {
        return failedResult<hwtest::hal::SampleTaskBlock>(
            hwtest::hal::HalStatusCode::NotInitialized,
            QStringLiteral("PXI-6259 sample task I/O is not bound"),
            QStringLiteral("boardFixture.capture6259Analog"));
    }
    if (resources.isEmpty() || sampleRateHz <= 0.0 ||
        samplesPerChannel <= 0 || timeoutMs <= 0) {
        return failedResult<hwtest::hal::SampleTaskBlock>(
            hwtest::hal::HalStatusCode::InvalidArgument,
            QStringLiteral("PXI-6259 finite capture settings are invalid"),
            QStringLiteral("boardFixture.capture6259Analog"));
    }

    hwtest::hal::SampleTaskConfig config;
    config.kind = hwtest::hal::SampleTaskKind::AnalogInput;
    config.mode = hwtest::hal::SampleTaskMode::Finite;
    config.channels = resources;
    config.clock.rateHz = sampleRateHz;
    config.sampleRateHz = sampleRateHz;
    config.samplesPerChannel = samplesPerChannel;
    config.bufferSamplesPerChannel = samplesPerChannel;

    hwtest::hal::OperationOptions options;
    options.timeoutMs = timeoutMs;
    options.retryCount = 0;
    hwtest::hal::ISampleTaskIo* tasks = m_pxi6259->sampleTasks();
    const auto created = tasks->createTask(config, options);
    if (!created.ok()) return {created.status, {}};

    const hwtest::hal::SampleTaskId taskId = created.value;
    const hwtest::hal::HalStatus started = tasks->startTask(taskId, options);
    if (!started.ok()) {
        tasks->closeTask(taskId, options);
        return {started, {}};
    }

    auto captured = tasks->readTask(taskId, samplesPerChannel, options);
    const hwtest::hal::HalStatus stopped = tasks->stopTask(taskId, options);
    const hwtest::hal::HalStatus closed = tasks->closeTask(taskId, options);
    if (!captured.ok()) return captured;
    if (!stopped.ok()) return {stopped, {}};
    if (!closed.ok()) return {closed, {}};
    const qint64 expectedValueCount =
        static_cast<qint64>(resources.size()) * samplesPerChannel;
    const bool finiteValues = std::all_of(
        captured.value.analogValues.cbegin(),
        captured.value.analogValues.cend(),
        [](double value) { return std::isfinite(value); });
    if (captured.value.sampleType != hwtest::hal::SampleValueType::Float64 ||
        captured.value.channelCount != resources.size() ||
        captured.value.samplesPerChannel != samplesPerChannel ||
        captured.value.analogValues.size() != expectedValueCount ||
        !finiteValues) {
        return failedResult<hwtest::hal::SampleTaskBlock>(
            hwtest::hal::HalStatusCode::DataMismatch,
            QStringLiteral("PXI-6259 returned an incomplete, non-finite, or malformed analog block"),
            QStringLiteral("boardFixture.capture6259Analog"));
    }
    return captured;
}

hwtest::hal::HalStatus HalBoardTestFixture::write6733Analog(
    const QMap<hwtest::hal::ResourceId, double>& values,
    int timeoutMs)
{
    if (m_pxi6733 == nullptr || m_pxi6733->analogIo() == nullptr) {
        return failure(hwtest::hal::HalStatusCode::NotInitialized,
                       QStringLiteral("PXI-6733 analog output is not bound"),
                       QStringLiteral("boardFixture.write6733Analog"));
    }
    if (values.isEmpty() || timeoutMs <= 0) {
        return failure(hwtest::hal::HalStatusCode::InvalidArgument,
                       QStringLiteral("PXI-6733 output values and timeout are required"),
                       QStringLiteral("boardFixture.write6733Analog"));
    }
    hwtest::hal::AnalogWriteOptions options;
    options.op.timeoutMs = timeoutMs;
    options.op.retryCount = 0;
    options.range.minValue = 0.0;
    options.range.maxValue = 5.0;
    options.range.unit = hwtest::hal::AnalogUnit::Volt;
    options.safeClamp = true;
    bool allZero = true;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!std::isfinite(it.value())) {
            return failure(hwtest::hal::HalStatusCode::InvalidArgument,
                           QStringLiteral("PXI-6733 output values must be finite"),
                           QStringLiteral("boardFixture.write6733Analog"));
        }
        allZero = allZero && it.value() == 0.0;
    }
    if (!allZero) {
        return m_pxi6733->analogIo()->writeDaBatch(values, options);
    }

    hwtest::hal::HalStatus firstFailure;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const hwtest::hal::HalStatus written =
            m_pxi6733->analogIo()->writeDa(it.key(), 0.0, options);
        if (!written.ok() && firstFailure.ok()) firstFailure = written;
    }
    return firstFailure;
}

void HalBoardTestFixture::settle(int milliseconds)
{
    if (milliseconds > 0) {
        QThread::msleep(static_cast<unsigned long>(milliseconds));
    }
}

} // namespace hwtest::app
