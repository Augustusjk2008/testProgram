#include <gtest/gtest.h>

#include <algorithm/board_test_executor.h>
#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <biz/i_algorithm_executor.h>

#include <hal/hal_types.h>

#include <QMap>

#include <array>
#include <cmath>
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

hwtest::hal::HalStatus failedHal(const QString& message)
{
    hwtest::hal::HalStatus status;
    status.code = hwtest::hal::HalStatusCode::IoError;
    status.error.code = status.code;
    status.error.message = message;
    return status;
}

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
    void onProgress(const hwtest::biz::StepId&,
                    const hwtest::biz::TestItemId&,
                    int value,
                    const QString& stage) override
    {
        progress = value;
        lastStage = stage;
    }

    void onSample(const hwtest::biz::StepId&,
                  const hwtest::biz::RawSample& sample) override
    {
        samples.push_back(sample);
    }

    void onLog(const hwtest::logging::LogEvent&) override {}

    int progress = 0;
    QString lastStage;
    QVector<hwtest::biz::RawSample> samples;
};

class FakeBoardFixture final : public IBoardTestFixture {
public:
    hwtest::hal::HalResult<QVector<hwtest::hal::DigitalSample>>
    read6259Digital(const QVector<hwtest::hal::ResourceId>& resources,
                    int) override
    {
        ++digitalReadCount;
        hwtest::hal::HalResult<QVector<hwtest::hal::DigitalSample>> result;
        for (const QString& resource : resources) {
            hwtest::hal::DigitalSample sample;
            sample.channel = resource;
            if (resource.endsWith(QStringLiteral("TX_ENABLE_SENSE"))) {
                sample.level = (doMask & (1u << 2)) != 0
                    ? hwtest::hal::DigitalLevel::High
                    : hwtest::hal::DigitalLevel::Low;
            } else {
                sample.level = (doMask & (1u << 1)) != 0
                    ? hwtest::hal::DigitalLevel::High
                    : hwtest::hal::DigitalLevel::Low;
            }
            if (invertDigitalReadback) {
                sample.level = sample.level == hwtest::hal::DigitalLevel::High
                    ? hwtest::hal::DigitalLevel::Low
                    : hwtest::hal::DigitalLevel::High;
            }
            if (malformedDigitalReadback && result.value.isEmpty()) {
                sample.channel = QStringLiteral("WRONG_RESOURCE");
            }
            result.value.push_back(sample);
        }
        return result;
    }

    hwtest::hal::HalResult<hwtest::hal::SampleTaskBlock>
    capture6259Analog(const QVector<hwtest::hal::ResourceId>& resources,
                      double sampleRateHz,
                      int samplesPerChannel,
                      int) override
    {
        ++analogCaptureCount;
        hwtest::hal::HalResult<hwtest::hal::SampleTaskBlock> result;
        result.value.sampleType = hwtest::hal::SampleValueType::Float64;
        result.value.channelCount = resources.size();
        result.value.samplesPerChannel = samplesPerChannel;
        result.value.analogValues.reserve(resources.size() * samplesPerChannel);
        for (const QString& resource : resources) {
            int index = -1;
            if (resource.contains(QStringLiteral("PWM"))) {
                index = resource.mid(8, 1).toInt() - 1;
            } else if (resource.contains(QStringLiteral("DIR"))) {
                index = resource.mid(8, 1).toInt() - 1;
            }
            for (int sample = 0; sample < samplesPerChannel; ++sample) {
                double value = 0.0;
                if (resource.contains(QStringLiteral("PWM")) && index >= 0) {
                    const double duty = pwmDuty[static_cast<size_t>(index)] / 100.0;
                    const double phase = std::fmod(
                        sample * 4000.0 / sampleRateHz, 1.0);
                    value = phase < duty ? 5.0 : 0.0;
                    if (badPwmChannel == index && pwmDuty[static_cast<size_t>(index)] > 0) {
                        value = 0.0;
                    }
                } else if (resource.contains(QStringLiteral("DIR")) && index >= 0) {
                    value = directions[static_cast<size_t>(index)] ? 5.0 : 0.0;
                }
                result.value.analogValues.push_back(value);
            }
        }
        return result;
    }

