#include "configuration_service.h"

#include "mbddf_algorithm_registry.h"
#include "run_mode_capabilities.h"

#include <algorithm/run_parameter_schema.h>
#include <biz/test_config_manager.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QMetaType>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace hwtest::app {
namespace {

constexpr auto kCatalogDocumentId = "test-config-catalog";
constexpr auto kStationDocumentId = "mbddf-station";
constexpr auto kCatalogFileName = "test-config-catalog.json";
constexpr auto kStationFileName = "mbddf_station.json";

ActionResult failure(const QString& code, const QString& message)
{
    return ActionResult{false, code, message};
}

QString revisionFor(const QByteArray& contents)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

QByteArray serialized(const QVariantMap& value)
{
    return QJsonDocument(QJsonObject::fromVariantMap(value))
        .toJson(QJsonDocument::Indented);
}

QVariantMap defaultCatalog()
{
    return QVariantMap{
        {QStringLiteral("schemaVersion"), QStringLiteral("1")},
        {QStringLiteral("entries"), QVariantList{}},
    };
}

QVariantMap defaultStation()
{
    return QVariantMap{
        {QStringLiteral("schemaVersion"), QStringLiteral("1")},
        {QStringLiteral("control"), QVariantMap{}},
        {QStringLiteral("devices"), QVariantMap{}},
        {QStringLiteral("resources"), QVariantMap{}},
    };
}

bool isTestDocumentId(const QString& documentId)
{
    return !documentId.trimmed().isEmpty() &&
        !documentId.contains(QLatin1Char(':')) &&
        QFileInfo(documentId).fileName() == documentId &&
        documentId.endsWith(QStringLiteral(".testcfg.json"),
                            Qt::CaseSensitive);
}

QString documentStoragePath(const QString& configurationDirectory,
                            const QString& documentId)
{
    const QDir directory(configurationDirectory);
    if (documentId == QLatin1String(kCatalogDocumentId)) {
        return directory.filePath(QString::fromLatin1(kCatalogFileName));
    }
    if (documentId == QLatin1String(kStationDocumentId)) {
        return directory.filePath(QString::fromLatin1(kStationFileName));
    }
    return isTestDocumentId(documentId)
        ? directory.filePath(documentId)
        : QString{};
}

ActionResult readFileContents(const QString& path, QByteArray* output)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("config_not_found"),
                       QStringLiteral("Cannot open configuration '%1': %2")
                           .arg(path, file.errorString()));
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return failure(QStringLiteral("config_not_found"),
                       QStringLiteral("Cannot read configuration '%1': %2")
                           .arg(path, file.errorString()));
    }
    if (output != nullptr) *output = bytes;
    return {};
}

ActionResult readJsonObject(const QString& path,
                            QVariantMap* output,
                            QByteArray* contents = nullptr)
{
    QByteArray bytes;
    const ActionResult read = readFileContents(path, &bytes);
    if (!read.ok) return read;
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        const QString reason = parseError.error == QJsonParseError::NoError
            ? QStringLiteral("root must be an object")
            : parseError.errorString();
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("Cannot parse configuration '%1': %2")
                           .arg(path, reason));
    }
    if (output != nullptr) *output = parsed.object().toVariantMap();
    if (contents != nullptr) *contents = bytes;
    return {};
}

ActionResult readOptionalJsonObject(const QString& path,
                                    const QVariantMap& fallback,
                                    QVariantMap* output,
                                    QByteArray* contents)
{
    if (!QFileInfo::exists(path)) {
        const QByteArray bytes = serialized(fallback);
        if (output != nullptr) *output = fallback;
        if (contents != nullptr) *contents = bytes;
        return {};
    }
    return readJsonObject(path, output, contents);
}

bool jsonInteger(const QVariant& value, int* output)
{
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(std::numeric_limits<int>::min()) ||
        number > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (output != nullptr) *output = static_cast<int>(number);
    return true;
}

struct CatalogSetting {
    bool enabled = false;
    int order = std::numeric_limits<int>::max();
};

ActionResult parseCatalog(const QVariantMap& root,
                          QMap<QString, CatalogSetting>* output)
{
    if (root.value(QStringLiteral("schemaVersion")).toString() !=
        QStringLiteral("1")) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("Catalog schemaVersion must be '1'"));
    }
    const QVariant entriesValue = root.value(QStringLiteral("entries"));
    if (entriesValue.userType() != QMetaType::QVariantList) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("Catalog entries must be an array"));
    }

    QMap<QString, CatalogSetting> parsed;
    for (const QVariant& entryValue : entriesValue.toList()) {
        if (entryValue.userType() != QMetaType::QVariantMap) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("Each catalog entry must be an object"));
        }
        const QVariantMap entry = entryValue.toMap();
        const QString documentId = entry.value(QStringLiteral("documentId"))
                                       .toString().trimmed();
        int order = 0;
        if (!isTestDocumentId(documentId) || parsed.contains(documentId) ||
            entry.value(QStringLiteral("enabled")).userType() != QMetaType::Bool ||
            !jsonInteger(entry.value(QStringLiteral("order")), &order)) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("Catalog entries require unique test documentId, boolean enabled and integer order"));
        }
        parsed.insert(documentId,
                      CatalogSetting{entry.value(QStringLiteral("enabled")).toBool(),
                                     order});
    }
    if (output != nullptr) *output = parsed;
    return {};
}

ActionResult describeTestConfig(const QString& path,
                                ConfigurationCatalogItem* output)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(path);
    if (!loaded.ok()) {
        return failure(QStringLiteral("config_invalid"),
                       loaded.status.error.message);
    }

    QVector<QString> runModes;
    const QString runModeError = parseSupportedRunModes(
        loaded.value.reportFields, &runModes);
    if (!runModeError.isEmpty()) {
        return failure(QStringLiteral("config_invalid"), runModeError);
    }
    bool stoppable = true;
    const QString stoppableError = parseStoppableCapability(
        loaded.value.reportFields, runModes, &stoppable);
    if (!stoppableError.isEmpty()) {
        return failure(QStringLiteral("config_invalid"), stoppableError);
    }

    const hwtest::biz::TestStep* selectedStep = nullptr;
    for (const hwtest::biz::TestStep& step : loaded.value.steps) {
        if (!step.enabled) continue;
        if (selectedStep != nullptr || !isSupportedMbdDfAlgorithm(step.algorithmId)) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("Exactly one supported MB_DDF step must be enabled"));
        }
        selectedStep = &step;
    }
    if (selectedStep == nullptr) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("Exactly one supported MB_DDF step must be enabled"));
    }

    if (output != nullptr) {
        output->configId = loaded.value.configId;
        output->title = loaded.value.reportFields.value(QStringLiteral("title"))
                            .toString().trimmed();
        if (output->title.isEmpty()) output->title = selectedStep->name.trimmed();
        if (output->title.isEmpty()) output->title = selectedStep->testItemId;
        output->description = loaded.value.reportFields
                                  .value(QStringLiteral("description"))
                                  .toString().trimmed();
        output->algorithmId = selectedStep->algorithmId;
    }
    return {};
}

ActionResult validateImmutableTestFields(const QVariantMap& current,
                                         const QVariantMap& replacement)
{
    if (current.value(QStringLiteral("schemaVersion")) !=
            replacement.value(QStringLiteral("schemaVersion")) ||
        current.value(QStringLiteral("configId")) !=
            replacement.value(QStringLiteral("configId"))) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("schemaVersion and configId are read-only"));
    }
    const QVariantList currentSteps = current.value(QStringLiteral("steps")).toList();
    const QVariantList replacementSteps = replacement.value(QStringLiteral("steps")).toList();
    if (currentSteps.size() != replacementSteps.size()) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("The step list cannot be structurally changed"));
    }
    for (int index = 0; index < currentSteps.size(); ++index) {
        const QVariantMap currentStep = currentSteps.at(index).toMap();
        const QVariantMap replacementStep = replacementSteps.at(index).toMap();
        if (currentStep.value(QStringLiteral("algorithmId")) !=
            replacementStep.value(QStringLiteral("algorithmId"))) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("steps[].algorithmId is read-only"));
        }
    }
    return {};
}

ActionResult rejectUnknownKeys(const QVariantMap& value,
                               const QSet<QString>& allowed,
                               const QString& scope)
{
    for (auto it = value.cbegin(); it != value.cend(); ++it) {
        if (!allowed.contains(it.key())) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("Unknown station field '%1' in %2")
                               .arg(it.key(), scope));
        }
    }
    return {};
}

ActionResult objectValue(const QVariantMap& parent,
                         const QString& key,
                         QVariantMap* output)
{
    const QVariant value = parent.value(key);
    if (!value.isValid()) {
        if (output != nullptr) output->clear();
        return {};
    }
    if (value.userType() != QMetaType::QVariantMap) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("Station field '%1' must be an object")
                           .arg(key));
    }
    if (output != nullptr) *output = value.toMap();
    return {};
}

