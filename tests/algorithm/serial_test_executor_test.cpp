#include <gtest/gtest.h>

#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>
#include <algorithm/serial_test_executor.h>

#include <hal/i_control_channel.h>

#include <biz/test_config_manager.h>

#include <logging/log_types.h>

#include <memory>

namespace hwtest::algorithm::mbddf {
namespace {

QString catalogDirectory()
{
    const QString configured = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    return configured.isEmpty()
        ? QStringLiteral(HWTEST_MBDDF_PROTOCOL_CATALOG_DIR)
        : configured;
}

hwtest::biz::Status status(hwtest::biz::ErrorCode code, const QString& message)
{
    hwtest::biz::Status result;
    result.code = code;
    result.error.code = code;
    result.error.message = message;
    return result;
}

hwtest::hal::HalStatus halStatus(hwtest::hal::HalStatusCode code, const QString& message)
{
    hwtest::hal::HalStatus result;
    result.code = code;
    result.error.code = code;
    result.error.message = message;
    return result;
}

QByteArray responseFor(const ProtocolCatalog& catalog,
                       const QByteArray& requestFrame,
                       const QString& requestName,
                       const QString& responseName)
{
    QString error;
    QByteArray requestPayload;
    EXPECT_TRUE(decodeFrame(requestFrame, &requestPayload, &error))
        << error.toStdString();
    const MessageDefinition* request = catalog.findByName(requestName);
    const MessageDefinition* response = catalog.findByName(responseName);
    EXPECT_NE(request, nullptr);
    EXPECT_NE(response, nullptr);
    if (request == nullptr || response == nullptr) return {};

    QVariantMap requestValues;
    EXPECT_TRUE(decodePayload(*request, requestPayload, &requestValues, &error))
        << error.toStdString();
    QVariantMap responseValues{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("link_id"), requestValues.value(QStringLiteral("link_id"))},
    };
    if (requestName == QStringLiteral("bus_loop_test_request")) {
        responseValues.insert(QStringLiteral("error_count"), 0);
        responseValues.insert(QStringLiteral("total_count"),
                              requestValues.value(QStringLiteral("total_count")));
        responseValues.insert(QStringLiteral("elapsed_ms"), 17);
    } else {
        for (int index = 0; index < 114; ++index) {
            const QString field = QStringLiteral("data[%1]").arg(index);
            responseValues.insert(field, requestValues.value(field));
        }
    }

    QByteArray payload;
    EXPECT_TRUE(encodePayload(*response, responseValues,
                              static_cast<quint16>(requestValues.value(
                                  QStringLiteral("seq")).toUInt()),
                              &payload, &error))
        << error.toStdString();
    QByteArray frame;
    EXPECT_TRUE(encodeFrame(payload, &frame, &error)) << error.toStdString();
    return frame;
}

class RunControl final : public hwtest::biz::IRunControl {
public:
    hwtest::biz::RunControl current() const override
    {
        return m_stopped ? hwtest::biz::RunControl::Stop : hwtest::biz::RunControl::Run;
    }
    bool checkpoint() const override { return !m_stopped; }
    void stop() { m_stopped = true; }

private:
    bool m_stopped = false;
};

class Observer final : public hwtest::biz::IAlgorithmObserver {
public:
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
        if (stopAfterFirstSample && control != nullptr && samples.size() == 1) {
            control->stop();
        }
    }

    void onLog(const hwtest::logging::LogEvent&) override {}

    QVector<hwtest::biz::RawSample> samples;
    RunControl* control = nullptr;
    bool stopAfterFirstSample = false;
};

hwtest::biz::Criterion equalCriterion(const QString& metric, const QVariant& ref)
{
    hwtest::biz::Criterion criterion;
    criterion.metric = metric;
    criterion.op = hwtest::biz::CmpOp::Equal;
    criterion.ref = ref;
    return criterion;
}