    hwtest::hal::HalStatus write6733Analog(
        const QMap<hwtest::hal::ResourceId, double>& values,
        int) override
    {
        ++analogWriteCount;
        if (failWriteAt == analogWriteCount) {
            return failedHal(QStringLiteral("cleanup failed"));
        }
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            int index = it.key().mid(7, 1).toInt() - 1;
            if (index >= 0 && index < 4) {
                ao[static_cast<size_t>(index)] = it.value();
            }
        }
        analogWrites.push_back(values);
        return {};
    }

    void settle(int milliseconds) override
    {
        settleCalls.push_back(milliseconds);
    }

    quint32 doMask = 0x0018u;
    std::array<int, 4> pwmDuty{{0, 0, 0, 0}};
    std::array<bool, 4> directions{{false, false, false, false}};
    std::array<double, 4> ao{{0.0, 0.0, 0.0, 0.0}};
    int badPwmChannel = -1;
    int failWriteAt = -1;
    bool invertDigitalReadback = false;
    bool malformedDigitalReadback = false;
    int digitalReadCount = 0;
    int analogCaptureCount = 0;
    int analogWriteCount = 0;
    QVector<int> settleCalls;
    QVector<QMap<hwtest::hal::ResourceId, double>> analogWrites;
};

class BoardTransport final : public IByteTransport {
public:
    BoardTransport(const ProtocolCatalog* catalog, FakeBoardFixture* fixture)
        : m_catalog(catalog), m_fixture(fixture)
    {
    }

    bool configure(const QVariantMap&, QString* error) override
    {
        if (error != nullptr) error->clear();
        return true;
    }

    bool open(QString* error) override
    {
        openState = true;
        if (error != nullptr) error->clear();
        return true;
    }

    TransportResult transact(const QByteArray& frame, int) override
    {
        ++transactions;
        if (failTransaction == transactions) {
            TransportResult result;
            result.errorCode = TransportResult::Error::Io;
            result.error = QStringLiteral("injected transport failure");
            return result;
        }

        QString error;
        QByteArray payload;
        if (!decodeFrame(frame, &payload, &error) || payload.size() < 5) {
            return failed(error);
        }
        const auto* request = m_catalog->findByCommand(
            static_cast<quint8>(payload.at(1)),
            static_cast<quint8>(payload.at(2)), Direction::Request);
        if (request == nullptr) return failed(QStringLiteral("unknown request"));

        QVariantMap requestValues;
        if (!decodePayload(*request, payload, &requestValues, &error)) {
            return failed(error);
        }
        requests.push_back(requestValues);

        QVariantMap responseValues;
        QString responseName;
        if (request->name == QStringLiteral("do_write_request")) {
            const quint32 mask = requestValues.value(QStringLiteral("channel[0]")).toUInt();
            m_fixture->doMask = mask;
            responseName = QStringLiteral("do_write_response");
            responseValues.insert(QStringLiteral("applied_state[0]"), mask);
            responseValues.insert(QStringLiteral("applied_state[1]"), doAppliedStateWord1);
        } else {
            responseName = QStringLiteral("helm_board_test_response");
            for (int channel = 0; channel < 4; ++channel) {
                const int duty = requestValues
                    .value(QStringLiteral("pwm_duty_percent[%1]").arg(channel))
                    .toInt();
                const bool direction = requestValues
                    .value(QStringLiteral("direction[%1]").arg(channel))
                    .toBool();
                m_fixture->pwmDuty[static_cast<size_t>(channel)] = duty;
                m_fixture->directions[static_cast<size_t>(channel)] = direction;
                responseValues.insert(
                    QStringLiteral("pwm_duty_match[%1]").arg(channel), true);
                responseValues.insert(
                    QStringLiteral("direction_readback[%1]").arg(channel), direction);
                responseValues.insert(
                    QStringLiteral("pwm_duty[%1]").arg(channel), duty);
                const double encodedFeedback = qMin(
                    m_fixture->ao[static_cast<size_t>(channel)],
                    32767.0 * 5.0 / 32768.0);
                responseValues.insert(
                    QStringLiteral("helm_AD_value[%1]").arg(channel), encodedFeedback);
            }
            responseValues.insert(QStringLiteral("pwm_peak"), 0u);
            responseValues.insert(QStringLiteral("pwm_enable_mask"), 0x0Fu);
            responseValues.insert(QStringLiteral("pwm_update_enabled"), 1u);
            responseValues.insert(QStringLiteral("ad_acquisition_enabled"), 1u);
            responseValues.insert(QStringLiteral("ad_filter_enabled"), 1u);
        }
        responseValues.insert(QStringLiteral("status"), 0u);
        responseValues.insert(QStringLiteral("err_code"), 0u);

        const auto* response = m_catalog->findByName(responseName);
        QByteArray responsePayload;
        if (response == nullptr ||
            !encodePayload(*response, responseValues,
                           static_cast<quint16>(requestValues.value(QStringLiteral("seq")).toUInt()),
                           &responsePayload, &error)) {
            return failed(error);
        }
        TransportResult result;
        result.ok = encodeFrame(responsePayload, &result.frame, &error);
        result.error = error;
        return result;
    }

