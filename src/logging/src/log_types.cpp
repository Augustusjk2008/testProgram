#include "logging/log_types.h"

#include <QMetaType>

namespace hwtest::logging {

void registerLogMetaTypes()
{
    qRegisterMetaType<LogLevel>("hwtest::logging::LogLevel");
    qRegisterMetaType<LogEvent>("hwtest::logging::LogEvent");
    qRegisterMetaType<QVector<LogEvent>>("QVector<hwtest::logging::LogEvent>");
}

QString toString(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace: return QStringLiteral("TRACE");
    case LogLevel::Debug: return QStringLiteral("DEBUG");
    case LogLevel::Info: return QStringLiteral("INFO");
    case LogLevel::Warn: return QStringLiteral("WARN");
    case LogLevel::Error: return QStringLiteral("ERROR");
    case LogLevel::Fatal: return QStringLiteral("FATAL");
    case LogLevel::Off: return QStringLiteral("OFF");
    }
    return QStringLiteral("INFO");
}

LogLevel logLevelFromString(const QString& level, LogLevel fallback)
{
    const QString normalized = level.trimmed();
    if (normalized.compare(QStringLiteral("TRACE"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Trace;
    }
    if (normalized.compare(QStringLiteral("DEBUG"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Debug;
    }
    if (normalized.compare(QStringLiteral("INFO"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Info;
    }
    if (normalized.compare(QStringLiteral("WARN"), Qt::CaseInsensitive) == 0
        || normalized.compare(QStringLiteral("WARNING"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Warn;
    }
    if (normalized.compare(QStringLiteral("ERROR"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Error;
    }
    if (normalized.compare(QStringLiteral("FATAL"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Fatal;
    }
    if (normalized.compare(QStringLiteral("OFF"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Off;
    }
    return fallback;
}

bool isLogLevelEnabled(LogLevel level, LogLevel minimumLevel)
{
    if (level == LogLevel::Off || minimumLevel == LogLevel::Off) {
        return false;
    }
    return static_cast<int>(level) >= static_cast<int>(minimumLevel);
}

} // namespace hwtest::logging