hwtest::biz::TestStep serialStep()
{
    hwtest::biz::TestStep step;
    step.stepId = QStringLiteral("serial_test");
    step.testItemId = QStringLiteral("serial_test");
    step.algorithmId = QStringLiteral("mbddf.serial_test");
    step.timeoutMs = 12000;
    step.parameters = {{QStringLiteral("protocol"), QVariantMap{
        {QStringLiteral("requestValues"), QVariantMap{
            {QStringLiteral("link_id"), 0},
            {QStringLiteral("total_count"), 1000},
            {QStringLiteral("data[0]"), 0x4D},
            {QStringLiteral("data[1]"), 0x42},
            {QStringLiteral("data[2]"), 0x31},
        }},
    }}};
    step.criteria = {
        equalCriterion(QStringLiteral("status"), 0),
        equalCriterion(QStringLiteral("err_code"), 0),
        equalCriterion(QStringLiteral("link_id"), 0),
        equalCriterion(QStringLiteral("error_count"), 0),
        equalCriterion(QStringLiteral("total_count"), 1000),
    };
    return step;
}

QVariantMap executionConfig()
{
    return {
        {QStringLiteral("protocolAssetRoot"), catalogDirectory()},
        {QStringLiteral("serialTest"), QVariantMap{
             {QStringLiteral("loopback"), QVariantMap{
                  {QStringLiteral("requestProfileId"),
                   QStringLiteral("bus_loop_test_request")},
                  {QStringLiteral("responseProfileId"),
                   QStringLiteral("bus_loop_test_response")},
              }},
             {QStringLiteral("echo"), QVariantMap{
                  {QStringLiteral("requestProfileId"),
                   QStringLiteral("bus_echo_test_request")},
                  {QStringLiteral("responseProfileId"),
                   QStringLiteral("bus_echo_test_response")},
              }},
         }},
        {QStringLiteral("transport"), QVariantMap{
             {QStringLiteral("openTimeoutMs"), 1000},
             {QStringLiteral("readChunkBytes"), 260},
             {QStringLiteral("busEcho"), QVariantMap{
                  {QStringLiteral("payloadBytes"), 114},
                  {QStringLiteral("resourceByLink"), QVariantMap{
                       {QStringLiteral("0"), QStringLiteral("BUS_ECHO_COM1")},
                       {QStringLiteral("1"), QStringLiteral("BUS_ECHO_COM2")},
                       {QStringLiteral("3"), QStringLiteral("BUS_ECHO_COM4")},
                   }},
              }},
         }},
        {QStringLiteral("initialSequence"), 0x1234},
    };
}

class RawEchoChannel final : public hwtest::hal::IControlChannel {
public:
    hwtest::hal::HalStatus openControl(
        const hwtest::hal::ResourceId& resourceId,
        const hwtest::hal::OperationOptions&) override
    {
        openResource = resourceId;
        return {};
    }

    hwtest::hal::HalStatus closeControl(
        const hwtest::hal::ResourceId& resourceId,
        const hwtest::hal::OperationOptions&) override
    {
        if (openResource == resourceId) openResource.clear();
        return {};
    }

    hwtest::hal::HalStatus writeControl(
        const hwtest::hal::ResourceId& resourceId,
        const QByteArray& data,
        const hwtest::hal::OperationOptions&) override
    {
        if (openResource != resourceId) {
            return halStatus(hwtest::hal::HalStatusCode::InvalidState,
                             QStringLiteral("auxiliary resource is not open"));
        }
        writtenPayloads.push_back(data);
        return {};
    }

    hwtest::hal::HalResult<QByteArray> readControl(
        const hwtest::hal::ResourceId& resourceId,
        int maxBytes,
        const hwtest::hal::OperationOptions&) override
    {
        if (openResource != resourceId) {
            return {halStatus(hwtest::hal::HalStatusCode::InvalidState,
                              QStringLiteral("auxiliary resource is not open")), {}};
        }
        if (readOffset >= queuedPayload.size()) {
            return {halStatus(hwtest::hal::HalStatusCode::Timeout,
                              QStringLiteral("auxiliary payload is exhausted")), {}};
        }
        const QByteArray chunk = queuedPayload.mid(readOffset, maxBytes);
        readOffset += chunk.size();
        return {{}, chunk};
    }

    void queuePayload(const QByteArray& payload)
    {
        queuedPayload = payload;
        readOffset = 0;
    }

    QString openResource;
    QByteArray queuedPayload;
    int readOffset = 0;
    QVector<QByteArray> writtenPayloads;
};

