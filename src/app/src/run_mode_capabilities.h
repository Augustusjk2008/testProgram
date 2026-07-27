#pragma once

#include <QString>
#include <QVariantMap>
#include <QVector>

namespace hwtest::app {

QString parseSupportedRunModes(const QVariantMap& reportFields,
                               QVector<QString>* runModes);

} // namespace hwtest::app
