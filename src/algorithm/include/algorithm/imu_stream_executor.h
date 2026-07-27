#pragma once

#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace hwtest::algorithm::mbddf {

class ImuStreamAlgorithmExecutor final : public hwtest::biz::IAlgorithmExecutor {
public:
    explicit ImuStreamAlgorithmExecutor(std::unique_ptr<IByteTransport> transport);
    ~ImuStreamAlgorithmExecutor() override;

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

    const ProtocolCatalog& catalog() const noexcept;

private:
    hwtest::biz::Status stopStream(const hwtest::biz::TestStep* step,
                                   hwtest::biz::IAlgorithmObserver* observer);
    hwtest::biz::Status decodeResponse(const QByteArray& frame,
                                       const MessageDefinition** definition,
                                       QVariantMap* values) const;
    hwtest::biz::Status publishFeedback(const QByteArray& frame,
                                        const QVariantMap& values,
                                        const hwtest::biz::TestStep& step,
                                        hwtest::biz::IAlgorithmObserver& observer);
    TransportResult writeFrame(const QByteArray& frame, int timeoutMs);
    TransportResult readFrame(int timeoutMs);

    std::unique_ptr<IByteTransport> m_transport;
    ProtocolCatalog m_catalog;
    hwtest::biz::TestContext m_context;
    const MessageDefinition* m_startRequest = nullptr;
    const MessageDefinition* m_startResponse = nullptr;
    const MessageDefinition* m_feedbackResponse = nullptr;
    const MessageDefinition* m_stopRequest = nullptr;
    const MessageDefinition* m_stopResponse = nullptr;
    quint16 m_nextSequence = 0;
    int m_readTimeoutMs = 20;
    int m_startTimeoutMs = 2000;
    int m_stopTimeoutMs = 2000;
    quint64 m_sampleCount = 0;
    QVariantMap m_lastValues;
    QByteArray m_lastFeedbackFrame;
    std::atomic_bool m_stopRequested{false};
    mutable std::mutex m_transportMutex;
    bool m_prepared = false;
    bool m_streamMayBeActive = false;
    bool m_stopConfirmed = false;
    // A failed write or ACK still consumes the single STOP attempt for this session.
    bool m_stopAttempted = false;
};

} // namespace hwtest::algorithm::mbddf
