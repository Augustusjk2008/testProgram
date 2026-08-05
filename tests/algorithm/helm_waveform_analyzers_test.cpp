#include <gtest/gtest.h>

#include "helm_performance_test_support.h"

#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <utility>

namespace hwtest::algorithm::mbddf {
namespace {

void discardLeadingSamples(test::HelmPerformanceFixture* fixture, qint64 beforeUs)
{
    ASSERT_NE(fixture, nullptr);
    while (!fixture->samples.isEmpty() &&
           fixture->samples.first().streamElapsedUs < beforeUs) {
        fixture->samples.removeFirst();
    }
    ASSERT_FALSE(fixture->samples.isEmpty());
    const qint64 firstElapsedUs = fixture->samples.first().streamElapsedUs;
    for (PostRunSample& sample : fixture->samples) {
        sample.streamElapsedUs -= firstElapsedUs;
    }
}

void corruptFeedbackAfter(test::HelmPerformanceFixture* fixture, qint64 afterUs)
{
    ASSERT_NE(fixture, nullptr);
    for (PostRunSample& sample : fixture->samples) {
        if (sample.streamElapsedUs <= afterUs) continue;
        for (int channel = 0; channel < 4; ++channel) {
            sample.values.insert(QStringLiteral("fdb[%1]").arg(channel),
                                 100.0 + static_cast<double>(channel));
        }
    }
}

void prependNeutralStartup(test::HelmPerformanceFixture* fixture,
                           qint64 durationUs,
                           qint64 intervalUs = 10000)
{
    ASSERT_NE(fixture, nullptr);
    ASSERT_FALSE(fixture->samples.isEmpty());
    ASSERT_GT(durationUs, 0);
    ASSERT_GT(intervalUs, 0);
    QVector<PostRunSample> samples;
    samples.reserve(static_cast<int>(durationUs / intervalUs) +
                    fixture->samples.size() + 1);
    for (qint64 elapsedUs = 0; elapsedUs < durationUs; elapsedUs += intervalUs) {
        PostRunSample sample = fixture->samples.first();
        sample.streamElapsedUs = elapsedUs;
        for (int channel = 0; channel < 4; ++channel) {
            sample.values.insert(QStringLiteral("ins[%1]").arg(channel), 0.0);
            sample.values.insert(QStringLiteral("fdb[%1]").arg(channel), 0.0);
        }
        samples.push_back(std::move(sample));
    }
    for (PostRunSample sample : fixture->samples) {
        sample.streamElapsedUs += durationUs;
        samples.push_back(std::move(sample));
    }
    fixture->samples = std::move(samples);
}

void shiftCommandAndFeedback(test::HelmPerformanceFixture* fixture,
                             double offset)
{
    ASSERT_NE(fixture, nullptr);
    fixture->parameters.insert(QStringLiteral("offset"), offset);
    for (PostRunSample& sample : fixture->samples) {
        for (int channel = 0; channel < 4; ++channel) {
            const QString commandKey = QStringLiteral("ins[%1]").arg(channel);
            const QString feedbackKey = QStringLiteral("fdb[%1]").arg(channel);
            sample.values.insert(commandKey,
                                 sample.values.value(commandKey).toDouble() + offset);
            sample.values.insert(feedbackKey,
                                 sample.values.value(feedbackKey).toDouble() + offset);
        }
    }
}

TEST(HelmWaveformAnalyzersTest, ConstantUsesTailWindowWithoutDynamicExcitationFloor)
{
    const auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("constant"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(result.state, AnalysisState::Completed);
    EXPECT_EQ(channel->state, AnalysisChannelState::Completed);
    EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                          QStringLiteral("steady_mean_error")),
                fixture.expected.value(QStringLiteral("steadyMeanError")).toDouble(), 1e-6);
    EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                          QStringLiteral("steady_stddev")),
                fixture.expected.value(QStringLiteral("steadyStddev")).toDouble(), 1e-6);
    const AnalysisMetric* commandRange = test::findMetric(channel->commonMetrics,
                                                           QStringLiteral("command_range"));
    const AnalysisMetric* feedbackRange = test::findMetric(channel->commonMetrics,
                                                            QStringLiteral("feedback_range"));
    ASSERT_NE(commandRange, nullptr);
    ASSERT_NE(feedbackRange, nullptr);
    EXPECT_DOUBLE_EQ(commandRange->value, 0.0);
    EXPECT_GT(feedbackRange->value, 0.0);
    EXPECT_DOUBLE_EQ(test::requiredMetricValue(channel->commonMetrics,
                                               QStringLiteral("raw_sample_count")),
                     static_cast<double>(fixture.samples.size()));
    EXPECT_DOUBLE_EQ(test::requiredMetricValue(channel->commonMetrics,
                                               QStringLiteral("analysis_sample_count")),
                     31.0);
    EXPECT_NEAR(test::requiredMetricValue(channel->commonMetrics,
                                          QStringLiteral("analysis_duration_s")),
                0.3, 1e-12);
}

