#pragma once

#include <app/test_application_controller.h>

#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <functional>

namespace hwtest::app::web {

struct WebTelemetryBatcherOptions {
    int maxSamples = 64;
    qint64 maxBytes = 32768;
    int maxLatencyMs = 20;
};

class WebTelemetryBatcher final : public QObject {
public:
    using FlushCallback = std::function<void(const QJsonObject& batch)>;

    explicit WebTelemetryBatcher(WebTelemetryBatcherOptions options = {},
                                 QObject* parent = nullptr);

    void setFlushCallback(FlushCallback callback);
    bool enqueueSample(quint64 sequence, const ApplicationSample& sample);
    void flush();
    void clear();
    bool hasPendingSamples() const;

private:
    void beginBatch(quint64 firstSequence, const ApplicationSample& sample);
    bool batchWouldReachByteLimit(const QVector<ApplicationSample>& samples) const;

    WebTelemetryBatcherOptions m_options;
    FlushCallback m_flushCallback;
    QTimer m_latencyTimer;
    QVector<ApplicationSample> m_samples;
    quint64 m_firstSequence = 0;
    QString m_taskId;
};

} // namespace hwtest::app::web
