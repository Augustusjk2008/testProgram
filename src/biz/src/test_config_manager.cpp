// 包含本模块头文件
#include <biz/test_config_manager.h>

// Qt 文件处理
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>

// Qt JSON 处理
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

// Qt 原子保存（先写临时文件，成功后再替换目标文件）
#include <QSaveFile>

// Qt 集合
#include <QSet>

// 数学函数（isfinite, floor）
#include <cmath>

// initializer_list 用于字段白名单检查
#include <initializer_list>

// 整数极限值
#include <limits>

namespace hwtest::biz {
namespace { // 匿名命名空间 - 内部辅助函数，仅本编译单元可见

// ---------------------------------------------------------------------------
// 构造带有错误码和消息的 Status 对象
// ---------------------------------------------------------------------------
Status makeStatus(ErrorCode code, const QString& message)
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

// ---------------------------------------------------------------------------
// 构造失败的 Result<T>
// ---------------------------------------------------------------------------
template <typename T>
Result<T> failure(ErrorCode code, const QString& message)
{
    Result<T> result;
    result.status = makeStatus(code, message);
    return result;
}

// ---------------------------------------------------------------------------
// 枚举 ↔ 字符串互转
// ---------------------------------------------------------------------------

// 比较运算符枚举 → 字符串名
QString criterionOperationName(CmpOp operation)
{
    switch (operation) {
    case CmpOp::GreaterThan:    return QStringLiteral("GreaterThan");
    case CmpOp::GreaterOrEqual: return QStringLiteral("GreaterOrEqual");
    case CmpOp::LessThan:       return QStringLiteral("LessThan");
    case CmpOp::LessOrEqual:    return QStringLiteral("LessOrEqual");
    case CmpOp::Equal:          return QStringLiteral("Equal");
    case CmpOp::NotEqual:       return QStringLiteral("NotEqual");
    case CmpOp::InRange:        return QStringLiteral("InRange");
    }
    return {};
}

// 字符串名 → 比较运算符枚举（大小写不敏感）
bool criterionOperationFromName(const QString& name, CmpOp& operation)
{
    const QString normalized = name.trimmed();
    const CmpOp values[] = {CmpOp::GreaterThan, CmpOp::GreaterOrEqual,
                            CmpOp::LessThan,    CmpOp::LessOrEqual,
                            CmpOp::Equal,       CmpOp::NotEqual,
                            CmpOp::InRange};
    for (const CmpOp candidate : values) {
        if (normalized.compare(criterionOperationName(candidate), Qt::CaseInsensitive) == 0) {
            operation = candidate;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// JSON 字段白名单检查
// ---------------------------------------------------------------------------

// 检查 key 是否在白名单内
bool isAllowed(const QString& key, std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        if (key == QLatin1String(name)) {
            return true;
        }
    }
    return false;
}

// 检查 JSON 对象中是否包含未知字段
// 若发现未知字段则将错误信息写入 error 并返回 false
bool rejectUnknown(const QJsonObject& object,
                   std::initializer_list<const char*> names,
                   const QString& scope,
                   QString& error)
{
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!isAllowed(it.key(), names)) {
            error = QStringLiteral("Unknown field '%1' in %2").arg(it.key(), scope);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 基本类型的 JSON 读取辅助函数（可选字段，不存在时保留默认值）
// ---------------------------------------------------------------------------

bool optionalString(const QJsonObject& object, const char* name, QString& value,
                    const QString& scope, QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) return true; // 字段不存在则跳过
    if (!json.isString()) {
        error = QStringLiteral("Field '%1' in %2 must be a string").arg(QLatin1String(name), scope);
        return false;
    }
    value = json.toString();
    return true;
}

bool optionalBool(const QJsonObject& object, const char* name, bool& value,
                  const QString& scope, QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) return true;
    if (!json.isBool()) {
        error = QStringLiteral("Field '%1' in %2 must be a boolean").arg(QLatin1String(name), scope);
        return false;
    }
    value = json.toBool();
    return true;
}

// 验证 JSON 数值是否为 [minimum, maximum] 范围内的整数
bool jsonInteger(const QJsonValue& json, qint64 minimum, qint64 maximum, qint64& value)
{
    if (!json.isDouble()) return false;
    const double number = json.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        return false;
    }
    value = static_cast<qint64>(number);
    return true;
}

bool optionalInt(const QJsonObject& object, const char* name, int& value,
                 const QString& scope, QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) return true;
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

bool optionalInt64(const QJsonObject& object, const char* name, qint64& value,
                   const QString& scope, QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) return true;
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

bool optionalDouble(const QJsonObject& object, const char* name, double& value,
                    const QString& scope, QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) return true;
    if (!json.isDouble() || !std::isfinite(json.toDouble())) {
        error = QStringLiteral("Field '%1' in %2 must be a number").arg(QLatin1String(name), scope);
        return false;
    }
    value = json.toDouble();
    return true;
}

// ---------------------------------------------------------------------------
// JSON ↔ QVariant 互转
// 整数会保留为 int/qint64，避免全部变成 double
// ---------------------------------------------------------------------------

QVariant variantFromJson(const QJsonValue& value);

QVariantMap mapFromJson(const QJsonObject& object)
{
    QVariantMap result;
    for (auto it = object.begin(); it != object.end(); ++it) {
        result.insert(it.key(), variantFromJson(it.value()));
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
    if (value.isObject()) return mapFromJson(value.toObject());
    if (value.isArray())  return listFromJson(value.toArray());
    if (value.isBool())   return value.toBool();
    if (value.isString()) return value.toString();
    if (value.isDouble()) {
        const double number = value.toDouble();
        // 如果是整数值且在 int 范围内 → int
        if (std::isfinite(number) && std::floor(number) == number) {
            if (number >= std::numeric_limits<int>::min() &&
                number <= std::numeric_limits<int>::max()) {
                return static_cast<int>(number);
            }
            if (number >= std::numeric_limits<qint64>::min() &&
                number <= std::numeric_limits<qint64>::max()) {
                return static_cast<qint64>(number);
            }
        }
        return number;
    }
    return QVariant();
}

bool optionalMap(const QJsonObject& object, const char* name, QVariantMap& value,
                 const QString& scope, QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) return true;
    if (!json.isObject()) {
        error = QStringLiteral("Field '%1' in %2 must be an object").arg(QLatin1String(name), scope);
        return false;
    }
    value = mapFromJson(json.toObject());
    return true;
}

bool optionalStringList(const QJsonObject& object, const char* name, QList<QString>& value,
                        const QString& scope, QString& error)
{
    const QJsonValue json = object.value(QLatin1String(name));
    if (json.isUndefined()) return true;
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

// QStringList → QJsonArray
QJsonArray stringArray(const QList<QString>& values)
{
    QJsonArray array;
    for (const QString& value : values) {
        array.append(value);
    }
    return array;
}

// ---------------------------------------------------------------------------
// 各结构体的 JSON ↔ 对象 序列化/反序列化
// ---------------------------------------------------------------------------

// ---- Criterion ----
QJsonObject criterionToJson(const Criterion& c)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("metric"), c.metric);
    obj.insert(QStringLiteral("op"), criterionOperationName(c.op));
    obj.insert(QStringLiteral("ref"), QJsonValue::fromVariant(c.ref));
    obj.insert(QStringLiteral("lo"), c.lo);
    obj.insert(QStringLiteral("hi"), c.hi);
    obj.insert(QStringLiteral("tol"), c.tol);
    obj.insert(QStringLiteral("passIfMatched"), c.passIfMatched);
    return obj;
}