ActionResult stationString(const QVariantMap& patch,
                           const QString& key,
                           QString* output)
{
    const QVariant value = patch.value(key);
    if (value.userType() != QMetaType::QString ||
        value.toString().trimmed().isEmpty()) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("Station field '%1' must be a non-empty string")
                           .arg(key));
    }
    if (output != nullptr) *output = value.toString().trimmed();
    return {};
}

ActionResult applyStationOverlay(const QVariantMap& base,
                                 const QVariantMap& station,
                                 QVariantMap* output)
{
    if (station.value(QStringLiteral("schemaVersion")).toString() !=
        QStringLiteral("1")) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("Station schemaVersion must be '1'"));
    }
    ActionResult result = rejectUnknownKeys(
        station,
        QSet<QString>{QStringLiteral("schemaVersion"),
                      QStringLiteral("control"),
                      QStringLiteral("devices"),
                      QStringLiteral("resources")},
        QStringLiteral("station root"));
    if (!result.ok) return result;

    QVariantMap merged = base;
    QVariantMap hardware = merged.value(QStringLiteral("hardware")).toMap();
    QVariantList devices = hardware.value(QStringLiteral("devices")).toList();
    QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    if (devices.isEmpty() || resources.isEmpty()) {
        return failure(QStringLiteral("config_invalid"),
                       QStringLiteral("Base HAL configuration has no devices or resources"));
    }

    QVariantMap controlPatch;
    result = objectValue(station, QStringLiteral("control"), &controlPatch);
    if (!result.ok) return result;
    result = rejectUnknownKeys(
        controlPatch, QSet<QString>{QStringLiteral("resourceId")},
        QStringLiteral("control"));
    if (!result.ok) return result;
    if (controlPatch.contains(QStringLiteral("resourceId"))) {
        QString resourceId;
        result = stationString(controlPatch, QStringLiteral("resourceId"),
                               &resourceId);
        if (!result.ok) return result;
        const QVariantMap resource = resources.value(resourceId).toMap();
        if (resource.isEmpty() ||
            resource.value(QStringLiteral("module")).toString() !=
                QStringLiteral("control") ||
            resource.value(QStringLiteral("direction")).toString() !=
                QStringLiteral("bidirectional") ||
            resource.value(QStringLiteral("properties")).toMap()
                    .value(QStringLiteral("role")).toString() ==
                QStringLiteral("auxiliary-link")) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("control.resourceId must reference a declared primary control resource"));
        }
        QVariantMap control = merged.value(QStringLiteral("control")).toMap();
        control.insert(QStringLiteral("resourceId"), resourceId);
        merged.insert(QStringLiteral("control"), control);
    }

    QVariantMap devicePatches;
    result = objectValue(station, QStringLiteral("devices"), &devicePatches);
    if (!result.ok) return result;
    for (auto patchIt = devicePatches.cbegin(); patchIt != devicePatches.cend();
         ++patchIt) {
        if (patchIt.value().userType() != QMetaType::QVariantMap ||
            (patchIt.key() != QStringLiteral("ni6259_stimulus") &&
             patchIt.key() != QStringLiteral("ni6733_fixture"))) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("Unknown station device '%1'")
                               .arg(patchIt.key()));
        }
        const QVariantMap patch = patchIt.value().toMap();
        result = rejectUnknownKeys(
            patch,
            QSet<QString>{QStringLiteral("physicalDeviceName"),
                          QStringLiteral("serialNumber")},
            QStringLiteral("devices.%1").arg(patchIt.key()));
        if (!result.ok) return result;

        int deviceIndex = -1;
        for (int index = 0; index < devices.size(); ++index) {
            if (devices.at(index).toMap().value(QStringLiteral("alias")).toString() ==
                patchIt.key()) {
                deviceIndex = index;
                break;
            }
        }
        if (deviceIndex < 0) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("Station device '%1' is not declared by the base HAL configuration")
                               .arg(patchIt.key()));
        }
        QVariantMap device = devices.at(deviceIndex).toMap();
        if (patch.contains(QStringLiteral("serialNumber"))) {
            QString serialNumber;
            result = stationString(patch, QStringLiteral("serialNumber"),
                                   &serialNumber);
            if (!result.ok) return result;
            device.insert(QStringLiteral("serialNumber"), serialNumber);
        }
        if (patch.contains(QStringLiteral("physicalDeviceName"))) {
            QString deviceName;
            result = stationString(patch,
                                   QStringLiteral("physicalDeviceName"),
                                   &deviceName);
            if (!result.ok) return result;
            QVariantMap properties = device.value(QStringLiteral("properties")).toMap();
            QVariantMap vendor = properties.value(QStringLiteral("vendor")).toMap();
            QVariantMap ni = vendor.value(QStringLiteral("ni")).toMap();
            ni.insert(QStringLiteral("deviceName"), deviceName);
            vendor.insert(QStringLiteral("ni"), ni);
            properties.insert(QStringLiteral("vendor"), vendor);
            device.insert(QStringLiteral("properties"), properties);
        }
        devices.replace(deviceIndex, device);
    }

    QVariantMap resourcePatches;
    result = objectValue(station, QStringLiteral("resources"), &resourcePatches);
    if (!result.ok) return result;
    for (auto patchIt = resourcePatches.cbegin(); patchIt != resourcePatches.cend();
         ++patchIt) {
        if (patchIt.value().userType() != QMetaType::QVariantMap ||
            !resources.contains(patchIt.key())) {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("Unknown station resource '%1'")
                               .arg(patchIt.key()));
        }
        QVariantMap resource = resources.value(patchIt.key()).toMap();
        QVariantMap properties = resource.value(QStringLiteral("properties")).toMap();
        const QVariantMap patch = patchIt.value().toMap();
        const QString providerId = resource.value(QStringLiteral("providerId")).toString();
        const QString deviceAlias = resource.value(QStringLiteral("device")).toString();
        const QString module = resource.value(QStringLiteral("module")).toString();
        const QString direction = resource.value(QStringLiteral("direction")).toString();

        if (providerId == QStringLiteral("qt.serial")) {
            result = rejectUnknownKeys(
                patch,
                QSet<QString>{QStringLiteral("portName"),
                              QStringLiteral("baudRate"),
                              QStringLiteral("dataBits"),
                              QStringLiteral("parity"),
                              QStringLiteral("stopBits"),
                              QStringLiteral("flowControl")},
                QStringLiteral("resources.%1").arg(patchIt.key()));
            if (!result.ok) return result;
            for (const QString& key :
                 {QStringLiteral("portName"), QStringLiteral("parity"),
                  QStringLiteral("flowControl")}) {
                if (!patch.contains(key)) continue;
                QString text;
                result = stationString(patch, key, &text);
                if (!result.ok) return result;
                properties.insert(key, text);
            }
            for (const QString& key :
                 {QStringLiteral("baudRate"), QStringLiteral("dataBits")}) {
                if (!patch.contains(key)) continue;
                int number = 0;
                if (!jsonInteger(patch.value(key), &number) || number <= 0) {
                    return failure(QStringLiteral("config_invalid"),
                                   QStringLiteral("Station field '%1' must be a positive integer")
                                       .arg(key));
                }
                properties.insert(key, number);
            }
            if (patch.contains(QStringLiteral("stopBits"))) {
                bool ok = false;
                const double stopBits = patch.value(QStringLiteral("stopBits"))
                                            .toDouble(&ok);
                if (!ok || !std::isfinite(stopBits) || stopBits <= 0.0) {
                    return failure(QStringLiteral("config_invalid"),
                                   QStringLiteral("Station stopBits must be a positive number"));
                }
                properties.insert(QStringLiteral("stopBits"), stopBits);
            }
            resource.insert(QStringLiteral("properties"), properties);
        } else if (deviceAlias == QStringLiteral("ni6259_stimulus") &&
                   module == QStringLiteral("digital")) {
            result = rejectUnknownKeys(
                patch,
                QSet<QString>{QStringLiteral("portNumber"),
                              QStringLiteral("lineNumber")},
                QStringLiteral("resources.%1").arg(patchIt.key()));
            if (!result.ok) return result;
            for (const QString& key :
                 {QStringLiteral("portNumber"), QStringLiteral("lineNumber")}) {
                if (!patch.contains(key)) continue;
                int number = 0;
                if (!jsonInteger(patch.value(key), &number) || number < 0) {
                    return failure(QStringLiteral("config_invalid"),
                                   QStringLiteral("Station field '%1' must be a non-negative integer")
                                       .arg(key));
                }
                properties.insert(key, number);
            }
            const int port = properties.value(QStringLiteral("portNumber")).toInt();
            const int line = properties.value(QStringLiteral("lineNumber")).toInt();
            const bool validLine = (port == 0 && line <= 31) ||
                ((port == 1 || port == 2) && line <= 7);
            if (!validLine) {
                return failure(QStringLiteral("config_invalid"),
                               QStringLiteral("PXI-6259 digital port/line is out of range"));
            }
            resource.insert(QStringLiteral("properties"), properties);
        } else if (module == QStringLiteral("analog") &&
                   ((deviceAlias == QStringLiteral("ni6259_stimulus") &&
                     direction == QStringLiteral("input")) ||
                    (deviceAlias == QStringLiteral("ni6733_fixture") &&
                     direction == QStringLiteral("output")))) {
            result = rejectUnknownKeys(
                patch, QSet<QString>{QStringLiteral("physicalIndex")},
                QStringLiteral("resources.%1").arg(patchIt.key()));
            if (!result.ok) return result;
            int physicalIndex = 0;
            if (!patch.contains(QStringLiteral("physicalIndex")) ||
                !jsonInteger(patch.value(QStringLiteral("physicalIndex")),
                             &physicalIndex) ||
                physicalIndex < 0 ||
                (deviceAlias == QStringLiteral("ni6259_stimulus") &&
                 physicalIndex > 31) ||
                (deviceAlias == QStringLiteral("ni6733_fixture") &&
                 physicalIndex > 7)) {
                return failure(QStringLiteral("config_invalid"),
                               QStringLiteral("Analog physicalIndex is out of range"));
            }
            resource.insert(QStringLiteral("physicalIndex"), physicalIndex);
        } else {
            return failure(QStringLiteral("config_invalid"),
                           QStringLiteral("Resource '%1' has no station-editable hardware fields")
                               .arg(patchIt.key()));
        }
        resources.insert(patchIt.key(), resource);
    }

    hardware.insert(QStringLiteral("devices"), devices);
    hardware.insert(QStringLiteral("resources"), resources);
    merged.insert(QStringLiteral("hardware"), hardware);
    if (output != nullptr) *output = std::move(merged);
    return {};
}

