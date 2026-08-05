#include <gtest/gtest.h>

#include <algorithm/helm_stream_executor.h>
#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <QDateTime>
#include <QFileInfo>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace hwtest::algorithm::mbddf {
namespace {

QString catalogDirectory()
{
    const QString configured = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    return configured.isEmpty()
        ? QStringLiteral("H:/WorkSpace/QtWorkspace/testProgram/dut/docs/design/product_protocol_csv")
        : configured;
}

QByteArray frameFor(const ProtocolCatalog& catalog,
                    const QString& name,
                    quint16 sequence,
                    const QVariantMap& values = {})
{
    const MessageDefinition* definition = catalog.findByName(name);
    EXPECT_NE(definition, nullptr) << name.toStdString();
    if (definition == nullptr) return {};
    QString error;
    QByteArray payload;
    EXPECT_TRUE(encodePayload(*definition, values, sequence, &payload, &error))
        << error.toStdString();
    QByteArray frame;
    EXPECT_TRUE(encodeFrame(payload, &frame, &error)) << error.toStdString();
    return frame;
}

class StreamingTransport final : public IByteTransport {
public:
    StreamingTransport(QByteArray startAck,
                       std::vector<QByteArray> feedback,
                       QByteArray stopAck)
        : m_startAck(std::move(startAck))
        , m_feedback(std::move(feedback))
        , m_stopAck(std::move(stopAck))
    {
    }

    bool open(QString* error) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_openCount;
        m_open = true;
        if (error != nullptr) error->clear();
        return true;
    }

    TransportResult transact(const QByteArray&, int) override
    {
        return failure(TransportResult::Error::Io,
                       QStringLiteral("split I/O required"));
    }

    TransportResult writeFrame(const QByteArray& frame, int) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_open) return failure(TransportResult::Error::Io,
                                    QStringLiteral("closed"));
        m_writes.push_back(frame);
        if (m_writes.size() == 1) {
            m_reads.push_back(m_startAck);
            for (const QByteArray& item : m_feedback) m_reads.push_back(item);
        } else if (m_writes.size() == 2 && !m_stopAck.isEmpty()) {
            m_reads.push_back(m_stopAck);
        } else if (m_writes.size() == 3) {
            m_reads.push_back(m_retryStartAck);
            for (const QByteArray& item : m_retryFeedback) m_reads.push_back(item);
        } else if (m_writes.size() == 4 && !m_retryStopAck.isEmpty()) {
            m_reads.push_back(m_retryStopAck);
        }
        TransportResult result;
        result.ok = true;
        return result;
    }

    TransportResult readFrame(int) override
    {
        std::function<void()> emptyCallback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_open) return failure(TransportResult::Error::Io,
                                        QStringLiteral("closed"));
            if (!m_reads.empty()) {
                TransportResult result;
                result.ok = true;
                result.frame = std::move(m_reads.front());
                m_reads.pop_front();
                return result;
            }
            emptyCallback = m_emptyCallback;
        }
        if (emptyCallback) emptyCallback();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return failure(TransportResult::Error::Timeout,
                       QStringLiteral("poll timeout"));
    }

    void close() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_open = false;
    }

    void setEmptyCallback(std::function<void()> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_emptyCallback = std::move(callback);
    }

    void setRetryAttempt(QByteArray startAck,
                         std::vector<QByteArray> feedback,
                         QByteArray stopAck)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_retryStartAck = std::move(startAck);
        m_retryFeedback = std::move(feedback);
        m_retryStopAck = std::move(stopAck);
    }

    std::vector<QByteArray> writes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_writes;
    }

    int openCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_openCount;
    }

private:
    static TransportResult failure(TransportResult::Error code,
                                   const QString& message)
    {
        TransportResult result;
        result.errorCode = code;
        result.error = message;
        return result;
    }

    mutable std::mutex m_mutex;
    QByteArray m_startAck;
    std::vector<QByteArray> m_feedback;
    QByteArray m_stopAck;
    QByteArray m_retryStartAck;
    std::vector<QByteArray> m_retryFeedback;
    QByteArray m_retryStopAck;
    std::deque<QByteArray> m_reads;
    std::vector<QByteArray> m_writes;
    std::function<void()> m_emptyCallback;
    bool m_open = false;
    int m_openCount = 0;
};

class RunControl final : public hwtest::biz::IRunControl {
public:
    hwtest::biz::RunControl current() const override
    {
        return m_stop.load() ? hwtest::biz::RunControl::Stop
                             : hwtest::biz::RunControl::Run;
    }
    bool checkpoint() const override { return !m_stop.load(); }

private:
    std::atomic_bool m_stop{false};
};

