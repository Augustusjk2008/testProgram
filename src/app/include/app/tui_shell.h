#pragma once

#include <app/test_application_controller.h>

#include <QString>
#include <QStringList>

namespace hwtest::app {

enum class TuiCommandType {
    Invalid = 0,
    Help,
    Load,
    Controls,
    Ports,
    Use,
    Port,
    Prepare,
    Run,
    Pause,
    Resume,
    Stop,
    Status,
    Wait,
    Result,
    Disconnect,
    Quit
};

struct TuiCommand {
    TuiCommandType type = TuiCommandType::Invalid;
    QStringList arguments;
    QString error;
};

struct TuiReply {
    QStringList lines;
    bool quit = false;
    int exitCode = 0;
};

TuiCommand parseTuiCommand(const QString& line);

class TuiShell {
public:
    TuiShell(TestApplicationController* controller,
             QString defaultTestConfigPath,
             QString defaultHalConfigPath,
             QString defaultControlResource = {},
             QString defaultSerialPort = {});

    TuiReply execute(const QString& line);
    static QStringList helpLines();

private:
    static TuiReply actionReply(const QString& action, const ActionResult& result);
    static QString statusLine(const ApplicationSnapshot& snapshot);

    TestApplicationController* m_controller = nullptr;
    QString m_defaultTestConfigPath;
    QString m_defaultHalConfigPath;
    QString m_defaultControlResource;
    QString m_defaultSerialPort;
};

} // namespace hwtest::app
