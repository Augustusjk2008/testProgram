#include <gtest/gtest.h>

#include <algorithm/imu_stream_executor.h>
#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <QDateTime>
#include <QFileInfo>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
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
    if (definition == nullptr) {
        return {};
    }
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
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    TransportResult transact(const QByteArray&, int) override
    {
        TransportResult result;
        result.errorCode = TransportResult::Error::Io;
        result.error = QStringLiteral("stream test requires split I/O");
        return result;
    }

    TransportResult writeFrame(const QByteArray& frame, int) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_open) {
            return failure(TransportResult::Error::Io, QStringLiteral("closed"));
        }
        m_writes.push_back(frame);
        if (m_writes.size() == 1) {
            m_reads.push_back(m_startAck);
            for (const QByteArray& feedback : m_feedback) {
                m_reads.push_back(feedback);
            }
        } else if (m_writes.size() == 2) {
            if (m_failStopWrite) {
                return failure(TransportResult::Error::Io,
                               QStringLiteral("injected STOP write failure"));
            }
            if (!m_stopAck.isEmpty()) {
                m_reads.push_back(m_stopAck);
            }
        } else if (m_writes.size() == 3) {
            m_reads.push_back(m_retryStartAck);
            for (const QByteArray& feedback : m_retryFeedback) {
                m_reads.push_back(feedback);
            }
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
            ++m_readCount;
            if (!m_open) {
                return failure(TransportResult::Error::Io, QStringLiteral("closed"));
            }
            if (!m_reads.empty()) {
                TransportResult result;
                result.ok = true;
                result.frame = std::move(m_reads.front());
                m_reads.pop_front();
                return result;
            }
            emptyCallback = m_emptyCallback;
        }
        if (emptyCallback) {
            emptyCallback();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return failure(TransportResult::Error::Timeout, QStringLiteral("poll timeout"));
    }

    void close() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_closeCount;
        m_open = false;
    }

    void setEmptyCallback(std::function<void()> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_emptyCallback = std::move(callback);
    }

    void setFailStopWrite(bool fail)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failStopWrite = fail;
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
    static TransportResult failure(TransportResult::Error code, const QString& message)
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
    bool m_failStopWrite = false;
    int m_openCount = 0;
    int m_closeCount = 0;
    int m_readCount = 0;
};

class RunControl final : public hwtest::biz::IRunControl {
public:
    hwtest::biz::RunControl current() const override
    {
        return m_stop.load() ? hwtest::biz::RunControl::Stop
                             : hwtest::biz::RunControl::Run;
    }

    bool checkpoint() const override { return !m_stop.load(); }
    void stop() { m_stop.store(true); }

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
                    const QString&) override
    {
    }

    void onSample(const hwtest::biz::StepId&,
                  const hwtest::biz::RawSample& sample) override
    {
        samples.push_back(sample);
        if (m_onSample) {
            m_onSample();
        }
    }

    void onLog(const hwtest::logging::LogEvent&) override {}

    QVector<hwtest::biz::RawSample> samples;

private:
    std::function<void()> m_onSample;
};

QVariantMap executionConfig(const QString& assets)
{
    return {
        {QStringLiteral("protocolAssetRoot"), assets},
        {QStringLiteral("protocol"),
         QVariantMap{{QStringLiteral("startRequestProfileId"),
                      QStringLiteral("imu_stream_start_request")},
                     {QStringLiteral("startResponseProfileId"),
                      QStringLiteral("imu_stream_start_response")},
                     {QStringLiteral("feedbackResponseProfileId"),
                      QStringLiteral("imu_stream_feedback_response")},
                     {QStringLiteral("stopRequestProfileId"),
                      QStringLiteral("imu_stream_stop_request")},
                     {QStringLiteral("stopResponseProfileId"),
                      QStringLiteral("imu_stream_stop_response")}}},
        {QStringLiteral("stream"),
         QVariantMap{{QStringLiteral("readTimeoutMs"), 10},
                     {QStringLiteral("startTimeoutMs"), 100},
                     {QStringLiteral("stopTimeoutMs"), 100}}},
        {QStringLiteral("transport"),
         QVariantMap{{QStringLiteral("openTimeoutMs"), 100},
                     {QStringLiteral("readChunkBytes"), 260}}},
        {QStringLiteral("initialSequence"), 0x1234},
    };
}

hwtest::biz::TestStep streamStep()
{
    hwtest::biz::TestStep step;
    step.stepId = QStringLiteral("IMU_STREAM");
    step.testItemId = QStringLiteral("imu_stream");
    step.algorithmId = QStringLiteral("mbddf.imu_stream");
    step.timeoutMs = 100;
    return step;
}