bool criterionFromJson(const QJsonValue& json, Criterion& c, QString& error)
{
    if (!json.isObject()) {
        error = QStringLiteral("Each criterion must be an object");
        return false;
    }
    const QJsonObject obj = json.toObject();
    if (!rejectUnknown(obj, {"metric","op","ref","lo","hi","tol","passIfMatched"},
                       QStringLiteral("criterion"), error) ||
        !optionalString(obj, "metric", c.metric, QStringLiteral("criterion"), error) ||
        !optionalDouble(obj, "lo", c.lo, QStringLiteral("criterion"), error) ||
        !optionalDouble(obj, "hi", c.hi, QStringLiteral("criterion"), error) ||
        !optionalDouble(obj, "tol", c.tol, QStringLiteral("criterion"), error) ||
        !optionalBool(obj, "passIfMatched", c.passIfMatched, QStringLiteral("criterion"), error)) {
        return false;
    }

    const QJsonValue opJson = obj.value(QStringLiteral("op"));
    if (!opJson.isUndefined()) {
        if (opJson.isString()) {
            if (!criterionOperationFromName(opJson.toString(), c.op)) {
                error = QStringLiteral("Field 'op' in criterion has an unknown value");
                return false;
            }
        } else {
            qint64 opInt = 0;
            if (!jsonInteger(opJson, static_cast<int>(CmpOp::GreaterThan),
                             static_cast<int>(CmpOp::InRange), opInt)) {
                error = QStringLiteral("Field 'op' in criterion must be a known string or integer");
                return false;
            }
            c.op = static_cast<CmpOp>(opInt);
        }
    }

    const QJsonValue refJson = obj.value(QStringLiteral("ref"));
    if (!refJson.isUndefined()) {
        if (refJson.isObject() || refJson.isArray()) {
            error = QStringLiteral("Field 'ref' in criterion must be a scalar value");
            return false;
        }
        c.ref = variantFromJson(refJson);
    }
    return true;
}

