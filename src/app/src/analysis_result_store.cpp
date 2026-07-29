#include "analysis_result_store.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryFile>

namespace hwtest::app {
namespace {

ActionResult storageFailure(const QString& message)
{
    return ActionResult{false, QStringLiteral("analysis_storage"), message};
}

} // namespace

AnalysisResultStore::AnalysisResultStore() = default;

AnalysisResultStore::~AnalysisResultStore()
{
    cancelPending();
}

ActionResult AnalysisResultStore::closePendingForSession()
{
    if (m_pendingCapture == nullptr || m_pendingCapture->fileName().isEmpty()) {
        return storageFailure(QStringLiteral("No pending analysis input is available"));
    }
    if (m_pendingCapture->isOpen()) {
        if (!m_pendingCapture->flush()) {
            return storageFailure(
                QStringLiteral("Cannot flush pending analysis input: %1")
                    .arg(m_pendingCapture->errorString()));
        }
        m_pendingCapture->close();
    }
    return {};
}

QString AnalysisResultStore::retainPendingCapture()
{
    if (m_pendingCapture == nullptr) return {};
    if (m_pendingCapture->isOpen()) {
        (void)m_pendingCapture->flush();
        m_pendingCapture->close();
    }
    const QString path = m_pendingCapture->fileName();
    m_pendingCapture->setAutoRemove(false);
    m_pendingCapture.reset();
    return path;
}

ActionResult AnalysisResultStore::preparePending(
    const QString& dataStorageDirectory,
    const PostRunAnalysisConfig& config)
{
    cancelPending();
    const QFileInfo baseInfo(dataStorageDirectory);
    if (baseInfo.exists() && !baseInfo.isDir()) {
        return storageFailure(
            QStringLiteral("Analysis data directory is not a directory: %1")
                .arg(dataStorageDirectory));
    }

    const QString directory = QDir(dataStorageDirectory).filePath(
        QStringLiteral("analysis"));
    if (!QDir().mkpath(directory) || !QFileInfo(directory).isDir()) {
        return storageFailure(
            QStringLiteral("Cannot create analysis directory: %1").arg(directory));
    }
    const QDateTime retentionCutoff =
        QDateTime::currentDateTimeUtc().addDays(-config.diagnosticRetentionDays);
    const QFileInfoList orphanCaptures = QDir(directory).entryInfoList(
        {QStringLiteral(".capture-pending-*.bin")},
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo& orphan : orphanCaptures) {
        if (orphan.lastModified().toUTC() < retentionCutoff) {
            (void)QFile::remove(orphan.absoluteFilePath());
        }
    }
    QStorageInfo storage(directory);
    storage.refresh();
    if (!storage.isValid() || !storage.isReady()) {
        return storageFailure(
            QStringLiteral("Cannot inspect free space for analysis directory: %1")
                .arg(directory));
    }
    if (storage.bytesAvailable() < static_cast<qint64>(config.minFreeBytes)) {
        return storageFailure(
            QStringLiteral("Analysis directory has insufficient free space"));
    }

    auto pending = std::make_unique<QTemporaryFile>(
        QDir(directory).filePath(QStringLiteral(".capture-pending-XXXXXX.bin")));
    pending->setAutoRemove(true);
    if (!pending->open()) {
        return storageFailure(
            QStringLiteral("Cannot create pending analysis input in '%1': %2")
                .arg(directory, pending->errorString()));
    }
    m_analysisDirectory = directory;
    m_pendingCapture = std::move(pending);
    return {};
}

void AnalysisResultStore::cancelPending()
{
    if (m_pendingCapture != nullptr) {
        const QString path = m_pendingCapture->fileName();
        m_pendingCapture->close();
        m_pendingCapture.reset();
        if (!path.isEmpty()) QFile::remove(path);
    }
}

QString AnalysisResultStore::pendingCapturePath() const
{
    return m_pendingCapture == nullptr ? QString{} : m_pendingCapture->fileName();
}

QString AnalysisResultStore::analysisDirectory() const
{
    return m_analysisDirectory;
}

ActionResult AnalysisResultStore::commitResult(
    const QString& analysisDirectory,
    const QString& taskId,
    quint64 analysisGeneration,
    const QByteArray& utf8,
    quint64 maxResultBytes,
    QString* resultPath) const
{
    if (resultPath != nullptr) resultPath->clear();
    if (taskId.trimmed().isEmpty() || analysisGeneration == 0) {
        return ActionResult{false,
                            QStringLiteral("analysis_identity"),
                            QStringLiteral("A valid analysis identity is required")};
    }
    if (utf8.size() < 0 || static_cast<quint64>(utf8.size()) > maxResultBytes) {
        return ActionResult{
            false,
            QStringLiteral("analysis_result_limit"),
            QStringLiteral("Serialized analysis result exceeds the configured limit")};
    }
    if (!QDir().mkpath(analysisDirectory) ||
        !QFileInfo(analysisDirectory).isDir()) {
        return storageFailure(
            QStringLiteral("Cannot create analysis result directory: %1")
                .arg(analysisDirectory));
    }

    const QString path = resultPathForIdentity(
        analysisDirectory, taskId, analysisGeneration);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        return storageFailure(
            QStringLiteral("Cannot open analysis result '%1': %2")
                .arg(path, output.errorString()));
    }
    if (output.write(utf8) != utf8.size()) {
        output.cancelWriting();
        return storageFailure(
            QStringLiteral("Cannot write analysis result '%1': %2")
                .arg(path, output.errorString()));
    }
    if (!output.commit()) {
        return storageFailure(
            QStringLiteral("Cannot commit analysis result '%1': %2")
                .arg(path, output.errorString()));
    }
    if (resultPath != nullptr) *resultPath = path;
    return {};
}

QString AnalysisResultStore::resultPathForIdentity(
    const QString& analysisDirectory,
    const QString& taskId,
    quint64 analysisGeneration)
{
    QString safeTaskId;
    safeTaskId.reserve(taskId.size());
    for (const QChar character : taskId) {
        safeTaskId.append(character.isLetterOrNumber() ||
                                  character == QLatin1Char('-') ||
                                  character == QLatin1Char('_')
                              ? character
                              : QLatin1Char('_'));
    }
    return QDir(analysisDirectory).filePath(
        QStringLiteral("analysis-%1-%2.json")
            .arg(safeTaskId, QString::number(analysisGeneration)));
}

QByteArray AnalysisResultStore::sha256File(
    const QString& path,
    QString* error,
    const std::function<bool()>& cancellationRequested)
{
    if (error != nullptr) error->clear();
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = input.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!input.atEnd()) {
        if (cancellationRequested && cancellationRequested()) {
            if (error != nullptr) *error = QStringLiteral("analysis_cancelled");
            return {};
        }
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty() && !input.atEnd()) {
            if (error != nullptr) *error = input.errorString();
            return {};
        }
        hash.addData(chunk);
    }
    if (cancellationRequested && cancellationRequested()) {
        if (error != nullptr) *error = QStringLiteral("analysis_cancelled");
        return {};
    }
    return hash.result();
}

} // namespace hwtest::app