QString escapedJsonPointerSegment(QString value)
{
    value.replace(QLatin1Char('~'), QStringLiteral("~0"));
    value.replace(QLatin1Char('/'), QStringLiteral("~1"));
    return value;
}

QVariantMap formOption(const QVariant& value, const QString& label)
{
    return QVariantMap{{QStringLiteral("value"), value},
                       {QStringLiteral("label"), label}};
}

QVariantMap formField(const QString& path,
                      const QString& label,
                      const QString& kind)
{
    return QVariantMap{{QStringLiteral("path"), path},
                       {QStringLiteral("label"), label},
                       {QStringLiteral("kind"), kind}};
}

QVariantMap formSection(const QString& id,
                        const QString& title,
                        const QString& description = {})
{
    QVariantMap section{{QStringLiteral("id"), id},
                        {QStringLiteral("title"), title},
                        {QStringLiteral("fields"), QVariantList{}},
                        {QStringLiteral("lists"), QVariantList{}}};
    if (!description.isEmpty()) {
        section.insert(QStringLiteral("description"), description);
    }
    return section;
}

void appendFormField(QVariantMap* section, QVariantMap field)
{
    QVariantList fields = section->value(QStringLiteral("fields")).toList();
    fields.push_back(std::move(field));
    section->insert(QStringLiteral("fields"), fields);
}

void appendFormList(QVariantMap* section, QVariantMap list)
{
    QVariantList lists = section->value(QStringLiteral("lists")).toList();
    lists.push_back(std::move(list));
    section->insert(QStringLiteral("lists"), lists);
}

QVariantMap emptyFormSchema()
{
    return QVariantMap{{QStringLiteral("contractVersion"), 1},
                       {QStringLiteral("mode"), QStringLiteral("form")},
                       {QStringLiteral("sections"), QVariantList{}}};
}

QString runParameterKindName(
    hwtest::algorithm::mbddf::RunParameterKind kind)
{
    using Kind = hwtest::algorithm::mbddf::RunParameterKind;
    switch (kind) {
    case Kind::Integer: return QStringLiteral("integer");
    case Kind::Number: return QStringLiteral("number");
    case Kind::Boolean: return QStringLiteral("boolean");
    case Kind::Choice: return QStringLiteral("choice");
    }
    return {};
}

QVariantList criterionMetricOptions(const QVariantMap& value,
                                    const QVariantMap& firstStep)
{
    QMap<QString, QString> labels;
    const auto addMetric = [&labels](const QString& id, QString label) {
        const QString normalized = id.trimmed();
        if (normalized.isEmpty() || labels.contains(normalized)) return;
        label = label.trimmed();
        labels.insert(normalized, label.isEmpty() ? normalized : label);
    };

    const QVariantMap reportFields = value.value(QStringLiteral("reportFields")).toMap();
    for (const QVariant& measurementValue :
         reportFields.value(QStringLiteral("measurements")).toList()) {
        const QVariantMap measurement = measurementValue.toMap();
        addMetric(measurement.value(QStringLiteral("id")).toString(),
                  measurement.value(QStringLiteral("label")).toString());
    }
    for (const QVariant& criterionValue :
         firstStep.value(QStringLiteral("criteria")).toList()) {
        const QString metric = criterionValue.toMap()
                                   .value(QStringLiteral("metric"))
                                   .toString();
        addMetric(metric, metric);
    }

    QVariantList options;
    for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
        options.push_back(formOption(it.key(), it.value()));
    }
    return options;
}

QVariantMap criteriaListSchema(const QVariantMap& value,
                               const QVariantMap& firstStep)
{
    const QVariantList metricOptions = criterionMetricOptions(value, firstStep);
    QVariantMap metric = formField(QStringLiteral("metric"),
                                   QStringLiteral("指标"),
                                   QStringLiteral("choice"));
    metric.insert(QStringLiteral("options"), metricOptions);

    QVariantMap operation = formField(QStringLiteral("op"),
                                      QStringLiteral("比较"),
                                      QStringLiteral("choice"));
    operation.insert(
        QStringLiteral("options"),
         QVariantList{formOption(QStringLiteral("GreaterThan"),
                                 QStringLiteral("大于 (>)")),
                      formOption(QStringLiteral("GreaterOrEqual"),
                                 QStringLiteral("大于等于 (≥)")),
                      formOption(QStringLiteral("LessThan"),
                                 QStringLiteral("小于 (<)")),
                      formOption(QStringLiteral("LessOrEqual"),
                                 QStringLiteral("小于等于 (≤)")),
                      formOption(QStringLiteral("Equal"), QStringLiteral("等于 (=)")),
                      formOption(QStringLiteral("NotEqual"), QStringLiteral("不等于 (≠)")),
                      formOption(QStringLiteral("InRange"), QStringLiteral("范围内"))});
    operation.insert(QStringLiteral("acceptOptionIndex"), true);

    QVariantMap defaults{{QStringLiteral("metric"),
                          metricOptions.isEmpty()
                              ? QVariant{}
                              : metricOptions.first().toMap()
                                    .value(QStringLiteral("value"))},
                         {QStringLiteral("op"), QStringLiteral("GreaterThan")},
                         {QStringLiteral("ref"), 0.0},
                         {QStringLiteral("lo"), 0.0},
                         {QStringLiteral("hi"), 0.0},
                         {QStringLiteral("tol"), 0.0},
                         {QStringLiteral("passIfMatched"), true}};
    return QVariantMap{
        {QStringLiteral("path"), QStringLiteral("/steps/0/criteria")},
        {QStringLiteral("label"), QStringLiteral("判定条件")},
        {QStringLiteral("description"), QStringLiteral("测试结果判定阈值")},
        {QStringLiteral("addLabel"), QStringLiteral("添加判定条件")},
        {QStringLiteral("itemDefaults"), defaults},
        {QStringLiteral("allowAdd"), true},
        {QStringLiteral("allowRemove"), true},
        {QStringLiteral("columns"),
         QVariantList{metric,
                      operation,
                      formField(QStringLiteral("ref"), QStringLiteral("参考值"),
                                QStringLiteral("scalar")),
                      formField(QStringLiteral("lo"), QStringLiteral("下限"),
                                QStringLiteral("number")),
                      formField(QStringLiteral("hi"), QStringLiteral("上限"),
                                QStringLiteral("number")),
                      formField(QStringLiteral("tol"), QStringLiteral("容差"),
                                QStringLiteral("number")),
                      formField(QStringLiteral("passIfMatched"),
                                QStringLiteral("匹配即通过"),
                                QStringLiteral("boolean"))}},
    };
}

