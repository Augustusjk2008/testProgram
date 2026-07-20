#include "gui_main_window.h"

#include <app/frontend_launch_options.h>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("hwtest_gui"));
    QApplication::setOrganizationName(QStringLiteral("hwtest"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Qt Widgets frontend for MB_DDF_v2 SYSTEM_STATUS"));
    parser.addHelpOption();
    const hwtest::app::FrontendOptionDefaults defaults{
        QStringLiteral("configs/mbddf_system_status.testcfg.json"),
        QStringLiteral("configs/mbddf_pc_hal.json"),
        false};
    hwtest::app::addFrontendOptions(parser, defaults);
    QCommandLineOption smokeTestOption(QStringLiteral("smoke-test"));
    smokeTestOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(smokeTestOption);
    parser.process(application);

    hwtest::app::FrontendLaunchOptions options;
    const hwtest::app::ActionResult parsed = hwtest::app::readFrontendOptions(
        parser, QDir::currentPath(), defaults, &options);
    if (!parsed.ok) {
        const QByteArray message = QStringLiteral("%1: %2\n")
                                       .arg(parsed.code, parsed.message)
                                       .toLocal8Bit();
        std::fwrite(message.constData(), 1,
                    static_cast<std::size_t>(message.size()), stderr);
        return 2;
    }

    hwtest::app::TestApplicationController controller;
    hwtest::app::GuiMainWindow window(&controller, options);
    window.show();
    if (parser.isSet(smokeTestOption)) {
        QTimer::singleShot(0, &window, &QWidget::close);
    }
    return application.exec();
}
