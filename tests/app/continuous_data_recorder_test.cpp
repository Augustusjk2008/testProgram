#include "continuous_data_recorder.h"

#include <gtest/gtest.h>

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTime>
#include <QTemporaryDir>
#include <QVector>

#include <memory>
#include <utility>

namespace hwtest::app {
namespace {

qint64 utcMicroseconds(int hour,
                       int minute,
                       int second,
                       int microseconds)
{
    const QDateTime timestamp(QDate(2026, 1, 1),
                              QTime(hour, minute, second),
                              Qt::UTC);
    return timestamp.toMSecsSinceEpoch() * 1000 + microseconds;
}

TestDescriptor descriptor(QString title = QStringLiteral("Project Alpha"))
{
    TestDescriptor value;
    value.configId = QStringLiteral("fallback-config");
    value.reportTitle = title;
    value.title = std::move(title);
    value.algorithmId = QStringLiteral("example.continuous");
    value.measurements = {
        TestMeasurementDescriptor{QStringLiteral("value"),
                                  QStringLiteral("Value"),
                                  QStringLiteral("V"),
                                  true},
    };
    return value;
}

QString readTextFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    EXPECT_TRUE(bytes.startsWith(QByteArray::fromHex("EFBBBF")));
    return QString::fromUtf8(bytes.mid(3));
}

TEST(ContinuousDataRecorderTest,
     EmptyDestinationUsesSafeTitleAndShanghaiStartAndFinishTimes)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QVector<qint64> now{
        utcMicroseconds(16, 30, 0, 123456),
        utcMicroseconds(16, 30, 1, 654321),
    };
    int nextNow = 0;
    ContinuousDataRecorder recorder([&now, &nextNow] { return now.at(nextNow++); });

    const ActionResult began = recorder.begin(directory.path(),
                                               descriptor(),
                                               QStringLiteral("pc_periodic"),
                                               0,
                                               0,
                                               {},
                                               {});
    ASSERT_TRUE(began.ok);
    ASSERT_TRUE(recorder.finish(QStringLiteral("已完成"), QStringLiteral("done")).ok);

    EXPECT_EQ(QFileInfo(recorder.outputPath()).absolutePath(),
              QFileInfo(directory.path()).absoluteFilePath());
    EXPECT_EQ(QFileInfo(recorder.outputPath()).fileName(),
              QStringLiteral(
                  "Project_Alpha_20260102_003000_123456-20260102_003001_654321.txt"));
    const QString text = readTextFile(recorder.outputPath());
    EXPECT_TRUE(text.contains(
        QStringLiteral("# started_at=2026-01-02 00:30:00.123456+08:00\n")));
    EXPECT_TRUE(text.contains(
        QStringLiteral("# finished_at=2026-01-02 00:30:01.654321+08:00\n")));
}

TEST(ContinuousDataRecorderTest,
     ExplicitBaseNameAddsTxtKeepsTxtAndAvoidsOverwrite)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString requestedDirectory =
        QDir(directory.filePath(QStringLiteral("requested-output"))).absolutePath();

    QVector<qint64> firstNow{
        utcMicroseconds(16, 31, 0, 100000),
        utcMicroseconds(16, 31, 1, 100000),
    };
    int firstNextNow = 0;
    ContinuousDataRecorder first(
        [&firstNow, &firstNextNow] { return firstNow.at(firstNextNow++); });
    const ActionResult firstBegan = first.begin(directory.path(),
                                                 descriptor(),
                                                 QStringLiteral("pc_periodic"),
                                                 0,
                                                 0,
                                                 requestedDirectory,
                                                 QStringLiteral("operator-capture"));
    ASSERT_TRUE(firstBegan.ok);
    ASSERT_TRUE(first.finish(QStringLiteral("已完成"), QStringLiteral("done")).ok);

    QVector<qint64> secondNow{
        utcMicroseconds(16, 32, 0, 100000),
        utcMicroseconds(16, 32, 1, 100000),
    };
    int secondNextNow = 0;
    ContinuousDataRecorder second(
        [&secondNow, &secondNextNow] { return secondNow.at(secondNextNow++); });
    const ActionResult secondBegan = second.begin(directory.path(),
                                                   descriptor(),
                                                   QStringLiteral("pc_periodic"),
                                                   0,
                                                   0,
                                                   requestedDirectory,
                                                   QStringLiteral("operator-capture.txt"));
    ASSERT_TRUE(secondBegan.ok);
    ASSERT_TRUE(second.finish(QStringLiteral("已完成"), QStringLiteral("done")).ok);

    EXPECT_EQ(QFileInfo(first.outputPath()).absolutePath(),
              QFileInfo(requestedDirectory).absoluteFilePath());
    EXPECT_EQ(QFileInfo(first.outputPath()).fileName(),
              QStringLiteral("operator-capture.txt"));
    EXPECT_EQ(QFileInfo(second.outputPath()).fileName(),
              QStringLiteral("operator-capture_1.txt"));
    EXPECT_TRUE(QFileInfo(first.outputPath()).isFile());
    EXPECT_TRUE(QFileInfo(second.outputPath()).isFile());
}