TEST(HelmWaveformAnalyzersTest, SineUsesPositiveCommandMinusFeedbackPhaseLag)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sine"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.parameters.insert(QStringLiteral("enable"), 15);
    shiftCommandAndFeedback(&fixture, 10.0);
    prependNeutralStartup(&fixture, 200000);
    corruptFeedbackAfter(&fixture, 1000000);
    discardLeadingSamples(&fixture, 100000);
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    for (int channelIndex = 0; channelIndex < 4; ++channelIndex) {
        const auto* channel = test::findChannel(result, channelIndex);
        ASSERT_NE(channel, nullptr) << error.toStdString();
        EXPECT_EQ(channel->state, AnalysisChannelState::Completed);
        EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                              QStringLiteral("phase_lag_deg")),
                    fixture.expected.value(QStringLiteral("phaseLagDeg")).toDouble(), 0.6);
        EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                              QStringLiteral("principal_delay_ms")),
                    fixture.expected.value(QStringLiteral("principalDelayMs")).toDouble(), 0.5);
        EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                              QStringLiteral("amplitude_ratio")),
                    fixture.expected.value(QStringLiteral("amplitudeRatio")).toDouble(), 0.01);
        EXPECT_NEAR(test::requiredMetricValue(channel->commonMetrics,
                                              QStringLiteral("analysis_duration_s")),
                    0.5, 0.01);
        EXPECT_NEAR(channel->diagnostics.value(QStringLiteral("analysisStartUs")).toLongLong(),
                    100000, 6000);
    }
}

TEST(HelmWaveformAnalyzersTest, SquareMeasuresCompleteRisingAndFallingEdges)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("square"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.parameters.insert(QStringLiteral("enable"), 15);
    shiftCommandAndFeedback(&fixture, 10.0);
    prependNeutralStartup(&fixture, 200000);
    corruptFeedbackAfter(&fixture, 1700000);
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    for (int channelIndex = 0; channelIndex < 4; ++channelIndex) {
        const auto* channel = test::findChannel(result, channelIndex);
        ASSERT_NE(channel, nullptr) << error.toStdString();
        EXPECT_NE(channel->state, AnalysisChannelState::Unavailable);
        EXPECT_DOUBLE_EQ(test::requiredMetricValue(channel->waveformMetrics,
                                                   QStringLiteral("rising_edge_count")), 1.0);
        EXPECT_DOUBLE_EQ(test::requiredMetricValue(channel->waveformMetrics,
                                                   QStringLiteral("falling_edge_count")), 1.0);
        EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                              QStringLiteral("rising_delay_ms_mean")),
                    fixture.expected.value(QStringLiteral("delayMs")).toDouble(), 12.0);
        EXPECT_NEAR(test::requiredMetricValue(channel->commonMetrics,
                                              QStringLiteral("analysis_duration_s")),
                    1.0, 0.02);
        EXPECT_NEAR(channel->diagnostics.value(QStringLiteral("analysisStartUs")).toLongLong(),
                    700000, 10000);
    }
}

TEST(HelmWaveformAnalyzersTest, SquareMarksUnsettledEdgesExplicitly)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("square"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    for (PostRunSample& sample : fixture.samples) {
        const double command = sample.values.value(QStringLiteral("ins[0]")).toDouble();
        sample.values.insert(QStringLiteral("fdb[0]"), command + 0.1);
    }
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    const AnalysisMetric* rising = test::findMetric(
        channel->waveformMetrics, QStringLiteral("rising_settling_time_ms_mean"));
    const AnalysisMetric* falling = test::findMetric(
        channel->waveformMetrics, QStringLiteral("falling_settling_time_ms_mean"));
    ASSERT_NE(rising, nullptr);
    ASSERT_NE(falling, nullptr);
    EXPECT_FALSE(rising->hasValue);
    EXPECT_FALSE(falling->hasValue);
    EXPECT_EQ(rising->status, AnalysisMetricStatus::NotSettled);
    EXPECT_EQ(falling->status, AnalysisMetricStatus::NotSettled);
}

