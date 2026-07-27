#pragma once

#include <app/test_application_controller.h>

#include <QFile>
#include <QString>
#include <QVector>

namespace hwtest::app {

class ContinuousDataRecorder final {
public:
    ContinuousDataRecorder() = default;
    ~ContinuousDataRecorder();

    ActionResult begin(const QString& directory,
                       const TestDescriptor& descriptor,
                       int intervalMs,
                       quint64 maxCycles);
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
    void resetState();

    QFile m_partialFile;
    TestDescriptor m_descriptor;
    QVector<Column> m_columns;
    QString m_taskId;
    QString m_outputPath;
    QString m_partialPath;
    QString m_writeError;
    qint64 m_startedAtUs = 0;
    quint64 m_sampleCount = 0;
    quint64 m_maxCycles = 0;
    int m_intervalMs = 0;
    bool m_active = false;
    bool m_electricalHealthFormat = false;
};

} // namespace hwtest::app
