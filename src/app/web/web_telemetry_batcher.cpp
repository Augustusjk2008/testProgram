#include "web_telemetry_batcher.h"

#include "web_protocol.h"

#include <algorithm>
#include <utility>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>

namespace hwtest::app::web {
namespace {

constexpr quint64 kMaxJsonSafeInteger = 9007199254740991ULL;

qint64 projectedBatchUtf8Bytes(quint64 firstSequence,
                               int sampleCount,
                               qint64 projectedObjectBytes)
{
    if (sampleCount < 1 || projectedObjectBytes < 0 ||
        firstSequence > kMaxJsonSafeInteger) {
        return -1;
    }
    const quint64 sampleCountMinusOne = static_cast<quint64>(sampleCount - 1);
    if (sampleCountMinusOne > kMaxJsonSafeInteger - firstSequence) {
        return -1;
    }
    const quint64 lastSequence = firstSequence + sampleCountMinusOne;
    return QByteArrayLiteral("{\"v\":1,\"type\":\"sampleBatch\",\"firstSeq\":").size() +
        QByteArray::number(firstSequence).size() +
        QByteArrayLiteral(",\"lastSeq\":").size() +
        QByteArray::number(lastSequence).size() +
        QByteArrayLiteral(",\"samples\":[").size() +
        projectedObjectBytes + static_cast<qint64>(sampleCount - 1) +
        QByteArrayLiteral("]}").size();
}

} // namespace

WebTelemetryBatcher::WebTelemetryBatcher(WebTelemetryBatcherOptions options,
                                         QObject* parent)
    : QObject(parent)
    , m_options(options)
{
    m_options.maxSamples = std::max(1, std::min(m_options.maxSamples, 64));
    m_options.maxBytes = std::max<qint64>(1, m_options.maxBytes);
    m_options.maxLatencyMs = std::max(0, m_options.maxLatencyMs);
    m_samples.reserve(m_options.maxSamples);
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
    const QJsonObject event = makeSample(sequence, sample);
    const QJsonObject projectedObject =
        event.value(QStringLiteral("sample")).toObject();
    if (projectedObject.isEmpty()) {
        return false;
    }
    ProjectedSample projected{
        projectedObject,
        QJsonDocument(projectedObject).toJson(QJsonDocument::Compact).size(),
    };

    if (!m_samples.isEmpty() && sample.taskId != m_taskId) {
        flush();
    }
    if (!m_samples.isEmpty()) {
        const quint64 expectedSequence =
            m_firstSequence + static_cast<quint64>(m_samples.size());
        if (sequence != expectedSequence) {
            return false;
        }

        if (batchWouldReachByteLimit(
                m_samples.size() + 1,
                m_projectedObjectBytes + projected.compactUtf8Bytes)) {
            flush();
        }
    }

    if (m_samples.isEmpty()) {
        beginBatch(sequence, sample.taskId, std::move(projected));
    } else {
        m_projectedObjectBytes += projected.compactUtf8Bytes;
        m_samples.push_back(std::move(projected));
    }

    if (m_samples.size() >= m_options.maxSamples ||
        batchWouldReachByteLimit(m_samples.size(), m_projectedObjectBytes)) {
        flush();
    }
    return true;
}

void WebTelemetryBatcher::flush()
{
    if (m_samples.isEmpty()) {
        return;
    }

    const QJsonObject batch = makeBatch();
    clear();
    if (!batch.isEmpty() && m_flushCallback) {
        m_flushCallback(batch);
    }
}

void WebTelemetryBatcher::clear()
{
    m_latencyTimer.stop();
    m_samples.clear();
    m_projectedObjectBytes = 0;
    m_firstSequence = 0;
    m_taskId.clear();
}

bool WebTelemetryBatcher::hasPendingSamples() const
{
    return !m_samples.isEmpty();
}

void WebTelemetryBatcher::beginBatch(quint64 firstSequence,
                                     const QString& taskId,
                                     ProjectedSample sample)
{
    m_firstSequence = firstSequence;
    m_taskId = taskId;
    m_projectedObjectBytes = sample.compactUtf8Bytes;
    m_samples.push_back(std::move(sample));
    m_latencyTimer.start(m_options.maxLatencyMs);
}

QJsonObject WebTelemetryBatcher::makeBatch() const
{
    if (m_samples.isEmpty()) {
        return {};
    }
    QJsonArray projectedSamples;
    for (const ProjectedSample& sample : m_samples) {
        projectedSamples.push_back(sample.object);
    }
    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QStringLiteral("sampleBatch")},
        {QStringLiteral("firstSeq"), static_cast<double>(m_firstSequence)},
        {QStringLiteral("lastSeq"),
         static_cast<double>(m_firstSequence +
                             static_cast<quint64>(m_samples.size() - 1))},
        {QStringLiteral("samples"), projectedSamples},
    };
}

bool WebTelemetryBatcher::batchWouldReachByteLimit(
    int sampleCount,
    qint64 projectedObjectBytes) const
{
    const qint64 bytes = projectedBatchUtf8Bytes(
        m_firstSequence, sampleCount, projectedObjectBytes);
    return bytes >= 0 && bytes >= m_options.maxBytes;
}

} // namespace hwtest::app::web