// ---- TestStep ----
QJsonObject stepToJson(const TestStep& step)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("stepId"), step.stepId);
    obj.insert(QStringLiteral("testItemId"), step.testItemId);
    obj.insert(QStringLiteral("name"), step.name);
    obj.insert(QStringLiteral("type"), step.type);
    obj.insert(QStringLiteral("board"), step.board);
    obj.insert(QStringLiteral("algorithmId"), step.algorithmId);
    obj.insert(QStringLiteral("parameters"), QJsonObject::fromVariantMap(step.parameters));
    obj.insert(QStringLiteral("timeoutMs"), step.timeoutMs);
    obj.insert(QStringLiteral("retryCount"), step.retryCount);
    obj.insert(QStringLiteral("enabled"), step.enabled);
    obj.insert(QStringLiteral("dependsOn"), stringArray(step.dependsOn));
    QJsonArray criteria;
    for (const Criterion& c : step.criteria)
        criteria.append(criterionToJson(c));
    obj.insert(QStringLiteral("criteria"), criteria);
    return obj;
}

bool stepFromJson(const QJsonValue& json, TestStep& step, QString& error)
{
    if (!json.isObject()) {
        error = QStringLiteral("Each step must be an object");
        return false;
    }
    const QJsonObject obj = json.toObject();
    const QString scope = QStringLiteral("step");
    if (!rejectUnknown(obj,
                       {"stepId","testItemId","name","type","board","algorithmId",
                        "parameters","timeoutMs","retryCount","enabled","dependsOn","criteria"},
                       scope, error) ||
        !optionalString(obj, "stepId", step.stepId, scope, error) ||
        !optionalString(obj, "testItemId", step.testItemId, scope, error) ||
        !optionalString(obj, "name", step.name, scope, error) ||
        !optionalString(obj, "type", step.type, scope, error) ||
        !optionalString(obj, "board", step.board, scope, error) ||
        !optionalString(obj, "algorithmId", step.algorithmId, scope, error) ||
        !optionalMap(obj, "parameters", step.parameters, scope, error) ||
        !optionalInt(obj, "timeoutMs", step.timeoutMs, scope, error) ||
        !optionalInt(obj, "retryCount", step.retryCount, scope, error) ||
        !optionalBool(obj, "enabled", step.enabled, scope, error) ||
        !optionalStringList(obj, "dependsOn", step.dependsOn, scope, error)) {
        return false;
    }

    const QJsonValue criteriaJson = obj.value(QStringLiteral("criteria"));
    if (criteriaJson.isUndefined()) return true;
    if (!criteriaJson.isArray()) {
        error = QStringLiteral("Field 'criteria' in step must be an array");
        return false;
    }
    QList<Criterion> criteria;
    for (const QJsonValue& cj : criteriaJson.toArray()) {
        Criterion c;
        if (!criterionFromJson(cj, c, error)) return false;
        criteria.append(c);
    }
    step.criteria = criteria;
    return true;
}