class Observer final : public hwtest::biz::IAlgorithmObserver {
public:
    explicit Observer(std::function<void()> onSample = {})
        : m_onSample(std::move(onSample))
    {
    }
    void onProgress(const hwtest::biz::StepId&,
                    const hwtest::biz::TestItemId&,
                    int,
                    const QString&) override {}
    void onSample(const hwtest::biz::StepId&,
                  const hwtest::biz::RawSample& sample) override
    {
        samples.push_back(sample);
        if (m_onSample) m_onSample();
    }
    void onLog(const hwtest::logging::LogEvent& event) override
    {
        logs.push_back(event);
    }

    QVector<hwtest::biz::RawSample> samples;
    QVector<hwtest::logging::LogEvent> logs;

private:
    std::function<void()> m_onSample;
};

QVariantMap executionConfig(const QString& assets)
{
    return {
        {QStringLiteral("protocolAssetRoot"), assets},
        {QStringLiteral("protocol"),
         QVariantMap{{QStringLiteral("startRequestProfileId"),
                      QStringLiteral("helm_start_request")},
                     {QStringLiteral("startResponseProfileId"),
                      QStringLiteral("helm_start_response")},
                     {QStringLiteral("feedbackResponseProfileId"),
                      QStringLiteral("helm_feedback_response")},
                     {QStringLiteral("stopRequestProfileId"),
                      QStringLiteral("helm_stop_request")},
                     {QStringLiteral("stopResponseProfileId"),
                      QStringLiteral("helm_stop_response")}}},
        {QStringLiteral("stream"),
         QVariantMap{{QStringLiteral("readTimeoutMs"), 10},
                     {QStringLiteral("startTimeoutMs"), 100},
                     {QStringLiteral("stopTimeoutMs"), 100}}},
        {QStringLiteral("transport"),
         QVariantMap{{QStringLiteral("openTimeoutMs"), 100},
                     {QStringLiteral("readChunkBytes"), 260}}},
        {QStringLiteral("initialSequence"), 0x2345},
    };
}

hwtest::biz::TestStep streamStep()
{
    hwtest::biz::TestStep step;
    step.stepId = QStringLiteral("HELM_STREAM");
    step.testItemId = QStringLiteral("helm_stream");
    step.algorithmId = QStringLiteral("mbddf.helm_stream");
    step.timeoutMs = 100;
    return step;
}

hwtest::biz::TestContext streamContext()
{
    hwtest::biz::TestContext context;
    context.runId = QStringLiteral("run-helm");
    context.requestId = QStringLiteral("request-helm");
    context.tags.insert(QStringLiteral("runMode"), QStringLiteral("device_stream"));
    context.runParameters = {
        {QStringLiteral("waveform"), 4},
        {QStringLiteral("freq"), 0.5},
        {QStringLiteral("ampl"), 250.0},
        {QStringLiteral("offset"), -100.0},
        {QStringLiteral("start"), 0.25},
        {QStringLiteral("max_freq"), 80.0},
        {QStringLiteral("sweep_duration_s"), 25.0},
        {QStringLiteral("enable"), 15},
    };
    return context;
}

QString requestName(const ProtocolCatalog& catalog, const QByteArray& frame,
                    QVariantMap* values = nullptr)
{
    QByteArray payload;
    QString error;
    EXPECT_TRUE(decodeFrame(frame, &payload, &error)) << error.toStdString();
    if (payload.size() < 3) return {};
    const MessageDefinition* definition = catalog.findByCommand(
        static_cast<quint8>(payload.at(1)),
        static_cast<quint8>(payload.at(2)),
        Direction::Request);
    if (definition != nullptr && values != nullptr) {
        EXPECT_TRUE(decodePayload(*definition, payload, values, &error))
            << error.toStdString();
    }
    return definition == nullptr ? QString{} : definition->name;
}

