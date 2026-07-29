#include "analysis_result_store.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace hwtest::app {
namespace {

TEST(AnalysisResultStoreTest, ClosesPendingHandleForTheCaptureSession)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    PostRunAnalysisConfig config;
    config.minFreeBytes = 0;
    AnalysisResultStore store;
    ASSERT_TRUE(store.preparePending(directory.path(), config).ok);
    const QString path = store.pendingCapturePath();
    ASSERT_FALSE(path.isEmpty());

    EXPECT_TRUE(store.closePendingForSession().ok);
    QFile capture(path);
    ASSERT_TRUE(capture.open(QIODevice::WriteOnly | QIODevice::Truncate));
    EXPECT_EQ(capture.write("capture"), 7);
    capture.close();

    store.cancelPending();
    EXPECT_FALSE(QFile::exists(path));
}

TEST(AnalysisResultStoreTest, CommitsOnlyResultsWithinTheConfiguredLimit)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    AnalysisResultStore store;
    QString resultPath;

    const ActionResult oversized = store.commitResult(
        directory.path(), QStringLiteral("task-1"), 7,
        QByteArray("12345"), 4, &resultPath);

    EXPECT_FALSE(oversized.ok);
    EXPECT_EQ(oversized.code, QStringLiteral("analysis_result_limit"));
    EXPECT_TRUE(resultPath.isEmpty());
    EXPECT_TRUE(QDir(directory.path()).entryList(
                    {QStringLiteral("*.json")}, QDir::Files).isEmpty());

    const QByteArray payload("{\"schemaVersion\":\"1\"}");
    const ActionResult committed = store.commitResult(
        directory.path(), QStringLiteral("task-1"), 7,
        payload, 4096, &resultPath);

    ASSERT_TRUE(committed.ok) << committed.message.toStdString();
    ASSERT_FALSE(resultPath.isEmpty());
    QFile result(resultPath);
    ASSERT_TRUE(result.open(QIODevice::ReadOnly));
    EXPECT_EQ(result.readAll(), payload);
}

TEST(AnalysisResultStoreTest, RetainsDiagnosticCaptureBeyondStoreLifetime)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString retainedPath;
    {
        PostRunAnalysisConfig config;
        config.minFreeBytes = 0;
        AnalysisResultStore store;
        ASSERT_TRUE(store.preparePending(directory.path(), config).ok);
        ASSERT_TRUE(store.closePendingForSession().ok);
        retainedPath = store.retainPendingCapture();
        ASSERT_FALSE(retainedPath.isEmpty());
    }

    EXPECT_TRUE(QFile::exists(retainedPath));
    EXPECT_TRUE(QFile::remove(retainedPath));
}

TEST(AnalysisResultStoreTest, HashesACompletedSourceArtifact)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("source.txt"));
    QFile source(path);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write("abc"), 3);
    source.close();
    QString error;

    const QByteArray digest = AnalysisResultStore::sha256File(path, &error);

    EXPECT_TRUE(error.isEmpty());
    EXPECT_EQ(digest.toHex(),
              QByteArray("ba7816bf8f01cfea414140de5dae2223"
                         "b00361a396177a9cb410ff61f20015ad"));
}

TEST(AnalysisResultStoreTest, StopsHashingWhenCancellationIsRequested)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("source.txt"));
    QFile source(path);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write(QByteArray(2 * 1024 * 1024, 'x')), 2 * 1024 * 1024);
    source.close();
    QString error;
    int cancellationChecks = 0;

    const QByteArray digest = AnalysisResultStore::sha256File(
        path, &error, [&cancellationChecks] {
            ++cancellationChecks;
            return cancellationChecks >= 2;
        });

    EXPECT_TRUE(digest.isEmpty());
    EXPECT_EQ(error, QStringLiteral("analysis_cancelled"));
    EXPECT_GE(cancellationChecks, 2);
}

TEST(AnalysisResultStoreTest, RemovesOnlyExpiredOrphanCapturesDuringPreflight)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString analysisDirectory =
        QDir(directory.path()).filePath(QStringLiteral("analysis"));
    ASSERT_TRUE(QDir().mkpath(analysisDirectory));
    const QString expiredPath = QDir(analysisDirectory).filePath(
        QStringLiteral(".capture-pending-expired.bin"));
    const QString freshPath = QDir(analysisDirectory).filePath(
        QStringLiteral(".capture-pending-fresh.bin"));
    QFile expired(expiredPath);
    ASSERT_TRUE(expired.open(QIODevice::WriteOnly));
    ASSERT_EQ(expired.write("expired"), 7);
    ASSERT_TRUE(expired.setFileTime(QDateTime::currentDateTimeUtc().addDays(-2),
                                    QFileDevice::FileModificationTime));
    expired.close();
    QFile fresh(freshPath);
    ASSERT_TRUE(fresh.open(QIODevice::WriteOnly));
    ASSERT_EQ(fresh.write("fresh"), 5);
    fresh.close();

    PostRunAnalysisConfig config;
    config.minFreeBytes = 0;
    config.diagnosticRetentionDays = 1;
    AnalysisResultStore store;
    ASSERT_TRUE(store.preparePending(directory.path(), config).ok);

    EXPECT_FALSE(QFile::exists(expiredPath));
    EXPECT_TRUE(QFile::exists(freshPath));
    store.cancelPending();
}

} // namespace
} // namespace hwtest::app
