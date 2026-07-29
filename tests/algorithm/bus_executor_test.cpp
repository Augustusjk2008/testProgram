#include <gtest/gtest.h>

#include <algorithm/mbddf_exchange_executor.h>
#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <logging/log_types.h>

#include <atomic>
#include <functional>
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

class RunControl final : public hwtest::biz::IRunControl {
public:
    hwtest::biz::RunControl current() const override
    {
        return hwtest::biz::RunControl::Run;
    }
    bool checkpoint() const override { return true; }
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
    }
    void onLog(const hwtest::logging::LogEvent&) override {}

    QVector<hwtest::biz::RawSample> samples;
};

hwtest::biz::Criterion equalCriterion(const QString& metric, const QVariant& ref)
{
    hwtest::biz::Criterion criterion;
    criterion.metric = metric;
    criterion.op = hwtest::biz::CmpOp::Equal;
    criterion.ref = ref;
    return criterion;
}

QVariantMap executionConfig(const QString& requestProfile,
                            const QString& responseProfile)
{
    return {
        {QStringLiteral("protocolAssetRoot"), catalogDirectory()},
        {QStringLiteral("protocol"), QVariantMap{
             {QStringLiteral("requestProfileId"), requestProfile},
             {QStringLiteral("responseProfileId"), responseProfile},
         }},
        {QStringLiteral("initialSequence"), 0x1234},
    };
}

QByteArray makeResponse(const ProtocolCatalog& catalog,
                        const QByteArray& requestFrame,
                        const QString& requestName,
                        const QString& responseName,
                        const std::function<void(QVariantMap*)>& mutate = {})
{
    QString error;
    QByteArray requestPayload;
    EXPECT_TRUE(decodeFrame(requestFrame, &requestPayload, &error))
        << error.toStdString();
    const MessageDefinition* requestDefinition = catalog.findByName(requestName);
    const MessageDefinition* responseDefinition = catalog.findByName(responseName);
    EXPECT_NE(requestDefinition, nullptr);
    EXPECT_NE(responseDefinition, nullptr);
    if (requestDefinition == nullptr || responseDefinition == nullptr) return {};
    QVariantMap requestValues;
    EXPECT_TRUE(decodePayload(*requestDefinition, requestPayload, &requestValues, &error))
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
    if (mutate) mutate(&responseValues);

    QByteArray responsePayload;
    EXPECT_TRUE(encodePayload(*responseDefinition, responseValues,
                              static_cast<quint16>(requestValues.value(
                                  QStringLiteral("seq")).toUInt()),
                              &responsePayload, &error))
        << error.toStdString();
    QByteArray responseFrame;
    EXPECT_TRUE(encodeFrame(responsePayload, &responseFrame, &error))
        << error.toStdString();
    return responseFrame;
}