TEST(HelmWaveformAnalyzersTest, SquareAtActualExcitationFloorRemainsComputableWithWarning)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("square"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.parameters.insert(QStringLiteral("ampl"), 1.0);
    for (PostRunSample& sample : fixture.samples) {
        sample.values.insert(QStringLiteral("ins[0]"),
                             sample.values.value(QStringLiteral("ins[0]")).toDouble() * 0.01);
        sample.values.insert(QStringLiteral("fdb[0]"),
                             sample.values.value(QStringLiteral("fdb[0]")).toDouble() * 0.01);
    }
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_NE(channel->state, AnalysisChannelState::Unavailable);
    EXPECT_TRUE(channel->warnings.contains(QStringLiteral("near_excitation_floor")));
}

TEST(HelmWaveformAnalyzersTest, SquareBelowActualExcitationFloorIsUnavailable)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("square"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.parameters.insert(QStringLiteral("ampl"), 1.0);
    for (PostRunSample& sample : fixture.samples) {
        sample.values.insert(QStringLiteral("ins[0]"),
                             sample.values.value(QStringLiteral("ins[0]")).toDouble() * 0.009);
        sample.values.insert(QStringLiteral("fdb[0]"),
                             sample.values.value(QStringLiteral("fdb[0]")).toDouble() * 0.009);
    }
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(channel->state, AnalysisChannelState::Unavailable);
    EXPECT_EQ(channel->reasonCode, QStringLiteral("weak_excitation"));
}

TEST(HelmWaveformAnalyzersTest, TriangleSeparatesDirectionAndExcludesTurningDeadZone)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("triangle"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.parameters.insert(QStringLiteral("enable"), 15);
    shiftCommandAndFeedback(&fixture, 10.0);
    prependNeutralStartup(&fixture, 200000);
    corruptFeedbackAfter(&fixture, 1700000);
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    for (int channelIndex = 0; channelIndex < 4; ++channelIndex) {
        const auto* channel = test::findChannel(result, channelIndex);
        ASSERT_NE(channel, nullptr) << error.toStdString();
        EXPECT_NE(channel->state, AnalysisChannelState::Unavailable);
        EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                              QStringLiteral("rising_slope_ratio")),
                    fixture.expected.value(QStringLiteral("slopeRatio")).toDouble(), 0.08);
        EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                              QStringLiteral("falling_slope_ratio")),
                    fixture.expected.value(QStringLiteral("slopeRatio")).toDouble(), 0.08);
        EXPECT_GT(test::requiredMetricValue(channel->waveformMetrics,
                                            QStringLiteral("directional_tracking_difference")), 0.0);
        EXPECT_NEAR(test::requiredMetricValue(channel->commonMetrics,
                                              QStringLiteral("analysis_duration_s")),
                    1.0, 0.02);
        EXPECT_NEAR(channel->diagnostics.value(QStringLiteral("analysisStartUs")).toLongLong(),
                    700000, 10000);
    }
}

TEST(HelmWaveformAnalyzersTest, DynamicExcitationFloorDistinguishesBelowEqualAndAbove)
{
    const auto nominal = test::loadHelmPerformanceFixture(QStringLiteral("sine"));
    ASSERT_FALSE(nominal.samples.isEmpty());
    struct Case {
        double amplitude;
        AnalysisChannelState expected;
        bool nearFloor;
    };
    const std::array<Case, 4> cases{{
        {0.0, AnalysisChannelState::Unavailable, false},
        {0.009, AnalysisChannelState::Unavailable, false},
        {0.01, AnalysisChannelState::Completed, true},
        {0.011, AnalysisChannelState::Completed, false},
    }};
    for (const Case& item : cases) {
        auto fixture = nominal;
        fixture.parameters.insert(QStringLiteral("ampl"), 1.0);
        const double scale = item.amplitude / 2.0;
        for (PostRunSample& sample : fixture.samples) {
            sample.values.insert(QStringLiteral("ins[0]"),
                                 sample.values.value(QStringLiteral("ins[0]")).toDouble() * scale);
            sample.values.insert(QStringLiteral("fdb[0]"),
                                 sample.values.value(QStringLiteral("fdb[0]")).toDouble() * scale);
        }
        QTemporaryDir directory;
        QString error;
        const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
        const auto* channel = test::findChannel(result, 0);
        ASSERT_NE(channel, nullptr) << error.toStdString();
        EXPECT_EQ(channel->state, item.expected) << item.amplitude;
        EXPECT_EQ(channel->warnings.contains(QStringLiteral("near_excitation_floor")),
                  item.nearFloor) << item.amplitude;
    }
}

