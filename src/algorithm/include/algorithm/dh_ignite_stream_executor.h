#pragma once

#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace hwtest::algorithm::mbddf {

class DhIgniteStreamAlgorithmExecutor final
    : public hwtest::biz::IAlgorithmExecutor {
public:
    explicit DhIgniteStreamAlgorithmExecutor(
        std::unique_ptr<IByteTransport> transport);
    ~DhIgniteStreamAlgorithmExecutor() override;

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
    hwtest::biz::Status decodeResponse(const QByteArray& frame,
                                       QVariantMap* values) const;
    TransportResult writeFrame(const QByteArray& frame, int timeoutMs);
    TransportResult readFrame(int timeoutMs);

    std::unique_ptr<IByteTransport> m_transport;
    ProtocolCatalog m_catalog;
    hwtest::biz::TestContext m_context;
    const MessageDefinition* m_request = nullptr;
    const MessageDefinition* m_response = nullptr;
    QVariantMap m_effectiveRunParameters;
    QVariantMap m_requestValues;
    quint16 m_nextSequence = 0;
    quint32 m_reportCount = 0;
    quint32 m_intervalUs = 0;
    quint32 m_delayFrames = 0;
    int m_readTimeoutMs = 2000;
    int m_writeTimeoutMs = 2000;
    mutable std::mutex m_transportMutex;
    std::atomic_bool m_requestAccepted{false};
    bool m_prepared = false;
};

} // namespace hwtest::algorithm::mbddf
