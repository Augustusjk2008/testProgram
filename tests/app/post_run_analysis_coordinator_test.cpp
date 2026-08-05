#include "post_run_analysis_coordinator.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <limits>
#include <memory>

namespace hwtest::app {
namespace {

using namespace hwtest::algorithm::mbddf;

QCoreApplication& ensureQtApplication()
{
    if (QCoreApplication* existing = QCoreApplication::instance()) {
        return *existing;
    }
    static int argc = 1;
    static char argument[] = "post_run_analysis_coordinator_test";
    static char* argv[] = {argument, nullptr};
    static QCoreApplication application(argc, argv);
    return application;
}

struct AnalysisProbe {
    std::atomic<quint64> sealedSampleCount{0};
    std::atomic<quint64> lateSampleCount{0};
    std::atomic_bool analyzerStarted{false};
};

class FakeSession final : public IPostRunAnalysisSession {
public:
    FakeSession(AnalysisSessionSpec spec, std::shared_ptr<AnalysisProbe> probe)
        : m_spec(std::move(spec)), m_probe(std::move(probe))
    {
    }

    AnalysisError bindIdentity(const AnalysisIdentity& identity) override
    {
        m_identity = identity;
        return {};
    }

    AnalysisAcceptResult append(const AnalysisIdentity& identity,
                                const PostRunSample&) override
    {
        AnalysisAcceptResult result;
        if (identity.taskId != m_identity.taskId ||
            identity.analysisGeneration != m_identity.analysisGeneration) {
            result.disposition = AnalysisAppendDisposition::IgnoredIdentity;
            return result;
        }
        if (m_sealed) {
            result.disposition = AnalysisAppendDisposition::Late;
            result.late = true;
            result.lateSampleCount = ++m_lateCount;
            m_probe->lateSampleCount = m_lateCount;
            return result;
        }
        result.disposition = AnalysisAppendDisposition::Accepted;
        result.accepted = true;
        result.acceptedSampleCount = ++m_acceptedCount;
        return result;
    }

    AnalysisInputSeal seal(const AnalysisTermination& termination) override
    {
        m_sealed = true;
        m_probe->sealedSampleCount = m_acceptedCount;
        AnalysisInputSeal seal;
        seal.valid = true;
        seal.identity = m_identity;
        seal.algorithmId = m_spec.algorithmId;
        seal.configId = m_spec.configId;
        seal.schemaVersion = m_spec.schemaVersion;
        seal.captureFilePath = m_spec.captureFilePath;
        seal.acceptedSampleCount = m_acceptedCount;
        seal.termination = termination;
        return seal;
    }

private:
    AnalysisSessionSpec m_spec;
    std::shared_ptr<AnalysisProbe> m_probe;
    AnalysisIdentity m_identity;
    quint64 m_acceptedCount = 0;
    quint64 m_lateCount = 0;
    bool m_sealed = false;
};

class FakeAnalyzer final : public IPostRunAnalyzer {
public:
    explicit FakeAnalyzer(std::shared_ptr<AnalysisProbe> probe,
                          QString analyzerVersion = QStringLiteral("test"))
        : m_probe(std::move(probe))
        , m_analyzerVersion(std::move(analyzerVersion))
    {
    }

