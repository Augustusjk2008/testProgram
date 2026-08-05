#pragma once

#include <algorithm/post_run_analysis.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <memory>

namespace hwtest::algorithm::mbddf::test {

struct HelmPerformanceFixture {
    QString name;
    QVariantMap parameters;
    QVector<PostRunSample> samples;
    QVariantMap expected;
};

inline QString helmPerformanceFixturePath(const QString& name)
{
    return QStringLiteral(HWTEST_HELM_PERFORMANCE_FIXTURE_DIR) +
           QLatin1Char('/') + name + QStringLiteral(".json");
}

inline HelmPerformanceFixture loadHelmPerformanceFixture(const QString& name)
{
    QFile file(helmPerformanceFixturePath(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};

    const QJsonObject root = document.object();
    HelmPerformanceFixture fixture;
    fixture.name = root.value(QStringLiteral("name")).toString();
    fixture.parameters = root.value(QStringLiteral("parameters")).toObject().toVariantMap();
    fixture.expected = root.value(QStringLiteral("expected")).toObject().toVariantMap();
    const QJsonArray samples = root.value(QStringLiteral("samples")).toArray();
    fixture.samples.reserve(samples.size());
    for (const QJsonValue& value : samples) {
        const QJsonObject object = value.toObject();
        PostRunSample sample;
        sample.sourceId = QStringLiteral("HELM_STREAM");
        sample.streamElapsedUs = object.value(QStringLiteral("tUs")).toVariant().toLongLong();
        const QJsonArray command = object.value(QStringLiteral("command")).toArray();
        const QJsonArray feedback = object.value(QStringLiteral("feedback")).toArray();
        for (int channel = 0; channel < 4; ++channel) {
            sample.values.insert(QStringLiteral("ins[%1]").arg(channel),
                                 command.at(channel).toDouble());
            sample.values.insert(QStringLiteral("fdb[%1]").arg(channel),
                                 feedback.at(channel).toDouble());
        }
        sample.values.insert(QStringLiteral("status"),
                             object.value(QStringLiteral("status")).toInt());
        sample.values.insert(QStringLiteral("err_code"),
                             object.value(QStringLiteral("errCode")).toInt());
        sample.values.insert(QStringLiteral("timeout"),
                             object.value(QStringLiteral("timeout")).toInt());
        sample.values.insert(QStringLiteral("product_frame_sequence"),
                             object.value(QStringLiteral("productSequence")).toInt());
        sample.values.insert(QStringLiteral("serial_a"),
                             object.value(QStringLiteral("serialA")).toInt());
        sample.values.insert(QStringLiteral("serial_b"),
                             object.value(QStringLiteral("serialB")).toInt());
        sample.values.insert(QStringLiteral("dds_timestamp_us"),
                             object.value(QStringLiteral("ddsSequence")).toVariant());
        sample.values.insert(QStringLiteral("batch_sample_index"),
                             object.value(QStringLiteral("batchIndex")).toInt());
        fixture.samples.push_back(sample);
    }
    return fixture;
}

inline AnalysisSessionSpec makeHelmPerformanceSpec(const HelmPerformanceFixture& fixture,
                                                   const QString& captureFilePath)
{
    AnalysisSessionSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("fixture-config");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.captureFilePath = captureFilePath;
    spec.effectiveRunParameters = fixture.parameters;
    spec.maxCaptureBytes = 16 * 1024 * 1024;
    spec.maxInputSamples = 100000;
    spec.maxAnalysisDurationUs = 60000000;
    return spec;
}

inline AnalysisResult analyzeHelmFixture(const HelmPerformanceFixture& fixture,
                                         QTemporaryDir* directory,
                                         QString* error = nullptr)
{
    if (directory == nullptr || !directory->isValid()) {
        if (error != nullptr) *error = QStringLiteral("temporary directory is unavailable");
        return {};
    }
    AnalysisSessionSpec spec = makeHelmPerformanceSpec(
        fixture, directory->filePath(QStringLiteral("capture.bin")));
    std::unique_ptr<IPostRunAnalysisSession> session =
        createPostRunAnalysisSession(spec, error);
    if (session == nullptr) return {};

    AnalysisIdentity identity;
    identity.taskId = QStringLiteral("fixture-task-%1").arg(fixture.name);
    identity.analysisGeneration = 1;
    if (!session->bindIdentity(identity, error)) return {};
    for (const PostRunSample& sample : fixture.samples) {
        const AnalysisAcceptResult appended = session->append(identity, sample);
        if (!appended.accepted && error != nullptr) {
            *error = appended.message;
            return {};
        }
    }
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Stopped;
    const AnalysisInputSeal seal = session->seal(termination);
    if (!seal.valid) {
        if (error != nullptr) *error = seal.message;
        return {};
    }
    std::unique_ptr<IPostRunAnalyzer> analyzer =
        createPostRunAnalyzer(spec.algorithmId, error);
    if (analyzer == nullptr) return {};
    return analyzer->analyze(seal, {}, AnalysisCancelToken{});
}

inline const AnalysisChannelResult* findChannel(const AnalysisResult& result, int index)
{
    for (const AnalysisChannelResult& channel : result.channels) {
        if (channel.channel == index) return &channel;
    }
    return nullptr;
}

inline const AnalysisMetric* findMetric(const QVector<AnalysisMetric>& metrics,
                                        const QString& key)
{
    for (const AnalysisMetric& metric : metrics) {
        if (metric.key == key) return &metric;
    }
    return nullptr;
}

inline double requiredMetricValue(const QVector<AnalysisMetric>& metrics,
                                  const QString& key)
{
    const AnalysisMetric* metric = findMetric(metrics, key);
    return metric != nullptr && metric->hasValue ? metric->value : 0.0;
}

} // namespace hwtest::algorithm::mbddf::test
