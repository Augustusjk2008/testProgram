#include <biz/test_config_manager.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include <cmath>
#include <initializer_list>
#include <limits>

namespace hwtest::biz {
namespace {

Status makeStatus(ErrorCode code, const QString& message)
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

template <typename T>
Result<T> failure(ErrorCode code, const QString& message)
{
    Result<T> result;
    result.status = makeStatus(code, message);
    return result;
}

QString criterionOperationName(CmpOp operation)
{
    switch (operation) {
    case CmpOp::GreaterThan: return QStringLiteral("GreaterThan");
    case CmpOp::GreaterOrEqual: return QStringLiteral("GreaterOrEqual");
    case CmpOp::LessThan: return QStringLiteral("LessThan");
    case CmpOp::LessOrEqual: return QStringLiteral("LessOrEqual");
    case CmpOp::Equal: return QStringLiteral("Equal");
    case CmpOp::NotEqual: return QStringLiteral("NotEqual");
    case CmpOp::InRange: return QStringLiteral("InRange");
    }
    return {};
}

bool criterionOperationFromName(const QString& name, CmpOp& operation)
{
    const QString normalized = name.trimmed();
    const CmpOp values[] = {CmpOp::GreaterThan,
                            CmpOp::GreaterOrEqual,
                            CmpOp::LessThan,
                            CmpOp::LessOrEqual,
                            CmpOp::Equal,
                            CmpOp::NotEqual,
                            CmpOp::InRange};
    for (const CmpOp candidate : values) {
        if (normalized.compare(criterionOperationName(candidate), Qt::CaseInsensitive) == 0) {
            operation = candidate;
            return true;
        }
    }
    return false;
}

bool isAllowed(const QString& key, std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        if (key == QLatin1String(name)) {
            return true;
        }
    }
    return false;
}

bool rejectUnknown(const QJsonObject& object,
                   std::initializer_list<const char*> names,
                   const QString& scope,
                   QString& error)
{
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        if (!isAllowed(iterator.key(), names)) {
            error = QStringLiteral("Unknown field '%1' in %2").arg(iterator.key(), scope);
            return false;
        }
    }
    return true;
}

bool optionalString(const QJsonObject& object,
                    const char* name,
                    QString& value,
                    const QString& scope,
                    QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isString()) {
        error = QStringLiteral("Field '%1' in %2 must be a string").arg(QLatin1String(name), scope);
        return false;
    }
    value = json.toString();
    return true;
}

bool optionalBool(const QJsonObject& object,
                  const char* name,
                  bool& value,
                  const QString& scope,
                  QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isBool()) {
        error = QStringLiteral("Field '%1' in %2 must be a boolean").arg(QLatin1String(name), scope);
        return false;
    }
    value = json.toBool();
    return true;
}

bool jsonInteger(const QJsonValue& json, qint64 minimum, qint64 maximum, qint64& value)
{
    if (!json.isDouble()) {
        return false;
    }

    const double number = json.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        return false;
    }

    value = static_cast<qint64>(number);
    return true;
}

bool optionalInt(const QJsonObject& object,
                 const char* name,
                 int& value,
                 const QString& scope,
                 QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) {
        return true;
    }

    qint64 parsed = 0;
    if (!jsonInteger(json,
                     static_cast<qint64>(std::numeric_limits<int>::min()),
                     static_cast<qint64>(std::numeric_limits<int>::max()),
                     parsed)) {
        error = QStringLiteral("Field '%1' in %2 must be an integer").arg(QLatin1String(name), scope);
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool optionalInt64(const QJsonObject& object,
                   const char* name,
                   qint64& value,
                   const QString& scope,
                   QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) {
        return true;
    }

    qint64 parsed = 0;
    if (!jsonInteger(json,
                     std::numeric_limits<qint64>::min(),
                     std::numeric_limits<qint64>::max(),
                     parsed)) {
        error = QStringLiteral("Field '%1' in %2 must be an integer").arg(QLatin1String(name), scope);
        return false;
    }
    value = parsed;
    return true;
}

