#pragma once

#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace hwtest::algorithm::mbddf {

class HelmStreamAlgorithmExecutor final : public hwtest::biz::IAlgorithmExecutor {
public:
    explicit HelmStreamAlgorithmExecutor(std::unique_ptr<IByteTransport> transport);
    ~HelmStreamAlgorithmExecutor() override;

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

    quint64 sampleCount() const noexcept;
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
    QVariantMap m_effectiveParameters;
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
    qint64 m_timestampAnchorUtcUs = 0;
    quint64 m_firstDdsTimestampUs = 0;
    quint64 m_lastDdsTimestampUs = 0;
    QVariantMap m_lastValues;
    QByteArray m_lastFeedbackFrame;
    quint16 m_lastProductSequence = 0;
    quint16 m_lastSerialA = 0;
    quint16 m_lastSerialB = 0;
    bool m_hasProductSequence = false;
    bool m_hasSerialA = false;
    bool m_hasSerialB = false;
    bool m_hasDdsTimestamp = false;
    std::atomic_bool m_stopRequested{false};
    mutable std::mutex m_transportMutex;
    bool m_prepared = false;
    bool m_streamMayBeActive = false;
    bool m_stopConfirmed = false;
    bool m_stopAttempted = false;
};

} // namespace hwtest::algorithm::mbddf
