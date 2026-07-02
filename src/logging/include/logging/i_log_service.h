#pragma once

#include "log_global.h"
#include "log_types.h"

#include <QObject>

namespace hwtest::logging {

class HWTEST_LOG_EXPORT ILogSink {
public:
    virtual ~ILogSink() = default;

    virtual void append(const LogEvent& event) = 0;
    virtual void flush() {}
};

class HWTEST_LOG_EXPORT ILogService : public QObject {
    Q_OBJECT

public:
    explicit ILogService(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    ~ILogService() override = default;

    virtual void append(const LogEvent& event) = 0;
    virtual QVector<LogEvent> recent(int maxCount) const = 0;

signals:
    void logAppended(const LogEvent& event);
};

} // namespace hwtest::logging
