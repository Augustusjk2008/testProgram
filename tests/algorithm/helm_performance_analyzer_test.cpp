#include <gtest/gtest.h>

#include "helm_performance_test_support.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <array>

namespace hwtest::algorithm::mbddf {
namespace {

TEST(HelmPerformanceAnalyzerTest, ReducesChannelStatesAccordingToPublishedTruthTable)
{
    QVector<AnalysisChannelResult> channels(2);
    channels[0].enabled = true;
    channels[1].enabled = true;
    channels[0].state = AnalysisChannelState::Completed;
    channels[1].state = AnalysisChannelState::Completed;
    EXPECT_EQ(reduceAnalysisState(channels), AnalysisState::Completed);
    channels[1].state = AnalysisChannelState::Partial;
    EXPECT_EQ(reduceAnalysisState(channels), AnalysisState::Partial);
    channels[0].state = AnalysisChannelState::Unavailable;
    channels[1].state = AnalysisChannelState::Unavailable;
    EXPECT_EQ(reduceAnalysisState(channels), AnalysisState::Unavailable);
    EXPECT_EQ(reduceAnalysisState(channels, true, false), AnalysisState::Failed);
    EXPECT_EQ(reduceAnalysisState(channels, false, true), AnalysisState::Cancelled);
}

TEST(HelmPerformanceAnalyzerTest, ReducesEveryEnabledChannelStateCombinationDeterministically)
{
    const std::array<AnalysisChannelState, 4> states{{
        AnalysisChannelState::NotApplicable,
        AnalysisChannelState::Completed,
        AnalysisChannelState::Partial,
        AnalysisChannelState::Unavailable,
    }};
    for (quint32 enableMask = 0; enableMask < 16; ++enableMask) {
        for (int combination = 0; combination < 256; ++combination) {
            QVector<AnalysisChannelResult> channels(4);
            int encoded = combination;
            bool anyEnabled = false;
            bool allCompleted = true;
            bool anyPublishable = false;
            for (int channel = 0; channel < 4; ++channel) {
                channels[channel].enabled = (enableMask & (1u << channel)) != 0u;
                channels[channel].state = states[static_cast<size_t>(encoded % 4)];
                encoded /= 4;
                if (!channels[channel].enabled) continue;
                anyEnabled = true;
                allCompleted = allCompleted &&
                    channels[channel].state == AnalysisChannelState::Completed;
                anyPublishable = anyPublishable ||
                    channels[channel].state == AnalysisChannelState::Completed ||
                    channels[channel].state == AnalysisChannelState::Partial;
            }
            const AnalysisState expected = !anyEnabled || !anyPublishable
                ? AnalysisState::Unavailable
                : (allCompleted ? AnalysisState::Completed : AnalysisState::Partial);
            EXPECT_EQ(reduceAnalysisState(channels), expected)
                << "enableMask=" << enableMask << " combination=" << combination;
        }
    }
}

TEST(HelmPerformanceAnalyzerTest, SelfCheckAndNonStrictTimeAreUnavailableRatherThanPass)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("constant"));
    ASSERT_GT(fixture.samples.size(), 3);
    fixture.samples[1].streamElapsedUs = fixture.samples[0].streamElapsedUs;
    fixture.samples[1].values.insert(QStringLiteral("self_check_or"), 1);
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(result.state, AnalysisState::Unavailable);
    EXPECT_EQ(channel->state, AnalysisChannelState::Unavailable);
}