// ---- HardwareRequirement ----
QJsonObject requirementToJson(const HardwareRequirement& r)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("requirementId"), r.requirementId);
    obj.insert(QStringLiteral("deviceId"), r.deviceId);
    obj.insert(QStringLiteral("adapterId"), r.adapterId);
    obj.insert(QStringLiteral("requiredModules"), stringArray(r.requiredModules));
    obj.insert(QStringLiteral("requiredResources"), stringArray(r.requiredResources));
    obj.insert(QStringLiteral("priority"), r.priority);
    obj.insert(QStringLiteral("match"), QJsonObject::fromVariantMap(r.match));
    return obj;
}

bool requirementFromJson(const QJsonValue& json, HardwareRequirement& r, QString& error)
{
    if (!json.isObject()) {
        error = QStringLiteral("Each resource requirement must be an object");
        return false;
    }
    const QJsonObject obj = json.toObject();
    const QString scope = QStringLiteral("resource requirement");
    return rejectUnknown(obj,
                         {"requirementId","deviceId","adapterId","requiredModules",
                          "requiredResources","priority","match"},
                         scope, error) &&
           optionalString(obj, "requirementId", r.requirementId, scope, error) &&
           optionalString(obj, "deviceId", r.deviceId, scope, error) &&
           optionalString(obj, "adapterId", r.adapterId, scope, error) &&
           optionalStringList(obj, "requiredModules", r.requiredModules, scope, error) &&
           optionalStringList(obj, "requiredResources", r.requiredResources, scope, error) &&
           optionalInt(obj, "priority", r.priority, scope, error) &&
           optionalMap(obj, "match", r.match, scope, error);
}

// ---- ProtocolProfile ----
QJsonObject profileToJson(const ProtocolProfile& p)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), p.id);
    obj.insert(QStringLiteral("busType"), p.busType);
    obj.insert(QStringLiteral("payloadEncoding"), p.payloadEncoding);
    obj.insert(QStringLiteral("frameFormat"), QJsonObject::fromVariantMap(p.frameFormat));
    obj.insert(QStringLiteral("timing"), QJsonObject::fromVariantMap(p.timing));
    obj.insert(QStringLiteral("responseRules"), QJsonObject::fromVariantMap(p.responseRules));
    obj.insert(QStringLiteral("fieldMapping"), QJsonObject::fromVariantMap(p.fieldMapping));
    return obj;
}

bool profileFromJson(const QJsonValue& json, ProtocolProfile& p, QString& error)
{
    if (!json.isObject()) {
        error = QStringLiteral("Each profile must be an object");
        return false;
    }
    const QJsonObject obj = json.toObject();
    const QString scope = QStringLiteral("profile");
    return rejectUnknown(obj,
                         {"id","busType","payloadEncoding","frameFormat","timing",
                          "responseRules","fieldMapping"},
                         scope, error) &&
           optionalString(obj, "id", p.id, scope, error) &&
           optionalString(obj, "busType", p.busType, scope, error) &&
           optionalString(obj, "payloadEncoding", p.payloadEncoding, scope, error) &&
           optionalMap(obj, "frameFormat", p.frameFormat, scope, error) &&
           optionalMap(obj, "timing", p.timing, scope, error) &&
           optionalMap(obj, "responseRules", p.responseRules, scope, error) &&
           optionalMap(obj, "fieldMapping", p.fieldMapping, scope, error);
}

// ---- SafetyPolicy ----
QJsonObject safetyToJson(const SafetyPolicy& s)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("outputLimits"), QJsonObject::fromVariantMap(s.outputLimits));
    obj.insert(QStringLiteral("safeState"), QJsonObject::fromVariantMap(s.safeState));
    obj.insert(QStringLiteral("enterSafeStateOnStop"), s.enterSafeStateOnStop);
    obj.insert(QStringLiteral("enterSafeStateOnError"), s.enterSafeStateOnError);
    obj.insert(QStringLiteral("daMinVoltage"), s.daMinVoltage);
    obj.insert(QStringLiteral("daMaxVoltage"), s.daMaxVoltage);
    obj.insert(QStringLiteral("doMinSwitchIntervalMs"), s.doMinSwitchIntervalMs);
    obj.insert(QStringLiteral("canSendMaxHz"), s.canSendMaxHz);
    obj.insert(QStringLiteral("resourceLockTimeoutMs"), s.resourceLockTimeoutMs);
    return obj;
}

