#include <app/tui_shell.h>

#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace hwtest::app {

namespace {

QStringList splitCommandLine(const QString& line, QString* error)
{
    QStringList words;
    QString current;
    bool quoted = false;
    for (const QChar character : line) {
        if (character == QLatin1Char('"')) {
            quoted = !quoted;
            continue;
        }
        if (character.isSpace() && !quoted) {
            if (!current.isEmpty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current.append(character);
    }
    if (quoted) {
        if (error != nullptr) {
            *error = QStringLiteral("Unterminated quoted argument");
        }
        return {};
    }
    if (!current.isEmpty()) {
        words.push_back(current);
    }
    return words;
}

TuiCommand invalid(const QString& message)
{
    TuiCommand command;
    command.error = message;
    return command;
}

bool positiveInteger(const QString& value)
{
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok && parsed > 0;
}

QString singleLine(QString value)
{
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return value;
}

} // namespace

TuiCommand parseTuiCommand(const QString& line)
{
    QString splitError;
    QStringList words = splitCommandLine(line.trimmed(), &splitError);
    if (!splitError.isEmpty()) {
        return invalid(splitError);
    }
    if (words.isEmpty()) {
        return invalid(QStringLiteral("Command is empty; enter 'help' for available commands"));
    }

    const QString name = words.takeFirst().toLower();
    TuiCommand command;
    command.arguments = words;
    if (name == QStringLiteral("help") || name == QStringLiteral("?")) {
        command.type = TuiCommandType::Help;
    } else if (name == QStringLiteral("load")) {
        command.type = TuiCommandType::Load;
        if (words.size() != 0 && words.size() != 2) {
            return invalid(QStringLiteral("Usage: load [test-config hal-config]"));
        }
    } else if (name == QStringLiteral("controls")) {
        command.type = TuiCommandType::Controls;
    } else if (name == QStringLiteral("use")) {
        command.type = TuiCommandType::Use;
        if (words.size() != 1) {
            return invalid(QStringLiteral("Usage: use <ResourceId>"));
        }
    } else if (name == QStringLiteral("prepare")) {
        command.type = TuiCommandType::Prepare;
    } else if (name == QStringLiteral("run")) {
        command.type = TuiCommandType::Run;
    } else if (name == QStringLiteral("pause")) {
        command.type = TuiCommandType::Pause;
    } else if (name == QStringLiteral("resume")) {
        command.type = TuiCommandType::Resume;
    } else if (name == QStringLiteral("stop")) {
        command.type = TuiCommandType::Stop;
        if (words.size() > 1 || (words.size() == 1 && !positiveInteger(words.first()))) {
            return invalid(QStringLiteral("Usage: stop [positive-timeout-ms]"));
        }
    } else if (name == QStringLiteral("status")) {
        command.type = TuiCommandType::Status;
    } else if (name == QStringLiteral("wait")) {
        command.type = TuiCommandType::Wait;
        if (words.size() > 1 || (words.size() == 1 && !positiveInteger(words.first()))) {
            return invalid(QStringLiteral("Usage: wait [positive-timeout-ms]"));
        }
    } else if (name == QStringLiteral("result")) {
        command.type = TuiCommandType::Result;
    } else if (name == QStringLiteral("disconnect")) {
        command.type = TuiCommandType::Disconnect;
    } else if (name == QStringLiteral("quit") || name == QStringLiteral("exit")) {
        command.type = TuiCommandType::Quit;
    } else {
        return invalid(QStringLiteral("Unknown command '%1'; enter 'help'").arg(name));
    }

    const bool noArgumentsExpected = command.type == TuiCommandType::Help ||
        command.type == TuiCommandType::Controls ||
        command.type == TuiCommandType::Prepare ||
        command.type == TuiCommandType::Run ||
        command.type == TuiCommandType::Pause ||
        command.type == TuiCommandType::Resume ||
        command.type == TuiCommandType::Status ||
        command.type == TuiCommandType::Result ||
        command.type == TuiCommandType::Disconnect ||
        command.type == TuiCommandType::Quit;
    if (noArgumentsExpected && !words.isEmpty()) {
        return invalid(QStringLiteral("Command '%1' does not accept arguments").arg(name));
    }
    return command;
}

TuiShell::TuiShell(TestApplicationController* controller,
                   QString defaultTestConfigPath,
                   QString defaultHalConfigPath)
    : m_controller(controller)
    , m_defaultTestConfigPath(std::move(defaultTestConfigPath))
    , m_defaultHalConfigPath(std::move(defaultHalConfigPath))
{
}

TuiReply TuiShell::execute(const QString& line)
{
    const TuiCommand command = parseTuiCommand(line);
    if (command.type == TuiCommandType::Invalid) {
        return {{QStringLiteral("error parse %1").arg(command.error)}, false};
    }
    if (command.type == TuiCommandType::Help) {
        return {helpLines(), false};
    }
    if (m_controller == nullptr) {
        return {{QStringLiteral("error controller Application controller is unavailable")}, false};
    }

    switch (command.type) {
    case TuiCommandType::Load: {
        const QString testPath = command.arguments.isEmpty()
            ? m_defaultTestConfigPath
            : command.arguments.at(0);
        const QString halPath = command.arguments.isEmpty()
            ? m_defaultHalConfigPath
            : command.arguments.at(1);
        if (testPath.isEmpty() || halPath.isEmpty()) {
            return {{QStringLiteral("error load Test and HAL configuration paths are required")}, false};
        }
        const ActionResult result = m_controller->loadConfigurations(testPath, halPath);
        if (result.ok) {
            m_defaultTestConfigPath = testPath;
            m_defaultHalConfigPath = halPath;
        }
        return actionReply(QStringLiteral("load"), result);
    }
    case TuiCommandType::Controls: {
        QStringList lines;
        for (const ControlResource& control : m_controller->availableControls()) {
            lines.push_back(QStringLiteral("resource=%1 provider=%2")
                                .arg(singleLine(control.resourceId),
                                     singleLine(control.providerId)));
        }
        if (lines.isEmpty()) {
            lines.push_back(QStringLiteral("controls=unavailable"));
        }
        return {lines, false};
    }
    case TuiCommandType::Use:
        return actionReply(QStringLiteral("use"),
                           m_controller->selectControl(command.arguments.first()));
    case TuiCommandType::Prepare:
        return actionReply(QStringLiteral("prepare"), m_controller->prepare());
    case TuiCommandType::Run:
        return actionReply(QStringLiteral("run"), m_controller->start());
    case TuiCommandType::Pause:
        return actionReply(QStringLiteral("pause"), m_controller->pause());
    case TuiCommandType::Resume:
        return actionReply(QStringLiteral("resume"), m_controller->resume());
    case TuiCommandType::Stop:
        return actionReply(QStringLiteral("stop"),
                           m_controller->stop(command.arguments.isEmpty()
                                                  ? 5000
                                                  : command.arguments.first().toInt()));
    case TuiCommandType::Status:
        return {{statusLine(m_controller->snapshot())}, false};
    case TuiCommandType::Wait:
        return actionReply(QStringLiteral("wait"),
                           m_controller->waitForTerminal(command.arguments.isEmpty()
                                                             ? -1
                                                             : command.arguments.first().toInt()));
    case TuiCommandType::Result: {
        const ApplicationSnapshot snapshot = m_controller->snapshot();
        if (!snapshot.hasResult) {
            return {{QStringLiteral("result=unavailable")}, false};
        }
        const QString raw = QString::fromUtf8(
            QJsonDocument(QJsonObject::fromVariantMap(snapshot.rawData))
                .toJson(QJsonDocument::Compact));
        return {{QStringLiteral("step=%1 item=%2 algorithm=%3 attempts=%4")
                     .arg(singleLine(snapshot.stepId),
                          singleLine(snapshot.testItemId),
                          singleLine(snapshot.algorithmId),
                          QString::number(snapshot.attempts)),
                 QStringLiteral("verdict=%1 error=%2 message=%3")
                     .arg(singleLine(snapshot.verdict),
                          singleLine(snapshot.errorCode),
                          singleLine(snapshot.message)),
                 QStringLiteral("rawData=%1").arg(raw)},
                false};
    }
    case TuiCommandType::Disconnect:
        return actionReply(QStringLiteral("disconnect"), m_controller->shutdown());
    case TuiCommandType::Quit: {
        const ActionResult shutdown = m_controller->shutdown();
        TuiReply reply = actionReply(QStringLiteral("quit"), shutdown);
        reply.quit = true;
        reply.exitCode = shutdown.ok ? 0 : 2;
        return reply;
    }
    case TuiCommandType::Invalid:
    case TuiCommandType::Help:
        break;
    }
    return {{QStringLiteral("error internal Unhandled TUI command")}, false};
}

QStringList TuiShell::helpLines()
{
    return {
        QStringLiteral("load [test-config hal-config]  validate configuration files"),
        QStringLiteral("controls                       list configured control resources"),
        QStringLiteral("use <ResourceId>               select the PC control resource"),
        QStringLiteral("prepare                        initialize HAL, algorithm and BIZ"),
        QStringLiteral("run                            start SYSTEM_STATUS"),
        QStringLiteral("pause | resume                 control an active test"),
        QStringLiteral("stop [timeout-ms]              stop an active test"),
        QStringLiteral("status                         show current state and progress"),
        QStringLiteral("wait [timeout-ms]              wait for a terminal state"),
        QStringLiteral("result                         show the latest result"),
        QStringLiteral("disconnect                     release BIZ and HAL resources"),
        QStringLiteral("quit                           disconnect and exit"),
    };
}

TuiReply TuiShell::actionReply(const QString& action, const ActionResult& result)
{
    if (result.ok) {
        return {{QStringLiteral("ok %1").arg(action)}, false};
    }
    return {{QStringLiteral("error %1 %2")
                 .arg(singleLine(result.code), singleLine(result.message))},
            false};
}

QString TuiShell::statusLine(const ApplicationSnapshot& snapshot)
{
    return QStringLiteral("phase=%1 state=%2 control=%3 provider=%4 progress=%5 task=%6 step=%7 error=%8 message=%9")
        .arg(singleLine(snapshot.phase),
             singleLine(snapshot.testState),
             singleLine(snapshot.controlResourceId),
             singleLine(snapshot.providerId),
             QString::number(snapshot.progress),
             singleLine(snapshot.taskId),
             singleLine(snapshot.progressStep),
             singleLine(snapshot.errorCode),
             singleLine(snapshot.message));
}

} // namespace hwtest::app
