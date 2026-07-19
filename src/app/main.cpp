#include <algorithm/mbddf_transport.h>
#include <algorithm/system_status_executor.h>

#include <biz/biz_factory.h>
#include <biz/i_test_run_service.h>
#include <biz/test_config_manager.h>

#include <hal/hal_factory.h>
#include <hal/i_hal_device.h>
#include <hal/i_hal_service.h>

#include <logging/hal_log_bridge.h>
#include <logging/log_file_sink.h>
#include <logging/log_service.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace {

using HalServicePtr = std::unique_ptr<hwtest::hal::IHalService,
                                      void (*)(hwtest::hal::IHalService*)>;
using TestServicePtr = std::unique_ptr<hwtest::biz::ITestRunService,
                                       void (*)(hwtest::biz::ITestRunService*)>;

int writeJson(FILE* stream, const QJsonObject& object, int exitCode)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::fprintf(stream, "%s\n", payload.constData());
    return exitCode;
}

int fail(const QString& stage, const QString& message, int exitCode = 2)
{
    return writeJson(stderr,
                     {{QStringLiteral("ok"), false},
                      {QStringLiteral("stage"), stage},
                      {QStringLiteral("error"), message}},
                     exitCode);
}

bool loadJsonMap(const QString& path, QVariantMap* output, QString* error)
{
    if (output == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("JSON output is null");
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Cannot open '%1': %2").arg(path, file.errorString());
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid JSON object in '%1': %2")
                         .arg(path, parseError.errorString());
        }
        return false;
    }

    *output = document.object().toVariantMap();
    return true;
}

QString resolvedPath(const QString& configPath, const QString& value)
{
    if (value.isEmpty() || QFileInfo(value).isAbsolute()) {
        return value;
    }
    return QDir(QFileInfo(configPath).absolutePath()).absoluteFilePath(value);
}