bool safetyFromJson(const QJsonValue& json, SafetyPolicy& s, QString& error)
{
    if (json.isUndefined()) return true;
    if (!json.isObject()) {
        error = QStringLiteral("Field 'safetyPolicy' must be an object");
        return false;
    }
    const QJsonObject obj = json.toObject();
    const QString scope = QStringLiteral("safety policy");
    return rejectUnknown(obj,
                         {"outputLimits","safeState","enterSafeStateOnStop",
                          "enterSafeStateOnError","daMinVoltage","daMaxVoltage",
                          "doMinSwitchIntervalMs","canSendMaxHz","resourceLockTimeoutMs"},
                         scope, error) &&
           optionalMap(obj, "outputLimits", s.outputLimits, scope, error) &&
           optionalMap(obj, "safeState", s.safeState, scope, error) &&
           optionalBool(obj, "enterSafeStateOnStop", s.enterSafeStateOnStop, scope, error) &&
           optionalBool(obj, "enterSafeStateOnError", s.enterSafeStateOnError, scope, error) &&
           optionalDouble(obj, "daMinVoltage", s.daMinVoltage, scope, error) &&
           optionalDouble(obj, "daMaxVoltage", s.daMaxVoltage, scope, error) &&
           optionalInt(obj, "doMinSwitchIntervalMs", s.doMinSwitchIntervalMs, scope, error) &&
           optionalInt(obj, "canSendMaxHz", s.canSendMaxHz, scope, error) &&
           optionalInt(obj, "resourceLockTimeoutMs", s.resourceLockTimeoutMs, scope, error);
}

// ---- RuntimeConfig ----
QJsonObject runtimeToJson(const RuntimeConfig& r)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("parallelEnabled"), r.parallelEnabled);
    obj.insert(QStringLiteral("maxParallel"), r.maxParallel);
    obj.insert(QStringLiteral("defaultTimeoutMs"), r.defaultTimeoutMs);
    obj.insert(QStringLiteral("defaultRetryCount"), r.defaultRetryCount);
    obj.insert(QStringLiteral("retryIntervalMs"), r.retryIntervalMs);
    obj.insert(QStringLiteral("taskPriorityDefault"), r.taskPriorityDefault);
    obj.insert(QStringLiteral("pauseAutoReleaseMs"), r.pauseAutoReleaseMs);
    obj.insert(QStringLiteral("stopOnFirstFailure"), r.stopOnFirstFailure);
    obj.insert(QStringLiteral("allowResume"), r.allowResume);
    obj.insert(QStringLiteral("reportDir"), r.reportDir);
    obj.insert(QStringLiteral("logDir"), r.logDir);
    obj.insert(QStringLiteral("logRotateBytes"), static_cast<double>(r.logRotateBytes));
    obj.insert(QStringLiteral("logKeepFiles"), r.logKeepFiles);
    obj.insert(QStringLiteral("tags"), QJsonObject::fromVariantMap(r.tags));
    return obj;
}

