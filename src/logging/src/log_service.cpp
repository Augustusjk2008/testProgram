#include "logging/log_service.h"

#include <QDateTime>
#include <QMutexLocker>

namespace hwtest::logging {

namespace {

qint64 nowUs()
{
    return static_cast<qint64>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) * 1000;
}

} // namespace

LogService::LogService(QObject* parent)
    : ILogService(parent)
{
    registerLogMetaTypes();
}

void LogService::append(const LogEvent& event)
{
    const LogEvent normalized = normalize(event);
    const LogLevel eventLevel = logLevelFromString(normalized.level, LogLevel::Info);

    QVector<ILogSink*> sinks;
    {
        QMutexLocker locker(&m_mutex);
        if (!isLogLevelEnabled(eventLevel, m_minimumLevel)) {
            return;
        }

        if (m_recentLimit > 0) {
            m_recent.push_back(normalized);
            trimRecentLocked();
        }
        sinks = m_sinks;
    }

    for (ILogSink* sink : sinks) {
        if (sink != nullptr) {
            sink->append(normalized);
        }
    }

    emit logAppended(normalized);
}

QVector<LogEvent> LogService::recent(int maxCount) const
{
    if (maxCount <= 0) {
        return {};
    }

    QMutexLocker locker(&m_mutex);
    const int count = qMin(maxCount, m_recent.size());
    QVector<LogEvent> result;
    result.reserve(count);
    const int start = m_recent.size() - count;
    for (int i = start; i < m_recent.size(); ++i) {
        result.push_back(m_recent.at(i));
    }
    return result;
}

void LogService::setMinimumLevel(LogLevel level)
{
    QMutexLocker locker(&m_mutex);
    m_minimumLevel = level;
}

LogLevel LogService::minimumLevel() const
{
    QMutexLocker locker(&m_mutex);
    return m_minimumLevel;
}

void LogService::setRecentLimit(int limit)
{
    QMutexLocker locker(&m_mutex);
    m_recentLimit = qMax(0, limit);
    trimRecentLocked();
}

int LogService::recentLimit() const
{
    QMutexLocker locker(&m_mutex);
    return m_recentLimit;
}

void LogService::addSink(ILogSink* sink)
{
    if (sink == nullptr) {
        return;
    }

    QMutexLocker locker(&m_mutex);
    if (!m_sinks.contains(sink)) {
        m_sinks.push_back(sink);
    }
}

void LogService::removeSink(ILogSink* sink)
{
    QMutexLocker locker(&m_mutex);
    m_sinks.removeAll(sink);
}

void LogService::clearSinks()
{
    QMutexLocker locker(&m_mutex);
    m_sinks.clear();
}

LogEvent LogService::normalize(const LogEvent& event) const
{
    LogEvent normalized = event;
    if (normalized.timestampUs <= 0) {
        normalized.timestampUs = nowUs();
    }

    const LogLevel level = logLevelFromString(normalized.level, LogLevel::Info);
    normalized.level = toString(level);
    return normalized;
}

void LogService::trimRecentLocked()
{
    if (m_recentLimit <= 0) {
        m_recent.clear();
        return;
    }

    const int extra = m_recent.size() - m_recentLimit;
    if (extra > 0) {
        m_recent.remove(0, extra);
    }
}

} // namespace hwtest::logging