bool optionalDouble(const QJsonObject& object,
                    const char* name,
                    double& value,
                    const QString& scope,
                    QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isDouble() || !std::isfinite(json.toDouble())) {
        error = QStringLiteral("Field '%1' in %2 must be a number").arg(QLatin1String(name), scope);
        return false;
    }
    value = json.toDouble();
    return true;
}

QVariant variantFromJson(const QJsonValue& value);

QVariantMap mapFromJson(const QJsonObject& object)
{
    QVariantMap result;
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        result.insert(iterator.key(), variantFromJson(iterator.value()));
    }
    return result;
}

QVariantList listFromJson(const QJsonArray& array)
{
    QVariantList result;
    result.reserve(array.size());
    for (const QJsonValue& value : array) {
        result.append(variantFromJson(value));
    }
    return result;
}

QVariant variantFromJson(const QJsonValue& value)
{
    if (value.isObject()) {
        return mapFromJson(value.toObject());
    }
    if (value.isArray()) {
        return listFromJson(value.toArray());
    }
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::isfinite(number) && std::floor(number) == number) {
            if (number >= static_cast<double>(std::numeric_limits<int>::min()) &&
                number <= static_cast<double>(std::numeric_limits<int>::max())) {
                return static_cast<int>(number);
            }
            if (number >= static_cast<double>(std::numeric_limits<qint64>::min()) &&
                number <= static_cast<double>(std::numeric_limits<qint64>::max())) {
                return static_cast<qint64>(number);
            }
        }
        return number;
    }
    return QVariant();
}

bool optionalMap(const QJsonObject& object,
                 const char* name,
                 QVariantMap& value,
                 const QString& scope,
                 QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isObject()) {
        error = QStringLiteral("Field '%1' in %2 must be an object").arg(QLatin1String(name), scope);
        return false;
    }
    value = mapFromJson(json.toObject());
    return true;
}

bool optionalStringList(const QJsonObject& object,
                        const char* name,
                        QList<QString>& value,
                        const QString& scope,
                        QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isArray()) {
        error = QStringLiteral("Field '%1' in %2 must be an array").arg(QLatin1String(name), scope);
        return false;
    }

    QList<QString> parsed;
    const QJsonArray array = json.toArray();
    parsed.reserve(array.size());
    for (const QJsonValue& item : array) {
        if (!item.isString()) {
            error = QStringLiteral("Field '%1' in %2 must contain strings")
                        .arg(QLatin1String(name), scope);
            return false;
        }
        parsed.append(item.toString());
    }
    value = parsed;
    return true;
}

QJsonArray stringArray(const QList<QString>& values)
{
    QJsonArray array;
    for (const QString& value : values) {
        array.append(value);
    }
    return array;
}

QJsonObject criterionToJson(const Criterion& criterion)
{
    QJsonObject object;
    object.insert(QStringLiteral("metric"), criterion.metric);
    object.insert(QStringLiteral("op"), criterionOperationName(criterion.op));
    object.insert(QStringLiteral("ref"), QJsonValue::fromVariant(criterion.ref));
    object.insert(QStringLiteral("lo"), criterion.lo);
    object.insert(QStringLiteral("hi"), criterion.hi);
    object.insert(QStringLiteral("tol"), criterion.tol);
    object.insert(QStringLiteral("passIfMatched"), criterion.passIfMatched);
    return object;
}

