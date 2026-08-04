#pragma once

#include <app/test_application_controller.h>

#include <QFile>
#include <QSaveFile>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

namespace hwtest::app {

class ContinuousDataRecorder final {
public:
    using Clock = std::function<qint64()>;
    using OutputFileFactory =
        std::function<std::unique_ptr<QSaveFile>(const QString&)>;

    explicit ContinuousDataRecorder(Clock clock = {},
                                    OutputFileFactory outputFileFactory = {});
    ~ContinuousDataRecorder();

    static ActionResult validateDestinationOverrides(
        const QString& dataDirectory,
        const QString& dataFileName,
        QString* normalizedFileName = nullptr);

    ActionResult begin(const QString& directory,
                       const TestDescriptor& descriptor,
                       const QString& runMode,
                       int intervalMs,
                       quint64 maxCycles,
                       const QString& dataDirectory = {},
                       const QString& dataFileName = {});
    void setTaskId(const QString& taskId);
    ActionResult append(const ApplicationSample& sample);
    ActionResult finish(const QString& finalStatus,
                        const QString& finalDetail);
    void cancel();

    bool active() const;
    QString outputPath() const;

private:
    struct Column {
        QString valueKey;
        QString header;
    };

    ActionResult writePartial(const QByteArray& bytes, bool flush);
    void releaseOutputReservation();
    void resetState();

    QFile m_partialFile;
    TestDescriptor m_descriptor;
    QVector<Column> m_columns;
    QString m_taskId;
    QString m_runMode;
    QString m_outputDirectory;
    QString m_requestedFileName;
    QString m_outputPath;
    QString m_partialPath;
    QString m_writeError;
    Clock m_clock;
    OutputFileFactory m_outputFileFactory;
    bool m_outputReservationActive = false;
    qint64 m_startedAtUs = 0;
    quint64 m_sampleCount = 0;
    quint64 m_maxCycles = 0;
    int m_intervalMs = 0;
    bool m_active = false;
    bool m_electricalHealthFormat = false;
};

} // namespace hwtest::app
