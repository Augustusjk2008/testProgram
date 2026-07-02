#pragma once

#include "i_log_service.h"

#include <QFile>
#include <QIODevice>
#include <QMutex>
#include <QString>

namespace hwtest::logging {

class HWTEST_LOG_EXPORT JsonLineFileSink : public ILogSink {
public:
    explicit JsonLineFileSink(const QString& filePath);
    ~JsonLineFileSink() override;

    bool open(QIODevice::OpenMode mode = QIODevice::WriteOnly
              | QIODevice::Append
              | QIODevice::Text);
    void close();
    bool isOpen() const;

    QString filePath() const;
    QString errorString() const;

    void append(const LogEvent& event) override;
    void flush() override;

private:
    bool openLocked(QIODevice::OpenMode mode);

    mutable QMutex m_mutex;
    QString m_filePath;
    QFile m_file;
    QString m_errorString;
};

} // namespace hwtest::logging