hwtest::biz::TestContext streamContext()
{
    hwtest::biz::TestContext context;
    context.runId = QStringLiteral("run-1");
    context.requestId = QStringLiteral("request-1");
    context.tags.insert(QStringLiteral("runMode"), QStringLiteral("device_stream"));
    return context;
}

QString commandName(const ProtocolCatalog& catalog, const QByteArray& frame)
{
    QByteArray payload;
    QString error;
    EXPECT_TRUE(decodeFrame(frame, &payload, &error)) << error.toStdString();
    if (payload.size() < 3) {
        return {};
    }
    const MessageDefinition* definition = catalog.findByCommand(
        static_cast<quint8>(payload.at(1)),
        static_cast<quint8>(payload.at(2)),
        Direction::Request);
    return definition == nullptr ? QString{} : definition->name;
}

TEST(ImuStreamExecutorTest, SendsOneStartStreamsCompleteSamplesAndSendsOneStop)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    QVariantMap feedbackValues;
    feedbackValues.insert(QStringLiteral("status"), 0);
    feedbackValues.insert(QStringLiteral("err_code"), 0);
    feedbackValues.insert(QStringLiteral("source_seq"), 0x00F1);
    feedbackValues.insert(QStringLiteral("delta_angle_x"), 1.25);
    feedbackValues.insert(QStringLiteral("temperature"), -12.3);
    feedbackValues.insert(QStringLiteral("self_test_status"), 0x3456);
    feedbackValues.insert(QStringLiteral("work_status"), 0x78);
    feedbackValues.insert(QStringLiteral("software_version"), 0xABCD);
    feedbackValues.insert(QStringLiteral("source_reserved"), 0xEF01);
    QVariantMap secondFeedbackValues = feedbackValues;
    secondFeedbackValues.insert(QStringLiteral("source_seq"), 0x00F2);
    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("imu_stream_start_response"), 0x1234,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{
            frameFor(catalog, QStringLiteral("imu_stream_feedback_response"), 0x9000,
                     feedbackValues),
            frameFor(catalog, QStringLiteral("imu_stream_feedback_response"), 0x9001,
                     secondFeedbackValues)},
        frameFor(catalog, QStringLiteral("imu_stream_stop_response"), 0x1235,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}));
    StreamingTransport* transportPtr = transport.get();
    ImuStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    int observedSamples = 0;
    Observer observer([&executor, &observedSamples] {
        if (++observedSamples >= 2) {
            EXPECT_TRUE(executor.requestStop(100).ok());
        }
    });

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(outcome.value.rawData.value(QStringLiteral("sampleCount")).toULongLong(), 2u);
    ASSERT_EQ(observer.samples.size(), 2);
    EXPECT_EQ(observer.samples.at(1).timestampUs - observer.samples.at(0).timestampUs,
              2500);
    EXPECT_EQ(observer.samples.at(0).streamElapsedUs, 0);
    EXPECT_EQ(observer.samples.at(1).streamElapsedUs, 2500);
    const QVariantMap values = observer.samples.first().values;
    EXPECT_EQ(values.value(QStringLiteral("source_seq")).toUInt(), 0x00F1u);
    EXPECT_NEAR(values.value(QStringLiteral("delta_angle_x")).toDouble(), 1.25, 1e-6);
    EXPECT_NEAR(values.value(QStringLiteral("temperature")).toDouble(), -12.3, 1e-9);
    EXPECT_EQ(values.value(QStringLiteral("software_version")).toUInt(), 0xABCDu);

    const std::vector<QByteArray> writes = transportPtr->writes();
    ASSERT_EQ(writes.size(), 2u);
    EXPECT_EQ(commandName(catalog, writes[0]), QStringLiteral("imu_stream_start_request"));
    EXPECT_EQ(commandName(catalog, writes[1]), QStringLiteral("imu_stream_stop_request"));
    EXPECT_TRUE(executor.finishRun().ok());
}