    AnalysisResult analyze(const AnalysisInputSeal& input,
                           const AnalysisProgressCallback&,
                           const AnalysisCancelToken&) override
    {
        m_probe->analyzerStarted = true;
        AnalysisResult result;
        result.schemaVersion = QStringLiteral("1");
        result.analyzerId = QStringLiteral("mbddf.helm.performance");
        result.analyzerVersion = m_analyzerVersion;
        result.identity = input.identity;
        result.state = AnalysisState::Completed;
        result.acceptedSampleCount = input.acceptedSampleCount;
        for (int channel = 0; channel < 4; ++channel) {
            AnalysisChannelResult channelResult;
            channelResult.channel = channel;
            channelResult.enabled = channel == 0;
            channelResult.state = channel == 0
                ? AnalysisChannelState::Completed
                : AnalysisChannelState::NotApplicable;
            result.channels.push_back(channelResult);
        }
        return result;
    }

private:
    std::shared_ptr<AnalysisProbe> m_probe;
    QString m_analyzerVersion;
};

hwtest::algorithm::mbddf::AnalysisMetric publishedMetric(const QString& key,
                                                          int channel)
{
    hwtest::algorithm::mbddf::AnalysisMetric metric;
    metric.key = key;
    metric.label = QStringLiteral("Published %1 for helm channel %2").arg(key).arg(channel);
    metric.unit = QStringLiteral("degree");
    metric.status = AnalysisMetricStatus::Valid;
    metric.hasValue = true;
    metric.value = static_cast<double>(channel + 1);
    metric.detail = QStringLiteral("First complete command cycle");
    return metric;
}

class FullSquareMetricsAnalyzer final : public IPostRunAnalyzer {
public:
    AnalysisResult analyze(const AnalysisInputSeal& input,
                           const AnalysisProgressCallback&,
                           const AnalysisCancelToken&) override
    {
        static const QStringList commonKeys{
            QStringLiteral("raw_sample_count"),
            QStringLiteral("raw_duration_s"),
            QStringLiteral("analysis_sample_count"),
            QStringLiteral("analysis_duration_s"),
            QStringLiteral("sampling_frequency_hz"),
            QStringLiteral("command_peak"),
            QStringLiteral("command_range"),
            QStringLiteral("command_rms"),
            QStringLiteral("feedback_peak"),
            QStringLiteral("feedback_range"),
            QStringLiteral("feedback_rms"),
            QStringLiteral("mean_error"),
            QStringLiteral("mae"),
            QStringLiteral("rmse"),
            QStringLiteral("max_abs_error"),
            QStringLiteral("correlation_coefficient"),
        };
        static const QStringList edgeSuffixes{
            QStringLiteral("edge_count"),
            QStringLiteral("not_settled_edge_count"),
            QStringLiteral("delay_ms_mean"),
            QStringLiteral("delay_ms_mean_worst"),
            QStringLiteral("rise_time_ms_mean"),
            QStringLiteral("rise_time_ms_mean_worst"),
            QStringLiteral("overshoot_percent_mean"),
            QStringLiteral("overshoot_percent_mean_worst"),
            QStringLiteral("settling_time_ms_mean"),
            QStringLiteral("settling_time_ms_mean_worst"),
            QStringLiteral("steady_state_error_mean"),
        };

        AnalysisResult result;
        result.schemaVersion = QStringLiteral("1");
        result.analyzerId = QStringLiteral("mbddf.helm.performance");
        result.analyzerVersion = QStringLiteral("full-square-test");
        result.identity = input.identity;
        result.state = AnalysisState::Completed;
        result.acceptedSampleCount = input.acceptedSampleCount;
        for (int channelIndex = 0; channelIndex < 4; ++channelIndex) {
            AnalysisChannelResult channel;
            channel.channel = channelIndex;
            channel.enabled = true;
            channel.state = AnalysisChannelState::Completed;
            for (const QString& key : commonKeys) {
                channel.commonMetrics.push_back(publishedMetric(key, channelIndex));
            }
            for (const QString& prefix : {QStringLiteral("rising"),
                                          QStringLiteral("falling")}) {
                for (const QString& suffix : edgeSuffixes) {
                    channel.waveformMetrics.push_back(
                        publishedMetric(prefix + QLatin1Char('_') + suffix, channelIndex));
                }
            }
            result.channels.push_back(channel);
        }
        return result;
    }
};

class DenseBodeAnalyzer final : public IPostRunAnalyzer {
public:
    explicit DenseBodeAnalyzer(bool includeHiddenInvalidPoint = false)
        : m_includeHiddenInvalidPoint(includeHiddenInvalidPoint)
    {
    }

    AnalysisResult analyze(const AnalysisInputSeal& input,
                           const AnalysisProgressCallback&,
                           const AnalysisCancelToken&) override
    {
        AnalysisResult result;
        result.schemaVersion = QStringLiteral("1");
        result.analyzerId = QStringLiteral("mbddf.helm.performance");
        result.analyzerVersion = QStringLiteral("test");
        result.identity = input.identity;
        result.state = AnalysisState::Completed;
        AnalysisChannelResult channel;
        channel.channel = 0;
        channel.enabled = true;
        channel.state = AnalysisChannelState::Completed;
        hwtest::algorithm::mbddf::AnalysisMetric resonance;
        resonance.key = QStringLiteral("resonance_frequency_hz");
        resonance.status = AnalysisMetricStatus::Valid;
        resonance.hasValue = true;
        resonance.value = 7.5;
        channel.waveformMetrics.push_back(resonance);
        for (int frequency = 1; frequency <= 12; ++frequency) {
            AnalysisBodePoint point;
            point.frequencyHz = frequency;
            point.hasMagnitude = true;
            point.magnitudeDb = m_includeHiddenInvalidPoint && frequency == 6
                ? std::numeric_limits<double>::infinity()
                : -frequency;
            point.hasPhase = true;
            point.phaseDeg = -2.0 * frequency;
            point.status = AnalysisPointStatus::Valid;
            channel.bodePoints.push_back(point);
        }
        result.channels.push_back(channel);
        return result;
    }

private:
    bool m_includeHiddenInvalidPoint = false;
};

class BlockingAnalyzer final : public IPostRunAnalyzer {
public:
    BlockingAnalyzer(QSemaphore* started, QSemaphore* release)
        : m_started(started), m_release(release)
    {
    }