QVariantMap measurementDisplayListSchema(const QVariantMap& value)
{
    const QVariantList measurements = value.value(QStringLiteral("reportFields")).toMap()
                                          .value(QStringLiteral("measurements")).toList();
    if (measurements.isEmpty()) return {};

    QVariantMap id = formField(QStringLiteral("id"), QStringLiteral("指标 ID"),
                               QStringLiteral("text"));
    id.insert(QStringLiteral("readOnly"), true);
    QVariantMap label = formField(QStringLiteral("label"), QStringLiteral("显示名称"),
                                  QStringLiteral("text"));
    label.insert(QStringLiteral("required"), true);
    return QVariantMap{
        {QStringLiteral("path"), QStringLiteral("/reportFields/measurements")},
        {QStringLiteral("label"), QStringLiteral("结果显示")},
        {QStringLiteral("description"),
         QStringLiteral("指标 ID 由算法固定，只调整界面名称、单位和重点显示")},
        {QStringLiteral("allowAdd"), false},
        {QStringLiteral("allowRemove"), false},
        {QStringLiteral("itemDefaults"), QVariantMap{}},
        {QStringLiteral("columns"),
         QVariantList{id,
                      label,
                      formField(QStringLiteral("unit"), QStringLiteral("单位"),
                                QStringLiteral("text")),
                      formField(QStringLiteral("primary"), QStringLiteral("重点显示"),
                                QStringLiteral("boolean"))}},
    };
}

void appendExecutionField(QVariantMap* section,
                          const QVariantMap& owner,
                          const QString& key,
                          const QString& path,
                          const QString& label,
                          const QString& kind,
                          const QString& unit = {},
                          double minimum = 0.0)
{
    if (!owner.contains(key)) return;
    QVariantMap field = formField(path, label, kind);
    field.insert(QStringLiteral("required"), true);
    field.insert(QStringLiteral("minimum"), minimum);
    if (!unit.isEmpty()) field.insert(QStringLiteral("unit"), unit);
    appendFormField(section, std::move(field));
}

QVariantMap executionTimingSection(const QVariantMap& value)
{
    const QVariantMap execution = value.value(QStringLiteral("executionConfig")).toMap();
    QVariantMap section = formSection(
        QStringLiteral("executionTiming"),
        QStringLiteral("执行时序"),
        QStringLiteral("硬件打开、流式收发和夹具采样的工程参数"));

    const QVariantMap transport = execution.value(QStringLiteral("transport")).toMap();
    appendExecutionField(&section, transport, QStringLiteral("openTimeoutMs"),
                         QStringLiteral("/executionConfig/transport/openTimeoutMs"),
                         QStringLiteral("硬件打开超时"), QStringLiteral("integer"),
                         QStringLiteral("ms"));

    const QVariantMap stream = execution.value(QStringLiteral("stream")).toMap();
    appendExecutionField(&section, stream, QStringLiteral("readTimeoutMs"),
                         QStringLiteral("/executionConfig/stream/readTimeoutMs"),
                         QStringLiteral("流式读取超时"), QStringLiteral("integer"),
                         QStringLiteral("ms"));
    appendExecutionField(&section, stream, QStringLiteral("startTimeoutMs"),
                         QStringLiteral("/executionConfig/stream/startTimeoutMs"),
                         QStringLiteral("启动响应超时"), QStringLiteral("integer"),
                         QStringLiteral("ms"));
    appendExecutionField(&section, stream, QStringLiteral("stopTimeoutMs"),
                         QStringLiteral("/executionConfig/stream/stopTimeoutMs"),
                         QStringLiteral("停止响应超时"), QStringLiteral("integer"),
                         QStringLiteral("ms"));

    const QVariantMap stimulus = execution.value(QStringLiteral("digitalStimulus")).toMap();
    appendExecutionField(&section, stimulus, QStringLiteral("settlingMs"),
                         QStringLiteral("/executionConfig/digitalStimulus/settlingMs"),
                         QStringLiteral("数字激励稳定时间"), QStringLiteral("integer"),
                         QStringLiteral("ms"));

    const QVariantMap fixture = execution.value(QStringLiteral("boardFixture")).toMap();
    appendExecutionField(&section, fixture, QStringLiteral("settlingMs"),
                         QStringLiteral("/executionConfig/boardFixture/settlingMs"),
                         QStringLiteral("夹具稳定时间"), QStringLiteral("integer"),
                         QStringLiteral("ms"));
    appendExecutionField(&section, fixture, QStringLiteral("directionSampleRateHz"),
                         QStringLiteral("/executionConfig/boardFixture/directionSampleRateHz"),
                         QStringLiteral("方向采样率"), QStringLiteral("number"),
                         QStringLiteral("Hz"), 1.0);
    appendExecutionField(&section, fixture, QStringLiteral("directionSamplesPerChannel"),
                         QStringLiteral("/executionConfig/boardFixture/directionSamplesPerChannel"),
                         QStringLiteral("方向每通道采样点数"), QStringLiteral("integer"),
                         QStringLiteral("点"), 1.0);
    appendExecutionField(&section, fixture, QStringLiteral("pwmSampleRateHz"),
                         QStringLiteral("/executionConfig/boardFixture/pwmSampleRateHz"),
                         QStringLiteral("PWM 采样率"), QStringLiteral("number"),
                         QStringLiteral("Hz"), 1.0);
    appendExecutionField(&section, fixture, QStringLiteral("pwmSamplesPerChannel"),
                         QStringLiteral("/executionConfig/boardFixture/pwmSamplesPerChannel"),
                         QStringLiteral("PWM 每通道采样点数"), QStringLiteral("integer"),
                         QStringLiteral("点"), 1.0);
    return section;
}

QVariantMap digitalStimulusChannelsListSchema(const QVariantMap& value,
                                              const QVariantMap& base)
{
    const QVariantList steps = value.value(QStringLiteral("steps")).toList();
    if (steps.isEmpty() ||
        steps.first().toMap().value(QStringLiteral("algorithmId")).toString() !=
            QStringLiteral("mbddf.di_read")) {
        return {};
    }
    const QVariantMap stimulus = value.value(QStringLiteral("executionConfig")).toMap()
                                     .value(QStringLiteral("digitalStimulus"))
                                     .toMap();
    if (stimulus.value(QStringLiteral("channels")).userType() !=
        QMetaType::QVariantList) {
        return {};
    }

    QVariantList resourceOptions;
    const QVariantMap resources = base.value(QStringLiteral("hardware")).toMap()
                                      .value(QStringLiteral("resources")).toMap();
    for (auto it = resources.cbegin(); it != resources.cend(); ++it) {
        const QVariantMap resource = it.value().toMap();
        if (resource.value(QStringLiteral("device")).toString() ==
                QStringLiteral("ni6259_stimulus") &&
            resource.value(QStringLiteral("module")).toString() ==
                QStringLiteral("digital") &&
            resource.value(QStringLiteral("direction")).toString() ==
                QStringLiteral("output")) {
            resourceOptions.push_back(formOption(it.key(), it.key()));
        }
    }

    QVariantMap resourceId = formField(QStringLiteral("resourceId"),
                                       QStringLiteral("资源"),
                                       QStringLiteral("choice"));
    resourceId.insert(QStringLiteral("options"), resourceOptions);
    const QVariantMap activeLevel = QVariantMap{
        {QStringLiteral("path"), QStringLiteral("activeLevel")},
        {QStringLiteral("label"), QStringLiteral("有效电平")},
        {QStringLiteral("kind"), QStringLiteral("choice")},
        {QStringLiteral("options"),
         QVariantList{formOption(QStringLiteral("High"), QStringLiteral("高电平")),
                      formOption(QStringLiteral("Low"), QStringLiteral("低电平"))}},
    };
    return QVariantMap{
        {QStringLiteral("path"),
         QStringLiteral("/executionConfig/digitalStimulus/channels")},
        {QStringLiteral("label"), QStringLiteral("数字激励通道")},
        {QStringLiteral("addLabel"), QStringLiteral("添加数字激励通道")},
        {QStringLiteral("itemDefaults"),
         QVariantMap{{QStringLiteral("switchId"), QString{}},
                     {QStringLiteral("resourceId"),
                      resourceOptions.isEmpty()
                          ? QVariant{}
                          : resourceOptions.first().toMap()
                                .value(QStringLiteral("value"))},
                     {QStringLiteral("label"), QString{}},
                     {QStringLiteral("dutBit"), 0},
                     {QStringLiteral("activeLevel"), QStringLiteral("High")}}},
        {QStringLiteral("allowAdd"), true},
        {QStringLiteral("allowRemove"), true},
        {QStringLiteral("columns"),
         QVariantList{formField(QStringLiteral("switchId"), QStringLiteral("开关 ID"),
                                QStringLiteral("text")),
                      resourceId,
                      formField(QStringLiteral("label"), QStringLiteral("标签"),
                                QStringLiteral("text")),
                      QVariantMap{{QStringLiteral("path"), QStringLiteral("dutBit")},
                                  {QStringLiteral("label"), QStringLiteral("DUT 位")},
                                  {QStringLiteral("kind"), QStringLiteral("integer")},
                                  {QStringLiteral("minimum"), 0},
                                  {QStringLiteral("maximum"), 15}},
                      activeLevel}},
    };
}

