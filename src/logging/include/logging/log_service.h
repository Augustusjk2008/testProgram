#pragma once

#include "i_log_service.h"

#include <QMutex>
#include <QVector>

namespace hwtest::logging {

class HWTEST_LOG_EXPORT LogService : public ILogService {
    Q_OBJECT

public:
    explicit LogService(QObject* parent = nullptr);

    void append(const LogEvent& event) override;
    QVector<LogEvent> recent(int maxCount) const override;

    void setMinimumLevel(LogLevel level);
    LogLevel minimumLevel() const;

    void setRecentLimit(int limit);
    int recentLimit() const;

    void addSink(ILogSink* sink);
    void removeSink(ILogSink* sink);
    void clearSinks();

private:
    LogEvent normalize(const LogEvent& event) const;
    void trimRecentLocked();

    mutable QMutex m_mutex;
    QVector<LogEvent> m_recent;
    QVector<ILogSink*> m_sinks;
    int m_recentLimit = 1000;
    LogLevel m_minimumLevel = LogLevel::Trace;
};

} // namespace hwtest::logging