    AnalysisResult analyze(const AnalysisInputSeal& input,
                           const AnalysisProgressCallback&,
                           const AnalysisCancelToken&) override
    {
        m_started->release();
        m_release->acquire();
        AnalysisResult result;
        result.identity = input.identity;
        result.state = AnalysisState::Completed;
        return result;
    }

private:
    QSemaphore* m_started = nullptr;
    QSemaphore* m_release = nullptr;
};

class CancellationAwareAnalyzer final : public IPostRunAnalyzer {
public:
    AnalysisResult analyze(const AnalysisInputSeal& input,
                           const AnalysisProgressCallback&,
                           const AnalysisCancelToken& cancel) override
    {
        while (!cancel.isCancellationRequested()) QThread::yieldCurrentThread();
        AnalysisResult result;
        result.identity = input.identity;
        result.state = AnalysisState::Cancelled;
        return result;
    }
};

bool isTerminalAnalysisState(const QString& state)
{
    return state == QStringLiteral("completed") ||
        state == QStringLiteral("partial") ||
        state == QStringLiteral("unavailable") ||
        state == QStringLiteral("failed") ||
        state == QStringLiteral("cancelled");
}

TEST(PostRunAnalysisCoordinatorTest, WaitsForStopCompletionAfterSealingQueuedTail)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [probe](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeAnalyzer>(probe);
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-1")).ok);

    ApplicationSample first;
    first.taskId = QStringLiteral("task-1");
    first.stepId = QStringLiteral("HELM_STREAM");
    first.streamElapsedUs = 1000;
    coordinator.append(first);
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Stopped;
    coordinator.requestTerminal(termination, true, {});
    ApplicationSample tail = first;
    tail.streamElapsedUs = 2000;
    coordinator.append(tail);

    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(probe->sealedSampleCount.load(), 2u);
    EXPECT_FALSE(probe->analyzerStarted.load());
    EXPECT_EQ(coordinator.snapshot().state, QStringLiteral("queued"));

    QEventLoop loop;
    QTimer timeout;
    QStringList observedStates;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.setUpdateCallback([&] {
        observedStates.push_back(coordinator.snapshot().state);
        if (isTerminalAnalysisState(coordinator.snapshot().state)) loop.quit();
    });
    coordinator.notifyStopCompleted();
    timeout.start(5000);
    loop.exec();

    EXPECT_TRUE(probe->analyzerStarted.load());
    EXPECT_TRUE(observedStates.contains(QStringLiteral("persisting")));
    EXPECT_EQ(coordinator.snapshot().state, QStringLiteral("completed"));
    EXPECT_FALSE(coordinator.snapshot().resultFilePath.isEmpty());
}

TEST(PostRunAnalysisCoordinatorTest, CountsAppendAfterSealAsLateWithoutChangingInput)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [probe](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeAnalyzer>(probe);
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-2")).ok);
    ApplicationSample sample;
    sample.taskId = QStringLiteral("task-2");
    sample.stepId = QStringLiteral("HELM_STREAM");
    sample.streamElapsedUs = 1000;
    coordinator.append(sample);
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Stopped;
    coordinator.requestTerminal(termination, true, {});
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    sample.streamElapsedUs = 2000;
    coordinator.append(sample);

    EXPECT_EQ(probe->sealedSampleCount.load(), 1u);
    EXPECT_EQ(probe->lateSampleCount.load(), 1u);
}

TEST(PostRunAnalysisCoordinatorTest, RetainsWorkerWhenCancellationJoinTimesOut)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    QSemaphore started;
    QSemaphore release;
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [&](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<BlockingAnalyzer>(&started, &release);
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-blocked")).ok);
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Finished;
    coordinator.requestTerminal(termination, false, {});
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_TRUE(started.tryAcquire(1, 5000));

    const ActionResult timedOut = coordinator.cancelAndWait(1);

    EXPECT_FALSE(timedOut.ok);
    EXPECT_EQ(timedOut.code, QStringLiteral("analysis_shutdown_timeout"));
    EXPECT_TRUE(coordinator.blocksWrites());

    release.release();
    const ActionResult joined = coordinator.cancelAndWait(5000);
    EXPECT_TRUE(joined.ok) << joined.message.toStdString();
    EXPECT_EQ(coordinator.snapshot().state, QStringLiteral("cancelled"));
}

TEST(PostRunAnalysisCoordinatorTest, ConvertsConfiguredAnalysisTimeoutToFailure)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<CancellationAwareAnalyzer>();
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    spec.resources.analysisTimeoutMs = 1;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-timeout")).ok);
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.setUpdateCallback([&] {
        if (isTerminalAnalysisState(coordinator.snapshot().state)) loop.quit();
    });
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Finished;
    coordinator.requestTerminal(termination, false, {});
    guard.start(5000);
    loop.exec();