bool runtimeFromJson(const QJsonValue& json, RuntimeConfig& r, QString& error)
{
    if (json.isUndefined()) return true;
    if (!json.isObject()) {
        error = QStringLiteral("Field 'runtimeConfig' must be an object");
        return false;
    }
    const QJsonObject obj = json.toObject();
    const QString scope = QStringLiteral("runtime configuration");
    return rejectUnknown(obj,
                         {"parallelEnabled","maxParallel","defaultTimeoutMs",
                          "defaultRetryCount","retryIntervalMs","taskPriorityDefault",
                          "pauseAutoReleaseMs","stopOnFirstFailure","allowResume",
                          "reportDir","logDir","logRotateBytes","logKeepFiles","tags"},
                         scope, error) &&
           optionalBool(obj, "parallelEnabled", r.parallelEnabled, scope, error) &&
           optionalInt(obj, "maxParallel", r.maxParallel, scope, error) &&
           optionalInt(obj, "defaultTimeoutMs", r.defaultTimeoutMs, scope, error) &&
           optionalInt(obj, "defaultRetryCount", r.defaultRetryCount, scope, error) &&
           optionalInt(obj, "retryIntervalMs", r.retryIntervalMs, scope, error) &&
           optionalInt(obj, "taskPriorityDefault", r.taskPriorityDefault, scope, error) &&
           optionalInt(obj, "pauseAutoReleaseMs", r.pauseAutoReleaseMs, scope, error) &&
           optionalBool(obj, "stopOnFirstFailure", r.stopOnFirstFailure, scope, error) &&
           optionalBool(obj, "allowResume", r.allowResume, scope, error) &&
           optionalString(obj, "reportDir", r.reportDir, scope, error) &&
           optionalString(obj, "logDir", r.logDir, scope, error) &&
           optionalInt64(obj, "logRotateBytes", r.logRotateBytes, scope, error) &&
           optionalInt(obj, "logKeepFiles", r.logKeepFiles, scope, error) &&
           optionalMap(obj, "tags", r.tags, scope, error);
}

// ---- TestConfig ----
QJsonObject configToJson(const TestConfig& config)
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), config.schemaVersion);
    root.insert(QStringLiteral("configId"), config.configId);
    root.insert(QStringLiteral("productModel"), config.productModel);
    root.insert(QStringLiteral("productName"), config.productName);
    root.insert(QStringLiteral("configVersion"), config.configVersion);

    QJsonArray steps;
    for (const TestStep& step : config.steps)
        steps.append(stepToJson(step));
    root.insert(QStringLiteral("steps"), steps);

    QJsonArray requirements;
    for (const HardwareRequirement& r : config.hardwareRequirements)
        requirements.append(requirementToJson(r));
    root.insert(QStringLiteral("hardwareRequirements"), requirements);

    QJsonArray profiles;
    for (const ProtocolProfile& p : config.protocolProfiles)
        profiles.append(profileToJson(p));
    root.insert(QStringLiteral("protocolProfiles"), profiles);

    root.insert(QStringLiteral("executionConfig"),
                QJsonObject::fromVariantMap(config.executionConfig));
    root.insert(QStringLiteral("safetyPolicy"), safetyToJson(config.safetyPolicy));
    root.insert(QStringLiteral("runtimeConfig"), runtimeToJson(config.runtimeConfig));
    root.insert(QStringLiteral("reportFields"),
                QJsonObject::fromVariantMap(config.reportFields));
    return root;
}

