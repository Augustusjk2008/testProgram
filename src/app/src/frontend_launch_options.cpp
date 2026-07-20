#include <app/frontend_launch_options.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>

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
