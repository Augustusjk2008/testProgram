#include "logging/log_file_sink.h"

#include "logging/log_formatter.h"

#include <QMutexLocker>

namespace hwtest::logging {

JsonLineFileSink::JsonLineFileSink(const QString& filePath)
    : m_filePath(filePath)
{
}

JsonLineFileSink::~JsonLineFileSink()
{
    close();
}

bool JsonLineFileSink::open(QIODevice::OpenMode mode)
{
    QMutexLocker locker(&m_mutex);
    return openLocked(mode);
}

void JsonLineFileSink::close()
{
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) {
        m_file.close();
    }
}

bool JsonLineFileSink::isOpen() const
{
    QMutexLocker locker(&m_mutex);
    return m_file.isOpen();
}

QString JsonLineFileSink::filePath() const
{
    QMutexLocker locker(&m_mutex);
    return m_filePath;
}

QString JsonLineFileSink::errorString() const
{
    QMutexLocker locker(&m_mutex);
    return m_errorString;
}

void JsonLineFileSink::append(const LogEvent& event)
{
    QMutexLocker locker(&m_mutex);
    if (!m_file.isOpen() && !openLocked(QIODevice::WriteOnly
                                        | QIODevice::Append
                                        | QIODevice::Text)) {
        return;
    }

    const QByteArray line = LogFormatter::toJsonLine(event);
    if (m_file.write(line) != line.size()) {
        m_errorString = m_file.errorString();
    }
}

void JsonLineFileSink::flush()
{
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen() && !m_file.flush()) {
        m_errorString = m_file.errorString();
    }
}

bool JsonLineFileSink::openLocked(QIODevice::OpenMode mode)
{
    if (m_file.isOpen()) {
        return true;
    }
    if (m_filePath.isEmpty()) {
        m_errorString = QStringLiteral("Log file path is empty");
        return false;
    }

    m_file.setFileName(m_filePath);
    if (!m_file.open(mode)) {
        m_errorString = m_file.errorString();
        return false;
    }

    m_errorString.clear();
    return true;
}

} // namespace hwtest::logging