bool configFromJson(const QJsonObject& root, TestConfig& config, QString& error)
{
    if (!rejectUnknown(root,
                       {"schemaVersion","configId","productModel","productName",
                        "configVersion","steps","hardwareRequirements","protocolProfiles",
                        "executionConfig","halConfig","safetyPolicy","runtimeConfig","reportFields"},
                       QStringLiteral("configuration"), error)) {
        return false;
    }

    // executionConfig 和 halConfig 不能同时存在（旧兼容字段 halConfig）
    if (root.contains(QStringLiteral("executionConfig")) &&
        root.contains(QStringLiteral("halConfig"))) {
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

    // 解析 steps
    const QJsonValue stepsJson = root.value(QStringLiteral("steps"));
    if (!stepsJson.isUndefined()) {
        if (!stepsJson.isArray()) {
            error = QStringLiteral("Field 'steps' in configuration must be an array");
            return false;
        }
        QVector<TestStep> steps;
        steps.reserve(stepsJson.toArray().size());
        for (const QJsonValue& v : stepsJson.toArray()) {
            TestStep step;
            if (!stepFromJson(v, step, error)) return false;
            steps.append(step);
        }
        config.steps = steps;
    }

    // 解析 hardwareRequirements
    const QJsonValue reqJson = root.value(QStringLiteral("hardwareRequirements"));
    if (!reqJson.isUndefined()) {
        if (!reqJson.isArray()) {
            error = QStringLiteral("Field 'hardwareRequirements' in configuration must be an array");
            return false;
        }
        QVector<HardwareRequirement> reqs;
        reqs.reserve(reqJson.toArray().size());
        for (const QJsonValue& v : reqJson.toArray()) {
            HardwareRequirement r;
            if (!requirementFromJson(v, r, error)) return false;
            reqs.append(r);
        }
        config.hardwareRequirements = reqs;
    }

    // 解析 protocolProfiles
    const QJsonValue profilesJson = root.value(QStringLiteral("protocolProfiles"));
    if (!profilesJson.isUndefined()) {
        if (!profilesJson.isArray()) {
            error = QStringLiteral("Field 'protocolProfiles' in configuration must be an array");
            return false;
        }
        QVector<ProtocolProfile> profiles;
        profiles.reserve(profilesJson.toArray().size());
        for (const QJsonValue& v : profilesJson.toArray()) {
            ProtocolProfile p;
            if (!profileFromJson(v, p, error)) return false;
            profiles.append(p);
        }
        config.protocolProfiles = profiles;
    }

    // 兼容：executionConfig 或 halConfig
    const char* executionKey = root.contains(QStringLiteral("executionConfig"))
        ? "executionConfig" : "halConfig";
    if (!optionalMap(root, executionKey, config.executionConfig,
                     QStringLiteral("configuration"), error) ||
        !safetyFromJson(root.value(QStringLiteral("safetyPolicy")),
                        config.safetyPolicy, error) ||
        !runtimeFromJson(root.value(QStringLiteral("runtimeConfig")),
                         config.runtimeConfig, error) ||
        !optionalMap(root, "reportFields", config.reportFields,
                     QStringLiteral("configuration"), error)) {
        return false;
    }
    return true;
}

} // 匿名命名空间结束

// ========================================================================
// TestConfigManager 成员函数实现
// ========================================================================

// 从文件加载并解析配置
Result<TestConfig> TestConfigManager::load(const ConfigPath& filePath) const
{
    // 打开文件
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure<TestConfig>(ErrorCode::ConfigParseError,
                                   QStringLiteral("Cannot open configuration '%1': %2")
                                       .arg(filePath, file.errorString()));
    }

    return parse(file.readAll(), filePath);
}

// 从内存中的 JSON 内容解析配置，供保存前的非破坏性校验复用
Result<TestConfig> TestConfigManager::parse(const QByteArray& contents,
                                            const QString& sourceName) const
{
    const QString source = sourceName.trimmed().isEmpty()
        ? QStringLiteral("configuration")
        : sourceName;

    // 解析 JSON
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        const QString reason = (parseError.error == QJsonParseError::NoError)
            ? QStringLiteral("root must be an object") : parseError.errorString();
        return failure<TestConfig>(ErrorCode::ConfigParseError,
                                   QStringLiteral("Cannot parse configuration '%1': %2")
                                       .arg(source, reason));
    }

    // JSON → TestConfig
    TestConfig config;
    QString error;
    if (!configFromJson(doc.object(), config, error)) {
        return failure<TestConfig>(ErrorCode::ConfigSchemaError, error);
    }

    // 验证
    const Result<QVector<QString>> validation = validate(config);
    if (!validation.ok()) {
        return failure<TestConfig>(validation.status.code, validation.status.error.message);
    }
    return Result<TestConfig>{Status{}, config};
}

// 保存配置到文件（原子写入）
Status TestConfigManager::save(const ConfigPath& filePath, const TestConfig& config) const
{
    // 先验证
    const Result<QVector<QString>> validation = validate(config);
    if (!validation.ok()) return validation.status;

    if (filePath.trimmed().isEmpty()) {
        return makeStatus(ErrorCode::ParameterRangeError,
                          QStringLiteral("Configuration path must not be empty"));
    }

    // 确保目录存在
    const QFileInfo fileInfo(filePath);
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        return makeStatus(ErrorCode::ConfigParseError,
                          QStringLiteral("Cannot create configuration directory '%1'")
                              .arg(fileInfo.absolutePath()));
    }

    // 原子写入（QSaveFile：先写临时文件，成功后再 rename）
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return makeStatus(ErrorCode::ConfigParseError,
                          QStringLiteral("Cannot write configuration '%1': %2")
                              .arg(filePath, file.errorString()));
    }
    const QByteArray serialized = QJsonDocument(configToJson(config))
                                      .toJson(QJsonDocument::Indented);
    if (file.write(serialized) != serialized.size() || !file.commit()) {
        return makeStatus(ErrorCode::ConfigParseError,
                          QStringLiteral("Cannot save configuration '%1': %2")
                              .arg(filePath, file.errorString()));
    }
    return Status{};
}

