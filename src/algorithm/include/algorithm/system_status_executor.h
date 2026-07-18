#pragma once

#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace hwtest::algorithm::mbddf {

class SystemStatusAlgorithmExecutor final : public hwtest::biz::IAlgorithmExecutor {
public:
    explicit SystemStatusAlgorithmExecutor(std::unique_ptr<IByteTransport> transport);
    ~SystemStatusAlgorithmExecutor() override;

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

    const ProtocolCatalog& catalog() const noexcept;

private:
    hwtest::biz::Status makeStatus(hwtest::biz::ErrorCode code,
                                   const QString& message,
                                   const QString& operation = {}) const;
    hwtest::biz::Result<hwtest::biz::TestResult> failure(
        const hwtest::biz::TestStep& step,
        hwtest::biz::ErrorCode code,
        const QString& message) const;
    hwtest::biz::Result<hwtest::biz::TestResult> protocolFailure(
        const hwtest::biz::TestStep& step,
        const QString& message) const;
    bool evaluateCriteria(const hwtest::biz::TestStep& step,
                          const QVariantMap& values,
                          QString* failureMessage) const;
    static bool compare(const QVariant& actual,
                        const hwtest::biz::Criterion& criterion);
    static hwtest::biz::MeasurementRecord measurement(const QString& name,
                                                      const QVariant& value);

    std::unique_ptr<IByteTransport> m_transport;
    ProtocolCatalog m_catalog;
    hwtest::biz::TestPlan m_plan;
    hwtest::biz::TestContext m_context;
    const MessageDefinition* m_request = nullptr;
    const MessageDefinition* m_response = nullptr;
    quint16 m_nextSequence = 0;
    std::atomic_bool m_stopRequested{false};
    mutable std::mutex m_transportMutex;
    bool m_prepared = false;
};

} // namespace hwtest::algorithm::mbddf
