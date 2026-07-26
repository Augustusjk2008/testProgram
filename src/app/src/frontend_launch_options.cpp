#include <app/frontend_launch_options.h>

#include <biz/test_config_manager.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

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

bool isSupportedAlgorithm(const QString& algorithmId)
{
    return algorithmId == QStringLiteral("mbddf.system_status") ||
        algorithmId == QStringLiteral("mbddf.elec_health_status");
}

bool makeTestConfigOption(const QString& configPath,
                          FrontendTestConfigOption* output)
{
    if (output == nullptr) {
        return false;
    }

    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(configPath);
    if (!loaded.ok()) {
        return false;
    }

    const hwtest::biz::TestStep* selectedStep = nullptr;
    for (const hwtest::biz::TestStep& step : loaded.value.steps) {
        if (!step.enabled) {
            continue;
        }
        if (selectedStep != nullptr || !isSupportedAlgorithm(step.algorithmId)) {
            return false;
        }
        selectedStep = &step;
    }
    if (selectedStep == nullptr) {
        return false;
    }

    QString title = loaded.value.reportFields.value(QStringLiteral("title"))
                        .toString().trimmed();
    if (title.isEmpty()) {
        title = selectedStep->name.trimmed();
    }
    if (title.isEmpty()) {
        title = selectedStep->testItemId;
    }
    *output = FrontendTestConfigOption{
        loaded.value.configId,
        title,
        loaded.value.reportFields.value(QStringLiteral("description"))
            .toString().trimmed(),
        selectedStep->algorithmId,
        QFileInfo(configPath).absoluteFilePath(),
    };
    return true;
}

QVector<FrontendTestConfigOption> discoverTestConfigs(
    const QString& baseDirectory,
    const QString& selectedConfigPath)
{
    QVector<FrontendTestConfigOption> result;
    const QDir configDirectory(QDir(baseDirectory).filePath(QStringLiteral("configs")));
    QStringList configPaths;
    const QString selectedAbsolute = QFileInfo(selectedConfigPath).absoluteFilePath();
    if (!selectedConfigPath.trimmed().isEmpty()) {
        configPaths.push_back(selectedAbsolute);
    }
    if (configDirectory.exists()) {
        for (const QString& name : configDirectory.entryList(
                 QStringList{QStringLiteral("*.testcfg.json")},
                 QDir::Files,
                 QDir::Name)) {
            const QString path = configDirectory.absoluteFilePath(name);
            if (!configPaths.contains(path)) {
                configPaths.push_back(path);
            }
        }
    }

    QSet<QString> configIds;
    for (const QString& configPath : configPaths) {
        FrontendTestConfigOption option;
        if (!makeTestConfigOption(configPath, &option) ||
            option.configId.trimmed().isEmpty() ||
            configIds.contains(option.configId)) {
            continue;
        }
        configIds.insert(option.configId);
        result.push_back(option);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.configId < right.configId;
    });
    return result;
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
    result.testConfigs = discoverTestConfigs(baseDirectory, result.testConfigPath);
    *output = result;
    return {};
}

ActionResult configureController(TestApplicationController& controller,
                                 const FrontendLaunchOptions& options)
{
    ActionResult result = controller.loadConfigurations(options.testConfigPath,
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