hwtest::biz::TestStep loopStep()
{
    hwtest::biz::TestStep step;
    step.stepId = QStringLiteral("bus_loop");
    step.testItemId = QStringLiteral("bus_loop");
    step.algorithmId = QStringLiteral("mbddf.bus_loop");
    step.timeoutMs = 5000;
    step.parameters = {{QStringLiteral("protocol"), QVariantMap{
        {QStringLiteral("requestValues"), QVariantMap{
             {QStringLiteral("link_id"), 0},
             {QStringLiteral("total_count"), 1000},
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

hwtest::biz::TestStep echoStep()
{
    hwtest::biz::TestStep step;
    step.stepId = QStringLiteral("bus_echo");
    step.testItemId = QStringLiteral("bus_echo");
    step.algorithmId = QStringLiteral("mbddf.bus_echo");
    step.timeoutMs = 5000;
    step.parameters = {{QStringLiteral("protocol"), QVariantMap{
        {QStringLiteral("requestValues"), QVariantMap{
             {QStringLiteral("link_id"), 0},
             {QStringLiteral("data[0]"), 0x4D},
             {QStringLiteral("data[1]"), 0x42},
             {QStringLiteral("data[2]"), 0x31},
         }},
    }}};
    step.criteria = {
        equalCriterion(QStringLiteral("status"), 0),
        equalCriterion(QStringLiteral("err_code"), 0),
        equalCriterion(QStringLiteral("link_id"), 0),
    };
    return step;
}

TEST(BusExecutorTest, LoopCriteriaTrackRuntimeLinkAndCount)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error))
        << error.toStdString();
    auto transport = std::make_unique<ScriptedByteTransport>(
        [&](const QByteArray& request, int) {
            return TransportResult{true, TransportResult::Error::None,
                                   makeResponse(catalog, request,
                                                QStringLiteral("bus_loop_test_request"),
                                                QStringLiteral("bus_loop_test_response")), {}};
        });
    MbdDfExchangeAlgorithmExecutor executor(
        std::move(transport), QStringLiteral("mbddf.bus_loop"),
        QStringLiteral("bus_loop_test_request"),
        QStringLiteral("bus_loop_test_response"), QStringLiteral("BUS_LOOP_TEST"));
    const hwtest::biz::TestStep step = loopStep();
    hwtest::biz::TestPlan plan;
    plan.steps.push_back(step);
    hwtest::biz::TestContext context;
    context.runParameters = {{QStringLiteral("link_id"), 3},
                             {QStringLiteral("total_count"), 7}};
    ASSERT_TRUE(executor.prepare(plan, context,
                                 executionConfig(QStringLiteral("bus_loop_test_request"),
                                                 QStringLiteral("bus_loop_test_response"))).ok());
    RunControl control;
    Observer observer;

    const auto result = executor.executeStep(step, control, observer);

    ASSERT_TRUE(result.ok()) << result.status.error.message.toStdString();
    EXPECT_EQ(result.value.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_TRUE(executor.shutdown(100).ok());
}

TEST(BusExecutorTest, EchoUsesFixedPayloadButPublishesOnlyCompactSummary)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    auto transport = std::make_unique<ScriptedByteTransport>(
        [&](const QByteArray& request, int) {
            return TransportResult{true, TransportResult::Error::None,
                                   makeResponse(catalog, request,
                                                QStringLiteral("bus_echo_test_request"),
                                                QStringLiteral("bus_echo_test_response")), {}};
        });
    MbdDfExchangeAlgorithmExecutor executor(
        std::move(transport), QStringLiteral("mbddf.bus_echo"),
        QStringLiteral("bus_echo_test_request"),
        QStringLiteral("bus_echo_test_response"), QStringLiteral("BUS_ECHO_TEST"));
    const hwtest::biz::TestStep step = echoStep();
    hwtest::biz::TestPlan plan;
    plan.steps.push_back(step);
    hwtest::biz::TestContext context;
    context.runParameters = {{QStringLiteral("link_id"), 1}};
    ASSERT_TRUE(executor.prepare(plan, context,
                                 executionConfig(QStringLiteral("bus_echo_test_request"),
                                                 QStringLiteral("bus_echo_test_response"))).ok());
    RunControl control;
    Observer observer;

    const auto result = executor.executeStep(step, control, observer);

    ASSERT_TRUE(result.ok()) << result.status.error.message.toStdString();
    EXPECT_EQ(result.value.verdict, hwtest::biz::TestVerdict::Pass);
    ASSERT_EQ(observer.samples.size(), 1);
    EXPECT_FALSE(observer.samples.first().values.contains(QStringLiteral("data[0]")));
    EXPECT_EQ(observer.samples.first().values.value(QStringLiteral("echo_bytes")).toInt(), 114);
    EXPECT_EQ(observer.samples.first().values.value(QStringLiteral("mismatch_count")).toInt(), 0);
    for (const auto& measurement : result.value.measurements) {
        EXPECT_FALSE(measurement.name.startsWith(QStringLiteral("data[")));
    }
    EXPECT_TRUE(executor.shutdown(100).ok());
}

TEST(BusExecutorTest, EchoFailsWhenDutControlResponseChangesOneByte)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    auto transport = std::make_unique<ScriptedByteTransport>(
        [&](const QByteArray& request, int) {
            return TransportResult{true, TransportResult::Error::None,
                                   makeResponse(
                                       catalog, request,
                                       QStringLiteral("bus_echo_test_request"),
                                       QStringLiteral("bus_echo_test_response"),
                                       [](QVariantMap* values) {
                                           values->insert(QStringLiteral("data[57]"), 0x7F);
                                       }), {}};
        });
    MbdDfExchangeAlgorithmExecutor executor(
        std::move(transport), QStringLiteral("mbddf.bus_echo"),
        QStringLiteral("bus_echo_test_request"),
        QStringLiteral("bus_echo_test_response"), QStringLiteral("BUS_ECHO_TEST"));
    const hwtest::biz::TestStep step = echoStep();
    hwtest::biz::TestPlan plan;
    plan.steps.push_back(step);
    ASSERT_TRUE(executor.prepare(plan, {},
                                 executionConfig(QStringLiteral("bus_echo_test_request"),
                                                 QStringLiteral("bus_echo_test_response"))).ok());
    RunControl control;
    Observer observer;

    const auto result = executor.executeStep(step, control, observer);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value.verdict, hwtest::biz::TestVerdict::Fail);
    EXPECT_EQ(result.value.errorCode, hwtest::biz::ErrorCode::SampleFail);
    EXPECT_TRUE(result.value.message.contains(QStringLiteral("data[57]")));
    EXPECT_TRUE(executor.shutdown(100).ok());
}

} // namespace
} // namespace hwtest::algorithm::mbddf
