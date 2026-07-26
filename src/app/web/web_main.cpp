#include "web_socket_frontend_server.h"

#include <app/frontend_launch_options.h>
#include <app/test_application_controller.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTimer>

#include <cstdio>

namespace {

void writeLine(FILE* stream, const QString& line)
{
    const QByteArray encoded = line.toUtf8();
    std::fwrite(encoded.constData(),
                1,
                static_cast<std::size_t>(encoded.size()),
                stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hwtest_web"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Loopback WebSocket backend for MB_DDF_v2 SYSTEM_STATUS; "
        "no browser frontend is implemented"));
    parser.addHelpOption();

    const hwtest::app::FrontendOptionDefaults defaults{
        QStringLiteral("configs/mbddf_system_status.testcfg.json"),
        QStringLiteral("configs/mbddf_pc_hal.json"),
        false};
    hwtest::app::addFrontendOptions(parser, defaults);

    QCommandLineOption webPortOption(
        QStringLiteral("web-port"),
        QStringLiteral("WebSocket listen port on 127.0.0.1 (0 selects a free port)"),
        QStringLiteral("port"),
        QStringLiteral("8765"));
    parser.addOption(webPortOption);
    QCommandLineOption smokeTestOption(QStringLiteral("smoke-test"));
    smokeTestOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(smokeTestOption);
    parser.process(application);

    hwtest::app::FrontendLaunchOptions launchOptions;
    const hwtest::app::ActionResult parsed = hwtest::app::readFrontendOptions(
        parser, QDir::currentPath(), defaults, &launchOptions);
    if (!parsed.ok) {
        writeLine(stderr,
                  QStringLiteral("%1 %2").arg(parsed.code, parsed.message));
        return 2;
    }

    bool portOk = false;
    const uint parsedPort = parser.value(webPortOption).toUInt(&portOk);
    if (!portOk || parsedPort > 65535u) {
        writeLine(stderr,
                  QStringLiteral("invalid_web_port %1")
                      .arg(parser.value(webPortOption)));
        return 2;
    }

    hwtest::app::TestApplicationController controller;
    hwtest::app::web::WebSocketServerOptions serverOptions;
    serverOptions.port = parser.isSet(smokeTestOption)
        ? 0
        : static_cast<quint16>(parsedPort);
    hwtest::app::web::WebSocketFrontendServer server(
        &controller, launchOptions, serverOptions);
    QObject::connect(&server,
                     &hwtest::app::web::WebSocketFrontendServer::quitRequested,
                     &application,
                     &QCoreApplication::quit);

    QString listenError;
    if (!server.listen(&listenError)) {
        writeLine(stderr,
                  QStringLiteral("web_listen_failed %1").arg(listenError));
        return 2;
    }

    writeLine(stdout,
              QStringLiteral("ready wsUrl=%1")
                  .arg(server.webSocketUrl().toString(QUrl::FullyEncoded)));
    if (parser.isSet(smokeTestOption)) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