bool criterionFromJson(const QJsonValue& json, Criterion& criterion, QString& error)
{
    if (!json.isObject()) {
        error = QStringLiteral("Each criterion must be an object");
        return false;
    }
    const QJsonObject object = json.toObject();
    if (!rejectUnknown(object,
                       {"metric", "op", "ref", "lo", "hi", "tol", "passIfMatched"},
                       QStringLiteral("criterion"),
                       error) ||
        !optionalString(object, "metric", criterion.metric, QStringLiteral("criterion"), error) ||
        !optionalDouble(object, "lo", criterion.lo, QStringLiteral("criterion"), error) ||
        !optionalDouble(object, "hi", criterion.hi, QStringLiteral("criterion"), error) ||
        !optionalDouble(object, "tol", criterion.tol, QStringLiteral("criterion"), error) ||
        !optionalBool(object,
                      "passIfMatched",
                      criterion.passIfMatched,
                      QStringLiteral("criterion"),
                      error)) {
        return false;
    }

    const QJsonValue operationJson = object.value(QStringLiteral("op"));
    if (!operationJson.isUndefined()) {
        if (operationJson.isString()) {
            if (!criterionOperationFromName(operationJson.toString(), criterion.op)) {
                error = QStringLiteral("Field 'op' in criterion has an unknown value");
                return false;
            }
        } else {
            qint64 operation = 0;
            if (!jsonInteger(operationJson,
                             static_cast<int>(CmpOp::GreaterThan),
                             static_cast<int>(CmpOp::InRange),
                             operation)) {
                error = QStringLiteral("Field 'op' in criterion must be a known string or integer");
                return false;
            }
            criterion.op = static_cast<CmpOp>(operation);
        }
    }

    const QJsonValue referenceJson = object.value(QStringLiteral("ref"));
    if (!referenceJson.isUndefined()) {
        if (referenceJson.isObject() || referenceJson.isArray()) {
            error = QStringLiteral("Field 'ref' in criterion must be a scalar value");
            return false;
        }
        criterion.ref = variantFromJson(referenceJson);
    }
    return true;
}

QJsonObject stepToJson(const TestStep& step)
{
    QJsonObject object;
    object.insert(QStringLiteral("stepId"), step.stepId);
    object.insert(QStringLiteral("testItemId"), step.testItemId);
    object.insert(QStringLiteral("name"), step.name);
    object.insert(QStringLiteral("type"), step.type);
    object.insert(QStringLiteral("board"), step.board);
    object.insert(QStringLiteral("algorithmId"), step.algorithmId);
    object.insert(QStringLiteral("parameters"), QJsonObject::fromVariantMap(step.parameters));
    object.insert(QStringLiteral("timeoutMs"), step.timeoutMs);
    object.insert(QStringLiteral("retryCount"), step.retryCount);
    object.insert(QStringLiteral("enabled"), step.enabled);
    object.insert(QStringLiteral("dependsOn"), stringArray(step.dependsOn));
    QJsonArray criteria;
    for (const Criterion& criterion : step.criteria) {
        criteria.append(criterionToJson(criterion));
    }
    object.insert(QStringLiteral("criteria"), criteria);
    return object;
}

bool stepFromJson(const QJsonValue& json, TestStep& step, QString& error)
{
    if (!json.isObject()) {
        error = QStringLiteral("Each step must be an object");
        return false;
    }
    const QJsonObject object = json.toObject();
    const QString scope = QStringLiteral("step");
    if (!rejectUnknown(object,
                       {"stepId", "testItemId", "name", "type", "board", "algorithmId",
                        "parameters", "timeoutMs", "retryCount", "enabled", "dependsOn", "criteria"},
                       scope,
                       error) ||
        !optionalString(object, "stepId", step.stepId, scope, error) ||
        !optionalString(object, "testItemId", step.testItemId, scope, error) ||
        !optionalString(object, "name", step.name, scope, error) ||
        !optionalString(object, "type", step.type, scope, error) ||
        !optionalString(object, "board", step.board, scope, error) ||
        !optionalString(object, "algorithmId", step.algorithmId, scope, error) ||
        !optionalMap(object, "parameters", step.parameters, scope, error) ||
        !optionalInt(object, "timeoutMs", step.timeoutMs, scope, error) ||
        !optionalInt(object, "retryCount", step.retryCount, scope, error) ||
        !optionalBool(object, "enabled", step.enabled, scope, error) ||
        !optionalStringList(object, "dependsOn", step.dependsOn, scope, error)) {
        return false;
    }

    const QJsonValue criteriaJson = object.value(QStringLiteral("criteria"));
    if (criteriaJson.isUndefined()) {
        return true;
    }
    if (!criteriaJson.isArray()) {
        error = QStringLiteral("Field 'criteria' in step must be an array");
        return false;
    }
    QList<Criterion> criteria;
    for (const QJsonValue& criterionJson : criteriaJson.toArray()) {
        Criterion criterion;
        if (!criterionFromJson(criterionJson, criterion, error)) {
            return false;
        }
        criteria.append(criterion);
    }
    step.criteria = criteria;
    return true;
}

