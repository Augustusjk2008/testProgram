#include <gtest/gtest.h>

#include <algorithm/post_run_analysis.h>

#include <QTemporaryDir>

namespace hwtest::algorithm::mbddf {
namespace {

TEST(PostRunAnalysisContractTest, HelmFactoryCreatesGenericSessionAndAnalyzer)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    AnalysisSessionSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.captureFilePath = directory.filePath(QStringLiteral("capture.bin"));
    spec.effectiveRunParameters = {
        {QStringLiteral("waveform"), 3},
        {QStringLiteral("freq"), 1.0},
        {QStringLiteral("ampl"), 1.0},
        {QStringLiteral("enable"), 1},
    };

    QString error;
    std::unique_ptr<IPostRunAnalysisSession> session =
        createPostRunAnalysisSession(spec, &error);
    EXPECT_NE(session, nullptr) << error.toStdString();

    std::unique_ptr<IPostRunAnalyzer> analyzer =
        createPostRunAnalyzer(spec.algorithmId, &error);
    EXPECT_NE(analyzer, nullptr) << error.toStdString();
    std::unique_ptr<IPostRunAnalyzer> analyzerWithoutDetails =
        createPostRunAnalyzer(spec.algorithmId, nullptr);
    EXPECT_NE(analyzerWithoutDetails, nullptr);
    EXPECT_TRUE(supportsPostRunAnalysis(spec.algorithmId));
    EXPECT_FALSE(supportsPostRunAnalysis(QStringLiteral("mbddf.unknown")));
}

TEST(PostRunAnalysisContractTest, CancellationTokenSharesAWorkerSafeRequest)
{
    AnalysisCancelToken token;
    AnalysisCancelToken workerCopy = token;
    EXPECT_FALSE(token.isCancellationRequested());
    workerCopy.requestCancel();
    EXPECT_TRUE(token.isCancellationRequested());
}

} // namespace
} // namespace hwtest::algorithm::mbddf
