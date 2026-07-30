#include <gtest/gtest.h>

#include <algorithm/dh_ignite_stream_executor.h>
#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <QFileInfo>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace hwtest::algorithm::mbddf {
namespace {

QString catalogDirectory()
{
    const QString configured = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    return configured.isEmpty()
        ? QStringLiteral(HWTEST_MBDDF_PROTOCOL_CATALOG_DIR)
        : configured;
}

QByteArray frameFor(const ProtocolCatalog& catalog,
                    quint16 sequence,
                    const QVariantMap& values = {})
{
    const MessageDefinition* definition = catalog.findByName(
        QStringLiteral("dh_control_response"));
    EXPECT_NE(definition, nullptr);
    if (definition == nullptr) return {};

    QString error;
    QByteArray payload;
    EXPECT_TRUE(encodePayload(*definition, values, sequence, &payload, &error))
        << error.toStdString();
    QByteArray frame;
    EXPECT_TRUE(encodeFrame(payload, &frame, &error)) << error.toStdString();
    return frame;
}

class FiniteStreamTransport final : public IByteTransport {
public:
    explicit FiniteStreamTransport(std::vector<QByteArray> responses)
        : m_responses(std::move(responses))
    {
    }

    bool configure(const QVariantMap&, QString* error) override
    {
        if (error != nullptr) error->clear();
        return true;
    }

    bool open(QString* error) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_open = true;
        if (error != nullptr) error->clear();
        return true;
    }

    TransportResult transact(const QByteArray&, int) override
    {
        return failure(TransportResult::Error::Io,
                       QStringLiteral("DH stream requires split I/O"));
    }

    TransportResult writeFrame(const QByteArray& frame, int) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_open) {
            return failure(TransportResult::Error::Io, QStringLiteral("closed"));
        }
        m_writes.push_back(frame);
        for (QByteArray& response : m_responses) {
            m_reads.push_back(std::move(response));
        }
        m_responses.clear();
        TransportResult result;
        result.ok = true;
        return result;
    }

    TransportResult readFrame(int) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_readCalls;
        if (!m_open) {
            return failure(TransportResult::Error::Io, QStringLiteral("closed"));
        }
        if (m_reads.empty()) {
            return failure(TransportResult::Error::Timeout,
                           QStringLiteral("injected timeout"));
        }
        TransportResult result;
        result.ok = true;
        result.frame = std::move(m_reads.front());
        m_reads.pop_front();
        return result;
    }

    void close() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_open = false;
    }

    std::vector<QByteArray> writes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_writes;
    }

    int readCalls() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_readCalls;
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
    std::vector<QByteArray> m_responses;
    std::deque<QByteArray> m_reads;
    std::vector<QByteArray> m_writes;
    int m_readCalls = 0;
    bool m_open = false;
};

class RunControl final : public hwtest::biz::IRunControl {
public:
    hwtest::biz::RunControl current() const override
    {
        return stopped ? hwtest::biz::RunControl::Stop
                       : hwtest::biz::RunControl::Run;
    }

    bool checkpoint() const override { return !stopped; }

    bool stopped = false;
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
        if (m_onSample) m_onSample();
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
         QVariantMap{{QStringLiteral("requestProfileId"),
                      QStringLiteral("dh_control_request")},
                     {QStringLiteral("responseProfileId"),
                      QStringLiteral("dh_control_response")}}},
        {QStringLiteral("stream"),
         QVariantMap{{QStringLiteral("readTimeoutMs"), 25},
                     {QStringLiteral("writeTimeoutMs"), 100}}},
        {QStringLiteral("transport"),
         QVariantMap{{QStringLiteral("openTimeoutMs"), 100},
                     {QStringLiteral("readChunkBytes"), 260}}},
        {QStringLiteral("initialSequence"), 0xFFFE},
    };
}

hwtest::biz::TestContext streamContext(int reportCount = 3,
                                       int intervalUs = 2500,
                                       int delayFrames = 1)
{
    hwtest::biz::TestContext context;
    context.runId = QStringLiteral("run-dh");
    context.requestId = QStringLiteral("request-dh");
    context.tags.insert(QStringLiteral("runMode"),
                        QStringLiteral("device_stream"));
    context.runParameters = {
        {QStringLiteral("power_enable"), 0},
        {QStringLiteral("return_enable"), 0},
        {QStringLiteral("channel_enabled[0]"), true},
        {QStringLiteral("channel_enabled[22]"), true},
        {QStringLiteral("report_count"), reportCount},
        {QStringLiteral("interval_us"), intervalUs},
        {QStringLiteral("delay_frames"), delayFrames},
    };
    return context;
}

hwtest::biz::TestStep streamStep()
{
    hwtest::biz::TestStep step;
    step.stepId = QStringLiteral("DH_IGNITE_STREAM");
    step.testItemId = QStringLiteral("dh_ignite_stream");
    step.algorithmId = QStringLiteral("mbddf.dh_ignite_stream");
    step.timeoutMs = 100;
    return step;
}