QJsonObject requirementToJson(const HardwareRequirement& requirement)
{
    QJsonObject object;
    object.insert(QStringLiteral("requirementId"), requirement.requirementId);
    object.insert(QStringLiteral("deviceId"), requirement.deviceId);
    object.insert(QStringLiteral("adapterId"), requirement.adapterId);
    object.insert(QStringLiteral("requiredModules"), stringArray(requirement.requiredModules));
    object.insert(QStringLiteral("requiredResources"), stringArray(requirement.requiredResources));
    object.insert(QStringLiteral("priority"), requirement.priority);
    object.insert(QStringLiteral("match"), QJsonObject::fromVariantMap(requirement.match));
    return object;
}

bool requirementFromJson(const QJsonValue& json, HardwareRequirement& requirement, QString& error)
{
    if (!json.isObject()) {
        error = QStringLiteral("Each resource requirement must be an object");
        return false;
    }
    const QJsonObject object = json.toObject();
    const QString scope = QStringLiteral("resource requirement");
    return rejectUnknown(object,
                         {"requirementId", "deviceId", "adapterId", "requiredModules",
                          "requiredResources", "priority", "match"},
                         scope,
                         error) &&
           optionalString(object, "requirementId", requirement.requirementId, scope, error) &&
           optionalString(object, "deviceId", requirement.deviceId, scope, error) &&
           optionalString(object, "adapterId", requirement.adapterId, scope, error) &&
           optionalStringList(object, "requiredModules", requirement.requiredModules, scope, error) &&
           optionalStringList(object, "requiredResources", requirement.requiredResources, scope, error) &&
           optionalInt(object, "priority", requirement.priority, scope, error) &&
           optionalMap(object, "match", requirement.match, scope, error);
}

QJsonObject profileToJson(const ProtocolProfile& profile)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), profile.id);
    object.insert(QStringLiteral("busType"), profile.busType);
    object.insert(QStringLiteral("payloadEncoding"), profile.payloadEncoding);
    object.insert(QStringLiteral("frameFormat"), QJsonObject::fromVariantMap(profile.frameFormat));
    object.insert(QStringLiteral("timing"), QJsonObject::fromVariantMap(profile.timing));
    object.insert(QStringLiteral("responseRules"), QJsonObject::fromVariantMap(profile.responseRules));
    object.insert(QStringLiteral("fieldMapping"), QJsonObject::fromVariantMap(profile.fieldMapping));
    return object;
}

bool profileFromJson(const QJsonValue& json, ProtocolProfile& profile, QString& error)
{
    if (!json.isObject()) {
        error = QStringLiteral("Each profile must be an object");
        return false;
    }
    const QJsonObject object = json.toObject();
    const QString scope = QStringLiteral("profile");
    return rejectUnknown(object,
                         {"id", "busType", "payloadEncoding", "frameFormat", "timing",
                          "responseRules", "fieldMapping"},
                         scope,
                         error) &&
           optionalString(object, "id", profile.id, scope, error) &&
           optionalString(object, "busType", profile.busType, scope, error) &&
           optionalString(object, "payloadEncoding", profile.payloadEncoding, scope, error) &&
           optionalMap(object, "frameFormat", profile.frameFormat, scope, error) &&
           optionalMap(object, "timing", profile.timing, scope, error) &&
           optionalMap(object, "responseRules", profile.responseRules, scope, error) &&
           optionalMap(object, "fieldMapping", profile.fieldMapping, scope, error);
}

QJsonObject safetyToJson(const SafetyPolicy& policy)
{
    QJsonObject object;
    object.insert(QStringLiteral("outputLimits"), QJsonObject::fromVariantMap(policy.outputLimits));
    object.insert(QStringLiteral("safeState"), QJsonObject::fromVariantMap(policy.safeState));
    object.insert(QStringLiteral("enterSafeStateOnStop"), policy.enterSafeStateOnStop);
    object.insert(QStringLiteral("enterSafeStateOnError"), policy.enterSafeStateOnError);
    object.insert(QStringLiteral("daMinVoltage"), policy.daMinVoltage);
    object.insert(QStringLiteral("daMaxVoltage"), policy.daMaxVoltage);
    object.insert(QStringLiteral("doMinSwitchIntervalMs"), policy.doMinSwitchIntervalMs);
    object.insert(QStringLiteral("canSendMaxHz"), policy.canSendMaxHz);
    object.insert(QStringLiteral("resourceLockTimeoutMs"), policy.resourceLockTimeoutMs);
    return object;
}