TEST(ImuStreamExecutorTest, ConfiguredHostTimestampIntervalAffectsOnlyPcSampleTimes)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    const QVariantMap feedbackValues{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("source_seq"), 1},
    };
    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("imu_stream_start_response"), 0x1234,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{
            frameFor(catalog, QStringLiteral("imu_stream_feedback_response"), 0x9000,
                     feedbackValues),
            frameFor(catalog, QStringLiteral("imu_stream_feedback_response"), 0x9001,
                     feedbackValues)},
        frameFor(catalog, QStringLiteral("imu_stream_stop_response"), 0x1235,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}));
    StreamingTransport* transportPtr = transport.get();
    ImuStreamAlgorithmExecutor executor(std::move(transport));
    QVariantMap configured = executionConfig(assets);
    QVariantMap stream = configured.value(QStringLiteral("stream")).toMap();
    stream.insert(QStringLiteral("hostTimestampIntervalUs"), 4000);
    configured.insert(QStringLiteral("stream"), stream);
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(), configured).ok());
    RunControl control;
    int observedSamples = 0;
    Observer observer([&executor, &observedSamples] {
        if (++observedSamples >= 2) {
            EXPECT_TRUE(executor.requestStop(100).ok());
        }
    });

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    ASSERT_EQ(observer.samples.size(), 2);
    EXPECT_EQ(observer.samples.at(1).timestampUs - observer.samples.at(0).timestampUs,
              4000);
    EXPECT_EQ(observer.samples.at(0).streamElapsedUs, 0);
    EXPECT_EQ(observer.samples.at(1).streamElapsedUs, 4000);
    const std::vector<QByteArray> writes = transportPtr->writes();
    ASSERT_EQ(writes.size(), 2u);
    EXPECT_EQ(writes.at(0),
              frameFor(catalog, QStringLiteral("imu_stream_start_request"), 0x1234));
    EXPECT_EQ(writes.at(1),
              frameFor(catalog, QStringLiteral("imu_stream_stop_request"), 0x1235));
}

TEST(ImuStreamExecutorTest, RejectsNonPositiveHostTimestampIntervalBeforeOpeningTransport)
{
    const QString assets = catalogDirectory();
    auto transport = std::make_unique<StreamingTransport>(QByteArray{},
                                                          std::vector<QByteArray>{},
                                                          QByteArray{});
    StreamingTransport* transportPtr = transport.get();
    ImuStreamAlgorithmExecutor executor(std::move(transport));
    QVariantMap configured = executionConfig(assets);
    QVariantMap stream = configured.value(QStringLiteral("stream")).toMap();
    stream.insert(QStringLiteral("hostTimestampIntervalUs"), 0);
    configured.insert(QStringLiteral("stream"), stream);

    const auto prepared = executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                           configured);

    EXPECT_FALSE(prepared.ok());
    EXPECT_EQ(prepared.code, hwtest::biz::ErrorCode::ConfigSchemaError);
    EXPECT_EQ(transportPtr->openCount(), 0);
}

TEST(ImuStreamExecutorTest, ReanchorsUtcTimeWhenExecuteStepRunsAgain)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    const QVariantMap firstValues{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("source_seq"), 1},
    };
    const QVariantMap retryValues{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("source_seq"), 2},
    };
    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("imu_stream_start_response"), 0x1234,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{
            frameFor(catalog, QStringLiteral("imu_stream_feedback_response"), 0x9000,
                     firstValues),
            QByteArray::fromHex("55AA")},
        frameFor(catalog, QStringLiteral("imu_stream_stop_response"), 0x1235,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}));
    StreamingTransport* transportPtr = transport.get();
    transportPtr->setRetryAttempt(
        frameFor(catalog, QStringLiteral("imu_stream_start_response"), 0x1236,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{frameFor(
            catalog, QStringLiteral("imu_stream_feedback_response"), 0x9001,
            retryValues)},
        frameFor(catalog, QStringLiteral("imu_stream_stop_response"), 0x1237,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}));
    ImuStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer firstObserver;

    const auto first = executor.executeStep(streamStep(), control, firstObserver);

    EXPECT_FALSE(first.ok());
    ASSERT_EQ(firstObserver.samples.size(), 1);
    EXPECT_GT(firstObserver.samples.first().timestampUs, 0);

    Observer retryObserver([&executor] {
        EXPECT_TRUE(executor.requestStop(100).ok());
    });
    const qint64 earliestRetryAnchorUs =
        QDateTime::currentMSecsSinceEpoch() * 1000;
    const auto retry = executor.executeStep(streamStep(), control, retryObserver);
    const qint64 latestRetryAnchorUs =
        QDateTime::currentMSecsSinceEpoch() * 1000;

    ASSERT_TRUE(retry.ok()) << retry.status.error.message.toStdString();
    ASSERT_EQ(retryObserver.samples.size(), 1);
    EXPECT_EQ(retryObserver.samples.first().streamElapsedUs, 0);
    EXPECT_GE(retryObserver.samples.first().timestampUs, earliestRetryAnchorUs);
    EXPECT_LE(retryObserver.samples.first().timestampUs, latestRetryAnchorUs);
    EXPECT_TRUE(executor.finishRun().ok());
}