QVariantMap testConfigSchema(const QVariantMap& value, const QVariantMap& base)
{
    QVariantMap schema = emptyFormSchema();
    schema.insert(
        QStringLiteral("readOnlyPaths"),
        QVariantList{QStringLiteral("/schemaVersion"),
                     QStringLiteral("/configId"),
                     QStringLiteral("/steps/*/algorithmId")});
    schema.insert(QStringLiteral("advancedPaths"),
                  QVariantList{QStringLiteral("/executionConfig")});

    QVariantMap basic = formSection(QStringLiteral("basic"),
                                    QStringLiteral("基本信息"));
    QVariantMap title = formField(QStringLiteral("/reportFields/title"),
                                  QStringLiteral("标题"), QStringLiteral("text"));
    title.insert(QStringLiteral("required"), true);
    appendFormField(&basic, std::move(title));
    QVariantMap description = formField(QStringLiteral("/reportFields/description"),
                                        QStringLiteral("说明"),
                                        QStringLiteral("multiline"));
    description.insert(QStringLiteral("wide"), true);
    appendFormField(&basic, std::move(description));
    QVariantMap name = formField(QStringLiteral("/steps/0/name"),
                                 QStringLiteral("步骤名称"), QStringLiteral("text"));
    name.insert(QStringLiteral("required"), true);
    appendFormField(&basic, std::move(name));
    QVariantMap timeout = formField(QStringLiteral("/steps/0/timeoutMs"),
                                    QStringLiteral("超时"), QStringLiteral("integer"));
    timeout.insert(QStringLiteral("unit"), QStringLiteral("ms"));
    timeout.insert(QStringLiteral("required"), true);
    timeout.insert(QStringLiteral("minimum"), 0);
    appendFormField(&basic, std::move(timeout));
    QVariantMap retry = formField(QStringLiteral("/steps/0/retryCount"),
                                  QStringLiteral("重试次数"),
                                  QStringLiteral("integer"));
    retry.insert(QStringLiteral("required"), true);
    retry.insert(QStringLiteral("minimum"), -1);
    appendFormField(&basic, std::move(retry));

    const QVariantList steps = value.value(QStringLiteral("steps")).toList();
    const QVariantMap firstStep = steps.isEmpty() ? QVariantMap{} : steps.first().toMap();
    QVariantList sections{basic};

    const QString algorithmId = firstStep.value(QStringLiteral("algorithmId"))
                                    .toString()
                                    .trimmed();
    if (const auto* parameterSchema =
            hwtest::algorithm::mbddf::findRunParameterSchema(algorithmId);
        parameterSchema != nullptr) {
        QVariantMap parameters = formSection(
            QStringLiteral("runParameters"),
            QStringLiteral("默认测试参数"),
            QStringLiteral("保存后作为测试默认值；测试工作台仍可在单次运行时覆盖"));
        const QString parameterBase =
            QStringLiteral("/steps/0/parameters/protocol/requestValues/");
        for (const auto& descriptor : parameterSchema->parameters) {
            QVariantMap field = formField(
                parameterBase + escapedJsonPointerSegment(descriptor.id),
                descriptor.label, runParameterKindName(descriptor.kind));
            if (!descriptor.description.isEmpty()) {
                field.insert(QStringLiteral("description"), descriptor.description);
            }
            if (!descriptor.unit.isEmpty()) {
                field.insert(QStringLiteral("unit"), descriptor.unit);
            }
            field.insert(QStringLiteral("required"), descriptor.required);
            if (descriptor.defaultValue.isValid()) {
                field.insert(QStringLiteral("defaultValue"), descriptor.defaultValue);
            }
            if (descriptor.minimum.isValid()) {
                field.insert(QStringLiteral("minimum"), descriptor.minimum);
            }
            if (descriptor.maximum.isValid()) {
                field.insert(QStringLiteral("maximum"), descriptor.maximum);
            }
            if (!descriptor.choices.isEmpty()) {
                QVariantList options;
                for (const auto& choice : descriptor.choices) {
                    options.push_back(formOption(choice.value, choice.label));
                }
                field.insert(QStringLiteral("options"), options);
            }
            if (!descriptor.visibleWhenParameter.isEmpty()) {
                field.insert(
                    QStringLiteral("visibleWhen"),
                    QVariantMap{{QStringLiteral("path"),
                                 parameterBase + escapedJsonPointerSegment(
                                     descriptor.visibleWhenParameter)},
                                {QStringLiteral("equals"),
                                 descriptor.visibleWhenEquals}});
            }
            appendFormField(&parameters, std::move(field));
        }
        sections.push_back(parameters);
    }

    QVariantMap executionTiming = executionTimingSection(value);
    if (!executionTiming.value(QStringLiteral("fields")).toList().isEmpty()) {
        sections.push_back(std::move(executionTiming));
    }

    QVariantMap criteria = formSection(QStringLiteral("criteria"),
                                       QStringLiteral("判定条件"));
    appendFormList(&criteria, criteriaListSchema(value, firstStep));
    sections.push_back(criteria);
    const QVariantMap measurementDisplay = measurementDisplayListSchema(value);
    if (!measurementDisplay.isEmpty()) {
        QVariantMap reporting = formSection(QStringLiteral("reporting"),
                                            QStringLiteral("结果显示"));
        appendFormList(&reporting, measurementDisplay);
        sections.push_back(reporting);
    }
    const QVariantMap digitalChannels = digitalStimulusChannelsListSchema(value, base);
    if (!digitalChannels.isEmpty()) {
        QVariantMap digitalStimulus = formSection(QStringLiteral("digitalStimulus"),
                                                  QStringLiteral("数字激励"));
        appendFormList(&digitalStimulus, digitalChannels);
        sections.push_back(digitalStimulus);
    }
    schema.insert(QStringLiteral("sections"), sections);
    return schema;
}

QVariantList serialDataBitsOptions()
{
    return QVariantList{formOption(5, QStringLiteral("5")),
                        formOption(6, QStringLiteral("6")),
                        formOption(7, QStringLiteral("7")),
                        formOption(8, QStringLiteral("8"))};
}

QVariantList serialParityOptions()
{
    return QVariantList{formOption(QStringLiteral("None"), QStringLiteral("无校验")),
                        formOption(QStringLiteral("Odd"), QStringLiteral("奇校验")),
                        formOption(QStringLiteral("Even"), QStringLiteral("偶校验")),
                        formOption(QStringLiteral("Mark"), QStringLiteral("标记校验")),
                        formOption(QStringLiteral("Space"), QStringLiteral("空格校验"))};
}

QVariantList serialStopBitsOptions()
{
    return QVariantList{formOption(1.0, QStringLiteral("1")),
                        formOption(1.5, QStringLiteral("1.5")),
                        formOption(2.0, QStringLiteral("2"))};
}

QVariantList serialFlowControlOptions()
{
    return QVariantList{formOption(QStringLiteral("None"), QStringLiteral("无流控")),
                        formOption(QStringLiteral("Hardware"),
                                   QStringLiteral("硬件流控")),
                        formOption(QStringLiteral("Software"),
                                   QStringLiteral("软件流控"))};
}

QVariantMap stationField(const QString& path,
                         const QString& label,
                         const QString& kind,
                         const QVariant& defaultValue = {})
{
    QVariantMap field = formField(path, label, kind);
    field.insert(QStringLiteral("required"), true);
    if (defaultValue.isValid()) {
        field.insert(QStringLiteral("defaultValue"), defaultValue);
    }
    return field;
}

QString stationResourcePath(const QString& resourceId, const QString& leaf)
{
    return QStringLiteral("/resources/%1/%2")
        .arg(escapedJsonPointerSegment(resourceId),
             escapedJsonPointerSegment(leaf));
}