QVariantMap normalValues(double telemetry0)
{
    return {
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("power_enable_readback"), 0},
        {QStringLiteral("return_enable_readback"), 0},
        {QStringLiteral("dh_status.ch0"), 1},
        {QStringLiteral("dh_status.ch22"), 1},
        {QStringLiteral("telemetry[0]"), telemetry0},
        {QStringLiteral("telemetry[22]"), 22.125},
    };
}

QVariantMap decodeRequest(const ProtocolCatalog& catalog,
                          const QByteArray& frame)
{
    QByteArray payload;
    QString error;
    EXPECT_TRUE(decodeFrame(frame, &payload, &error)) << error.toStdString();
    const MessageDefinition* definition = catalog.findByName(
        QStringLiteral("dh_control_request"));
    EXPECT_NE(definition, nullptr);
    QVariantMap values;
    if (definition != nullptr) {
        EXPECT_TRUE(decodePayload(*definition, payload, &values, &error))
            << error.toStdString();
    }
    return values;
}

TEST(DhIgniteStreamExecutorTest,
     SendsOneRequestCompletesFiniteStreamAndIgnoresStopAfterAcceptance)
{
    const QString assets = catalogDirectory();
    ASSERT_TRUE(QFileInfo(assets).isDir());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    auto transport = std::make_unique<FiniteStreamTransport>(
        std::vector<QByteArray>{
            frameFor(catalog, 0xFFFE, normalValues(1.25)),
            frameFor(catalog, 0xFFFF, normalValues(2.5)),
            frameFor(catalog, 0x0000, normalValues(3.75)),
        });
    FiniteStreamTransport* transportPtr = transport.get();
    DhIgniteStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    hwtest::biz::Status stopStatus;
    Observer observer([&] {
        stopStatus = executor.requestStop(100);
        control.stopped = true;
    });

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(outcome.value.rawData.value(QStringLiteral("sampleCount")).toUInt(), 3u);
    ASSERT_EQ(observer.samples.size(), 3);
    EXPECT_EQ(observer.samples.at(0).streamElapsedUs, 0);
    EXPECT_EQ(observer.samples.at(1).streamElapsedUs, 2500);
    EXPECT_EQ(observer.samples.at(2).streamElapsedUs, 5000);
    EXPECT_EQ(observer.samples.at(1).timestampUs - observer.samples.at(0).timestampUs,
              2500);
    EXPECT_EQ(observer.samples.at(2).timestampUs - observer.samples.at(0).timestampUs,
              5000);
    EXPECT_EQ(observer.samples.at(0).values.value(QStringLiteral("seq")).toUInt(),
              0xFFFEu);
    EXPECT_EQ(observer.samples.at(2).values.value(QStringLiteral("seq")).toUInt(),
              0u);
    EXPECT_EQ(observer.samples.at(0).values
                  .value(QStringLiteral("ignition_phase")).toString(),
              QStringLiteral("baseline"));
    EXPECT_EQ(observer.samples.at(1).values
                  .value(QStringLiteral("ignition_phase")).toString(),
              QStringLiteral("post_ignition"));
    EXPECT_EQ(observer.samples.at(2).values
                  .value(QStringLiteral("frame_index")).toUInt(),
              2u);
    EXPECT_EQ(observer.samples.at(0).values
                  .value(QStringLiteral("dh_status.ch22")).toUInt(),
              1u);
    EXPECT_NEAR(observer.samples.at(2).values
                    .value(QStringLiteral("telemetry[22]")).toDouble(),
                22.125, 1e-9);
    EXPECT_EQ(stopStatus.code, hwtest::biz::ErrorCode::CapabilityUnsupported);

    const std::vector<QByteArray> writes = transportPtr->writes();
    ASSERT_EQ(writes.size(), 1u);
    const QVariantMap request = decodeRequest(catalog, writes.front());
    EXPECT_EQ(request.value(QStringLiteral("seq")).toUInt(), 0xFFFEu);
    EXPECT_EQ(request.value(QStringLiteral("power_enable")).toUInt(), 0u);
    EXPECT_EQ(request.value(QStringLiteral("return_enable")).toUInt(), 0u);
    EXPECT_EQ(request.value(QStringLiteral("channel[0]")).toUInt(),
              (quint32{1} << 0U) | (quint32{1} << 22U));
    EXPECT_EQ(request.value(QStringLiteral("channel[1]")).toUInt(), 0u);
    EXPECT_EQ(request.value(QStringLiteral("report_count")).toUInt(), 3u);
    EXPECT_EQ(request.value(QStringLiteral("interval_us")).toUInt(), 2500u);
    EXPECT_EQ(request.value(QStringLiteral("delay_frames")).toUInt(), 1u);
}