    void close() override { openState = false; }

    static TransportResult failed(const QString& message)
    {
        TransportResult result;
        result.errorCode = TransportResult::Error::Io;
        result.error = message;
        return result;
    }

    const ProtocolCatalog* m_catalog = nullptr;
    FakeBoardFixture* m_fixture = nullptr;
    QVector<QVariantMap> requests;
    int transactions = 0;
    int failTransaction = -1;
    quint32 doAppliedStateWord1 = 0;
    bool openState = false;
};

QVariantMap commonExecutionConfig()
{
    return {
        {QStringLiteral("protocolAssetRoot"), catalogDirectory()},
        {QStringLiteral("transport"), QVariantMap{}},
        {QStringLiteral("boardFixture"),
         QVariantMap{
             {QStringLiteral("doSenseResources"),
              QVariantList{QStringLiteral("DUT_TX_ENABLE_SENSE"),
                           QStringLiteral("DUT_ATTENUATOR_SENSE")}},
             {QStringLiteral("pwmResources"),
              QVariantList{QStringLiteral("HELM_PWM1_SENSE"),
                           QStringLiteral("HELM_PWM2_SENSE"),
                           QStringLiteral("HELM_PWM3_SENSE"),
                           QStringLiteral("HELM_PWM4_SENSE")}},
             {QStringLiteral("directionResources"),
              QVariantList{QStringLiteral("HELM_DIR1_SENSE"),
                           QStringLiteral("HELM_DIR2_SENSE"),
                           QStringLiteral("HELM_DIR3_SENSE"),
                           QStringLiteral("HELM_DIR4_SENSE")}},
             {QStringLiteral("feedbackResources"),
              QVariantList{QStringLiteral("HELM_FK1_STIM"),
                           QStringLiteral("HELM_FK2_STIM"),
                           QStringLiteral("HELM_FK3_STIM"),
                           QStringLiteral("HELM_FK4_STIM")}},
             {QStringLiteral("settlingMs"), 100},
             {QStringLiteral("pwmSampleRateHz"), 1250000.0},
             {QStringLiteral("pwmSamplesPerChannel"), 7500},
         }},
    };
}

hwtest::biz::TestPlan plan(const QString& algorithmId)
{
    hwtest::biz::TestPlan plan;
    hwtest::biz::TestStep step;
    step.stepId = QStringLiteral("step-board");
    step.testItemId = QStringLiteral("item-board");
    step.algorithmId = algorithmId;
    step.timeoutMs = 1000;
    step.retryCount = 0;
    plan.steps.push_back(step);
    return plan;
}

hwtest::biz::TestContext context(const QVariantMap& parameters = {})
{
    hwtest::biz::TestContext context;
    context.runId = QStringLiteral("run-board");
    context.requestId = QStringLiteral("request-board");
    context.tags.insert(QStringLiteral("runMode"), QStringLiteral("single"));
    context.runParameters = parameters;
    return context;
}

