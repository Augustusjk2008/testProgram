#pragma once

#include <biz/biz_types.h>

#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

namespace hwtest::algorithm::mbddf {

enum class RunParameterKind {
    Integer,
    Number,
    Boolean,
    Choice,
};

struct RunParameterChoice {
    QVariant value;
    QString label;
};

struct RunParameterDescriptor {
    QString id;
    QString label;
    QString description;
    RunParameterKind kind = RunParameterKind::Number;
    QString unit;
    QVariant defaultValue;
    bool required = true;
    QVariant minimum;
    QVariant maximum;
    bool minimumExclusive = false;
    bool maximumExclusive = false;
    QVector<RunParameterChoice> choices;
    QString visibleWhenParameter;
    QVariant visibleWhenEquals;
};

struct RunParameterSchema {
    QString version;
    QVector<RunParameterDescriptor> parameters;
    // Defaults to the historical browser behavior. Run-scoped hardware
    // controls can opt out so values are reset for every invocation.
    bool persistValues = true;
};

const RunParameterSchema* findRunParameterSchema(const QString& algorithmId);

hwtest::biz::Result<QVariantMap> normalizeRunParameters(
    const QString& algorithmId,
    const QVariantMap& configuredDefaults,
    const QVariantMap& overrides);

} // namespace hwtest::algorithm::mbddf