bool safetyFromJson(const QJsonValue& json, SafetyPolicy& policy, QString& error)
{
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isObject()) {
        error = QStringLiteral("Field 'safetyPolicy' must be an object");
        return false;
    }
    const QJsonObject object = json.toObject();
    const QString scope = QStringLiteral("safety policy");
    return rejectUnknown(object,
                         {"outputLimits", "safeState", "enterSafeStateOnStop",
                          "enterSafeStateOnError", "daMinVoltage", "daMaxVoltage",
                          "doMinSwitchIntervalMs", "canSendMaxHz", "resourceLockTimeoutMs"},
                         scope,
                         error) &&
           optionalMap(object, "outputLimits", policy.outputLimits, scope, error) &&
           optionalMap(object, "safeState", policy.safeState, scope, error) &&
           optionalBool(object,
                        "enterSafeStateOnStop",
                        policy.enterSafeStateOnStop,
                        scope,
                        error) &&
           optionalBool(object,
                        "enterSafeStateOnError",
                        policy.enterSafeStateOnError,
                        scope,
                        error) &&
           optionalDouble(object, "daMinVoltage", policy.daMinVoltage, scope, error) &&
           optionalDouble(object, "daMaxVoltage", policy.daMaxVoltage, scope, error) &&
           optionalInt(object,
                       "doMinSwitchIntervalMs",
                       policy.doMinSwitchIntervalMs,
                       scope,
                       error) &&
           optionalInt(object, "canSendMaxHz", policy.canSendMaxHz, scope, error) &&
           optionalInt(object,
                       "resourceLockTimeoutMs",
                       policy.resourceLockTimeoutMs,
                       scope,
                       error);
}

QJsonObject runtimeToJson(const RuntimeConfig& runtime)
{
    QJsonObject object;
    object.insert(QStringLiteral("parallelEnabled"), runtime.parallelEnabled);
    object.insert(QStringLiteral("maxParallel"), runtime.maxParallel);
    object.insert(QStringLiteral("defaultTimeoutMs"), runtime.defaultTimeoutMs);
    object.insert(QStringLiteral("defaultRetryCount"), runtime.defaultRetryCount);
    object.insert(QStringLiteral("retryIntervalMs"), runtime.retryIntervalMs);
    object.insert(QStringLiteral("taskPriorityDefault"), runtime.taskPriorityDefault);
    object.insert(QStringLiteral("pauseAutoReleaseMs"), runtime.pauseAutoReleaseMs);
    object.insert(QStringLiteral("stopOnFirstFailure"), runtime.stopOnFirstFailure);
    object.insert(QStringLiteral("allowResume"), runtime.allowResume);
    object.insert(QStringLiteral("reportDir"), runtime.reportDir);
    object.insert(QStringLiteral("logDir"), runtime.logDir);
    object.insert(QStringLiteral("logRotateBytes"), static_cast<double>(runtime.logRotateBytes));
    object.insert(QStringLiteral("logKeepFiles"), runtime.logKeepFiles);
    object.insert(QStringLiteral("tags"), QJsonObject::fromVariantMap(runtime.tags));
    return object;
}

