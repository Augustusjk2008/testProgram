#pragma once

#include "post_run_analysis_config.h"

#include <app/test_application_controller.h>

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>

class QTemporaryFile;

namespace hwtest::app {

class AnalysisResultStore final {
public:
    AnalysisResultStore();
    ~AnalysisResultStore();

    ActionResult preparePending(const QString& dataStorageDirectory,
                                const PostRunAnalysisConfig& config);
    ActionResult closePendingForSession();
    QString retainPendingCapture();
    void cancelPending();
    QString pendingCapturePath() const;
    QString analysisDirectory() const;
    ActionResult commitResult(const QString& analysisDirectory,
                              const QString& taskId,
                              quint64 analysisGeneration,
                              const QByteArray& utf8,
                              quint64 maxResultBytes,
                              QString* resultPath) const;
    static QString resultPathForIdentity(const QString& analysisDirectory,
                                         const QString& taskId,
                                         quint64 analysisGeneration);
    static QByteArray sha256File(
        const QString& path,
        QString* error,
        const std::function<bool()>& cancellationRequested = {});

private:
    QString m_analysisDirectory;
    std::unique_ptr<QTemporaryFile> m_pendingCapture;
};

} // namespace hwtest::app