    EXPECT_EQ(coordinator.snapshot().state, QStringLiteral("failed"));
    EXPECT_EQ(coordinator.snapshot().reasonCode,
              QStringLiteral("analysis_timeout"));
}

TEST(PostRunAnalysisCoordinatorTest, LinksCompletedSourceArtifactBySha256)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("samples.txt"));
    QFile source(sourcePath);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write("abc"), 3);
    source.close();
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [probe](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeAnalyzer>(probe);
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-source")).ok);
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.setUpdateCallback([&] {
        if (isTerminalAnalysisState(coordinator.snapshot().state)) loop.quit();
    });
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Finished;
    coordinator.requestTerminal(termination, false, sourcePath);
    guard.start(5000);
    loop.exec();

    const QVariantMap summary = coordinator.snapshot().sourceSummary;
    EXPECT_TRUE(summary.value(QStringLiteral("reproducible")).toBool());
    EXPECT_EQ(summary.value(QStringLiteral("sourceArtifactPath")).toString(),
              sourcePath);
    EXPECT_EQ(summary.value(QStringLiteral("sourceArtifactSha256")).toString(),
              QStringLiteral("ba7816bf8f01cfea414140de5dae2223"
                             "b00361a396177a9cb410ff61f20015ad"));
}

TEST(PostRunAnalysisCoordinatorTest, RejectsProjectionAboveConfiguredByteLimit)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [probe](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeAnalyzer>(probe);
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    spec.resources.maxProjectedBytes = 32;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-projection")).ok);
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.setUpdateCallback([&] {
        if (isTerminalAnalysisState(coordinator.snapshot().state)) loop.quit();
    });
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Finished;
    coordinator.requestTerminal(termination, false, {});
    guard.start(5000);
    loop.exec();

    EXPECT_EQ(coordinator.snapshot().state, QStringLiteral("failed"));
    EXPECT_EQ(coordinator.snapshot().reasonCode,
              QStringLiteral("analysis_projection_limit"));
    EXPECT_TRUE(coordinator.snapshot().resultFilePath.isEmpty());
    EXPECT_FALSE(coordinator.snapshot().diagnosticInputFilePath.isEmpty());
}

TEST(PostRunAnalysisCoordinatorTest, DeterministicallySamplesBodeProjectionAndKeepsAnchors)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<DenseBodeAnalyzer>();
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    spec.resources.maxProjectedPoints = 8;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-sample")).ok);
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.setUpdateCallback([&] {
        if (isTerminalAnalysisState(coordinator.snapshot().state)) loop.quit();
    });
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Finished;
    coordinator.requestTerminal(termination, false, {});
    guard.start(5000);
    loop.exec();

    ASSERT_EQ(coordinator.snapshot().state, QStringLiteral("completed"));
    const QVector<AnalysisChannelProjection> projections = coordinator.projections();
    ASSERT_EQ(projections.size(), 1);
    const QVector<double> frequencies = projections.first().frequencyHz;
    ASSERT_EQ(frequencies.size(), 8);
    EXPECT_EQ(frequencies.first(), 1.0);
    EXPECT_EQ(frequencies.last(), 12.0);
    EXPECT_TRUE(frequencies.contains(4.0));
    EXPECT_TRUE(frequencies.contains(5.0));
    EXPECT_TRUE(frequencies.contains(7.0));
    EXPECT_TRUE(frequencies.contains(8.0));
    EXPECT_TRUE(frequencies.contains(9.0));
    EXPECT_TRUE(frequencies.contains(10.0));
}

