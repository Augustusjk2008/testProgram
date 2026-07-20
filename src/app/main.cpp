#include <app/frontend_launch_options.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace {

int writeJson(FILE* stream, const QJsonObject& object, int exitCode)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::fprintf(stream, "%s\n", payload.constData());
    return exitCode;
}

int fail(const QString& stage,
         const hwtest::app::ActionResult& result,
         int exitCode = 2)
{
    return writeJson(stderr,
                     {{QStringLiteral("ok"), false},
                      {QStringLiteral("stage"), stage},
                      {QStringLiteral("code"), result.code},
                      {QStringLiteral("error"), result.message}},
                     exitCode);
}

int failAfterShutdown(hwtest::app::TestApplicationController* controller,
                      const QString& stage,
                      hwtest::app::ActionResult result)
{
    const hwtest::app::ActionResult shutdown = controller->shutdown();
    if (!shutdown.ok) {
        result.message += QStringLiteral("; shutdown failed (%1): %2")
                              .arg(shutdown.code, shutdown.message);
    }
    return fail(stage, result);
}

QJsonObject resultJson(const hwtest::app::ApplicationSnapshot& result)
{
    return {
        {QStringLiteral("ok"), result.verdict == QStringLiteral("Pass")},
        {QStringLiteral("stepId"), result.stepId},
        {QStringLiteral("testItemId"), result.testItemId},
        {QStringLiteral("algorithmId"), result.algorithmId},
        {QStringLiteral("verdict"), result.verdict},
        {QStringLiteral("errorCode"), result.errorCode},
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
        QStringLiteral("Run the configured MB_DDF_v2 SYSTEM_STATUS test once"));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const hwtest::app::FrontendOptionDefaults defaults{{}, {}, true};
    hwtest::app::addFrontendOptions(parser, defaults);
    const QStringList arguments = application.arguments();
    if (arguments.contains(QStringLiteral("-?")) ||
        arguments.contains(QStringLiteral("-h")) ||
        arguments.contains(QStringLiteral("--help")) ||
        arguments.contains(QStringLiteral("--help-all"))) {
        const QByteArray help = parser.helpText().toLocal8Bit();
        std::fwrite(help.constData(), 1, static_cast<std::size_t>(help.size()), stdout);
        return 0;
    }
    parser.process(application);
    if (parser.isSet(helpOption)) {
        return 0;
    }

    hwtest::app::FrontendLaunchOptions options;
    hwtest::app::ActionResult action = hwtest::app::readFrontendOptions(
        parser, QDir::currentPath(), defaults, &options);
    if (!action.ok) {
        return fail(QStringLiteral("arguments"), action);
    }

    hwtest::app::TestApplicationController controller;
    action = hwtest::app::configureController(controller, options);
    if (!action.ok) {
        return failAfterShutdown(&controller, QStringLiteral("configuration"), action);
    }

    action = controller.prepare();
    if (!action.ok) {
        return failAfterShutdown(&controller, QStringLiteral("prepare"), action);
    }

    action = controller.start();
    if (!action.ok) {
        return failAfterShutdown(&controller, QStringLiteral("start"), action);
    }

    action = controller.waitForTerminal();
    if (!action.ok) {
        const QString phase = controller.snapshot().phase;
        if (phase == QStringLiteral("running") ||
            phase == QStringLiteral("paused") ||
            phase == QStringLiteral("stopping")) {
            const hwtest::app::ActionResult stopped = controller.stop();
            if (!stopped.ok) {
                action.message += QStringLiteral("; stop failed (%1): %2")
                                      .arg(stopped.code, stopped.message);
            }
        }
        return failAfterShutdown(&controller, QStringLiteral("run"), action);
    }

    const hwtest::app::ApplicationSnapshot snapshot = controller.snapshot();
    if (!snapshot.hasResult) {
        return failAfterShutdown(
            &controller,
            QStringLiteral("run"),
            {false,
             QStringLiteral("missing_result"),
             snapshot.message.isEmpty()
                 ? QStringLiteral("Run ended without a test result")
                 : snapshot.message});
    }

    action = controller.shutdown();
    if (!action.ok) {
        return fail(QStringLiteral("shutdown"), action);
    }

    return writeJson(stdout,
                     resultJson(snapshot),
                     snapshot.verdict == QStringLiteral("Pass") ? 0 : 1);
}
