#include <app/frontend_launch_options.h>

#include "configuration_service.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace hwtest::app {

namespace {

QString resolvedPath(const QString& value, const QString& baseDirectory)
{
    const QString normalized = value.trimmed();
    if (normalized.isEmpty() || QFileInfo(normalized).isAbsolute()) {
        return normalized;
    }
    return QDir(baseDirectory).absoluteFilePath(normalized);
}

ActionResult failure(const QString& code, const QString& message)
{
    return ActionResult{false, code, message};
}

ActionResult discoverTestConfigs(
    const QString& baseDirectory,
    QVector<FrontendTestConfigOption>* output,
    ConfigurationCatalog* catalogOutput)
{
    if (output == nullptr || catalogOutput == nullptr) {
        return failure(QStringLiteral("invalid_output"),
                       QStringLiteral("Test configuration discovery output is required"));
    }
    const QDir configDirectory(QDir(baseDirectory).filePath(QStringLiteral("configs")));
    ConfigurationService service(configDirectory.absolutePath(), {});
    ConfigurationCatalog catalog;
    const ActionResult listed = service.catalog(&catalog);
    if (!listed.ok) return listed;

    QVector<FrontendTestConfigOption> result;
    for (const ConfigurationCatalogItem& item : catalog.items) {
        if (!item.enabled || !item.valid) continue;
        result.push_back(FrontendTestConfigOption{
            item.configId,
            item.title,
            item.description,
            item.algorithmId,
            configDirectory.absoluteFilePath(item.documentId),
        });
    }
    *output = std::move(result);
    *catalogOutput = std::move(catalog);
    return {};
}

} // namespace

void addFrontendOptions(QCommandLineParser& parser,
                        const FrontendOptionDefaults& defaults)
{
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("t"), QStringLiteral("test-config")},
        QStringLiteral("BIZ test configuration JSON"),
        QStringLiteral("path"),
        defaults.testConfigPath));
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("H"), QStringLiteral("hal-config")},
        QStringLiteral("HAL deployment configuration JSON"),
        QStringLiteral("path"),
        defaults.halConfigPath));
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("c"), QStringLiteral("control")},
        QStringLiteral("Control ResourceId override"),
        QStringLiteral("resource-id")));
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("p"), QStringLiteral("serial-port")},
        QStringLiteral("Serial port override without changing the HAL JSON"),
        QStringLiteral("port-name")));
}

ActionResult readFrontendOptions(const QCommandLineParser& parser,
                                 const QString& baseDirectory,
                                 const FrontendOptionDefaults& defaults,
                                 FrontendLaunchOptions* output)
{
    if (output == nullptr) {
        return failure(QStringLiteral("invalid_output"),
                       QStringLiteral("Frontend launch options output is required"));
    }

    const QString testConfig = parser.value(QStringLiteral("test-config")).trimmed();
    const QString halConfig = parser.value(QStringLiteral("hal-config")).trimmed();
    if (defaults.requireConfigPaths && (testConfig.isEmpty() || halConfig.isEmpty())) {
        return failure(QStringLiteral("missing_argument"),
                       QStringLiteral("--test-config and --hal-config are required"));
    }

    FrontendLaunchOptions result;
    result.testConfigPath = resolvedPath(testConfig, baseDirectory);
    result.halConfigPath = resolvedPath(halConfig, baseDirectory);
    result.controlResourceId = parser.value(QStringLiteral("control")).trimmed();
    result.serialPortName = parser.value(QStringLiteral("serial-port")).trimmed();
    result.configurationDirectory = QDir(baseDirectory)
                                        .absoluteFilePath(QStringLiteral("configs"));
    ConfigurationCatalog catalog;
    const ActionResult discovered = discoverTestConfigs(
        baseDirectory, &result.testConfigs, &catalog);
    if (!discovered.ok) return discovered;
    const QString catalogPath = QDir(result.configurationDirectory).filePath(
        QStringLiteral("test-config-catalog.json"));
    if (QFileInfo(catalogPath).isFile()) {
        const QString selectedAbsolute = QFileInfo(result.testConfigPath).absoluteFilePath();
        const auto catalogItem = std::find_if(
            catalog.items.cbegin(), catalog.items.cend(),
            [&result, &selectedAbsolute](const ConfigurationCatalogItem& item) {
                return QFileInfo(QDir(result.configurationDirectory)
                                     .filePath(item.documentId))
                           .absoluteFilePath() == selectedAbsolute;
            });
        if (parser.isSet(QStringLiteral("test-config")) &&
            catalogItem != catalog.items.cend() && !catalogItem->enabled) {
            return failure(
                QStringLiteral("test_config_disabled"),
                QStringLiteral("Selected test configuration '%1' is disabled")
                    .arg(catalogItem->documentId));
        }
        const auto selected = std::find_if(
            result.testConfigs.cbegin(), result.testConfigs.cend(),
            [&selectedAbsolute](const FrontendTestConfigOption& option) {
                return QFileInfo(option.configPath).absoluteFilePath() == selectedAbsolute;
            });
        if (!parser.isSet(QStringLiteral("test-config")) &&
            selected == result.testConfigs.cend()) {
            result.testConfigPath = result.testConfigs.isEmpty()
                ? QString{}
                : result.testConfigs.first().configPath;
        }
    }
    *output = result;
    return {};
}

ActionResult configureController(TestApplicationController& controller,
                                 const FrontendLaunchOptions& options)
{
    const QString configurationDirectory = options.configurationDirectory.trimmed().isEmpty()
        ? QFileInfo(options.halConfigPath).absolutePath()
        : options.configurationDirectory;
    ActionResult result = controller.configureConfigurationStorage(
        configurationDirectory, options.halConfigPath);
    if (!result.ok || options.testConfigPath.trimmed().isEmpty()) return result;
    result = controller.loadConfigurations(options.testConfigPath,
                                           options.halConfigPath);
    if (!result.ok) {
        return result;
    }
    if (!options.controlResourceId.isEmpty()) {
        result = controller.selectControl(options.controlResourceId);
        if (!result.ok) {
            return result;
        }
    }
    if (!options.serialPortName.isEmpty()) {
        result = controller.selectSerialPort(options.serialPortName);
    }
    return result;
}

} // namespace hwtest::app
