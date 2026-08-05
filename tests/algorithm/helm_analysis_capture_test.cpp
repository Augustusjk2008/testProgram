#include <gtest/gtest.h>

#include "helm_performance_test_support.h"

#include <QFileInfo>
#include <QTemporaryDir>

namespace hwtest::algorithm::mbddf {
namespace {

PostRunSample captureSample(qint64 elapsedUs, double command, double feedback, int sequence)
{
    PostRunSample sample;
    sample.sourceId = QStringLiteral("HELM_STREAM");
    sample.streamElapsedUs = elapsedUs;
    for (int channel = 0; channel < 4; ++channel) {
        sample.values.insert(QStringLiteral("ins[%1]").arg(channel), command);
        sample.values.insert(QStringLiteral("fdb[%1]").arg(channel), feedback);
    }
    sample.values.insert(QStringLiteral("status"), 0);
    sample.values.insert(QStringLiteral("err_code"), 0);
    sample.values.insert(QStringLiteral("timeout"), 0);
    sample.values.insert(QStringLiteral("product_frame_sequence"), sequence);
    sample.values.insert(QStringLiteral("serial_a"), sequence);
    sample.values.insert(QStringLiteral("serial_b"), sequence);
    sample.values.insert(QStringLiteral("dds_timestamp_us"), elapsedUs + 1000);
    sample.values.insert(QStringLiteral("batch_sample_index"), 0);
    return sample;
}

TEST(HelmAnalysisCaptureTest, SealIsImmutableAndLateSamplesAreDiagnosticOnly)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    AnalysisSessionSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.captureFilePath = directory.filePath(QStringLiteral("capture.bin"));
    spec.effectiveRunParameters = {{QStringLiteral("waveform"), 3},
                                   {QStringLiteral("freq"), 1.0},
                                   {QStringLiteral("ampl"), 1.0},
                                   {QStringLiteral("enable"), 1}};
    QString error;
    auto session = createPostRunAnalysisSession(spec, &error);
    ASSERT_NE(session, nullptr) << error.toStdString();
    const AnalysisIdentity identity{QStringLiteral("capture-task"), 5};
    ASSERT_TRUE(session->bindIdentity(identity, &error)) << error.toStdString();

    EXPECT_TRUE(session->append(identity, captureSample(0, 2.0, 2.1, 1)).accepted);
    EXPECT_TRUE(session->append(identity, captureSample(1000000, 2.0, 2.1, 2)).accepted);
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Stopped;
    const AnalysisInputSeal firstSeal = session->seal(termination);

    ASSERT_TRUE(firstSeal.valid) << firstSeal.message.toStdString();
    EXPECT_EQ(firstSeal.captureFormatVersion, 1u);
    EXPECT_EQ(firstSeal.captureRecordBytes, 124u);
    EXPECT_EQ(firstSeal.acceptedSampleCount, 2u);
    EXPECT_TRUE(QFileInfo::exists(firstSeal.captureFilePath));
    const AnalysisAcceptResult late = session->append(
        identity, captureSample(2000000, 2.0, 2.1, 3));
    EXPECT_FALSE(late.accepted);
    EXPECT_TRUE(late.late);
    const AnalysisInputSeal secondSeal = session->seal(termination);
    EXPECT_EQ(secondSeal.acceptedSampleCount, firstSeal.acceptedSampleCount);
    EXPECT_EQ(secondSeal.normalizedInputSha256, firstSeal.normalizedInputSha256);
}

TEST(HelmAnalysisCaptureTest, SequenceChangesDoNotChangeNumericalResult)
{
    const auto first = test::loadHelmPerformanceFixture(QStringLiteral("sequence_diagnostic_a"));
    const auto second = test::loadHelmPerformanceFixture(QStringLiteral("sequence_diagnostic_b"));
    ASSERT_FALSE(first.samples.isEmpty());
    ASSERT_EQ(first.samples.size(), second.samples.size());
    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    QString error;
    const AnalysisResult firstResult = test::analyzeHelmFixture(first, &firstDirectory, &error);
    ASSERT_FALSE(firstResult.channels.isEmpty()) << error.toStdString();
    const AnalysisResult secondResult = test::analyzeHelmFixture(second, &secondDirectory, &error);
    ASSERT_FALSE(secondResult.channels.isEmpty()) << error.toStdString();
    const auto* firstChannel = test::findChannel(firstResult, 0);
    const auto* secondChannel = test::findChannel(secondResult, 0);
    ASSERT_NE(firstChannel, nullptr);
    ASSERT_NE(secondChannel, nullptr);
    EXPECT_EQ(firstResult.state, secondResult.state);
    EXPECT_EQ(firstChannel->state, secondChannel->state);
    EXPECT_DOUBLE_EQ(test::requiredMetricValue(firstChannel->commonMetrics,
                                               QStringLiteral("rmse")),
                     test::requiredMetricValue(secondChannel->commonMetrics,
                                               QStringLiteral("rmse")));
    EXPECT_NE(firstResult.diagnostics.value(QStringLiteral("sequenceDiscontinuities")).toULongLong(),
              secondResult.diagnostics.value(QStringLiteral("sequenceDiscontinuities")).toULongLong());
}

TEST(HelmAnalysisCaptureTest, MissingSequenceDiagnosticsDoNotGateAnalysis)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("constant"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    for (PostRunSample& sample : fixture.samples) {
        sample.values.remove(QStringLiteral("product_frame_sequence"));
        sample.values.remove(QStringLiteral("serial_a"));
        sample.values.remove(QStringLiteral("serial_b"));
        sample.values.remove(QStringLiteral("dds_timestamp_us"));
        sample.values.remove(QStringLiteral("batch_sample_index"));
    }
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(result.state, AnalysisState::Completed);
    EXPECT_EQ(channel->state, AnalysisChannelState::Completed);
}

TEST(HelmAnalysisCaptureTest, ReachingInputLimitSealsDiagnosticCaptureWithoutChangingInput)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    AnalysisSessionSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.captureFilePath = directory.filePath(QStringLiteral("limited.bin"));
    spec.maxInputSamples = 1;
    spec.maxCaptureBytes = 4096;
    spec.maxAnalysisDurationUs = 2000000;
    spec.effectiveRunParameters = {{QStringLiteral("waveform"), 3},
                                   {QStringLiteral("freq"), 1.0},
                                   {QStringLiteral("ampl"), 1.0},
                                   {QStringLiteral("offset"), 0.0},
                                   {QStringLiteral("start"), 0.0},
                                   {QStringLiteral("max_freq"), 1.0},
                                   {QStringLiteral("sweep_duration_s"), 1.0},
                                   {QStringLiteral("enable"), 1}};
    QString error;
    auto session = createPostRunAnalysisSession(spec, &error);
    ASSERT_NE(session, nullptr) << error.toStdString();
    const AnalysisIdentity identity{QStringLiteral("limited-task"), 1};
    ASSERT_TRUE(session->bindIdentity(identity, &error)) << error.toStdString();
    EXPECT_TRUE(session->append(identity, captureSample(0, 0.0, 0.0, 1)).accepted);
    const AnalysisAcceptResult limited = session->append(identity, captureSample(1000, 0.0, 0.0, 2));
    EXPECT_EQ(limited.disposition, AnalysisAppendDisposition::ResourceLimit);
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Stopped;
    const AnalysisInputSeal seal = session->seal(termination);
    ASSERT_TRUE(seal.valid) << seal.message.toStdString();
    EXPECT_TRUE(seal.resourceLimitReached);
    EXPECT_EQ(seal.acceptedSampleCount, 1u);
    auto analyzer = createPostRunAnalyzer(spec.algorithmId, &error);
    ASSERT_NE(analyzer, nullptr) << error.toStdString();
    const AnalysisResult result = analyzer->analyze(seal, {}, AnalysisCancelToken{});
    EXPECT_EQ(result.state, AnalysisState::Unavailable);
    EXPECT_EQ(result.reasonCode, QStringLiteral("analysis_resource_limit"));
}