QVariantMap stationSchema(const QVariantMap& base)
{
    QVariantMap schema = emptyFormSchema();
    QVariantMap control = formSection(QStringLiteral("control"),
                                      QStringLiteral("控制资源"));
    QVariantMap devices = formSection(QStringLiteral("devices"),
                                      QStringLiteral("PXI 设备"));
    QVariantMap serial = formSection(QStringLiteral("serial"),
                                     QStringLiteral("串口资源"));
    QVariantMap digital = formSection(QStringLiteral("digital"),
                                      QStringLiteral("PXI-6259 数字资源"));
    QVariantMap analog = formSection(QStringLiteral("analog"),
                                     QStringLiteral("PXI 模拟资源"));

    const QVariantMap hardware = base.value(QStringLiteral("hardware")).toMap();
    const QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    const QVariantList baseDevices = hardware.value(QStringLiteral("devices")).toList();
    const QVariantMap baseControl = base.value(QStringLiteral("control")).toMap();

    QVariantList controlOptions;
    for (auto it = resources.cbegin(); it != resources.cend(); ++it) {
        const QVariantMap resource = it.value().toMap();
        if (resource.value(QStringLiteral("module")).toString() !=
                QStringLiteral("control") ||
            resource.value(QStringLiteral("direction")).toString() !=
                QStringLiteral("bidirectional") ||
            resource.value(QStringLiteral("properties")).toMap()
                    .value(QStringLiteral("role")).toString() ==
                QStringLiteral("auxiliary-link")) {
            continue;
        }
        controlOptions.push_back(formOption(it.key(), it.key()));
    }
    if (!controlOptions.isEmpty()) {
        QVariantMap resourceId = stationField(QStringLiteral("/control/resourceId"),
                                              QStringLiteral("控制资源"),
                                              QStringLiteral("choice"),
                                              baseControl.value(QStringLiteral("resourceId")));
        resourceId.insert(QStringLiteral("options"), controlOptions);
        appendFormField(&control, std::move(resourceId));
    }

    QMap<QString, QVariantMap> devicesByAlias;
    for (const QVariant& deviceValue : baseDevices) {
        const QVariantMap device = deviceValue.toMap();
        const QString alias = device.value(QStringLiteral("alias"))
                                  .toString()
                                  .trimmed();
        if (!alias.isEmpty()) devicesByAlias.insert(alias, device);
    }
    for (const QString& alias : {QStringLiteral("ni6259_stimulus"),
                                 QStringLiteral("ni6733_fixture")}) {
        if (!devicesByAlias.contains(alias)) continue;
        const QVariantMap baseDevice = devicesByAlias.value(alias);
        const QVariant deviceNameDefault = baseDevice.value(QStringLiteral("properties"))
                                               .toMap()
                                               .value(QStringLiteral("vendor"))
                                               .toMap()
                                               .value(QStringLiteral("ni"))
                                               .toMap()
                                               .value(QStringLiteral("deviceName"));
        const QString productLabel = alias == QStringLiteral("ni6259_stimulus")
            ? QStringLiteral("PXI-6259")
            : QStringLiteral("PXI-6733");
        const QString devicePath = QStringLiteral("/devices/%1/")
                                       .arg(escapedJsonPointerSegment(alias));
        QVariantMap deviceName = stationField(
            devicePath + QStringLiteral("physicalDeviceName"),
            QStringLiteral("%1 设备名").arg(productLabel),
            QStringLiteral("choiceOrText"),
            deviceNameDefault);
        const QString deviceOptionsSource = alias == QStringLiteral("ni6259_stimulus")
            ? QStringLiteral("ni6259Devices")
            : QStringLiteral("ni6733Devices");
        const QString serialOptionsSource = alias == QStringLiteral("ni6259_stimulus")
            ? QStringLiteral("ni6259SerialNumbers")
            : QStringLiteral("ni6733SerialNumbers");
        deviceName.insert(QStringLiteral("optionsSource"), deviceOptionsSource);
        deviceName.insert(QStringLiteral("allowManualEntry"), true);
        appendFormField(&devices, std::move(deviceName));
        QVariantMap serialNumber = stationField(
            devicePath + QStringLiteral("serialNumber"),
            QStringLiteral("%1 序列号").arg(productLabel),
            QStringLiteral("choiceOrText"),
            baseDevice.value(QStringLiteral("serialNumber")));
        serialNumber.insert(QStringLiteral("optionsSource"), serialOptionsSource);
        serialNumber.insert(QStringLiteral("allowManualEntry"), true);
        appendFormField(&devices, std::move(serialNumber));
    }

    for (auto it = resources.cbegin(); it != resources.cend(); ++it) {
        const QString resourceId = it.key();
        const QVariantMap resource = it.value().toMap();
        const QString providerId = resource.value(QStringLiteral("providerId")).toString();
        const QString deviceAlias = resource.value(QStringLiteral("device")).toString();
        const QString module = resource.value(QStringLiteral("module")).toString();
        const QString direction = resource.value(QStringLiteral("direction")).toString();
        const QVariantMap properties = resource.value(QStringLiteral("properties")).toMap();

        if (providerId == QStringLiteral("qt.serial")) {
            QVariantMap portName = stationField(
                stationResourcePath(resourceId, QStringLiteral("portName")),
                QStringLiteral("%1 端口").arg(resourceId),
                QStringLiteral("choiceOrText"),
                properties.value(QStringLiteral("portName")));
            portName.insert(QStringLiteral("optionsSource"), QStringLiteral("serialPorts"));
            portName.insert(QStringLiteral("allowManualEntry"), true);
            appendFormField(&serial, std::move(portName));

            QVariantMap baudRate = stationField(
                stationResourcePath(resourceId, QStringLiteral("baudRate")),
                QStringLiteral("%1 波特率").arg(resourceId),
                QStringLiteral("integer"),
                properties.value(QStringLiteral("baudRate")));
            baudRate.insert(QStringLiteral("minimum"), 1);
            appendFormField(&serial, std::move(baudRate));

            QVariantMap dataBits = stationField(
                stationResourcePath(resourceId, QStringLiteral("dataBits")),
                QStringLiteral("%1 数据位").arg(resourceId), QStringLiteral("choice"),
                properties.value(QStringLiteral("dataBits")));
            dataBits.insert(QStringLiteral("options"), serialDataBitsOptions());
            appendFormField(&serial, std::move(dataBits));

            QVariantMap parity = stationField(
                stationResourcePath(resourceId, QStringLiteral("parity")),
                QStringLiteral("%1 校验位").arg(resourceId), QStringLiteral("choice"),
                properties.value(QStringLiteral("parity")));
            parity.insert(QStringLiteral("options"), serialParityOptions());
            appendFormField(&serial, std::move(parity));

            QVariantMap stopBits = stationField(
                stationResourcePath(resourceId, QStringLiteral("stopBits")),
                QStringLiteral("%1 停止位").arg(resourceId), QStringLiteral("choice"),
                properties.value(QStringLiteral("stopBits")));
            stopBits.insert(QStringLiteral("options"), serialStopBitsOptions());
            appendFormField(&serial, std::move(stopBits));

            QVariantMap flowControl = stationField(
                stationResourcePath(resourceId, QStringLiteral("flowControl")),
                QStringLiteral("%1 流控").arg(resourceId), QStringLiteral("choice"),
                properties.value(QStringLiteral("flowControl")));
            flowControl.insert(QStringLiteral("options"), serialFlowControlOptions());
            appendFormField(&serial, std::move(flowControl));
        }

        if (deviceAlias == QStringLiteral("ni6259_stimulus") &&
            module == QStringLiteral("digital")) {
            QVariantMap port = stationField(
                stationResourcePath(resourceId, QStringLiteral("portNumber")),
                QStringLiteral("%1 端口号").arg(resourceId),
                QStringLiteral("choice"),
                properties.value(QStringLiteral("portNumber")));
            port.insert(
                QStringLiteral("options"),
                QVariantList{formOption(0, QStringLiteral("端口 0")),
                             formOption(1, QStringLiteral("端口 1")),
                             formOption(2, QStringLiteral("端口 2"))});
            appendFormField(&digital, std::move(port));
            const QString portPath = stationResourcePath(
                resourceId, QStringLiteral("portNumber"));
            for (int portNumber = 0; portNumber <= 2; ++portNumber) {
                QVariantMap line = stationField(
                    stationResourcePath(resourceId, QStringLiteral("lineNumber")),
                    QStringLiteral("%1 线号").arg(resourceId),
                    QStringLiteral("integer"),
                    properties.value(QStringLiteral("lineNumber")));
                line.insert(QStringLiteral("minimum"), 0);
                line.insert(QStringLiteral("maximum"), portNumber == 0 ? 31 : 7);
                line.insert(
                    QStringLiteral("visibleWhen"),
                    QVariantMap{{QStringLiteral("path"), portPath},
                                {QStringLiteral("equals"), portNumber}});
                appendFormField(&digital, std::move(line));
            }
        }

        const bool ni6259Input = deviceAlias == QStringLiteral("ni6259_stimulus") &&
            module == QStringLiteral("analog") && direction == QStringLiteral("input");
        const bool ni6733Output = deviceAlias == QStringLiteral("ni6733_fixture") &&
            module == QStringLiteral("analog") && direction == QStringLiteral("output");
        if (ni6259Input || ni6733Output) {
            QVariantMap physicalIndex = stationField(
                stationResourcePath(resourceId, QStringLiteral("physicalIndex")),
                QStringLiteral("%1 通道").arg(resourceId),
                QStringLiteral("integer"),
                resource.value(QStringLiteral("physicalIndex")));
            physicalIndex.insert(QStringLiteral("minimum"), 0);
            physicalIndex.insert(QStringLiteral("maximum"), ni6259Input ? 31 : 7);
            appendFormField(&analog, std::move(physicalIndex));
        }
    }

    schema.insert(QStringLiteral("sections"),
                  QVariantList{control, devices, serial, digital, analog});
    return schema;
}

