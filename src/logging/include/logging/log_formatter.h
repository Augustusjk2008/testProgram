#pragma once

#include "log_global.h"
#include "log_types.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace hwtest::logging {

class HWTEST_LOG_EXPORT LogFormatter {
public:
    static QJsonObject toJsonObject(const LogEvent& event);
    static QByteArray toJsonLine(const LogEvent& event);
    static QString toTextLine(const LogEvent& event);
};

} // namespace hwtest::logging
