#pragma once

#include <QString>
#include <QVariantMap>
#include <QVector>

namespace hwtest::app {

QString parseSupportedRunModes(const QVariantMap& reportFields,
                               QVector<QString>* runModes);

QString parseStoppableCapability(const QVariantMap& reportFields,
                                  const QVector<QString>& runModes,
                                  bool* stoppable);

} // namespace hwtest::app