bool runtimeFromJson(const QJsonValue& json, RuntimeConfig& runtime, QString& error)
{
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isObject()) {
        error = QStringLiteral("Field 'runtimeConfig' must be an object");
        return false;
    }
    const QJsonObject object = json.toObject();
    const QString scope = QStringLiteral("runtime configuration");
    return rejectUnknown(object,
                         {"parallelEnabled", "maxParallel", "defaultTimeoutMs",
                          "defaultRetryCount", "retryIntervalMs", "taskPriorityDefault",
                          "pauseAutoReleaseMs", "stopOnFirstFailure", "allowResume", "reportDir",
                          "logDir", "logRotateBytes", "logKeepFiles", "tags"},
                         scope,
                         error) &&
           optionalBool(object, "parallelEnabled", runtime.parallelEnabled, scope, error) &&
           optionalInt(object, "maxParallel", runtime.maxParallel, scope, error) &&
           optionalInt(object, "defaultTimeoutMs", runtime.defaultTimeoutMs, scope, error) &&
           optionalInt(object, "defaultRetryCount", runtime.defaultRetryCount, scope, error) &&
           optionalInt(object, "retryIntervalMs", runtime.retryIntervalMs, scope, error) &&
           optionalInt(object,
                       "taskPriorityDefault",
                       runtime.taskPriorityDefault,
                       scope,
                       error) &&
           optionalInt(object,
                       "pauseAutoReleaseMs",
                       runtime.pauseAutoReleaseMs,
                       scope,
                       error) &&
           optionalBool(object, "stopOnFirstFailure", runtime.stopOnFirstFailure, scope, error) &&
           optionalBool(object, "allowResume", runtime.allowResume, scope, error) &&
           optionalString(object, "reportDir", runtime.reportDir, scope, error) &&
           optionalString(object, "logDir", runtime.logDir, scope, error) &&
           optionalInt64(object, "logRotateBytes", runtime.logRotateBytes, scope, error) &&
           optionalInt(object, "logKeepFiles", runtime.logKeepFiles, scope, error) &&
           optionalMap(object, "tags", runtime.tags, scope, error);
}

QJsonObject configToJson(const TestConfig& config)
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), config.schemaVersion);
    root.insert(QStringLiteral("configId"), config.configId);
    root.insert(QStringLiteral("productModel"), config.productModel);
    root.insert(QStringLiteral("productName"), config.productName);
    root.insert(QStringLiteral("configVersion"), config.configVersion);

    QJsonArray steps;
    for (const TestStep& step : config.steps) {
        steps.append(stepToJson(step));
    }
    root.insert(QStringLiteral("steps"), steps);

    QJsonArray requirements;
    for (const HardwareRequirement& requirement : config.hardwareRequirements) {
        requirements.append(requirementToJson(requirement));
    }
    root.insert(QStringLiteral("hardwareRequirements"), requirements);

    QJsonArray profiles;
    for (const ProtocolProfile& profile : config.protocolProfiles) {
        profiles.append(profileToJson(profile));
    }
    root.insert(QStringLiteral("protocolProfiles"), profiles);
    root.insert(QStringLiteral("executionConfig"), QJsonObject::fromVariantMap(config.executionConfig));
    root.insert(QStringLiteral("safetyPolicy"), safetyToJson(config.safetyPolicy));
    root.insert(QStringLiteral("runtimeConfig"), runtimeToJson(config.runtimeConfig));
    root.insert(QStringLiteral("reportFields"), QJsonObject::fromVariantMap(config.reportFields));
    return root;
}