TEST(DoWriteExecutorTest, SendsOneUserSelectedMaskAndReturnsCompleteReadback)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error))
        << error.toStdString();
    FakeBoardFixture fixture;
    auto transport = std::make_unique<BoardTransport>(&catalog, &fixture);
    BoardTransport* rawTransport = transport.get();
    DoWriteAlgorithmExecutor executor(std::move(transport), &fixture);
    const auto testPlan = plan(QStringLiteral("mbddf.do_write"));
    const QVariantMap parameters{
        {QStringLiteral("channel_enabled[1]"), true},
        {QStringLiteral("channel_enabled[2]"), true},
        {QStringLiteral("channel_enabled[3]"), false},
        {QStringLiteral("channel_enabled[4]"), true},
        {QStringLiteral("channel_enabled[7]"), true},
    };
    ASSERT_TRUE(executor.prepare(testPlan, context(parameters),
                                 commonExecutionConfig()).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(testPlan.steps.front(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(rawTransport->transactions, 1);
    EXPECT_EQ(fixture.digitalReadCount, 1);
    const QVariantMap board = outcome.value.rawData
                                  .value(QStringLiteral("boardTest")).toMap();
    EXPECT_EQ(board.value(QStringLiteral("kind")).toString(),
              QStringLiteral("do_write"));
    ASSERT_EQ(board.value(QStringLiteral("doSteps")).toList().size(), 1);
    EXPECT_EQ(board.value(QStringLiteral("totalPoints")).toInt(), 1);
    const QVariantMap point = board.value(QStringLiteral("doSteps")).toList()
                                  .front().toMap();
    EXPECT_EQ(point.value(QStringLiteral("commandMask")).toUInt(), 0x0096u);
    EXPECT_EQ(point.value(QStringLiteral("appliedMask")).toUInt(), 0x0096u);
    EXPECT_EQ(point.value(QStringLiteral("commandState")).toUInt(), 0x0096u);
    EXPECT_EQ(point.value(QStringLiteral("appliedState")).toUInt(), 0x0096u);
    const QVariantList commandState = point
                                          .value(QStringLiteral("commandStateWords"))
                                          .toList();
    const QVariantList appliedState = point
                                          .value(QStringLiteral("appliedStateWords"))
                                          .toList();
    ASSERT_EQ(commandState.size(), 2);
    ASSERT_EQ(appliedState.size(), 2);
    EXPECT_EQ(commandState.at(0).toUInt(), 0x0096u);
    EXPECT_EQ(commandState.at(1).toUInt(), 0u);
    EXPECT_EQ(appliedState.at(0).toUInt(), 0x0096u);
    EXPECT_EQ(appliedState.at(1).toUInt(), 0u);
    EXPECT_EQ(rawTransport->requests.back()
                  .value(QStringLiteral("channel[0]")).toUInt(), 0x0096u);
    EXPECT_EQ(rawTransport->requests.back()
                  .value(QStringLiteral("channel[1]")).toUInt(), 0u);
    ASSERT_EQ(fixture.settleCalls.size(), 1);
    EXPECT_EQ(fixture.settleCalls.front(), 100);
}

TEST(DoWriteExecutorTest, StopAndLifecycleNeverIssueLegacyResetMask)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    FakeBoardFixture fixture;
    auto transport = std::make_unique<BoardTransport>(&catalog, &fixture);
    BoardTransport* rawTransport = transport.get();
    DoWriteAlgorithmExecutor executor(std::move(transport), &fixture);
    const auto testPlan = plan(QStringLiteral("mbddf.do_write"));
    const QVariantMap parameters{{QStringLiteral("channel_enabled[0]"), true}};
    ASSERT_TRUE(executor.prepare(testPlan, context(parameters),
                                 commonExecutionConfig()).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(testPlan.steps.front(), control, observer);

    ASSERT_TRUE(outcome.ok());
    ASSERT_EQ(rawTransport->requests.size(), 1);
    EXPECT_EQ(rawTransport->requests.front()
                  .value(QStringLiteral("channel[0]")).toUInt(), 0x0019u);
    EXPECT_TRUE(executor.requestStop(1000).ok());
    EXPECT_TRUE(executor.finishRun().ok());
    EXPECT_TRUE(executor.shutdown(1000).ok());
    EXPECT_EQ(rawTransport->transactions, 1);
    EXPECT_EQ(rawTransport->requests.size(), 1);
}

TEST(HelmBoardExecutorTest, ManualModeSendsOnceAndNeverTouchesFixture)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    FakeBoardFixture fixture;
    auto transport = std::make_unique<BoardTransport>(&catalog, &fixture);
    BoardTransport* rawTransport = transport.get();
    HelmBoardTestAlgorithmExecutor executor(std::move(transport), &fixture);
    const auto testPlan = plan(QStringLiteral("mbddf.helm_board_test"));
    const QVariantMap parameters{
        {QStringLiteral("test_mode"), 1},
        {QStringLiteral("pwm_duty_percent[0]"), 10},
        {QStringLiteral("pwm_duty_percent[1]"), 20},
        {QStringLiteral("pwm_duty_percent[2]"), 30},
        {QStringLiteral("pwm_duty_percent[3]"), 40},
        {QStringLiteral("direction[0]"), false},
        {QStringLiteral("direction[1]"), true},
        {QStringLiteral("direction[2]"), false},
        {QStringLiteral("direction[3]"), true},
    };
    ASSERT_TRUE(executor.prepare(testPlan, context(parameters),
                                 commonExecutionConfig()).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(testPlan.steps.front(), control, observer);

    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(rawTransport->transactions, 1);
    EXPECT_EQ(fixture.digitalReadCount, 0);
    EXPECT_EQ(fixture.analogCaptureCount, 0);
    EXPECT_EQ(fixture.analogWriteCount, 0);
    const QVariantMap board = outcome.value.rawData
                                  .value(QStringLiteral("boardTest")).toMap();
    EXPECT_EQ(board.value(QStringLiteral("mode")).toString(),
              QStringLiteral("manual"));
    EXPECT_FALSE(board.value(QStringLiteral("manualResponse")).toMap().isEmpty());
}

TEST(HelmBoardExecutorTest, AutomaticModeCompletesDirectionPwmAndFeedbackSweeps)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    FakeBoardFixture fixture;
    auto transport = std::make_unique<BoardTransport>(&catalog, &fixture);
    BoardTransport* rawTransport = transport.get();
    HelmBoardTestAlgorithmExecutor executor(std::move(transport), &fixture);
    const auto testPlan = plan(QStringLiteral("mbddf.helm_board_test"));
    const QVariantMap parameters{
        {QStringLiteral("test_mode"), 0},
        {QStringLiteral("pwm_duty_percent[0]"), 0},
        {QStringLiteral("pwm_duty_percent[1]"), 0},
        {QStringLiteral("pwm_duty_percent[2]"), 0},
        {QStringLiteral("pwm_duty_percent[3]"), 0},
        {QStringLiteral("direction[0]"), false},
        {QStringLiteral("direction[1]"), false},
        {QStringLiteral("direction[2]"), false},
        {QStringLiteral("direction[3]"), false},
    };
    ASSERT_TRUE(executor.prepare(testPlan, context(parameters),
                                 commonExecutionConfig()).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(testPlan.steps.front(), control, observer);

    ASSERT_TRUE(outcome.ok()) << outcome.status.error.message.toStdString();
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Pass)
        << outcome.value.message.toStdString();
    EXPECT_EQ(rawTransport->transactions, 85);
    const QVariantMap board = outcome.value.rawData
                                  .value(QStringLiteral("boardTest")).toMap();
    EXPECT_EQ(board.value(QStringLiteral("directionPoints")).toList().size(), 5);
    EXPECT_EQ(board.value(QStringLiteral("pwmPoints")).toList().size(), 36);
    EXPECT_EQ(board.value(QStringLiteral("feedbackPoints")).toList().size(), 44);
    ASSERT_FALSE(fixture.analogWrites.isEmpty());
    const auto finalWrite = fixture.analogWrites.back();
    ASSERT_EQ(finalWrite.size(), 4);
    for (double value : finalWrite) EXPECT_DOUBLE_EQ(value, 0.0);
}

TEST(HelmBoardExecutorTest, CriteriaFailureIsFiniteFailWithAccuratePointCount)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    FakeBoardFixture fixture;
    fixture.badPwmChannel = 1;
    auto transport = std::make_unique<BoardTransport>(&catalog, &fixture);
    BoardTransport* rawTransport = transport.get();
    HelmBoardTestAlgorithmExecutor executor(std::move(transport), &fixture);
    const auto testPlan = plan(QStringLiteral("mbddf.helm_board_test"));
    QVariantMap parameters;
    parameters.insert(QStringLiteral("test_mode"), 0);
    for (int channel = 0; channel < 4; ++channel) {
        parameters.insert(QStringLiteral("pwm_duty_percent[%1]").arg(channel), 0);
        parameters.insert(QStringLiteral("direction[%1]").arg(channel), false);
    }
    ASSERT_TRUE(executor.prepare(testPlan, context(parameters),
                                 commonExecutionConfig()).ok());
    RunControl control;
    Observer observer;

    const auto outcome = executor.executeStep(testPlan.steps.front(), control, observer);

    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(rawTransport->transactions, 85);
    EXPECT_EQ(outcome.value.verdict, hwtest::biz::TestVerdict::Fail);
    const QVariantMap board = outcome.value.rawData
                                  .value(QStringLiteral("boardTest")).toMap();
    EXPECT_EQ(board.value(QStringLiteral("summary")).toMap()
                  .value(QStringLiteral("failedPoints")).toInt(), 9);
    const QVariantList pwm = board.value(QStringLiteral("pwmPoints")).toList();
    ASSERT_EQ(pwm.size(), 36);
    for (const QVariant& item : pwm) {
        const QVariantMap point = item.toMap();
        for (auto it = point.cbegin(); it != point.cend(); ++it) {
            if (it.value().userType() == QMetaType::Double) {
                EXPECT_TRUE(std::isfinite(it.value().toDouble()))
                    << it.key().toStdString();
            }
        }
    }
}

} // namespace
} // namespace hwtest::algorithm::mbddf