TEST(HelmAnalysisCaptureTest, HeaderMetadataMustFitConfiguredCaptureLimit)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    AnalysisSessionSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.captureFilePath = directory.filePath(QStringLiteral("too-small.bin"));
    spec.maxCaptureBytes = 64;
    spec.maxInputSamples = 1;
    spec.maxAnalysisDurationUs = 1000000;
    spec.effectiveRunParameters = {{QStringLiteral("waveform"), 3},
                                   {QStringLiteral("freq"), 1.0},
                                   {QStringLiteral("ampl"), 1.0},
                                   {QStringLiteral("offset"), 0.0},
                                   {QStringLiteral("start"), 0.0},
                                   {QStringLiteral("max_freq"), 1.0},
                                   {QStringLiteral("sweep_duration_s"), 1.0},
                                   {QStringLiteral("enable"), 1}};
    QString error;
    auto session = createPostRunAnalysisSession(spec, &error);
    ASSERT_NE(session, nullptr) << error.toStdString();
    const AnalysisError bound = session->bindIdentity(
        AnalysisIdentity{QStringLiteral("small-capture"), 1});
    EXPECT_FALSE(bound.ok());
    EXPECT_EQ(bound.code, QStringLiteral("analysis_resource_limit"));
}

TEST(HelmAnalysisCaptureTest, AnalyzerRejectsSealThatDoesNotMatchCapturedIdentityAndCount)
{
    const auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("constant"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    AnalysisSessionSpec spec = test::makeHelmPerformanceSpec(
        fixture, directory.filePath(QStringLiteral("sealed.bin")));
    QString error;
    auto session = createPostRunAnalysisSession(spec, &error);
    ASSERT_NE(session, nullptr) << error.toStdString();
    const AnalysisIdentity identity{QStringLiteral("sealed-task"), 9};
    ASSERT_TRUE(session->bindIdentity(identity, &error)) << error.toStdString();
    for (const PostRunSample& sample : fixture.samples) {
        ASSERT_TRUE(session->append(identity, sample).accepted);
    }
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Stopped;
    AnalysisInputSeal seal = session->seal(termination);
    ASSERT_TRUE(seal.valid);
    ++seal.acceptedSampleCount;
    auto analyzer = createPostRunAnalyzer(spec.algorithmId, &error);
    ASSERT_NE(analyzer, nullptr) << error.toStdString();
    const AnalysisResult result = analyzer->analyze(seal, {}, AnalysisCancelToken{});
    EXPECT_EQ(result.state, AnalysisState::Failed);
    EXPECT_EQ(result.reasonCode, QStringLiteral("analysis_capture_format"));
}

} // namespace
} // namespace hwtest::algorithm::mbddf
