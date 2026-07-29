#include <gtest/gtest.h>

#include "helm_performance_test_support.h"

#include <QTemporaryDir>

#include <cmath>

namespace hwtest::algorithm::mbddf {
namespace {

TEST(HelmSweepEstimatorTest, UsesDdsTimeAndKeepsBodeFrequenciesAscending)
{
    const auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sweep_first_order"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    ASSERT_FALSE(channel->bodePoints.isEmpty());
    for (int index = 1; index < channel->bodePoints.size(); ++index) {
        EXPECT_LT(channel->bodePoints.at(index - 1).frequencyHz,
                  channel->bodePoints.at(index).frequencyHz);
    }
    const AnalysisBodePoint* first = nullptr;
    const AnalysisBodePoint* last = nullptr;
    for (const AnalysisBodePoint& point : channel->bodePoints) {
        if (!point.hasMagnitude) continue;
        if (first == nullptr) first = &point;
        last = &point;
    }
    ASSERT_NE(first, nullptr);
    ASSERT_NE(last, nullptr);
    EXPECT_GT(first->magnitudeDb, last->magnitudeDb);
    EXPECT_TRUE(std::isfinite(first->phaseDeg));
    EXPECT_LT(first->phaseDeg, 1.0);
}

TEST(HelmSweepEstimatorTest, TruncatedFeedbackLeavesExplicitUncoveredPoints)
{
    const auto fixture = test::loadHelmPerformanceFixture(
        QStringLiteral("sweep_delay_truncated"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(result.state, AnalysisState::Partial);
    bool sawNotCovered = false;
    for (const AnalysisBodePoint& point : channel->bodePoints) {
        if (point.status == AnalysisPointStatus::NotCovered) sawNotCovered = true;
        EXPECT_TRUE(!point.hasMagnitude || std::isfinite(point.magnitudeDb));
        EXPECT_TRUE(!point.hasPhase || std::isfinite(point.phaseDeg));
    }
    EXPECT_TRUE(sawNotCovered);
}

TEST(HelmSweepEstimatorTest, EqualEndpointsFallsBackToSingleFrequencySine)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sine"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.parameters.insert(QStringLiteral("waveform"), 4);
    fixture.parameters.insert(QStringLiteral("max_freq"),
                              fixture.parameters.value(QStringLiteral("freq")));
    fixture.parameters.insert(QStringLiteral("sweep_duration_s"), 5.0);
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_NE(channel->state, AnalysisChannelState::Unavailable);
    EXPECT_TRUE(channel->bodePoints.isEmpty());
    EXPECT_NE(test::findMetric(channel->waveformMetrics, QStringLiteral("phase_lag_deg")),
              nullptr);
}

TEST(HelmSweepEstimatorTest, ReverseTruncationLeavesLowFrequencyCoverageExplicitlyUnavailable)
{
    const auto fixture = test::loadHelmPerformanceFixture(
        QStringLiteral("sweep_reverse_truncated"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    ASSERT_FALSE(channel->bodePoints.isEmpty());
    EXPECT_EQ(result.state, AnalysisState::Partial);
    bool sawLowFrequencyGap = false;
    for (const AnalysisBodePoint& point : channel->bodePoints) {
        if (point.frequencyHz < 2.0 && point.status == AnalysisPointStatus::NotCovered) {
            sawLowFrequencyGap = true;
        }
    }
    EXPECT_TRUE(sawLowFrequencyGap);
    const AnalysisMetric* lowFrequencyGain = test::findMetric(
        channel->waveformMetrics, QStringLiteral("low_frequency_gain_db"));
    ASSERT_NE(lowFrequencyGain, nullptr);
    EXPECT_FALSE(lowFrequencyGain->hasValue);
}

TEST(HelmSweepEstimatorTest, SecondOrderAndPureDelayFixturesKeepFiniteLocalEstimates)
{
    for (const QString& name : {QStringLiteral("sweep_second_order"),
                                QStringLiteral("sweep_pure_delay")}) {
        const auto fixture = test::loadHelmPerformanceFixture(name);
        ASSERT_FALSE(fixture.samples.isEmpty()) << name.toStdString();
        QTemporaryDir directory;
        QString error;
        const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
        const auto* channel = test::findChannel(result, 0);
        ASSERT_NE(channel, nullptr) << name.toStdString() << error.toStdString();
        ASSERT_FALSE(channel->bodePoints.isEmpty()) << name.toStdString();
        bool sawFinitePoint = false;
        for (const AnalysisBodePoint& point : channel->bodePoints) {
            if (point.hasMagnitude && point.hasPhase) {
                sawFinitePoint = std::isfinite(point.magnitudeDb) && std::isfinite(point.phaseDeg);
                if (sawFinitePoint) break;
            }
        }
        EXPECT_TRUE(sawFinitePoint) << name.toStdString();
    }
}

TEST(HelmSweepEstimatorTest, PureDelayBodePhaseUsesFeedbackOverCommandSignConvention)
{
    const auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sweep_pure_delay"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    const AnalysisBodePoint* closest = nullptr;
    for (const AnalysisBodePoint& point : channel->bodePoints) {
        if (!point.hasPhase || (closest != nullptr &&
            std::abs(point.frequencyHz - 5.0) >= std::abs(closest->frequencyHz - 5.0))) {
            continue;
        }
        closest = &point;
    }
    ASSERT_NE(closest, nullptr);
    EXPECT_LT(closest->phaseDeg, 0.0);
}

TEST(HelmSweepEstimatorTest, EarlyStopReportsPartialInsteadOfInventingTailCoverage)
{
    const auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sweep_early_stop"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    EXPECT_EQ(result.state, AnalysisState::Partial);
}

TEST(HelmSweepEstimatorTest, EarlyStopKeepsFixedMetricSchemaAndMarksPlannedTailUncovered)
{
    const auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sweep_early_stop"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(channel->state, AnalysisChannelState::Partial);

    for (const QString& key : {QStringLiteral("low_frequency_gain_db"),
                               QStringLiteral("bandwidth_minus_1db_hz"),
                               QStringLiteral("bandwidth_minus_3db_hz"),
                               QStringLiteral("phase_5hz_deg"),
                               QStringLiteral("phase_10hz_deg"),
                               QStringLiteral("phase_20hz_deg"),
                               QStringLiteral("resonance_peak_db"),
                               QStringLiteral("resonance_frequency_hz")}) {
        EXPECT_NE(test::findMetric(channel->waveformMetrics, key), nullptr) << key.toStdString();
    }

    bool sawPlannedTailGap = false;
    for (const AnalysisBodePoint& point : channel->bodePoints) {
        if (point.frequencyHz > 5.0 && point.status == AnalysisPointStatus::NotCovered) {
            sawPlannedTailGap = true;
            break;
        }
    }
    EXPECT_TRUE(sawPlannedTailGap);
}

TEST(HelmSweepEstimatorTest, EqualDynamicExcitationFloorRemainsComputableWithWarning)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sweep_first_order"));
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
    EXPECT_TRUE(channel->warnings.contains(QStringLiteral("near_excitation_floor")))
        << "min=" << channel->diagnostics.value(QStringLiteral("minimumLocalCommandAmplitude")).toDouble()
        << " max=" << channel->diagnostics.value(QStringLiteral("maximumLocalCommandAmplitude")).toDouble();
}

TEST(HelmSweepEstimatorTest, CompleteFlatResponseReportsBandwidthAboveObservedRange)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sweep_first_order"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    for (PostRunSample& sample : fixture.samples) {
        sample.values.insert(QStringLiteral("fdb[0]"),
                             sample.values.value(QStringLiteral("ins[0]")));
    }
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    const AnalysisMetric* minusOne = test::findMetric(
        channel->waveformMetrics, QStringLiteral("bandwidth_minus_1db_hz"));
    const AnalysisMetric* minusThree = test::findMetric(
        channel->waveformMetrics, QStringLiteral("bandwidth_minus_3db_hz"));
    ASSERT_NE(minusOne, nullptr);
    ASSERT_NE(minusThree, nullptr);
    EXPECT_FALSE(minusOne->hasValue);
    EXPECT_FALSE(minusThree->hasValue);
    EXPECT_EQ(minusOne->status, AnalysisMetricStatus::AboveObservedRange);
    EXPECT_EQ(minusThree->status, AnalysisMetricStatus::AboveObservedRange);
}

TEST(HelmSweepEstimatorTest, TooShortCommandOverlapIsRejectedAsAmbiguous)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("sweep_first_order"));
    ASSERT_GT(fixture.samples.size(), 62);
    fixture.samples = fixture.samples.mid(50, 12);
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(channel->state, AnalysisChannelState::Unavailable);
    EXPECT_EQ(channel->reasonCode, QStringLiteral("command_model_ambiguous"));
}

} // namespace
} // namespace hwtest::algorithm::mbddf
