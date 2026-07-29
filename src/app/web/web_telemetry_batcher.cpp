#include "web_telemetry_batcher.h"

#include "web_protocol.h"

#include <algorithm>
#include <utility>

namespace hwtest::app::web {

WebTelemetryBatcher::WebTelemetryBatcher(WebTelemetryBatcherOptions options,
                                         QObject* parent)
    : QObject(parent)
    , m_options(options)
{
    m_options.maxSamples = std::max(1, std::min(m_options.maxSamples, 64));
    m_options.maxBytes = std::max<qint64>(1, m_options.maxBytes);
    m_options.maxLatencyMs = std::max(0, m_options.maxLatencyMs);
    m_latencyTimer.setSingleShot(true);
    QObject::connect(&m_latencyTimer,
                     &QTimer::timeout,
                     this,
                     [this] { flush(); });
}

void WebTelemetryBatcher::setFlushCallback(FlushCallback callback)
{
    m_flushCallback = std::move(callback);
}

bool WebTelemetryBatcher::enqueueSample(quint64 sequence,
                                        const ApplicationSample& sample)
{
    if (makeSampleBatch(sequence, {sample}).isEmpty()) {
        return false;
    }

    if (!m_samples.isEmpty() && sample.taskId != m_taskId) {
        flush();
    }
    if (!m_samples.isEmpty()) {
        const quint64 expectedSequence =
            m_firstSequence + static_cast<quint64>(m_samples.size());
        if (sequence != expectedSequence) {
            return false;
        }

        QVector<ApplicationSample> candidate = m_samples;
        candidate.push_back(sample);
        if (batchWouldReachByteLimit(candidate)) {
            flush();
        }
    }

    if (m_samples.isEmpty()) {
        beginBatch(sequence, sample);
    } else {
        m_samples.push_back(sample);
    }

    if (m_samples.size() >= m_options.maxSamples ||
        batchWouldReachByteLimit(m_samples)) {
        flush();
    }
    return true;
}

void WebTelemetryBatcher::flush()
{
    if (m_samples.isEmpty()) {
        return;
    }

    const QJsonObject batch = makeSampleBatch(m_firstSequence, m_samples);
    clear();
    if (!batch.isEmpty() && m_flushCallback) {
        m_flushCallback(batch);
    }
}

void WebTelemetryBatcher::clear()
{
    m_latencyTimer.stop();
    m_samples.clear();
    m_firstSequence = 0;
    m_taskId.clear();
}

bool WebTelemetryBatcher::hasPendingSamples() const
{
    return !m_samples.isEmpty();
}

void WebTelemetryBatcher::beginBatch(quint64 firstSequence,
                                     const ApplicationSample& sample)
{
    m_firstSequence = firstSequence;
    m_taskId = sample.taskId;
    m_samples.push_back(sample);
    m_latencyTimer.start(m_options.maxLatencyMs);
}

bool WebTelemetryBatcher::batchWouldReachByteLimit(
    const QVector<ApplicationSample>& samples) const
{
    const QJsonObject batch = makeSampleBatch(m_firstSequence, samples);
    return !batch.isEmpty() &&
        compactJson(batch).toUtf8().size() >= m_options.maxBytes;
}

} // namespace hwtest::app::web