class EchoControlTransport final : public IByteTransport {
public:
    EchoControlTransport(const ProtocolCatalog* catalog, RawEchoChannel* raw)
        : m_catalog(catalog), m_raw(raw)
    {
    }

    bool open(QString* error) override
    {
        m_open = true;
        if (error != nullptr) error->clear();
        return true;
    }

    TransportResult transact(const QByteArray&, int) override
    {
        return failed(QStringLiteral("BUS_ECHO must use split control I/O"));
    }

    TransportResult writeFrame(const QByteArray& frame, int timeoutMs) override
    {
        writeTimeouts.push_back(timeoutMs);
        if (!m_open || m_catalog == nullptr || m_raw == nullptr) {
            return failed(QStringLiteral("control transport is not open"));
        }
        QString error;
        QByteArray payload;
        if (!decodeFrame(frame, &payload, &error) || payload.size() < 120) {
            return failed(error.isEmpty() ? QStringLiteral("invalid echo request") : error);
        }
        m_raw->queuePayload(payload.mid(6, 114));
        m_response = responseFor(*m_catalog, frame,
                                 QStringLiteral("bus_echo_test_request"),
                                 QStringLiteral("bus_echo_test_response"));
        ++writes;
        TransportResult result;
        result.ok = true;
        return result;
    }

    TransportResult readFrame(int timeoutMs) override
    {
        readTimeouts.push_back(timeoutMs);
        if (!m_open) return failed(QStringLiteral("control transport is not open"));
        TransportResult result;
        result.ok = true;
        result.frame = m_response;
        return result;
    }

    void close() override { m_open = false; }

    QVector<int> writeTimeouts;
    QVector<int> readTimeouts;
    int writes = 0;

private:
    static TransportResult failed(const QString& message)
    {
        TransportResult result;
        result.errorCode = TransportResult::Error::Io;
        result.error = message;
        return result;
    }

    const ProtocolCatalog* m_catalog = nullptr;
    RawEchoChannel* m_raw = nullptr;
    QByteArray m_response;
    bool m_open = false;
};

TEST(SerialTestExecutorTest, LoopbackMapsCycleCountToOne0301Request)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error))
        << error.toStdString();
    QByteArray request;
    int transactions = 0;
    auto transport = std::make_unique<ScriptedByteTransport>(
        [&](const QByteArray& frame, int) {
            ++transactions;
            request = frame;
            return TransportResult{true, TransportResult::Error::None,
                                   responseFor(catalog, frame,
                                               QStringLiteral("bus_loop_test_request"),
                                               QStringLiteral("bus_loop_test_response")), {}};
        });
    SerialTestAlgorithmExecutor executor(std::move(transport), nullptr,
                                         QStringLiteral("CONTROL_SERIAL"));
    const hwtest::biz::TestStep step = serialStep();
    hwtest::biz::TestPlan plan;
    plan.steps.push_back(step);
    hwtest::biz::TestContext context;
    context.runParameters = {{QStringLiteral("test_mode"), 0},
                             {QStringLiteral("link_id"), 3},
                             {QStringLiteral("cycle_count"), 7}};
    ASSERT_TRUE(executor.prepare(plan, context, executionConfig()).ok());
    RunControl control;
    Observer observer;

    const auto result = executor.executeStep(step, control, observer);

    ASSERT_TRUE(result.ok()) << result.status.error.message.toStdString();
    EXPECT_EQ(result.value.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.value.algorithmId, QStringLiteral("mbddf.serial_test"));
    EXPECT_EQ(transactions, 1);
    QByteArray payload;
    ASSERT_TRUE(decodeFrame(request, &payload, &error)) << error.toStdString();
    const MessageDefinition* definition = catalog.findByName(
        QStringLiteral("bus_loop_test_request"));
    ASSERT_NE(definition, nullptr);
    QVariantMap values;
    ASSERT_TRUE(decodePayload(*definition, payload, &values, &error)) << error.toStdString();
    EXPECT_EQ(values.value(QStringLiteral("link_id")).toInt(), 3);
    EXPECT_EQ(values.value(QStringLiteral("total_count")).toInt(), 7);
    EXPECT_EQ(result.value.rawData.value(QStringLiteral("completed_cycle_count")).toInt(), 7);
    EXPECT_EQ(result.value.rawData.value(QStringLiteral("failed_cycle_count")).toInt(), 0);
    EXPECT_EQ(result.value.rawData.value(QStringLiteral("cycles")).toList().size(), 1);
    EXPECT_FALSE(result.value.rawData
                     .value(QStringLiteral("cycle_diagnostics_truncated"))
                     .toBool());
    EXPECT_TRUE(executor.shutdown(100).ok());
}