bool configFromJson(const QJsonObject& root, TestConfig& config, QString& error)
{
    if (!rejectUnknown(root,
                       {"schemaVersion", "configId", "productModel", "productName", "configVersion",
                        "steps", "hardwareRequirements", "protocolProfiles", "executionConfig",
                        "halConfig", "safetyPolicy", "runtimeConfig", "reportFields"},
                       QStringLiteral("configuration"),
                       error)) {
        return false;
    }

    if (root.contains(QStringLiteral("executionConfig")) && root.contains(QStringLiteral("halConfig"))) {
        error = QStringLiteral("Configuration cannot contain both execution settings fields");
        return false;
    }

    if (!optionalString(root, "schemaVersion", config.schemaVersion, QStringLiteral("configuration"), error) ||
        !optionalString(root, "configId", config.configId, QStringLiteral("configuration"), error) ||
        !optionalString(root, "productModel", config.productModel, QStringLiteral("configuration"), error) ||
        !optionalString(root, "productName", config.productName, QStringLiteral("configuration"), error) ||
        !optionalString(root, "configVersion", config.configVersion, QStringLiteral("configuration"), error)) {
        return false;
    }

    const QJsonValue stepsJson = root.value(QStringLiteral("steps"));
    if (!stepsJson.isUndefined()) {
        if (!stepsJson.isArray()) {
            error = QStringLiteral("Field 'steps' in configuration must be an array");
            return false;
        }
        QVector<TestStep> steps;
        steps.reserve(stepsJson.toArray().size());
        for (const QJsonValue& stepJson : stepsJson.toArray()) {
            TestStep step;
            if (!stepFromJson(stepJson, step, error)) {
                return false;
            }
            steps.append(step);
        }
        config.steps = steps;
    }

    const QJsonValue requirementsJson = root.value(QStringLiteral("hardwareRequirements"));
    if (!requirementsJson.isUndefined()) {
        if (!requirementsJson.isArray()) {
            error = QStringLiteral("Field 'hardwareRequirements' in configuration must be an array");
            return false;
        }
        QVector<HardwareRequirement> requirements;
        requirements.reserve(requirementsJson.toArray().size());
        for (const QJsonValue& requirementJson : requirementsJson.toArray()) {
            HardwareRequirement requirement;
            if (!requirementFromJson(requirementJson, requirement, error)) {
                return false;
            }
            requirements.append(requirement);
        }
        config.hardwareRequirements = requirements;
    }

    const QJsonValue profilesJson = root.value(QStringLiteral("protocolProfiles"));
    if (!profilesJson.isUndefined()) {
        if (!profilesJson.isArray()) {
            error = QStringLiteral("Field 'protocolProfiles' in configuration must be an array");
            return false;
        }
        QVector<ProtocolProfile> profiles;
        profiles.reserve(profilesJson.toArray().size());
        for (const QJsonValue& profileJson : profilesJson.toArray()) {
            ProtocolProfile profile;
            if (!profileFromJson(profileJson, profile, error)) {
                return false;
            }
            profiles.append(profile);
        }
        config.protocolProfiles = profiles;
    }

    const char* executionKey = root.contains(QStringLiteral("executionConfig"))
        ? "executionConfig"
        : "halConfig";
    if (!optionalMap(root, executionKey, config.executionConfig, QStringLiteral("configuration"), error) ||
        !safetyFromJson(root.value(QStringLiteral("safetyPolicy")), config.safetyPolicy, error) ||
        !runtimeFromJson(root.value(QStringLiteral("runtimeConfig")), config.runtimeConfig, error) ||
        !optionalMap(root, "reportFields", config.reportFields, QStringLiteral("configuration"), error)) {
        return false;
    }
    return true;
}

} // namespace

Result<TestConfig> TestConfigManager::load(const ConfigPath& filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure<TestConfig>(ErrorCode::ConfigParseError,
                                   QStringLiteral("Cannot open configuration '%1': %2")
                                       .arg(filePath, file.errorString()));
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const QString reason = parseError.error == QJsonParseError::NoError
            ? QStringLiteral("root must be an object")
            : parseError.errorString();
        return failure<TestConfig>(ErrorCode::ConfigParseError,
                                   QStringLiteral("Cannot parse configuration '%1': %2")
                                       .arg(filePath, reason));
    }

    TestConfig config;
    QString error;
    if (!configFromJson(document.object(), config, error)) {
        return failure<TestConfig>(ErrorCode::ConfigSchemaError, error);
    }

    const Result<QVector<QString>> validation = validate(config);
    if (!validation.ok()) {
        return failure<TestConfig>(validation.status.code, validation.status.error.message);
    }
    return Result<TestConfig>{Status{}, config};
}