TEST(HelmWaveformAnalyzersTest, SinePrincipalDelayKeepsQuarterAndThreeQuarterCycleDistinct)
{
    const auto nominal = test::loadHelmPerformanceFixture(QStringLiteral("sine"));
    ASSERT_FALSE(nominal.samples.isEmpty());
    struct Case {
        double feedbackLagDegrees;
        double expectedPhaseLagDegrees;
        double expectedDelayMs;
    };
    const std::array<Case, 2> cases{{
        {90.0, 90.0, 125.0},
        {270.0, -90.0, -125.0},
    }};
    for (const Case& item : cases) {
        auto fixture = nominal;
        const double lagRadians = item.feedbackLagDegrees * 3.14159265358979323846 / 180.0;
        for (PostRunSample& sample : fixture.samples) {
            const double time = static_cast<double>(sample.streamElapsedUs) / 1000000.0;
            sample.values.insert(QStringLiteral("fdb[0]"),
                                 2.0 * std::sin(2.0 * 3.14159265358979323846 * 2.0 * time -
                                                lagRadians));
        }
        QTemporaryDir directory;
        QString error;
        const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
        const auto* channel = test::findChannel(result, 0);
        ASSERT_NE(channel, nullptr) << error.toStdString();
        EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                              QStringLiteral("phase_lag_deg")),
                    item.expectedPhaseLagDegrees, 0.6);
        EXPECT_NEAR(test::requiredMetricValue(channel->waveformMetrics,
                                              QStringLiteral("principal_delay_ms")),
                    item.expectedDelayMs, 0.6);
    }
}

TEST(HelmWaveformAnalyzersTest, TriangleBelowActualExcitationFloorIsUnavailable)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("triangle"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.parameters.insert(QStringLiteral("ampl"), 1.0);
    for (PostRunSample& sample : fixture.samples) {
        sample.values.insert(QStringLiteral("ins[0]"),
                             sample.values.value(QStringLiteral("ins[0]")).toDouble() * 0.009);
        sample.values.insert(QStringLiteral("fdb[0]"),
                             sample.values.value(QStringLiteral("fdb[0]")).toDouble() * 0.009);
    }
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(channel->state, AnalysisChannelState::Unavailable);
    EXPECT_EQ(channel->reasonCode, QStringLiteral("weak_excitation"));
}

TEST(HelmWaveformAnalyzersTest, TriangleAtActualExcitationFloorRemainsComputableWithWarning)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("triangle"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.parameters.insert(QStringLiteral("ampl"), 1.0);
    for (PostRunSample& sample : fixture.samples) {
        sample.values.insert(QStringLiteral("ins[0]"),
                             sample.values.value(QStringLiteral("ins[0]")).toDouble() * 0.01);
        sample.values.insert(QStringLiteral("fdb[0]"),
                             sample.values.value(QStringLiteral("fdb[0]")).toDouble() * 0.01);
    }
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_NE(channel->state, AnalysisChannelState::Unavailable);
    EXPECT_TRUE(channel->warnings.contains(QStringLiteral("near_excitation_floor")));
}

TEST(HelmWaveformAnalyzersTest, ConstantReportsFirstActualTailWindowSample)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("constant"));
    ASSERT_GT(fixture.samples.size(), 2);
    fixture.samples.last().streamElapsedUs += 7000;
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(channel->diagnostics.value(QStringLiteral("analysisStartUs")).toLongLong(),
              1210000);
}

} // namespace
} // namespace hwtest::algorithm::mbddf