TEST(SerialTestExecutorTest, EchoLoops0302WithFiveSecondBudgetAndAggregatesSamples)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error))
        << error.toStdString();
    RawEchoChannel raw;
    auto transport = std::make_unique<EchoControlTransport>(&catalog, &raw);
    EchoControlTransport* transportView = transport.get();
    SerialTestAlgorithmExecutor executor(std::move(transport), &raw,
                                         QStringLiteral("CONTROL_SERIAL"));
    const hwtest::biz::TestStep step = serialStep();
    hwtest::biz::TestPlan plan;
    plan.steps.push_back(step);
    hwtest::biz::TestContext context;
    context.runParameters = {{QStringLiteral("test_mode"), 1},
                             {QStringLiteral("link_id"), 1},
                             {QStringLiteral("cycle_count"), 3}};
    ASSERT_TRUE(executor.prepare(plan, context, executionConfig()).ok());
    RunControl control;
    Observer observer;

    const auto result = executor.executeStep(step, control, observer);

    ASSERT_TRUE(result.ok()) << result.status.error.message.toStdString();
    EXPECT_EQ(result.value.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(transportView->writes, 3);
    ASSERT_EQ(raw.writtenPayloads.size(), 3);
    ASSERT_EQ(observer.samples.size(), 3);
    EXPECT_EQ(observer.samples.at(0).cycleIndex, 1u);
    EXPECT_EQ(observer.samples.at(2).cycleIndex, 3u);
    EXPECT_EQ(result.value.rawData.value(QStringLiteral("completed_cycle_count")).toInt(), 3);
    EXPECT_EQ(result.value.rawData.value(QStringLiteral("cycles")).toList().size(), 3);
    for (int timeout : transportView->writeTimeouts) {
        EXPECT_GT(timeout, 0);
        EXPECT_LE(timeout, 5000);
    }
    for (int timeout : transportView->readTimeouts) {
        EXPECT_GT(timeout, 0);
        EXPECT_LE(timeout, 5000);
    }
    EXPECT_TRUE(executor.shutdown(100).ok());
}

TEST(SerialTestExecutorTest, EchoBoundsCycleDiagnosticsForLargeLoopCounts)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error))
        << error.toStdString();
    RawEchoChannel raw;
    auto transport = std::make_unique<EchoControlTransport>(&catalog, &raw);
    SerialTestAlgorithmExecutor executor(std::move(transport), &raw,
                                         QStringLiteral("CONTROL_SERIAL"));
    const hwtest::biz::TestStep step = serialStep();
    hwtest::biz::TestPlan plan;
    plan.steps.push_back(step);
    hwtest::biz::TestContext context;
    context.runParameters = {{QStringLiteral("test_mode"), 1},
                             {QStringLiteral("link_id"), 1},
                             {QStringLiteral("cycle_count"), 20}};
    ASSERT_TRUE(executor.prepare(plan, context, executionConfig()).ok());
    RunControl control;
    Observer observer;

    const auto result = executor.executeStep(step, control, observer);

    ASSERT_TRUE(result.ok()) << result.status.error.message.toStdString();
    EXPECT_EQ(result.value.rawData.value(QStringLiteral("completed_cycle_count")).toInt(), 20);
    const QVariantList cycles = result.value.rawData.value(QStringLiteral("cycles")).toList();
    ASSERT_EQ(cycles.size(), 16);
    EXPECT_EQ(cycles.first().toMap().value(QStringLiteral("cycle_index")).toInt(), 1);
    EXPECT_EQ(cycles.last().toMap().value(QStringLiteral("cycle_index")).toInt(), 20);
    EXPECT_TRUE(result.value.rawData
                    .value(QStringLiteral("cycle_diagnostics_truncated"))
                    .toBool());
    EXPECT_EQ(result.value.rawData
                  .value(QStringLiteral("cycle_diagnostics_limit"))
                  .toInt(),
              16);
    EXPECT_TRUE(executor.shutdown(100).ok());
}

