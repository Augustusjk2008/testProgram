#pragma once

#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <hal/hal_types.h>

#include <atomic>
#include <memory>

namespace hwtest::hal {
class IControlChannel;
}

namespace hwtest::algorithm::mbddf {

// Presents the product's internal loopback and PC-assisted echo protocols as
// one single-run algorithm. The PC-assisted path keeps the control transport
// owned by this executor and delegates the physical link handshake to
// BusEchoTransport for every 03/02 round.
class SerialTestAlgorithmExecutor final : public hwtest::biz::IAlgorithmExecutor {
public:
    SerialTestAlgorithmExecutor(std::unique_ptr<IByteTransport> controlTransport,
                                hwtest::hal::IControlChannel* auxiliaryControlChannel,
                                hwtest::hal::ResourceId controlResourceId);
    ~SerialTestAlgorithmExecutor() override;

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
    std::unique_ptr<IByteTransport> m_controlTransport;
    std::unique_ptr<hwtest::biz::IAlgorithmExecutor> m_delegate;
    hwtest::hal::IControlChannel* m_auxiliaryControlChannel = nullptr;
    hwtest::hal::ResourceId m_controlResourceId;
    hwtest::biz::TestStep m_delegateStep;
    int m_testMode = 0;
    int m_cycleCount = 1;
    std::atomic_bool m_stopRequested{false};
    bool m_prepared = false;
};

} // namespace hwtest::algorithm::mbddf