// 验证配置的完整性
Result<QVector<QString>> TestConfigManager::validate(const TestConfig& config) const
{
    // 必填标识字段 + 至少一个步骤
    if (config.schemaVersion.trimmed().isEmpty() || config.configId.trimmed().isEmpty() ||
        config.productModel.trimmed().isEmpty() || config.configVersion.trimmed().isEmpty() ||
        config.steps.isEmpty()) {
        return failure<QVector<QString>>(
            ErrorCode::ConfigSchemaError,
            QStringLiteral("Configuration identity and at least one step are required"));
    }

    QSet<StepId> stepIds;
    QSet<TestItemId> itemIds;
    for (const TestStep& step : config.steps) {
        // 步骤必填字段
        if (step.stepId.trimmed().isEmpty() || step.testItemId.trimmed().isEmpty() ||
            step.algorithmId.trimmed().isEmpty()) {
            return failure<QVector<QString>>(
                ErrorCode::ConfigSchemaError,
                QStringLiteral("Every step requires stepId, testItemId and algorithmId"));
        }
        // ID 唯一性
        if (stepIds.contains(step.stepId) || itemIds.contains(step.testItemId)) {
            return failure<QVector<QString>>(
                ErrorCode::ConfigSchemaError,
                QStringLiteral("Step and test item identifiers must be unique"));
        }
        stepIds.insert(step.stepId);
        itemIds.insert(step.testItemId);

        // 范围检查
        if (step.timeoutMs < 0) {
            return failure<QVector<QString>>(
                ErrorCode::ParameterRangeError,
                QStringLiteral("Step '%1' has a negative timeout").arg(step.stepId));
        }
        if (step.retryCount < -1) {
            return failure<QVector<QString>>(
                ErrorCode::ParameterRangeError,
                QStringLiteral("Step '%1' has an invalid retry count").arg(step.stepId));
        }
        // 每个 criterion 验证
        for (const Criterion& c : step.criteria) {
            if (c.metric.trimmed().isEmpty() || criterionOperationName(c.op).isEmpty()) {
                return failure<QVector<QString>>(
                    ErrorCode::ConfigSchemaError,
                    QStringLiteral("Criterion metric or operation is invalid"));
            }
            const QJsonValue ref = QJsonValue::fromVariant(c.ref);
            if (ref.isUndefined() || ref.isArray() || ref.isObject()) {
                return failure<QVector<QString>>(
                    ErrorCode::ConfigSchemaError,
                    QStringLiteral("Criterion reference must be a JSON scalar"));
            }
        }
    }

    // 运行时配置范围检查
    const RuntimeConfig& rt = config.runtimeConfig;
    if (rt.maxParallel < 0 || rt.defaultTimeoutMs < 0 || rt.defaultRetryCount < 0 ||
        rt.retryIntervalMs < 0 || rt.pauseAutoReleaseMs < 0 || rt.logRotateBytes < 0 ||
        rt.logKeepFiles < 0 || rt.taskPriorityDefault < 1 || rt.taskPriorityDefault > 3) {
        return failure<QVector<QString>>(
            ErrorCode::ParameterRangeError,
            QStringLiteral("Runtime configuration contains a negative limit"));
    }

    // 安全策略范围检查
    const SafetyPolicy& sp = config.safetyPolicy;
    if (sp.doMinSwitchIntervalMs < 0 || sp.canSendMaxHz < 0 ||
        sp.resourceLockTimeoutMs < 0 || sp.daMaxVoltage < sp.daMinVoltage) {
        return failure<QVector<QString>>(
            ErrorCode::ParameterRangeError,
            QStringLiteral("Safety policy contains an invalid range"));
    }

    return Result<QVector<QString>>{Status{}, {}};
}

} // namespace hwtest::biz
