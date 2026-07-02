#pragma once

#include "log_global.h"

#include <QMetaType>
#include <QString>
#include <QVariantMap>
#include <QVector>

namespace hwtest::logging {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off
};

struct LogEvent {
    qint64 timestampUs = 0;
    QString level;
    QString source;
    QString category;
    QString message;
    QString requestId;
    qint64 durationMs = -1;
    QString status;
    QString adapterCode;
    QVariantMap context;
};

HWTEST_LOG_EXPORT void registerLogMetaTypes();
HWTEST_LOG_EXPORT QString toString(LogLevel level);
HWTEST_LOG_EXPORT LogLevel logLevelFromString(const QString& level,
                                              LogLevel fallback = LogLevel::Info);
HWTEST_LOG_EXPORT bool isLogLevelEnabled(LogLevel level, LogLevel minimumLevel);

} // namespace hwtest::logging

Q_DECLARE_METATYPE(hwtest::logging::LogLevel)
Q_DECLARE_METATYPE(hwtest::logging::LogEvent)
Q_DECLARE_METATYPE(QVector<hwtest::logging::LogEvent>)