TEST(ImuStreamExecutorTest, ZeroValidFramesFailsAfterStopButStillCleansUp)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("imu_stream_start_response"), 0x1234,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{},
        frameFor(catalog, QStringLiteral("imu_stream_stop_response"), 0x1235,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}));
    StreamingTransport* transportPtr = transport.get();
    ImuStreamAlgorithmExecutor executor(std::move(transport));
    transportPtr->setEmptyCallback([&executor] { (void)executor.requestStop(100); });
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Fail);
    EXPECT_EQ(outcome.value.errorCode, hwtest::biz::ErrorCode::SampleFail);
    EXPECT_EQ(outcome.value.rawData.value(QStringLiteral("sampleCount")).toULongLong(), 0u);
    EXPECT_EQ(transportPtr->writes().size(), 2u);
}

TEST(ImuStreamExecutorTest, RejectsNonDeviceStreamModeBeforeOpeningTransport)
{
    const QString assets = catalogDirectory();
    auto transport = std::make_unique<StreamingTransport>(QByteArray{},
                                                          std::vector<QByteArray>{},
                                                          QByteArray{});
    StreamingTransport* transportPtr = transport.get();
    ImuStreamAlgorithmExecutor executor(std::move(transport));
    hwtest::biz::TestContext context;
    context.tags.insert(QStringLiteral("runMode"), QStringLiteral("pc_periodic"));

    const auto prepared = executor.prepare(hwtest::biz::TestPlan{}, context,
                                           executionConfig(assets));

    EXPECT_FALSE(prepared.ok());
    EXPECT_EQ(prepared.code, hwtest::biz::ErrorCode::CapabilityUnsupported);
    EXPECT_EQ(transportPtr->openCount(), 0);
}

TEST(ImuStreamExecutorTest, StopAckTimeoutDoesNotEmitSecondStopDuringFinishRun)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("imu_stream_start_response"), 0x1234,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{frameFor(
            catalog, QStringLiteral("imu_stream_feedback_response"), 0x9000,
            {{QStringLiteral("status"), 0},
             {QStringLiteral("err_code"), 0},
             {QStringLiteral("source_seq"), 1}})},
        QByteArray{});
    StreamingTransport* transportPtr = transport.get();
    ImuStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer([&executor] { EXPECT_TRUE(executor.requestStop(100).ok()); });

    const auto outcome = executor.executeStep(streamStep(), control, observer);
    EXPECT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.status.code, hwtest::biz::ErrorCode::BusTimeout);
    EXPECT_TRUE(executor.finishRun().ok());

    const std::vector<QByteArray> writes = transportPtr->writes();
    ASSERT_EQ(writes.size(), 2u);
    EXPECT_EQ(commandName(catalog, writes[0]), QStringLiteral("imu_stream_start_request"));
    EXPECT_EQ(commandName(catalog, writes[1]), QStringLiteral("imu_stream_stop_request"));
}

TEST(ImuStreamExecutorTest, StopWriteFailureDoesNotEmitSecondStopDuringFinishRun)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    auto transport = std::make_unique<StreamingTransport>(
        frameFor(catalog, QStringLiteral("imu_stream_start_response"), 0x1234,
                 {{QStringLiteral("status"), 0}, {QStringLiteral("err_code"), 0}}),
        std::vector<QByteArray>{frameFor(
            catalog, QStringLiteral("imu_stream_feedback_response"), 0x9000,
            {{QStringLiteral("status"), 0},
             {QStringLiteral("err_code"), 0},
             {QStringLiteral("source_seq"), 1}})},
        QByteArray{});
    StreamingTransport* transportPtr = transport.get();
    transportPtr->setFailStopWrite(true);
    ImuStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer([&executor] { EXPECT_TRUE(executor.requestStop(100).ok()); });

    const auto outcome = executor.executeStep(streamStep(), control, observer);
    EXPECT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.status.code, hwtest::biz::ErrorCode::RemoteCommandError);
    EXPECT_TRUE(executor.finishRun().ok());

    const std::vector<QByteArray> writes = transportPtr->writes();
    ASSERT_EQ(writes.size(), 2u);
    EXPECT_EQ(commandName(catalog, writes[0]), QStringLiteral("imu_stream_start_request"));
    EXPECT_EQ(commandName(catalog, writes[1]), QStringLiteral("imu_stream_stop_request"));
}

} // namespace
} // namespace hwtest::algorithm::mbddf
