#include <app/test_application_controller.h>
#include <app/tui_shell.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>

#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

bool stdinIsTerminal()
{
#ifdef Q_OS_WIN
    return ::_isatty(::_fileno(stdin)) != 0;
#else
    return ::isatty(::fileno(stdin)) != 0;
#endif
}

void writeLine(const QString& line)
{
    const QByteArray encoded = line.toUtf8();
    std::fwrite(encoded.constData(), 1, static_cast<std::size_t>(encoded.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

QString singleLine(QString value)
{
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return value;
}

struct InputGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool acknowledged = false;
    bool keepReading = true;
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hwtest_tui"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Interactive staged TUI for MB_DDF_v2 SYSTEM_STATUS"));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption testConfigOption(
        QStringList{QStringLiteral("t"), QStringLiteral("test-config")},
        QStringLiteral("Default BIZ test configuration JSON used by 'load'"),
        QStringLiteral("path"),
        QStringLiteral("configs/mbddf_system_status.testcfg.json"));
    const QCommandLineOption halConfigOption(
        QStringList{QStringLiteral("H"), QStringLiteral("hal-config")},
        QStringLiteral("Default HAL deployment configuration JSON used by 'load'"),
        QStringLiteral("path"),
        QStringLiteral("configs/mbddf_pc_hal.json"));
    const QCommandLineOption controlOption(
        QStringList{QStringLiteral("c"), QStringLiteral("control")},
        QStringLiteral("Control ResourceId applied after 'load'"),
        QStringLiteral("resource-id"));
    const QCommandLineOption serialPortOption(
        QStringList{QStringLiteral("p"), QStringLiteral("serial-port")},
        QStringLiteral("Serial port applied after 'load' without changing the HAL JSON"),
        QStringLiteral("port-name"));
    parser.addOption(testConfigOption);
    parser.addOption(halConfigOption);
    parser.addOption(controlOption);
    parser.addOption(serialPortOption);
    const QStringList arguments = application.arguments();
    if (arguments.contains(QStringLiteral("-?")) ||
        arguments.contains(QStringLiteral("-h")) ||
        arguments.contains(QStringLiteral("--help")) ||
        arguments.contains(QStringLiteral("--help-all"))) {
        writeLine(parser.helpText().trimmed());
        return 0;
    }
    parser.process(application);
    if (parser.isSet(helpOption)) {
        return 0;
    }

    const QString testConfigPath =
        QDir::current().absoluteFilePath(parser.value(testConfigOption));
    const QString halConfigPath =
        QDir::current().absoluteFilePath(parser.value(halConfigOption));
    const bool interactive = stdinIsTerminal();
#ifdef Q_OS_WIN
    if (interactive) {
        ::SetConsoleCP(CP_UTF8);
        ::SetConsoleOutputCP(CP_UTF8);
    }
#endif

    hwtest::app::TestApplicationController controller;
    hwtest::app::TuiShell shell(&controller,
                                testConfigPath,
                                halConfigPath,
                                parser.value(controlOption),
                                parser.value(serialPortOption));
    InputGate gate;
    int processExitCode = 0;

    if (interactive) {
        writeLine(QStringLiteral("hwtest_tui - staged SYSTEM_STATUS console"));
        writeLine(QStringLiteral("Enter 'help' for commands; start with 'load'."));
    }

    std::thread inputThread([&] {
        for (;;) {
            if (interactive) {
                std::fputs("hwtest> ", stdout);
                std::fflush(stdout);
            }

            std::string input;
            if (!std::getline(std::cin, input)) {
                QMetaObject::invokeMethod(
                    &application,
                    [&] {
                        const hwtest::app::ActionResult shutdown = controller.shutdown();
                        if (!shutdown.ok) {
                            writeLine(QStringLiteral("error %1 %2")
                                          .arg(singleLine(shutdown.code),
                                               singleLine(shutdown.message)));
                            processExitCode = 2;
                        }
                        application.exit(processExitCode);
                    },
                    Qt::QueuedConnection);
                break;
            }

            const QString command = QString::fromUtf8(input.data(),
                                                       static_cast<int>(input.size()));
            const bool requiresAcknowledgement =
                !interactive ||
                hwtest::app::parseTuiCommand(command).type !=
                    hwtest::app::TuiCommandType::Wait;
            if (requiresAcknowledgement) {
                std::lock_guard<std::mutex> lock(gate.mutex);
                gate.acknowledged = false;
            }
            QMetaObject::invokeMethod(
                &application,
                [&, command, requiresAcknowledgement] {
                    const hwtest::app::TuiReply reply = shell.execute(command);
                    for (const QString& line : reply.lines) {
                        writeLine(line);
                    }
                    if (requiresAcknowledgement) {
                        std::lock_guard<std::mutex> lock(gate.mutex);
                        gate.keepReading = !reply.quit;
                        gate.acknowledged = true;
                        gate.condition.notify_one();
                    }
                    if (reply.quit) {
                        processExitCode = reply.exitCode;
                        application.exit(processExitCode);
                    }
                },
                Qt::QueuedConnection);

            if (!requiresAcknowledgement) {
                continue;
            }
            std::unique_lock<std::mutex> lock(gate.mutex);
            gate.condition.wait(lock, [&] { return gate.acknowledged; });
            if (!gate.keepReading) {
                break;
            }
        }
    });

    const int exitCode = application.exec();
    inputThread.join();
    return exitCode;
}