Status TestConfigManager::save(const ConfigPath& filePath, const TestConfig& config) const
{
    const Result<QVector<QString>> validation = validate(config);
    if (!validation.ok()) {
        return validation.status;
    }
    if (filePath.trimmed().isEmpty()) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("Configuration path must not be empty"));
    }

    const QFileInfo fileInfo(filePath);
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        return makeStatus(ErrorCode::ConfigParseError,
                          QStringLiteral("Cannot create configuration directory '%1'")
                              .arg(fileInfo.absolutePath()));
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return makeStatus(ErrorCode::ConfigParseError,
                          QStringLiteral("Cannot write configuration '%1': %2")
                              .arg(filePath, file.errorString()));
    }
    const QByteArray serialized = QJsonDocument(configToJson(config)).toJson(QJsonDocument::Indented);
    if (file.write(serialized) != serialized.size() || !file.commit()) {
        return makeStatus(ErrorCode::ConfigParseError,
                          QStringLiteral("Cannot save configuration '%1': %2")
                              .arg(filePath, file.errorString()));
    }
    return Status{};
}

Result<QVector<QString>> TestConfigManager::validate(const TestConfig& config) const
{
    if (config.schemaVersion.trimmed().isEmpty() || config.configId.trimmed().isEmpty() ||
        config.productModel.trimmed().isEmpty() || config.configVersion.trimmed().isEmpty() ||
        config.steps.isEmpty()) {
        return failure<QVector<QString>>(ErrorCode::ConfigSchemaError,
                                         QStringLiteral("Configuration identity and at least one step are required"));
    }

    QSet<StepId> stepIds;
    QSet<TestItemId> itemIds;
    for (const TestStep& step : config.steps) {
        if (step.stepId.trimmed().isEmpty() || step.testItemId.trimmed().isEmpty() ||
            step.algorithmId.trimmed().isEmpty()) {
            return failure<QVector<QString>>(ErrorCode::ConfigSchemaError,
                                             QStringLiteral("Every step requires stepId, testItemId and algorithmId"));
        }
        if (stepIds.contains(step.stepId) || itemIds.contains(step.testItemId)) {
            return failure<QVector<QString>>(ErrorCode::ConfigSchemaError,
                                             QStringLiteral("Step and test item identifiers must be unique"));
        }
        stepIds.insert(step.stepId);
        itemIds.insert(step.testItemId);

        if (step.timeoutMs < 0) {
            return failure<QVector<QString>>(ErrorCode::ParameterRangeError,
                                             QStringLiteral("Step '%1' has a negative timeout")
                                                 .arg(step.stepId));
        }
        if (step.retryCount < -1) {
            return failure<QVector<QString>>(ErrorCode::ParameterRangeError,
                                             QStringLiteral("Step '%1' has an invalid retry count")
                                                 .arg(step.stepId));
        }
        for (const Criterion& criterion : step.criteria) {
            if (criterion.metric.trimmed().isEmpty() ||
                criterionOperationName(criterion.op).isEmpty()) {
                return failure<QVector<QString>>(ErrorCode::ConfigSchemaError,
                                                 QStringLiteral("Criterion metric or operation is invalid"));
            }
            const QJsonValue reference = QJsonValue::fromVariant(criterion.ref);
            if (reference.isUndefined() || reference.isArray() || reference.isObject()) {
                return failure<QVector<QString>>(ErrorCode::ConfigSchemaError,
                                                 QStringLiteral("Criterion reference must be a JSON scalar"));
            }
        }
    }

    const RuntimeConfig& runtime = config.runtimeConfig;
    if (runtime.maxParallel < 0 || runtime.defaultTimeoutMs < 0 ||
        runtime.defaultRetryCount < 0 || runtime.retryIntervalMs < 0 ||
        runtime.pauseAutoReleaseMs < 0 || runtime.logRotateBytes < 0 || runtime.logKeepFiles < 0 ||
        runtime.taskPriorityDefault < 1 || runtime.taskPriorityDefault > 3) {
        return failure<QVector<QString>>(ErrorCode::ParameterRangeError,
                                         QStringLiteral("Runtime configuration contains a negative limit"));
    }

    const SafetyPolicy& safety = config.safetyPolicy;
    if (safety.doMinSwitchIntervalMs < 0 || safety.canSendMaxHz < 0 ||
        safety.resourceLockTimeoutMs < 0 || safety.daMaxVoltage < safety.daMinVoltage) {
        return failure<QVector<QString>>(ErrorCode::ParameterRangeError,
                                         QStringLiteral("Safety policy contains an invalid range"));
    }

    return Result<QVector<QString>>{Status{}, {}};
}

} // namespace hwtest::biz
