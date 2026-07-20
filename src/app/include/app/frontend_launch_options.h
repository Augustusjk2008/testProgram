#pragma once

#include <app/test_application_controller.h>

#include <QString>

class QCommandLineParser;

namespace hwtest::app {

struct FrontendLaunchOptions {
    QString testConfigPath;
    QString halConfigPath;
    QString controlResourceId;
    QString serialPortName;
};

struct FrontendOptionDefaults {
    QString testConfigPath;
    QString halConfigPath;
    bool requireConfigPaths = false;
};

void addFrontendOptions(QCommandLineParser& parser,
                        const FrontendOptionDefaults& defaults);

ActionResult readFrontendOptions(const QCommandLineParser& parser,
                                 const QString& baseDirectory,
                                 const FrontendOptionDefaults& defaults,
                                 FrontendLaunchOptions* output);

ActionResult configureController(TestApplicationController& controller,
                                 const FrontendLaunchOptions& options);

} // namespace hwtest::app