TEST(HelmPerformanceAnalyzerTest, MissingRequiredDeviceDiagnosticIsUnavailableRatherThanSilentZero)
{
    auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("constant"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    fixture.samples.first().values.remove(QStringLiteral("timeout"));
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const auto* channel = test::findChannel(result, 0);
    ASSERT_NE(channel, nullptr) << error.toStdString();
    EXPECT_EQ(channel->state, AnalysisChannelState::Unavailable);
    EXPECT_EQ(channel->reasonCode, QStringLiteral("invalid_input"));
}

TEST(HelmPerformanceAnalyzerTest, SerializesVersionedFiniteResultAndFixtureManifestIsTraceable)
{
    const auto fixture = test::loadHelmPerformanceFixture(QStringLiteral("constant"));
    ASSERT_FALSE(fixture.samples.isEmpty());
    QTemporaryDir directory;
    QString error;
    const AnalysisResult result = test::analyzeHelmFixture(fixture, &directory, &error);
    const QByteArray json = serializeAnalysisResultJson(result, &error);
    ASSERT_FALSE(json.isEmpty()) << error.toStdString();
    const QJsonDocument document = QJsonDocument::fromJson(json);
    ASSERT_TRUE(document.isObject());
    EXPECT_EQ(document.object().value(QStringLiteral("schemaVersion")).toString(),
              QStringLiteral("1"));
    EXPECT_EQ(document.object().value(QStringLiteral("analyzerId")).toString(),
              QStringLiteral("mbddf.helm.performance"));
    EXPECT_TRUE(document.object().contains(QStringLiteral("generatedAtUtcUs")));
    EXPECT_TRUE(document.object().contains(QStringLiteral("reproducible")));
    EXPECT_TRUE(document.object().contains(QStringLiteral("sourceArtifact")));
    EXPECT_EQ(json.indexOf("NaN"), -1);
    EXPECT_EQ(json.indexOf("Infinity"), -1);

    QFile manifest(test::helmPerformanceFixturePath(QStringLiteral("manifest")));
    ASSERT_TRUE(manifest.open(QIODevice::ReadOnly));
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifest.readAll());
    ASSERT_TRUE(manifestDocument.isObject());
    EXPECT_EQ(manifestDocument.object().value(QStringLiteral("analyzerVersion")).toString(),
              document.object().value(QStringLiteral("analyzerVersion")).toString());
    const QJsonObject generator = manifestDocument.object().value(QStringLiteral("generator")).toObject();
    QString generatorPath = test::helmPerformanceFixturePath(QStringLiteral("generate_fixtures"));
    generatorPath.replace(QStringLiteral(".json"), QStringLiteral(".py"));
    QFile generatorFile(generatorPath);
    ASSERT_TRUE(generatorFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(QString::fromLatin1(QCryptographicHash::hash(
                  generatorFile.readAll(), QCryptographicHash::Sha256).toHex()),
              generator.value(QStringLiteral("sha256")).toString());
    const QJsonArray files = manifestDocument.object().value(QStringLiteral("files")).toArray();
    ASSERT_FALSE(files.isEmpty());
    for (const QJsonValue& entry : files) {
        const QJsonObject object = entry.toObject();
        QFile fixtureFile(test::helmPerformanceFixturePath(object.value(QStringLiteral("name")).toString()));
        ASSERT_TRUE(fixtureFile.open(QIODevice::ReadOnly));
        const QByteArray hash = QCryptographicHash::hash(
            fixtureFile.readAll(), QCryptographicHash::Sha256).toHex();
        EXPECT_EQ(QString::fromLatin1(hash), object.value(QStringLiteral("sha256")).toString());
    }
}

TEST(HelmPerformanceAnalyzerTest, CompleteJsonKeepsWarningsButFailureOmitsPartialChannels)
{
    AnalysisResult complete;
    complete.analyzerId = QStringLiteral("mbddf.helm.performance");
    complete.analyzerVersion = QStringLiteral("test");
    complete.identity = {QStringLiteral("json-task"), 1};
    complete.state = AnalysisState::Completed;
    complete.generatedAtUtcUs = 1;
    AnalysisChannelResult channel;
    channel.channel = 0;
    channel.enabled = true;
    channel.state = AnalysisChannelState::Completed;
    for (int index = 0; index < 20; ++index) {
        channel.warnings.push_back(QStringLiteral("warning-%1").arg(index));
    }
    complete.channels.push_back(channel);
    QString error;
    const QJsonDocument completeJson = QJsonDocument::fromJson(
        serializeAnalysisResultJson(complete, &error));
    ASSERT_TRUE(completeJson.isObject()) << error.toStdString();
    EXPECT_EQ(completeJson.object().value(QStringLiteral("channels")).toArray().at(0)
                  .toObject().value(QStringLiteral("warnings")).toArray().size(),
              20);

    complete.state = AnalysisState::Failed;
    const QJsonDocument failedJson = QJsonDocument::fromJson(
        serializeAnalysisResultJson(complete, &error));
    ASSERT_TRUE(failedJson.isObject()) << error.toStdString();
    EXPECT_TRUE(failedJson.object().value(QStringLiteral("channels")).toArray().isEmpty());
}

} // namespace
} // namespace hwtest::algorithm::mbddf