QJsonObject resultJson(const hwtest::biz::TestResult& result)
{
    return {
        {QStringLiteral("ok"), result.verdict == hwtest::biz::TestVerdict::Pass},
        {QStringLiteral("stepId"), result.stepId},
        {QStringLiteral("testItemId"), result.testItemId},
        {QStringLiteral("algorithmId"), result.algorithmId},
        {QStringLiteral("verdict"), hwtest::biz::testVerdictToString(result.verdict)},
        {QStringLiteral("errorCode"), hwtest::biz::errorCodeToString(result.errorCode)},
        {QStringLiteral("message"), result.message},
        {QStringLiteral("attempts"), result.attempts},
        {QStringLiteral("rawData"), QJsonObject::fromVariantMap(result.rawData)},
    };
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hwtest_pc_runner"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("MB_DDF_v2 SYSTEM_STATUS PC control runner"));
    parser.addHelpOption();
    const QCommandLineOption testConfigOption(
        QStringList{QStringLiteral("t"), QStringLiteral("test-config")},
        QStringLiteral("BIZ test configuration JSON"),
        QStringLiteral("path"));
    const QCommandLineOption halConfigOption(
        QStringList{QStringLiteral("H"), QStringLiteral("hal-config")},
        QStringLiteral("HAL deployment configuration JSON"),
        QStringLiteral("path"));
    parser.addOption(testConfigOption);
    parser.addOption(halConfigOption);
    parser.process(application);

    if (!parser.isSet(testConfigOption) || !parser.isSet(halConfigOption)) {
        return fail(QStringLiteral("arguments"),
                    QStringLiteral("--test-config and --hal-config are required"));
    }

    const QString testConfigPath = QFileInfo(parser.value(testConfigOption)).absoluteFilePath();
    const QString halConfigPath = QFileInfo(parser.value(halConfigOption)).absoluteFilePath();

    hwtest::biz::TestConfigManager configManager;
    const auto testConfig = configManager.load(testConfigPath);
    if (!testConfig.ok()) {
        return fail(QStringLiteral("test-config"), testConfig.status.error.message);
    }

    int enabledSystemStatusSteps = 0;
    for (const hwtest::biz::TestStep& step : testConfig.value.steps) {
        if (!step.enabled) {
            continue;
        }
        if (step.algorithmId != QStringLiteral("mbddf.system_status")) {
            return fail(QStringLiteral("test-config"),
                        QStringLiteral("This runner only supports mbddf.system_status"));
        }
        ++enabledSystemStatusSteps;
    }
    if (enabledSystemStatusSteps != 1) {
        return fail(QStringLiteral("test-config"),
                    QStringLiteral("Exactly one enabled SYSTEM_STATUS step is required"));
    }

    QVariantMap halConfig;
    QString jsonError;
    if (!loadJsonMap(halConfigPath, &halConfig, &jsonError)) {
        return fail(QStringLiteral("hal-config"), jsonError);
    }

    const QVariantMap control = halConfig.value(QStringLiteral("control")).toMap();
    const QString deviceId = control.value(QStringLiteral("deviceId")).toString().trimmed();
    const QString resourceId = control.value(QStringLiteral("resourceId")).toString().trimmed();
    bool timeoutOk = false;
    const int runTimeoutMs = control.value(QStringLiteral("runTimeoutMs")).toInt(&timeoutOk);
    if (deviceId.isEmpty() || resourceId.isEmpty() || !timeoutOk || runTimeoutMs <= 0) {
        return fail(QStringLiteral("hal-config"),
                    QStringLiteral("control.deviceId, control.resourceId and a positive control.runTimeoutMs are required"));
    }

    HalServicePtr hal(hwtest::hal::createHalService(), &hwtest::hal::destroyHalService);
    if (!hal) {
        return fail(QStringLiteral("hal-create"), QStringLiteral("Unable to create HAL service"));
    }

    const hwtest::hal::HalStatus initialized = hal->initialize(halConfig);
    if (!initialized.ok()) {
        return fail(QStringLiteral("hal-initialize"), initialized.error.message);
    }

    const auto opened = hal->openDevice(deviceId, hwtest::hal::OperationOptions{});
    if (!opened.ok()) {
        hal->shutdown();
        return fail(QStringLiteral("hal-open-device"), opened.status.error.message);
    }
    const hwtest::hal::SessionId sessionId = opened.value;
    const auto device = hal->device(sessionId);
    if (!device.ok() || device.value == nullptr) {
        hal->closeDevice(sessionId, hwtest::hal::OperationOptions{});
        hal->shutdown();
        return fail(QStringLiteral("hal-device"), device.status.error.message);
    }

    int exitCode = 1;
    {
        auto transport = std::make_unique<hwtest::algorithm::mbddf::HalControlTransport>(
            device.value, resourceId);
        hwtest::algorithm::mbddf::SystemStatusAlgorithmExecutor executor(std::move(transport));
        TestServicePtr runner(hwtest::biz::createTestRunService(&executor),
                              &hwtest::biz::destroyTestRunService);
        if (!runner) {
            hal->closeDevice(sessionId, hwtest::hal::OperationOptions{});
            hal->shutdown();
            return fail(QStringLiteral("biz-create"), QStringLiteral("Unable to create BIZ service"));
        }

        hwtest::logging::LogService logService;
        std::unique_ptr<hwtest::logging::JsonLineFileSink> fileSink;
        const QString configuredLogPath =
            halConfig.value(QStringLiteral("logging")).toMap()
                .value(QStringLiteral("filePath")).toString().trimmed();
        if (!configuredLogPath.isEmpty()) {
            const QString logPath = resolvedPath(halConfigPath, configuredLogPath);
            QDir().mkpath(QFileInfo(logPath).absolutePath());
            fileSink = std::make_unique<hwtest::logging::JsonLineFileSink>(logPath);
            if (!fileSink->open()) {
                runner->shutdown();
                hal->closeDevice(sessionId, hwtest::hal::OperationOptions{});
                hal->shutdown();
                return fail(QStringLiteral("logging"), fileSink->errorString());
            }
            logService.addSink(fileSink.get());
        }
        hwtest::logging::connectHalLogs(hal.get(), &logService, Qt::DirectConnection);
        QObject::connect(runner.get(),
                         &hwtest::biz::ITestRunService::logProduced,
                         &logService,
                         &hwtest::logging::LogService::append,
                         Qt::DirectConnection);

        const hwtest::biz::Status bizInitialized = runner->initialize();
        if (!bizInitialized.ok()) {
            exitCode = fail(QStringLiteral("biz-initialize"), bizInitialized.error.message);
        } else {
            const hwtest::biz::Status loaded = runner->loadConfiguration(testConfigPath);
            if (!loaded.ok()) {
                exitCode = fail(QStringLiteral("biz-load"), loaded.error.message);
            } else {
                QEventLoop loop;
                QTimer watchdog;
                watchdog.setSingleShot(true);
                bool timedOut = false;
                bool hasResult = false;
                hwtest::biz::TestResult finalResult;
                QString terminalError;

                QObject::connect(runner.get(),
                                 &hwtest::biz::ITestRunService::resultProduced,
                                 &application,
                                 [&](const hwtest::biz::TaskId&,
                                     const hwtest::biz::TestResult& result) {
                                     finalResult = result;
                                     hasResult = true;
                                 },
                                 Qt::QueuedConnection);
                QObject::connect(runner.get(),
                                 &hwtest::biz::ITestRunService::hardwareError,
                                 &application,
                                 [&](const hwtest::biz::TaskId&,
                                     const hwtest::biz::TestItemId&,
                                     hwtest::biz::ErrorCode,
                                     const QString& description) {
                                     terminalError = description;
                                 },
                                 Qt::QueuedConnection);
                QObject::connect(runner.get(),
                                 &hwtest::biz::ITestRunService::stateChanged,
                                 &application,
                                 [&](const hwtest::biz::TaskId&, hwtest::biz::TestState state) {
                                     if (state == hwtest::biz::TestState::Finished ||
                                         state == hwtest::biz::TestState::Error) {
                                         loop.quit();
                                     }
                                 },
                                 Qt::QueuedConnection);
                QObject::connect(&watchdog, &QTimer::timeout, &application, [&] {
                    timedOut = true;
                    loop.quit();
                });

                const auto started = runner->startTest();
                if (!started.ok()) {
                    exitCode = fail(QStringLiteral("biz-start"), started.status.error.message);
                } else {
                    watchdog.start(runTimeoutMs);
                    loop.exec();
                    watchdog.stop();
                    if (timedOut) {
                        runner->stopTest(runTimeoutMs);
                        exitCode = fail(QStringLiteral("run-timeout"),
                                        QStringLiteral("SYSTEM_STATUS run exceeded control.runTimeoutMs"));
                    } else if (!hasResult) {
                        exitCode = fail(QStringLiteral("run"),
                                        terminalError.isEmpty()
                                            ? QStringLiteral("Run ended without a test result")
                                            : terminalError);
                    } else {
                        exitCode = writeJson(stdout,
                                             resultJson(finalResult),
                                             finalResult.verdict == hwtest::biz::TestVerdict::Pass ? 0 : 1);
                    }
                }
            }
        }

        runner->shutdown();
        if (fileSink) {
            fileSink->flush();
        }
    }

    const hwtest::hal::HalStatus closed =
        hal->closeDevice(sessionId, hwtest::hal::OperationOptions{});
    const hwtest::hal::HalStatus shutDown = hal->shutdown();
    if (!closed.ok()) {
        return fail(QStringLiteral("hal-close-device"), closed.error.message);
    }
    if (!shutDown.ok()) {
        return fail(QStringLiteral("hal-shutdown"), shutDown.error.message);
    }
    return exitCode;
}