TEST(DhIgniteStreamExecutorTest,
     PublishesPostIgnitionRemoteErrorsAndContinuesToNaturalEnd)
{
    const QString assets = catalogDirectory();
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    QVariantMap remoteError = normalValues(2.5);
    remoteError.insert(QStringLiteral("status"), 1);
    remoteError.insert(QStringLiteral("err_code"), 0x3456);
    auto transport = std::make_unique<FiniteStreamTransport>(
        std::vector<QByteArray>{
            frameFor(catalog, 0xFFFE, normalValues(1.25)),
            frameFor(catalog, 0xFFFF, remoteError),
            frameFor(catalog, 0x0000, normalValues(3.75)),
        });
    FiniteStreamTransport* transportPtr = transport.get();
    DhIgniteStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, streamContext(),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Error);
    EXPECT_EQ(outcome.value.errorCode, hwtest::biz::ErrorCode::RemoteCommandError);
    EXPECT_EQ(observer.samples.size(), 3);
    EXPECT_EQ(observer.samples.at(1).values.value(QStringLiteral("err_code")).toUInt(),
              0x3456u);
    EXPECT_EQ(transportPtr->writes().size(), 1u);
}

TEST(DhIgniteStreamExecutorTest,
     DelayZeroPostIgnitionAcquisitionErrorStillConsumesRemainingFrames)
{
    const QString assets = catalogDirectory();
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    QVariantMap acquisitionError = normalValues(1.25);
    acquisitionError.insert(QStringLiteral("status"), 1);
    acquisitionError.insert(QStringLiteral("err_code"), 0x0203);
    auto transport = std::make_unique<FiniteStreamTransport>(
        std::vector<QByteArray>{
            frameFor(catalog, 0xFFFE, acquisitionError),
            frameFor(catalog, 0xFFFF, normalValues(2.5)),
        });
    FiniteStreamTransport* transportPtr = transport.get();
    DhIgniteStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{},
                                 streamContext(2, 2500, 0),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Error);
    ASSERT_EQ(observer.samples.size(), 2);
    EXPECT_EQ(observer.samples.at(0).values
                  .value(QStringLiteral("ignition_phase")).toString(),
              QStringLiteral("post_ignition"));
    EXPECT_EQ(transportPtr->readCalls(), 2);
}

TEST(DhIgniteStreamExecutorTest, RejectsWrongResponseSequence)
{
    const QString assets = catalogDirectory();
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    auto transport = std::make_unique<FiniteStreamTransport>(
        std::vector<QByteArray>{frameFor(catalog, 0xFFFF, normalValues(1.25))});
    DhIgniteStreamAlgorithmExecutor executor(std::move(transport));
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{},
                                 streamContext(1, 2500, 0),
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    EXPECT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.status.code, hwtest::biz::ErrorCode::ProtocolParseError);
    EXPECT_TRUE(observer.samples.isEmpty());
}

TEST(DhIgniteStreamExecutorTest, EncodesBusinessInvalidValuesForDutToReject)
{
    const QString assets = catalogDirectory();
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    QVariantMap remoteError = normalValues(0.0);
    remoteError.insert(QStringLiteral("status"), 1);
    remoteError.insert(QStringLiteral("err_code"), 0x0102);
    auto transport = std::make_unique<FiniteStreamTransport>(
        std::vector<QByteArray>{frameFor(catalog, 0xFFFE, remoteError)});
    FiniteStreamTransport* transportPtr = transport.get();
    DhIgniteStreamAlgorithmExecutor executor(std::move(transport));
    hwtest::biz::TestContext context = streamContext(2, 2499, 0);
    context.runParameters.insert(QStringLiteral("power_enable"), 2);
    context.runParameters.insert(QStringLiteral("return_enable"), 255);
    context.runParameters.insert(QStringLiteral("channel_enabled[0]"), false);
    context.runParameters.insert(QStringLiteral("channel_enabled[22]"), false);
    ASSERT_TRUE(executor.prepare(hwtest::biz::TestPlan{}, context,
                                 executionConfig(assets)).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(streamStep(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Error);
    ASSERT_EQ(observer.samples.size(), 1);
    EXPECT_EQ(observer.samples.at(0).values
                  .value(QStringLiteral("ignition_phase")).toString(),
              QStringLiteral("request_rejected"));
    EXPECT_EQ(transportPtr->readCalls(), 1);
    const std::vector<QByteArray> writes = transportPtr->writes();
    ASSERT_EQ(writes.size(), 1u);
    const QVariantMap request = decodeRequest(catalog, writes.front());
    EXPECT_EQ(request.value(QStringLiteral("power_enable")).toUInt(), 2u);
    EXPECT_EQ(request.value(QStringLiteral("return_enable")).toUInt(), 255u);
    EXPECT_EQ(request.value(QStringLiteral("channel[0]")).toUInt(), 0u);
    EXPECT_EQ(request.value(QStringLiteral("report_count")).toUInt(), 2u);
    EXPECT_EQ(request.value(QStringLiteral("interval_us")).toUInt(), 2499u);
    EXPECT_EQ(request.value(QStringLiteral("delay_frames")).toUInt(), 0u);
}

} // namespace
} // namespace hwtest::algorithm::mbddf