QVariantMap twoSampleFeedback()
{
    QVariantMap values{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("sample_count"), 2},
        {QStringLiteral("first_timestamp_us_low"), 0x10u},
        {QStringLiteral("first_timestamp_us_high"), 0x02u},
    };
    for (int sample = 0; sample < 2; ++sample) {
        const QString prefix = QStringLiteral("sample[%1].").arg(sample);
        values.insert(prefix + QStringLiteral("delta_us"), sample * 1000);
        values.insert(prefix + QStringLiteral("serial_b"), 100 + sample);
        values.insert(prefix + QStringLiteral("version"), 0x4000);
        values.insert(prefix + QStringLiteral("self_check"), 3);
        values.insert(prefix + QStringLiteral("self_check_1"), 1);
        values.insert(prefix + QStringLiteral("self_check_2"), 2);
        values.insert(prefix + QStringLiteral("self_check_3"), 3);
        values.insert(prefix + QStringLiteral("self_check_4"), 0);
        values.insert(prefix + QStringLiteral("self_check_combined"), 2);
        values.insert(prefix + QStringLiteral("timeout"), sample);
        values.insert(prefix + QStringLiteral("serial_a"), 90 + sample);
        for (int channel = 0; channel < 4; ++channel) {
            values.insert(prefix + QStringLiteral("fdb[%1]").arg(channel),
                          sample * 10.0 + channel + 0.5);
            values.insert(prefix + QStringLiteral("ins[%1]").arg(channel),
                          sample * 10.0 + channel + 1.5);
        }
    }
    return values;
}

QVariantMap oneSampleFeedback(quint64 timestamp, quint16 serialA, quint16 serialB)
{
    QVariantMap values = twoSampleFeedback();
    values.insert(QStringLiteral("sample_count"), 1);
    values.insert(QStringLiteral("first_timestamp_us_low"),
                  static_cast<quint32>(timestamp & 0xFFFFFFFFu));
    values.insert(QStringLiteral("first_timestamp_us_high"),
                  static_cast<quint32>(timestamp >> 32));
    values.insert(QStringLiteral("sample[0].delta_us"), 0);
    values.insert(QStringLiteral("sample[0].serial_a"), serialA);
    values.insert(QStringLiteral("sample[0].serial_b"), serialB);
    return values;
}

TEST(HelmStreamExecutorTest, SendsParametersSplitsBatchIntoCompleteSamplesAndStops)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("helm_start_response"), 0x2345,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{frameFor(
            catalog, QStringLiteral("helm_feedback_response"), 0x9000,
            twoSampleFeedback())},
        frameFor(catalog, QStringLiteral("helm_stop_response"), 0x2346,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}));
    StreamingTransport* transportPtr = transport.get();
    HelmStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer([&executor] {
        if (executor.sampleCount() >= 2) EXPECT_TRUE(executor.requestStop(100).ok());
    });

    const qint64 earliestAnchorUs = QDateTime::currentMSecsSinceEpoch() * 1000;
    const auto outcome = executor.executeStep(streamStep(), control, observer);
    const qint64 latestAnchorUs = QDateTime::currentMSecsSinceEpoch() * 1000;

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(outcome.value.rawData.value(QStringLiteral("sampleCount")).toULongLong(), 2u);
    ASSERT_EQ(observer.samples.size(), 2);
    EXPECT_GE(observer.samples.at(0).timestampUs, earliestAnchorUs);
    EXPECT_LE(observer.samples.at(0).timestampUs, latestAnchorUs);
    EXPECT_EQ(observer.samples.at(1).timestampUs - observer.samples.at(0).timestampUs,
              1000);
    EXPECT_EQ(observer.samples.at(0).streamElapsedUs, 0);
    EXPECT_EQ(observer.samples.at(1).streamElapsedUs, 1000);
    constexpr quint64 firstDdsTimestampUs = (quint64{2} << 32) + 0x10;
    EXPECT_EQ(observer.samples.at(0).values.value(
                  QStringLiteral("dds_timestamp_us")).toULongLong(),
              firstDdsTimestampUs);
    EXPECT_EQ(observer.samples.at(0).values.value(QStringLiteral("serial_a")).toUInt(), 90u);
    EXPECT_EQ(observer.samples.at(1).values.value(QStringLiteral("serial_b")).toUInt(), 101u);
    EXPECT_NEAR(observer.samples.at(1).values.value(QStringLiteral("fdb[3]")).toDouble(),
                13.5, 1e-6);
    EXPECT_NEAR(observer.samples.at(1).values.value(QStringLiteral("ins[3]")).toDouble(),
                14.5, 1e-6);
    EXPECT_EQ(observer.samples.at(1).values.value(QStringLiteral("timeout")).toUInt(), 1u);
    EXPECT_EQ(observer.samples.at(0).values.value(
                  QStringLiteral("product_frame_sequence")).toUInt(), 0x9000u);
    EXPECT_EQ(observer.samples.at(0).values.value(QStringLiteral("seq")).toUInt(),
              0x9000u);
    EXPECT_EQ(observer.samples.at(0).values.value(QStringLiteral("status")).toInt(),
              0);
    const QStringList removedSelfCheckFields{
        QStringLiteral("self_check"),
        QStringLiteral("self_check_1"),
        QStringLiteral("self_check_2"),
        QStringLiteral("self_check_3"),
        QStringLiteral("self_check_4"),
        QStringLiteral("self_check_combined"),
        QStringLiteral("self_check_reserved"),
        QStringLiteral("self_check_or"),
        QStringLiteral("self_check_or_timeout"),
    };
    for (const hwtest::biz::RawSample& sample : observer.samples) {
        for (const QString& field : removedSelfCheckFields) {
            EXPECT_FALSE(sample.values.contains(field)) << field.toStdString();
        }
    }
    EXPECT_FALSE(observer.samples.at(0).values.value(
                     QStringLiteral("product_frame_discontinuity")).toBool());
    EXPECT_DOUBLE_EQ(observer.samples.at(0).values.value(
                         QStringLiteral("parameter.ampl")).toDouble(),
                     250.0);
    EXPECT_EQ(observer.samples.at(0).tags.value(
                  QStringLiteral("productFrameSequence")).toUInt(), 0x9000u);
    EXPECT_EQ(observer.samples.at(0).tags.value(
                  QStringLiteral("effectiveRunParameters")).toMap()
                  .value(QStringLiteral("ampl")).toDouble(), 250.0);
    ASSERT_EQ(observer.logs.size(), 1);
    EXPECT_EQ(observer.logs.first().context.value(
                  QStringLiteral("effectiveRunParameters")).toMap()
                  .value(QStringLiteral("offset")).toDouble(), -100.0);

    const std::vector<QByteArray> writes = transportPtr->writes();
    ASSERT_EQ(writes.size(), 2u);
    QVariantMap startValues;
    EXPECT_EQ(requestName(catalog, writes[0], &startValues),
              QStringLiteral("helm_start_request"));
    EXPECT_EQ(requestName(catalog, writes[1]), QStringLiteral("helm_stop_request"));
    EXPECT_EQ(startValues.value(QStringLiteral("waveform")).toUInt(), 4u);
    EXPECT_DOUBLE_EQ(startValues.value(QStringLiteral("ampl")).toDouble(), 250.0);
    EXPECT_DOUBLE_EQ(startValues.value(QStringLiteral("offset")).toDouble(), -100.0);
    EXPECT_DOUBLE_EQ(startValues.value(
                         QStringLiteral("sweep_duration_s")).toDouble(),
                     25.0);
    EXPECT_EQ(startValues.value(QStringLiteral("enable")).toUInt(), 15u);
    EXPECT_TRUE(executor.finishRun().ok());
}