QVariantMap schemaFor(const QString& kind,
                      const QVariantMap& value,
                      const QVariantMap& base = {})
{
    if (kind == QStringLiteral("testcfg")) return testConfigSchema(value, base);
    if (kind == QStringLiteral("station")) return stationSchema(base);
    return {};
}

ActionResult writeAtomically(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return failure(QStringLiteral("config_save_failed"),
                       QStringLiteral("Cannot create configuration directory"));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        return failure(QStringLiteral("config_save_failed"),
                       QStringLiteral("Cannot save configuration '%1': %2")
                           .arg(path, file.errorString()));
    }
    return {};
}

} // namespace

ConfigurationService::ConfigurationService(QString configurationDirectory,
                                           QString baseHalConfigPath)
{
    configure(std::move(configurationDirectory), std::move(baseHalConfigPath));
}

void ConfigurationService::configure(QString configurationDirectory,
                                     QString baseHalConfigPath)
{
    m_configurationDirectory = configurationDirectory.trimmed().isEmpty()
        ? QString{}
        : QDir(configurationDirectory).absolutePath();
    m_baseHalConfigPath = baseHalConfigPath.trimmed().isEmpty()
        ? QString{}
        : QFileInfo(baseHalConfigPath).absoluteFilePath();
}

QString ConfigurationService::configurationDirectory() const
{
    return m_configurationDirectory;
}

QString ConfigurationService::baseHalConfigPath() const
{
    return m_baseHalConfigPath;
}

ActionResult ConfigurationService::catalog(ConfigurationCatalog* output) const
{
    if (output == nullptr) {
        return failure(QStringLiteral("invalid_output"),
                       QStringLiteral("Configuration catalog output is required"));
    }
    if (m_configurationDirectory.isEmpty()) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Configuration directory is not initialized"));
    }

    QVariantMap catalogValue;
    QByteArray catalogBytes;
    const ActionResult read = readOptionalJsonObject(
        QDir(m_configurationDirectory).filePath(QString::fromLatin1(kCatalogFileName)),
        defaultCatalog(), &catalogValue, &catalogBytes);
    if (!read.ok) return read;
    QMap<QString, CatalogSetting> settings;
    const ActionResult parsed = parseCatalog(catalogValue, &settings);
    if (!parsed.ok) return parsed;

    ConfigurationCatalog result;
    result.revision = revisionFor(catalogBytes);
    const QDir directory(m_configurationDirectory);
    const QStringList names = directory.entryList(
        QStringList{QStringLiteral("*.testcfg.json")}, QDir::Files, QDir::Name);
    QMap<QString, int> configIdCounts;
    for (const QString& name : names) {
        ConfigurationCatalogItem item;
        item.documentId = name;
        const CatalogSetting setting = settings.value(name, CatalogSetting{});
        item.enabled = setting.enabled;
        item.order = setting.order;
        const ActionResult described = describeTestConfig(
            directory.absoluteFilePath(name), &item);
        item.valid = described.ok;
        item.message = described.message;
        if (!item.configId.isEmpty()) {
            configIdCounts[item.configId] += 1;
        }
        result.items.push_back(item);
    }
    for (ConfigurationCatalogItem& item : result.items) {
        if (!item.configId.isEmpty() && configIdCounts.value(item.configId) > 1) {
            item.valid = false;
            item.message = QStringLiteral("Duplicate configId '%1'").arg(item.configId);
        }
    }
    std::sort(result.items.begin(), result.items.end(),
              [](const ConfigurationCatalogItem& left,
                 const ConfigurationCatalogItem& right) {
                  if (left.order != right.order) return left.order < right.order;
                  return left.documentId < right.documentId;
              });
    *output = std::move(result);
    return {};
}

ActionResult ConfigurationService::document(const QString& documentId,
                                             ConfigurationDocument* output) const
{
    if (output == nullptr) {
        return failure(QStringLiteral("invalid_output"),
                       QStringLiteral("Configuration document output is required"));
    }
    if (m_configurationDirectory.isEmpty()) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Configuration directory is not initialized"));
    }

    QString path;
    QString kind;
    QVariantMap fallback;
    if (documentId == QLatin1String(kCatalogDocumentId)) {
        path = QDir(m_configurationDirectory).filePath(
            QString::fromLatin1(kCatalogFileName));
        kind = QStringLiteral("catalog");
        fallback = defaultCatalog();
    } else if (documentId == QLatin1String(kStationDocumentId)) {
        path = QDir(m_configurationDirectory).filePath(
            QString::fromLatin1(kStationFileName));
        kind = QStringLiteral("station");
        fallback = defaultStation();
    } else if (isTestDocumentId(documentId)) {
        path = testConfigPath(documentId);
        kind = QStringLiteral("testcfg");
        if (!QFileInfo(path).isFile()) {
            return failure(QStringLiteral("config_not_found"),
                           QStringLiteral("Unknown configuration document '%1'")
                               .arg(documentId));
        }
    } else {
        return failure(QStringLiteral("config_not_found"),
                       QStringLiteral("Unknown configuration document '%1'")
                           .arg(documentId));
    }

    QVariantMap value;
    QByteArray bytes;
    const ActionResult read = fallback.isEmpty()
        ? readJsonObject(path, &value, &bytes)
        : readOptionalJsonObject(path, fallback, &value, &bytes);
    if (!read.ok) return read;
    QVariantMap schemaBase;
    if ((kind == QStringLiteral("station") || kind == QStringLiteral("testcfg")) &&
        !m_baseHalConfigPath.isEmpty()) {
        // Raw base HAL supplies station leaves and DI resource choices.
        const ActionResult baseRead = readJsonObject(m_baseHalConfigPath, &schemaBase);
        if (!baseRead.ok) schemaBase.clear();
    }
    *output = ConfigurationDocument{
        documentId, kind, revisionFor(bytes), value,
        schemaFor(kind, value, schemaBase)};
    return {};
}