TEST(PostRunAnalysisCoordinatorTest, RejectsInvalidBodePointEvenWhenSamplingWouldOmitIt)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<DenseBodeAnalyzer>(true);
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    spec.resources.maxProjectedPoints = 8;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-invalid")).ok);
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.setUpdateCallback([&] {
        if (isTerminalAnalysisState(coordinator.snapshot().state)) loop.quit();
    });
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Finished;
    coordinator.requestTerminal(termination, false, {});
    guard.start(5000);
    loop.exec();

    EXPECT_EQ(coordinator.snapshot().state, QStringLiteral("failed"));
    EXPECT_EQ(coordinator.snapshot().reasonCode,
              QStringLiteral("analysis_projection_invalid"));
    EXPECT_TRUE(coordinator.snapshot().resultFilePath.isEmpty());
}

TEST(PostRunAnalysisCoordinatorTest, SummaryLimitIncludesPublishedSourceMetadata)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [probe](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeAnalyzer>(
            probe, QString(9000, QLatin1Char('x')));
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    spec.resources.maxAnalysisSummaryBytes = 8192;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-summary")).ok);
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.setUpdateCallback([&] {
        if (isTerminalAnalysisState(coordinator.snapshot().state)) loop.quit();
    });
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Finished;
    coordinator.requestTerminal(termination, false, {});
    guard.start(5000);
    loop.exec();

    EXPECT_EQ(coordinator.snapshot().state, QStringLiteral("failed"));
    EXPECT_EQ(coordinator.snapshot().reasonCode,
              QStringLiteral("analysis_summary_limit"));
    EXPECT_TRUE(coordinator.snapshot().resultFilePath.isEmpty());
}

TEST(PostRunAnalysisCoordinatorTest, PublishesFourChannelFullSquareMetricSummaries)
{
    ensureQtApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto probe = std::make_shared<AnalysisProbe>();
    PostRunAnalysisDependencies dependencies;
    dependencies.sessionFactory = [probe](const AnalysisSessionSpec& spec,
                                          AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FakeSession>(spec, probe);
    };
    dependencies.analyzerFactory = [](const QString&, AnalysisError* error) {
        if (error != nullptr) *error = {};
        return std::make_unique<FullSquareMetricsAnalyzer>();
    };
    PostRunAnalysisCoordinator coordinator(std::move(dependencies));
    coordinator.configureCapability(PostRunAnalysisCapability{
        true, QStringLiteral("mbddf.helm.performance"), QStringLiteral("1")});
    PostRunAnalysisStartSpec spec;
    spec.algorithmId = QStringLiteral("mbddf.helm_stream");
    spec.configId = QStringLiteral("mbddf-helm-stream");
    spec.sourceStepId = QStringLiteral("HELM_STREAM");
    spec.dataStorageDirectory = directory.path();
    spec.resources.minFreeBytes = 0;
    ASSERT_TRUE(coordinator.preparePending(spec).ok);
    ASSERT_TRUE(coordinator.bindSuccessfulTask(QStringLiteral("task-full-square")).ok);
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.setUpdateCallback([&] {
        if (isTerminalAnalysisState(coordinator.snapshot().state)) loop.quit();
    });
    AnalysisTermination termination;
    termination.kind = AnalysisTerminationKind::Finished;
    coordinator.requestTerminal(termination, false, {});
    guard.start(5000);
    loop.exec();

    EXPECT_EQ(coordinator.snapshot().state, QStringLiteral("completed"));
    EXPECT_TRUE(coordinator.snapshot().reasonCode.isEmpty());
    EXPECT_FALSE(coordinator.snapshot().resultFilePath.isEmpty());
    ASSERT_EQ(coordinator.snapshot().channelSummaries.size(), 4);
    for (const AnalysisChannelSummary& channel : coordinator.snapshot().channelSummaries) {
        EXPECT_EQ(channel.commonMetrics.size(), 11);
        EXPECT_EQ(channel.waveformMetrics.size(), 22);
    }
}

} // namespace
} // namespace hwtest::app