TEST(HelmStreamExecutorTest, FeedbackStatusIsRecordedWithoutStoppingAcquisition)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    QVariantMap feedback = oneSampleFeedback((quint64{2} << 32) + 0x10, 90, 100);
    feedback.insert(QStringLiteral("status"), 1);
    feedback.insert(QStringLiteral("err_code"), 0x1234);
    QVariantMap normalFeedback = oneSampleFeedback(
        (quint64{2} << 32) + 0x20, 91, 101);
    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("helm_start_response"), 0x2345,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 0x9000, feedback),
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 0x9001,
                     normalFeedback)},
        frameFor(catalog, QStringLiteral("helm_stop_response"), 0x2346,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}));
    HelmStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer([&executor] {
        if (executor.sampleCount() >= 2) EXPECT_TRUE(executor.requestStop(100).ok());
    });

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Pass);
    ASSERT_EQ(observer.samples.size(), 2);
    EXPECT_EQ(observer.samples.first().values.value(QStringLiteral("status")).toUInt(), 1u);
    EXPECT_EQ(observer.samples.first().values.value(QStringLiteral("err_code")).toUInt(),
              0x1234u);
    EXPECT_EQ(observer.samples.at(1).values.value(QStringLiteral("status")).toUInt(), 0u);
    EXPECT_TRUE(executor.finishRun().ok());
}

