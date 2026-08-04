#include "web_telemetry_batcher.h"
#include "web_protocol.h"

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QJsonArray>
#include <QMetaType>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <limits>

struct CountingTelemetryValue {
    QString text;
};

Q_DECLARE_METATYPE(CountingTelemetryValue)

namespace hwtest::app::web {
namespace {

std::atomic<int> countingTelemetryConversions{0};

ApplicationSample makeApplicationSample(const QString& taskId,
                                        qint64 timestampUs = 1785000000123456LL)
{
    ApplicationSample sample;
    sample.taskId = taskId;
    sample.stepId = QStringLiteral("SYSTEM_STATUS");
    sample.channelId = QStringLiteral("SYSTEM_STATUS");
    sample.timestampUs = timestampUs;
    sample.cycleIndex = 1;
    sample.values.insert(QStringLiteral("cpu_usage"), 12.5);
    sample.tags.insert(QStringLiteral("source"), QStringLiteral("test"));
    return sample;
}

TEST(WebTelemetryBatcherTest, FlushesAtConfiguredSampleCountWithAssignedSequences)
{
    WebTelemetryBatcherOptions options;
    options.maxSamples = 2;
    options.maxBytes = 32768;
    options.maxLatencyMs = 1000;
    WebTelemetryBatcher batcher(options);
    QVector<QJsonObject> flushed;
    batcher.setFlushCallback([&flushed](const QJsonObject& batch) {
        flushed.push_back(batch);
    });

    ASSERT_TRUE(batcher.enqueueSample(17, makeApplicationSample(QStringLiteral("task-a"))));
    ASSERT_TRUE(batcher.enqueueSample(18, makeApplicationSample(QStringLiteral("task-a"))));

    ASSERT_EQ(flushed.size(), 1);
    EXPECT_EQ(flushed.first().value(QStringLiteral("firstSeq")).toInt(), 17);
    EXPECT_EQ(flushed.first().value(QStringLiteral("lastSeq")).toInt(), 18);
    EXPECT_EQ(flushed.first().value(QStringLiteral("samples")).toArray().size(), 2);
    EXPECT_FALSE(batcher.hasPendingSamples());
}

TEST(WebTelemetryBatcherTest, ProjectsEachSampleOnlyOnceForFullBatch)
{
    static const bool converterRegistered =
        QMetaType::registerConverter<CountingTelemetryValue, QString>(
            [](const CountingTelemetryValue& value) {
                countingTelemetryConversions.fetch_add(1, std::memory_order_relaxed);
                return value.text;
            });
    (void)converterRegistered;

    WebTelemetryBatcherOptions options;
    options.maxSamples = 64;
    options.maxBytes = std::numeric_limits<qint64>::max();
    options.maxLatencyMs = 1000;
    WebTelemetryBatcher batcher(options);
    QVector<QJsonObject> flushed;
    batcher.setFlushCallback([&flushed](const QJsonObject& batch) {
        flushed.push_back(batch);
    });

    ApplicationSample sample = makeApplicationSample(QStringLiteral("task-a"));
    sample.values.insert(
        QStringLiteral("counted"),
        QVariant::fromValue(CountingTelemetryValue{QStringLiteral("value")}));
    countingTelemetryConversions.store(0, std::memory_order_relaxed);

    for (quint64 sequence = 1; sequence <= 64; ++sequence) {
        ASSERT_TRUE(batcher.enqueueSample(sequence, sample));
    }

    ASSERT_EQ(flushed.size(), 1);
    EXPECT_EQ(flushed.first().value(QStringLiteral("firstSeq")).toInt(), 1);
    EXPECT_EQ(flushed.first().value(QStringLiteral("lastSeq")).toInt(), 64);
    EXPECT_EQ(flushed.first().value(QStringLiteral("samples")).toArray().size(), 64);
    EXPECT_EQ(countingTelemetryConversions.load(std::memory_order_relaxed), 64);
    EXPECT_FALSE(batcher.hasPendingSamples());
}

TEST(WebTelemetryBatcherTest, FlushesPriorTaskBeforeStartingNextTaskBatch)
{
    WebTelemetryBatcherOptions options;
    options.maxLatencyMs = 1000;
    WebTelemetryBatcher batcher(options);
    QVector<QJsonObject> flushed;
    batcher.setFlushCallback([&flushed](const QJsonObject& batch) {
        flushed.push_back(batch);
    });

    ASSERT_TRUE(batcher.enqueueSample(31, makeApplicationSample(QStringLiteral("task-a"))));
    ASSERT_TRUE(batcher.enqueueSample(32, makeApplicationSample(QStringLiteral("task-b"))));
    batcher.flush();

    ASSERT_EQ(flushed.size(), 2);
    EXPECT_EQ(flushed.at(0).value(QStringLiteral("firstSeq")).toInt(), 31);
    EXPECT_EQ(flushed.at(0).value(QStringLiteral("samples"))
                  .toArray()
                  .first()
                  .toObject()
                  .value(QStringLiteral("taskId"))
                  .toString(),
              QStringLiteral("task-a"));
    EXPECT_EQ(flushed.at(1).value(QStringLiteral("firstSeq")).toInt(), 32);
    EXPECT_EQ(flushed.at(1).value(QStringLiteral("samples"))
                  .toArray()
                  .first()
                  .toObject()
                  .value(QStringLiteral("taskId"))
                  .toString(),
              QStringLiteral("task-b"));
}

TEST(WebTelemetryBatcherTest, FlushesBeforeSoftByteLimitAndPreservesOversizeSingleton)
{
    const ApplicationSample sample = makeApplicationSample(QStringLiteral("task-a"));
    const int singletonBytes = compactJson(makeSampleBatch(40, {sample})).toUtf8().size();

    WebTelemetryBatcherOptions options;
    options.maxBytes = singletonBytes + 1;
    options.maxLatencyMs = 1000;
    WebTelemetryBatcher batcher(options);
    QVector<QJsonObject> flushed;
    batcher.setFlushCallback([&flushed](const QJsonObject& batch) {
        flushed.push_back(batch);
    });

    ASSERT_TRUE(batcher.enqueueSample(40, sample));
    ASSERT_TRUE(batcher.enqueueSample(41, sample));
    ASSERT_EQ(flushed.size(), 1);
    EXPECT_EQ(flushed.first().value(QStringLiteral("firstSeq")).toInt(), 40);
    EXPECT_EQ(flushed.first().value(QStringLiteral("samples")).toArray().size(), 1);
    EXPECT_TRUE(batcher.hasPendingSamples());
    batcher.flush();

    WebTelemetryBatcherOptions oversizeOptions;
    oversizeOptions.maxBytes = 1;
    oversizeOptions.maxLatencyMs = 1000;
    WebTelemetryBatcher oversizeBatcher(oversizeOptions);
    QVector<QJsonObject> oversizeFlushed;
    oversizeBatcher.setFlushCallback([&oversizeFlushed](const QJsonObject& batch) {
        oversizeFlushed.push_back(batch);
    });
    ASSERT_TRUE(oversizeBatcher.enqueueSample(42, sample));
    ASSERT_EQ(oversizeFlushed.size(), 1);
    EXPECT_EQ(oversizeFlushed.first().value(QStringLiteral("samples"))
                  .toArray()
                  .size(),
              1);
}

TEST(WebTelemetryBatcherTest, UsesExactByteLimitAtJsonSafeSequenceBoundary)
{
    constexpr quint64 firstSequence = 9007199254740990ULL;
    const ApplicationSample sample = makeApplicationSample(QStringLiteral("task-舵机"));
    const int twoSampleBytes =
        compactJson(makeSampleBatch(firstSequence, {sample, sample})).toUtf8().size();

    WebTelemetryBatcherOptions options;
    options.maxBytes = twoSampleBytes;
    options.maxLatencyMs = 1000;
    WebTelemetryBatcher batcher(options);
    QVector<QJsonObject> flushed;
    batcher.setFlushCallback([&flushed](const QJsonObject& batch) {
        flushed.push_back(batch);
    });

    ASSERT_TRUE(batcher.enqueueSample(firstSequence, sample));
    ASSERT_TRUE(batcher.enqueueSample(firstSequence + 1, sample));

    ASSERT_EQ(flushed.size(), 1);
    EXPECT_EQ(flushed.first().value(QStringLiteral("firstSeq")).toDouble(),
              static_cast<double>(firstSequence));
    EXPECT_EQ(flushed.first().value(QStringLiteral("samples")).toArray().size(), 1);
    EXPECT_TRUE(batcher.hasPendingSamples());
}

TEST(WebTelemetryBatcherTest, FlushesAfterMaximumLatency)
{
    WebTelemetryBatcherOptions options;
    options.maxLatencyMs = 1;
    WebTelemetryBatcher batcher(options);
    QVector<QJsonObject> flushed;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    batcher.setFlushCallback([&flushed, &loop](const QJsonObject& batch) {
        flushed.push_back(batch);
        loop.quit();
    });

    ASSERT_TRUE(batcher.enqueueSample(51, makeApplicationSample(QStringLiteral("task-a"))));
    timeout.start(500);
    loop.exec();

    ASSERT_EQ(flushed.size(), 1);
    EXPECT_EQ(flushed.first().value(QStringLiteral("firstSeq")).toInt(), 51);
    EXPECT_FALSE(batcher.hasPendingSamples());
}

TEST(WebTelemetryBatcherTest, RejectsInvalidSampleWithoutConsumingSequence)
{
    WebTelemetryBatcherOptions options;
    options.maxLatencyMs = 1000;
    WebTelemetryBatcher batcher(options);
    QVector<QJsonObject> flushed;
    batcher.setFlushCallback([&flushed](const QJsonObject& batch) {
        flushed.push_back(batch);
    });

    EXPECT_FALSE(batcher.enqueueSample(
        61, makeApplicationSample(QStringLiteral("task-a"), -1)));
    EXPECT_FALSE(batcher.hasPendingSamples());
    ASSERT_TRUE(batcher.enqueueSample(61, makeApplicationSample(QStringLiteral("task-a"))));
    batcher.flush();

    ASSERT_EQ(flushed.size(), 1);
    EXPECT_EQ(flushed.first().value(QStringLiteral("firstSeq")).toInt(), 61);
}

TEST(WebTelemetryBatcherTest, ClearDiscardsPendingSamplesAndCancelsLatencyFlush)
{
    WebTelemetryBatcherOptions options;
    options.maxLatencyMs = 1;
    WebTelemetryBatcher batcher(options);
    QVector<QJsonObject> flushed;
    batcher.setFlushCallback([&flushed](const QJsonObject& batch) {
        flushed.push_back(batch);
    });

    ASSERT_TRUE(batcher.enqueueSample(71, makeApplicationSample(QStringLiteral("task-a"))));
    batcher.clear();

    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_TRUE(flushed.isEmpty());
    EXPECT_FALSE(batcher.hasPendingSamples());
}

} // namespace
} // namespace hwtest::app::web