TEST(SerialTestExecutorTest, EchoChecksStopBetweenRoundsAndKeepsCompletedEvidence)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error))
        << error.toStdString();
    RawEchoChannel raw;
    auto transport = std::make_unique<EchoControlTransport>(&catalog, &raw);
    EchoControlTransport* transportView = transport.get();
    SerialTestAlgorithmExecutor executor(std::move(transport), &raw,
                                         QStringLiteral("CONTROL_SERIAL"));
    const hwtest::biz::TestStep step = serialStep();
    hwtest::biz::TestPlan plan;
    plan.steps.push_back(step);
    hwtest::biz::TestContext context;
    context.runParameters = {{QStringLiteral("test_mode"), 1},
                             {QStringLiteral("link_id"), 0},
                             {QStringLiteral("cycle_count"), 4}};
    ASSERT_TRUE(executor.prepare(plan, context, executionConfig()).ok());
    RunControl control;
    Observer observer;
    observer.control = &control;
    observer.stopAfterFirstSample = true;

    const auto result = executor.executeStep(step, control, observer);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, hwtest::biz::ErrorCode::Cancelled);
    EXPECT_EQ(transportView->writes, 1);
    EXPECT_EQ(observer.samples.size(), 1);
    EXPECT_EQ(result.value.rawData.value(QStringLiteral("completed_cycle_count")).toInt(), 1);
    EXPECT_TRUE(executor.shutdown(100).ok());
}

TEST(SerialTestConfigurationTest, ExposesSingleUnifiedSerialTestAndBothProtocolRoutes)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(
        QStringLiteral(HWTEST_MBDDF_SERIAL_TEST_CONFIG));

    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    EXPECT_EQ(loaded.value.configId, QStringLiteral("mbddf-serial-test"));
    EXPECT_EQ(loaded.value.productName, QStringLiteral("串口测试"));
    ASSERT_EQ(loaded.value.steps.size(), 1);
    const hwtest::biz::TestStep& step = loaded.value.steps.first();
    EXPECT_EQ(step.algorithmId, QStringLiteral("mbddf.serial_test"));
    EXPECT_EQ(step.timeoutMs, 5000);
    const QVariantMap requestValues = step.parameters.value(QStringLiteral("protocol")).toMap()
                                          .value(QStringLiteral("requestValues")).toMap();
    EXPECT_EQ(requestValues.value(QStringLiteral("test_mode")).toInt(), 0);
    EXPECT_EQ(requestValues.value(QStringLiteral("cycle_count")).toInt(), 1000);
    EXPECT_EQ(loaded.value.reportFields.value(QStringLiteral("title")).toString(),
              QStringLiteral("串口测试"));
    EXPECT_EQ(loaded.value.reportFields.value(QStringLiteral("supportedRunModes"))
                  .toStringList(),
              QStringList{QStringLiteral("single")});

    const QVariantMap serialTest = loaded.value.executionConfig
        .value(QStringLiteral("serialTest")).toMap();
    EXPECT_EQ(serialTest.value(QStringLiteral("loopback")).toMap()
                  .value(QStringLiteral("requestProfileId")).toString(),
              QStringLiteral("bus_loop_test_request"));
    EXPECT_EQ(serialTest.value(QStringLiteral("echo")).toMap()
                  .value(QStringLiteral("requestProfileId")).toString(),
              QStringLiteral("bus_echo_test_request"));
    const QVariantMap mapping = loaded.value.executionConfig
        .value(QStringLiteral("transport")).toMap()
        .value(QStringLiteral("busEcho")).toMap()
        .value(QStringLiteral("resourceByLink")).toMap();
    EXPECT_EQ(mapping.value(QStringLiteral("0")).toString(),
              QStringLiteral("BUS_ECHO_COM1"));
    EXPECT_EQ(mapping.value(QStringLiteral("1")).toString(),
              QStringLiteral("BUS_ECHO_COM2"));
    EXPECT_EQ(mapping.value(QStringLiteral("3")).toString(),
              QStringLiteral("BUS_ECHO_COM4"));
}

} // namespace
} // namespace hwtest::algorithm::mbddf