TEST(HelmStreamExecutorTest, NonForwardSequencesDoNotBecomeHugeLossCounts)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    constexpr quint64 firstDdsTimestampUs = (quint64{2} << 32) + 0x10;
    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("helm_start_response"), 0x2345,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 0x9000,
                     oneSampleFeedback(firstDdsTimestampUs, 100, 200)),
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 0x9000,
                     oneSampleFeedback(firstDdsTimestampUs + 1000, 100, 200)),
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 0x8FFF,
                     oneSampleFeedback(firstDdsTimestampUs + 2000, 99, 199)),
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 0x0FFF,
                     oneSampleFeedback(firstDdsTimestampUs + 3000,
                                       static_cast<quint16>(99u + 0x8000u),
                                       static_cast<quint16>(199u + 0x8000u)))},
        frameFor(catalog, QStringLiteral("helm_stop_response"), 0x2346,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}));
    HelmStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer([&executor] {
        if (executor.sampleCount() >= 4) EXPECT_TRUE(executor.requestStop(100).ok());
    });

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    ASSERT_EQ(observer.samples.size(), 4);
    for (int index = 1; index < observer.samples.size(); ++index) {
        const QVariantMap& values = observer.samples.at(index).values;
        EXPECT_EQ(values.value(QStringLiteral("missing_product_frames")).toUInt(), 0u);
        EXPECT_EQ(values.value(QStringLiteral("missing_serial_a")).toUInt(), 0u);
        EXPECT_EQ(values.value(QStringLiteral("missing_serial_b")).toUInt(), 0u);
        EXPECT_FALSE(values.value(QStringLiteral("product_frame_discontinuity")).toBool());
        EXPECT_FALSE(values.value(QStringLiteral("serial_a_discontinuity")).toBool());
        EXPECT_FALSE(values.value(QStringLiteral("serial_b_discontinuity")).toBool());
    }
    EXPECT_TRUE(executor.finishRun().ok());
}

TEST(HelmStreamExecutorTest, ForwardSequenceGapsReportOnlyMissingFrames)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    constexpr quint64 firstDdsTimestampUs = (quint64{2} << 32) + 0x10;
    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("helm_start_response"), 0x2345,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 100,
                     oneSampleFeedback(firstDdsTimestampUs, 200, 300)),
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 103,
                     oneSampleFeedback(firstDdsTimestampUs + 1000, 203, 303))},
        frameFor(catalog, QStringLiteral("helm_stop_response"), 0x2346,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}));
    HelmStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer([&executor] {
        if (executor.sampleCount() >= 2) EXPECT_TRUE(executor.requestStop(100).ok());
    });

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    ASSERT_EQ(observer.samples.size(), 2);
    const QVariantMap& values = observer.samples.at(1).values;
    EXPECT_EQ(values.value(QStringLiteral("missing_product_frames")).toUInt(), 2u);
    EXPECT_EQ(values.value(QStringLiteral("missing_serial_a")).toUInt(), 2u);
    EXPECT_EQ(values.value(QStringLiteral("missing_serial_b")).toUInt(), 2u);
    EXPECT_TRUE(values.value(QStringLiteral("product_frame_discontinuity")).toBool());
    EXPECT_TRUE(values.value(QStringLiteral("serial_a_discontinuity")).toBool());
    EXPECT_TRUE(values.value(QStringLiteral("serial_b_discontinuity")).toBool());
    EXPECT_TRUE(executor.finishRun().ok());
}

TEST(HelmStreamExecutorTest, RejectsDdsTimestampThatMovesBackwards)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    constexpr quint64 firstDdsTimestampUs = (quint64{2} << 32) + 0x10;
    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("helm_start_response"), 0x2345,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 100,
                     oneSampleFeedback(firstDdsTimestampUs, 200, 300)),
            frameFor(catalog, QStringLiteral("helm_feedback_response"), 101,
                     oneSampleFeedback(firstDdsTimestampUs - 1, 201, 301))},
        frameFor(catalog, QStringLiteral("helm_stop_response"), 0x2346,
                 {{QStringLiteral("status"), 0},
                  {QStringLiteral("err_code"), 0}}));
    HelmStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    EXPECT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.status.code, hwtest::biz::ErrorCode::ProtocolParseError);
    EXPECT_NE(outcome.status.error.message.indexOf(QStringLiteral("moved backwards")),
              -1);
    ASSERT_EQ(observer.samples.size(), 1);
    EXPECT_TRUE(executor.finishRun().ok());
}

TEST(HelmStreamExecutorTest, ZeroFeedbackFailsButStopRemainsIndependent)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("helm_start_response"), 0x2345,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{},
        frameFor(catalog, QStringLiteral("helm_stop_response"), 0x2346,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}));
    StreamingTransport* transportPtr = transport.get();
    HelmStreamAlgorithmExecutor executor(std::move(transport));
    transportPtr->setEmptyCallback([&executor] { (void)executor.requestStop(100); });
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Fail);
    EXPECT_EQ(outcome.value.errorCode, hwtest::biz::ErrorCode::SampleFail);
    EXPECT_EQ(transportPtr->writes().size(), 2u);
}
} // namespace
} // namespace hwtest::algorithm::mbddf