ActionResult ConfigurationService::saveDocument(
    const QString& documentId,
    const QString& expectedRevision,
    const QVariantMap& value,
    ConfigurationDocument* output,
    ConfigurationBackup* backup) const
{
    ConfigurationDocument current;
    const ActionResult currentResult = document(documentId, &current);
    if (!currentResult.ok) return currentResult;
    if (expectedRevision != current.revision) {
        return failure(QStringLiteral("config_conflict"),
                       QStringLiteral("Configuration changed since it was loaded"));
    }

    const QByteArray bytes = serialized(value);
    QString path;
    QVariantMap schemaBase;
    if (current.kind == QStringLiteral("catalog")) {
        QMap<QString, CatalogSetting> entries;
        const ActionResult valid = parseCatalog(value, &entries);
        if (!valid.ok) return valid;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if (!QFileInfo(testConfigPath(it.key())).isFile()) {
                return failure(QStringLiteral("config_invalid"),
                               QStringLiteral("Catalog references unknown document '%1'")
                                   .arg(it.key()));
            }
        }
        path = QDir(m_configurationDirectory).filePath(
            QString::fromLatin1(kCatalogFileName));
    } else if (current.kind == QStringLiteral("station")) {
        QVariantMap base;
        const ActionResult baseResult = readJsonObject(m_baseHalConfigPath, &base);
        if (!baseResult.ok) return baseResult;
        QVariantMap projected;
        const ActionResult valid = applyStationOverlay(base, value, &projected);
        if (!valid.ok) return valid;
        schemaBase = base;
        path = QDir(m_configurationDirectory).filePath(
            QString::fromLatin1(kStationFileName));
    } else {
        const ActionResult immutable = validateImmutableTestFields(current.value, value);
        if (!immutable.ok) return immutable;
        hwtest::biz::TestConfigManager manager;
        const auto parsed = manager.parse(bytes, documentId);
        if (!parsed.ok()) {
            return failure(QStringLiteral("config_invalid"),
                           parsed.status.error.message);
        }
        const QString temporaryPath = testConfigPath(documentId);
        // The BIZ parser above validates the draft. The app-specific enabled
        // algorithm rule is checked directly without exposing a second parser.
        const hwtest::biz::TestStep* enabledStep = nullptr;
        for (const hwtest::biz::TestStep& step : parsed.value.steps) {
            if (!step.enabled) continue;
            if (enabledStep != nullptr || !isSupportedMbdDfAlgorithm(step.algorithmId)) {
                return failure(QStringLiteral("config_invalid"),
                               QStringLiteral("Exactly one supported MB_DDF step must be enabled"));
            }
            enabledStep = &step;
        }
        if (enabledStep == nullptr) {
            return failure(QStringLiteral("config_invalid"),
                               QStringLiteral("Exactly one supported MB_DDF step must be enabled"));
        }
        QVector<QString> runModes;
        const QString runModeError = parseSupportedRunModes(
            parsed.value.reportFields, &runModes);
        if (!runModeError.isEmpty()) {
            return failure(QStringLiteral("config_invalid"), runModeError);
        }
        bool stoppable = true;
        const QString stoppableError = parseStoppableCapability(
            parsed.value.reportFields, runModes, &stoppable);
        if (!stoppableError.isEmpty()) {
            return failure(QStringLiteral("config_invalid"), stoppableError);
        }
        path = temporaryPath;
    }

    if (current.kind == QStringLiteral("testcfg") &&
        !m_baseHalConfigPath.isEmpty()) {
        const ActionResult baseRead = readJsonObject(m_baseHalConfigPath, &schemaBase);
        if (!baseRead.ok) schemaBase.clear();
    }

    const bool existed = QFileInfo(path).isFile();
    QByteArray originalBytes;
    ActionResult captured;
    if (existed) {
        captured = readFileContents(path, &originalBytes);
    } else if (current.kind == QStringLiteral("catalog")) {
        originalBytes = serialized(defaultCatalog());
    } else if (current.kind == QStringLiteral("station")) {
        originalBytes = serialized(defaultStation());
    } else {
        captured = failure(QStringLiteral("config_not_found"),
                           QStringLiteral("Unknown configuration document '%1'")
                               .arg(documentId));
    }
    if (!captured.ok) return captured;
    if (revisionFor(originalBytes) != expectedRevision) {
        return failure(QStringLiteral("config_conflict"),
                       QStringLiteral("Configuration changed while the draft was being validated"));
    }

    const ConfigurationBackup original{
        documentId, expectedRevision, originalBytes, existed};
    const ActionResult saved = writeAtomically(path, bytes);
    if (!saved.ok) return saved;
    if (backup != nullptr) *backup = original;
    if (output != nullptr) {
        *output = ConfigurationDocument{
            documentId, current.kind, revisionFor(bytes), value,
            schemaFor(current.kind, value, schemaBase)};
    }
    return {};
}

ActionResult ConfigurationService::restoreDocument(
    const ConfigurationBackup& backup,
    const QString& committedRevision) const
{
    const QString path = documentStoragePath(m_configurationDirectory,
                                             backup.documentId);
    if (path.isEmpty()) {
        return failure(QStringLiteral("config_save_failed"),
                       QStringLiteral("Cannot restore an unknown configuration document"));
    }

    QByteArray currentBytes;
    const ActionResult current = readFileContents(path, &currentBytes);
    if (!current.ok || revisionFor(currentBytes) != committedRevision) {
        return failure(QStringLiteral("config_save_failed"),
                       QStringLiteral("Configuration changed again before it could be restored"));
    }
    if (backup.existed) {
        return writeAtomically(path, backup.contents);
    }
    if (!QFile::remove(path)) {
        return failure(QStringLiteral("config_save_failed"),
                       QStringLiteral("Cannot remove newly created configuration '%1'")
                           .arg(path));
    }
    return {};
}

ActionResult ConfigurationService::loadSnapshot(
    const QString& testConfigPath,
    ConfigurationLoadSnapshot* output) const
{
    if (output == nullptr) {
        return failure(QStringLiteral("invalid_output"),
                       QStringLiteral("Configuration snapshot output is required"));
    }
    if (m_configurationDirectory.isEmpty() || m_baseHalConfigPath.isEmpty()) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Configuration storage is not initialized"));
    }

    ConfigurationLoadSnapshot snapshot;
    const ActionResult testResult = readFileContents(
        testConfigPath, &snapshot.testConfigBytes);
    if (!testResult.ok) return testResult;

    QVariantMap base;
    QByteArray baseBytes;
    const ActionResult baseResult = readJsonObject(
        m_baseHalConfigPath, &base, &baseBytes);
    if (!baseResult.ok) return baseResult;

    QVariantMap station;
    QByteArray stationBytes;
    const ActionResult stationResult = readOptionalJsonObject(
        QDir(m_configurationDirectory).filePath(
            QString::fromLatin1(kStationFileName)),
        defaultStation(), &station, &stationBytes);
    if (!stationResult.ok) return stationResult;

    const ActionResult merged = applyStationOverlay(
        base, station, &snapshot.mergedHalConfig);
    if (!merged.ok) return merged;
    snapshot.revisions.testConfig = revisionFor(snapshot.testConfigBytes);
    snapshot.revisions.station = revisionFor(stationBytes);
    snapshot.revisions.baseHal = revisionFor(baseBytes);
    *output = std::move(snapshot);
    return {};
}

ActionResult ConfigurationService::currentSourceRevisions(
    const QString& testConfigPath,
    ConfigurationSourceRevisions* output) const
{
    if (output == nullptr) {
        return failure(QStringLiteral("invalid_output"),
                       QStringLiteral("Configuration revision output is required"));
    }
    if (m_configurationDirectory.isEmpty() || m_baseHalConfigPath.isEmpty()) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Configuration storage is not initialized"));
    }

    QByteArray testBytes;
    const ActionResult testResult = readFileContents(testConfigPath, &testBytes);
    if (!testResult.ok) return testResult;
    QByteArray baseBytes;
    const ActionResult baseResult = readFileContents(m_baseHalConfigPath, &baseBytes);
    if (!baseResult.ok) return baseResult;
    const QString stationPath = QDir(m_configurationDirectory).filePath(
        QString::fromLatin1(kStationFileName));
    QByteArray stationBytes;
    if (QFileInfo::exists(stationPath)) {
        const ActionResult stationResult = readFileContents(stationPath, &stationBytes);
        if (!stationResult.ok) return stationResult;
    } else {
        stationBytes = serialized(defaultStation());
    }

    *output = ConfigurationSourceRevisions{
        revisionFor(testBytes), revisionFor(stationBytes), revisionFor(baseBytes)};
    return {};
}

ActionResult ConfigurationService::isTestDocumentSelectable(
    const QString& documentId,
    bool* output) const
{
    if (output == nullptr) {
        return failure(QStringLiteral("invalid_output"),
                       QStringLiteral("Test configuration selection output is required"));
    }
    if (!isTestDocumentId(documentId) || m_configurationDirectory.isEmpty()) {
        return failure(QStringLiteral("config_not_found"),
                       QStringLiteral("Unknown test configuration document '%1'")
                           .arg(documentId));
    }
    const QString catalogPath = QDir(m_configurationDirectory).filePath(
        QString::fromLatin1(kCatalogFileName));
    if (!QFileInfo(catalogPath).isFile()) {
        *output = true;
        return {};
    }

    ConfigurationCatalog current;
    const ActionResult listed = catalog(&current);
    if (!listed.ok) return listed;
    const auto item = std::find_if(
        current.items.cbegin(), current.items.cend(),
        [&documentId](const ConfigurationCatalogItem& candidate) {
            return candidate.documentId == documentId;
        });
    *output = item != current.items.cend() && item->enabled && item->valid;
    return {};
}

ActionResult ConfigurationService::mergedHalConfiguration(QVariantMap* output) const
{
    if (output == nullptr) {
        return failure(QStringLiteral("invalid_output"),
                       QStringLiteral("Merged HAL configuration output is required"));
    }
    if (m_baseHalConfigPath.isEmpty()) {
        return failure(QStringLiteral("invalid_state"),
                       QStringLiteral("Base HAL configuration is not initialized"));
    }
    QVariantMap base;
    const ActionResult baseResult = readJsonObject(m_baseHalConfigPath, &base);
    if (!baseResult.ok) return baseResult;
    QVariantMap station;
    QByteArray stationBytes;
    const ActionResult stationResult = readOptionalJsonObject(
        QDir(m_configurationDirectory).filePath(
            QString::fromLatin1(kStationFileName)),
        defaultStation(), &station, &stationBytes);
    if (!stationResult.ok) return stationResult;
    return applyStationOverlay(base, station, output);
}

QString ConfigurationService::testConfigPath(const QString& documentId) const
{
    if (!isTestDocumentId(documentId) || m_configurationDirectory.isEmpty()) return {};
    return QDir(m_configurationDirectory).absoluteFilePath(documentId);
}

} // namespace hwtest::app