TEST(ContinuousDataRecorderTest, EmptyTitleFallsBackToConfigIdForDefaultName)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QVector<qint64> now{
        utcMicroseconds(16, 33, 0, 100000),
        utcMicroseconds(16, 33, 1, 100000),
    };
    int nextNow = 0;
    ContinuousDataRecorder recorder([&now, &nextNow] { return now.at(nextNow++); });

    const ActionResult began = recorder.begin(directory.path(),
                                               descriptor({}),
                                               QStringLiteral("pc_periodic"),
                                               0,
                                               0,
                                               {},
                                               {});
    ASSERT_TRUE(began.ok);
    ASSERT_TRUE(recorder.finish(QStringLiteral("已完成"), QStringLiteral("done")).ok);

    EXPECT_EQ(QFileInfo(recorder.outputPath()).fileName(),
              QStringLiteral(
                  "fallback-config_20260102_003300_100000-20260102_003301_100000.txt"));
}

TEST(ContinuousDataRecorderTest,
     FinalizeCollisionPreservesCompetingFileAndSelectsNextSuffix)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QVector<qint64> now{
        utcMicroseconds(16, 34, 0, 100000),
        utcMicroseconds(16, 34, 1, 100000),
    };
    int nextNow = 0;
    ContinuousDataRecorder recorder([&now, &nextNow] { return now.at(nextNow++); });

    ASSERT_TRUE(recorder.begin(directory.path(),
                               descriptor(),
                               QStringLiteral("pc_periodic"),
                               0,
                               0,
                               {},
                               QStringLiteral("operator-capture")).ok);
    const QString competingPath = recorder.outputPath();
    QFile competing(competingPath);
    ASSERT_TRUE(competing.open(QIODevice::WriteOnly));
    ASSERT_EQ(competing.write("competing"), 9);
    competing.close();

    ASSERT_TRUE(recorder.finish(QStringLiteral("已完成"), QStringLiteral("done")).ok);
    EXPECT_EQ(QFileInfo(recorder.outputPath()).fileName(),
              QStringLiteral("operator-capture_1.txt"));
    ASSERT_TRUE(competing.open(QIODevice::ReadOnly));
    EXPECT_EQ(competing.readAll(), QByteArray("competing"));
}

TEST(ContinuousDataRecorderTest, CancelPreservesPartialAfterFinalizeFailure)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QVector<qint64> now{
        utcMicroseconds(16, 35, 0, 100000),
        utcMicroseconds(16, 35, 1, 100000),
    };
    int nextNow = 0;
    const QString blockedTarget = directory.filePath(QStringLiteral("blocked-target"));
    ASSERT_TRUE(QDir().mkpath(blockedTarget));
    ContinuousDataRecorder recorder(
        [&now, &nextNow] { return now.at(nextNow++); },
        [blockedTarget](const QString&) {
            return std::make_unique<QSaveFile>(blockedTarget);
        });

    ASSERT_TRUE(recorder.begin(directory.path(),
                               descriptor(),
                               QStringLiteral("pc_periodic"),
                               0,
                               0,
                               {},
                               QStringLiteral("blocked-output")).ok);
    EXPECT_FALSE(recorder.finish(QStringLiteral("错误"), QStringLiteral("blocked")).ok);
    const QString recoveryPath = recorder.outputPath();
    EXPECT_TRUE(recoveryPath.endsWith(QStringLiteral(".partial")));
    ASSERT_TRUE(QFileInfo(recoveryPath).isFile());

    recorder.cancel();
    EXPECT_TRUE(QFileInfo(recoveryPath).isFile());
}

} // namespace
} // namespace hwtest::app
