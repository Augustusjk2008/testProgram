#pragma once

#include "post_run_analysis_config.h"

#include <algorithm/post_run_analysis.h>
#include <app/test_application_controller.h>

#include <QObject>

#include <functional>
#include <memory>

namespace hwtest::app {

struct PostRunAnalysisStartSpec {
    QString algorithmId;
    QString configId;
    QString sourceStepId;
    QString dataStorageDirectory;
    QVariantMap effectiveRunParameters;
    QVariantMap metadata;
    PostRunAnalysisConfig resources;
};

struct PostRunAnalysisDependencies {
    std::function<std::unique_ptr<hwtest::algorithm::mbddf::IPostRunAnalysisSession>(
        const hwtest::algorithm::mbddf::AnalysisSessionSpec&,
        hwtest::algorithm::mbddf::AnalysisError*)> sessionFactory;
    std::function<std::unique_ptr<hwtest::algorithm::mbddf::IPostRunAnalyzer>(
        const QString&,
        hwtest::algorithm::mbddf::AnalysisError*)> analyzerFactory;
};

class PostRunAnalysisCoordinator final : public QObject {
public:
    explicit PostRunAnalysisCoordinator(
        PostRunAnalysisDependencies dependencies = {},
        QObject* parent = nullptr);
    ~PostRunAnalysisCoordinator() override;

    void configureCapability(const PostRunAnalysisCapability& capability);
    ActionResult preparePending(const PostRunAnalysisStartSpec& spec);
    void discardPrepared();
    ActionResult bindSuccessfulTask(const QString& taskId);
    void append(const ApplicationSample& sample);
    void requestTerminal(
        const hwtest::algorithm::mbddf::AnalysisTermination& termination,
        bool waitForStopCompletion,
        const QString& sourceArtifactPath);
    void notifyStopCompleted();
    ActionResult cancelAndWait(int timeoutMs);

    bool blocksWrites() const;
    PostRunAnalysisSnapshot snapshot() const;
    QVector<AnalysisChannelProjection> projections() const;
    void setUpdateCallback(std::function<void()> callback);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    void publishUpdate();
    void sealPendingInput();
    void queueWorkerIfReady();
    void startWorker();
};

} // namespace hwtest::app
