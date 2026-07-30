#pragma once

#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <hal/i_sample_task_io.h>

#include <QMap>
#include <QVector>

#include <memory>

namespace hwtest::algorithm::mbddf {

struct PwmMeasurement {
    bool valid = false;
    int validCycles = 0;
    double dutyPercent = 0.0;
    double frequencyHz = 0.0;
    double lowVolts = 0.0;
    double highVolts = 0.0;
    QString reason;
};

PwmMeasurement measurePwm(const QVector<double>& samples,
                          double sampleRateHz,
                          int minimumCompleteCycles = 20);

// The algorithm owns test sequencing and criteria; the application composition
// root owns the actual HAL sessions and binds them to this narrow fixture seam.
class IBoardTestFixture {
public:
    virtual ~IBoardTestFixture() = default;

    virtual hwtest::hal::HalResult<QVector<hwtest::hal::DigitalSample>>
    read6259Digital(const QVector<hwtest::hal::ResourceId>& resources,
                    int timeoutMs) = 0;

    virtual hwtest::hal::HalResult<hwtest::hal::SampleTaskBlock>
    capture6259Analog(const QVector<hwtest::hal::ResourceId>& resources,
                      double sampleRateHz,
                      int samplesPerChannel,
                      int timeoutMs) = 0;

    virtual hwtest::hal::HalStatus write6733Analog(
        const QMap<hwtest::hal::ResourceId, double>& values,
        int timeoutMs) = 0;

    virtual void settle(int milliseconds) = 0;
};

class BoardTestExecutorState;

class DoWriteAlgorithmExecutor final : public hwtest::biz::IAlgorithmExecutor {
public:
    DoWriteAlgorithmExecutor(std::unique_ptr<IByteTransport> transport,
                             IBoardTestFixture* fixture);
    ~DoWriteAlgorithmExecutor() override;

    hwtest::biz::Status prepare(const hwtest::biz::TestPlan& plan,
                                const hwtest::biz::TestContext& context,
                                const QVariantMap& executionConfig) override;
    hwtest::biz::Result<hwtest::biz::TestResult> executeStep(
        const hwtest::biz::TestStep& step,
        const hwtest::biz::IRunControl& control,
        hwtest::biz::IAlgorithmObserver& observer) override;
    hwtest::biz::Status requestStop(int timeoutMs) override;
    hwtest::biz::Status reset() override;
    hwtest::biz::Status shutdown(int timeoutMs) override;
    hwtest::biz::Status finishRun() override;

private:
    std::unique_ptr<BoardTestExecutorState> m_state;
};

class HelmBoardTestAlgorithmExecutor final
    : public hwtest::biz::IAlgorithmExecutor {
public:
    HelmBoardTestAlgorithmExecutor(std::unique_ptr<IByteTransport> transport,
                                   IBoardTestFixture* fixture);
    ~HelmBoardTestAlgorithmExecutor() override;

    hwtest::biz::Status prepare(const hwtest::biz::TestPlan& plan,
                                const hwtest::biz::TestContext& context,
                                const QVariantMap& executionConfig) override;
    hwtest::biz::Result<hwtest::biz::TestResult> executeStep(
        const hwtest::biz::TestStep& step,
        const hwtest::biz::IRunControl& control,
        hwtest::biz::IAlgorithmObserver& observer) override;
    hwtest::biz::Status requestStop(int timeoutMs) override;
    hwtest::biz::Status reset() override;
    hwtest::biz::Status shutdown(int timeoutMs) override;
    hwtest::biz::Status finishRun() override;

private:
    std::unique_ptr<BoardTestExecutorState> m_state;
};

} // namespace hwtest::algorithm::mbddf
