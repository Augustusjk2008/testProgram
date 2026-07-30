#include "run_mode_capabilities.h"

#include <QMetaType>
#include <QSet>
#include <QVariantList>

namespace hwtest::app {

QString parseSupportedRunModes(const QVariantMap& reportFields,
                               QVector<QString>* runModes)
{
    if (runModes == nullptr) {
        return QStringLiteral("Run mode capability output is required");
    }
    runModes->clear();

    const QVariant value = reportFields.value(QStringLiteral("supportedRunModes"));
    if (!value.isValid()) {
        runModes->push_back(QStringLiteral("single"));
        return {};
    }
    if (value.userType() != QMetaType::QVariantList) {
        return QStringLiteral("reportFields.supportedRunModes must be an array");
    }

    const QVariantList values = value.toList();
    if (values.isEmpty()) {
        return QStringLiteral("reportFields.supportedRunModes must not be empty");
    }

    static const QSet<QString> knownModes{
        QStringLiteral("single"),
        QStringLiteral("pc_periodic"),
        QStringLiteral("device_stream"),
    };
    QSet<QString> seenModes;
    for (const QVariant& modeValue : values) {
        if (modeValue.userType() != QMetaType::QString) {
            return QStringLiteral(
                "reportFields.supportedRunModes entries must be strings");
        }
        const QString mode = modeValue.toString().trimmed();
        if (!knownModes.contains(mode)) {
            return QStringLiteral("Unknown supported run mode '%1'").arg(mode);
        }
        if (seenModes.contains(mode)) {
            return QStringLiteral("Duplicate supported run mode '%1'").arg(mode);
        }
        runModes->push_back(mode);
        seenModes.insert(mode);
    }

    if (seenModes.contains(QStringLiteral("pc_periodic")) &&
        seenModes.contains(QStringLiteral("device_stream"))) {
        return QStringLiteral(
            "A test configuration cannot support both pc_periodic and device_stream");
    }
    return {};
}

QString parseStoppableCapability(const QVariantMap& reportFields,
                                  const QVector<QString>& runModes,
                                  bool* stoppable)
{
    if (stoppable == nullptr) {
        return QStringLiteral("Stoppable capability output is required");
    }
    *stoppable = true;
    const QVariant value = reportFields.value(QStringLiteral("stoppable"));
    if (!value.isValid()) return {};
    if (value.userType() != QMetaType::Bool) {
        return QStringLiteral("reportFields.stoppable must be a boolean");
    }
    *stoppable = value.toBool();
    if (!*stoppable &&
        (runModes.size() != 1 ||
         runModes.first() != QStringLiteral("device_stream"))) {
        return QStringLiteral(
            "reportFields.stoppable=false requires device_stream as the only supported run mode");
    }
    return {};
}

} // namespace hwtest::app
