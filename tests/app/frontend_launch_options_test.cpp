#include <app/frontend_launch_options.h>

#include <gtest/gtest.h>

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

namespace hwtest::app {
namespace {

void parseFrontendArguments(QCommandLineParser* parser,
                            const FrontendOptionDefaults& defaults,
                            const QStringList& arguments)
{
    ASSERT_NE(parser, nullptr);
    addFrontendOptions(*parser, defaults);
    EXPECT_TRUE(parser->parse(arguments)) << parser->errorText().toStdString();
}

TEST(FrontendLaunchOptionsTest, ResolvesRelativePathsAgainstProvidedBase)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const FrontendOptionDefaults defaults{QStringLiteral("config/test.json"),
                                          QStringLiteral("config/hal.json"),
                                          false};
    QCommandLineParser parser;
    parseFrontendArguments(
        &parser, defaults,
        {QStringLiteral("frontend"),
         QStringLiteral("--test-config"), QStringLiteral("relative/test.json"),
         QStringLiteral("--hal-config"), QStringLiteral("relative/hal.json")});

    FrontendLaunchOptions options;
    const ActionResult result = readFrontendOptions(parser, directory.path(), defaults,
                                                    &options);

    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_EQ(QDir::cleanPath(options.testConfigPath),
              QDir::cleanPath(directory.filePath(QStringLiteral("relative/test.json"))));
    EXPECT_EQ(QDir::cleanPath(options.halConfigPath),
              QDir::cleanPath(directory.filePath(QStringLiteral("relative/hal.json"))));
}

TEST(FrontendLaunchOptionsTest, DiscoversSelectableTestConfigsFromProjectConfigDirectory)
{
    const FrontendOptionDefaults defaults{
        QStringLiteral("configs/mbddf_system_status.testcfg.json"),
        QStringLiteral("configs/mbddf_pc_hal.json"),
        false};
    QCommandLineParser parser;
    parseFrontendArguments(&parser, defaults, {QStringLiteral("frontend")});

    FrontendLaunchOptions options;
    const ActionResult result = readFrontendOptions(
        parser, QStringLiteral(HWTEST_PROJECT_SOURCE_DIR), defaults, &options);

    ASSERT_TRUE(result.ok) << result.message.toStdString();
    ASSERT_EQ(options.testConfigs.size(), 7);
    bool foundSystem = false;
    bool foundElectrical = false;
    bool foundMemory = false;
    bool foundSpiFlash = false;
    bool foundDhPulse = false;
    bool foundTimer = false;
    bool foundDiRead = false;
    for (const FrontendTestConfigOption& option : options.testConfigs) {
        foundSystem = foundSystem || option.configId == QStringLiteral("mbddf-system-status");
        foundElectrical = foundElectrical || option.configId == QStringLiteral("mbddf-elec-health");
        foundMemory = foundMemory || option.configId == QStringLiteral("mbddf-memperf");
        foundSpiFlash = foundSpiFlash || option.configId == QStringLiteral("mbddf-spi-flash");
        foundDhPulse = foundDhPulse || option.configId == QStringLiteral("mbddf-dh-pulse-config");
        foundTimer = foundTimer || option.configId == QStringLiteral("mbddf-timer-jitter");
        foundDiRead = foundDiRead || option.configId == QStringLiteral("mbddf-di-read");
        EXPECT_FALSE(option.configPath.isEmpty());
        EXPECT_FALSE(option.title.isEmpty());
    }
    EXPECT_TRUE(foundSystem);
    EXPECT_TRUE(foundElectrical);
    EXPECT_TRUE(foundMemory);
    EXPECT_TRUE(foundSpiFlash);
    EXPECT_TRUE(foundDhPulse);
    EXPECT_TRUE(foundTimer);
    EXPECT_TRUE(foundDiRead);
}

TEST(FrontendLaunchOptionsTest, EveryDiscoveredConfigurationCanBeLoadedByController)
{
    const FrontendOptionDefaults defaults{
        QStringLiteral("configs/mbddf_system_status.testcfg.json"),
        QStringLiteral("configs/mbddf_pc_hal.json"),
        false};
    QCommandLineParser parser;
    parseFrontendArguments(&parser, defaults, {QStringLiteral("frontend")});

    FrontendLaunchOptions options;
    const ActionResult discovered = readFrontendOptions(
        parser, QStringLiteral(HWTEST_PROJECT_SOURCE_DIR), defaults, &options);
    ASSERT_TRUE(discovered.ok) << discovered.message.toStdString();

    TestApplicationController controller;
    for (const FrontendTestConfigOption& option : options.testConfigs) {
        const ActionResult loaded = controller.loadConfigurations(
            option.configPath, QStringLiteral(HWTEST_APP_HAL_CONFIG));
        EXPECT_TRUE(loaded.ok)
            << option.algorithmId.toStdString() << ": "
            << loaded.code.toStdString() << " " << loaded.message.toStdString();
    }
}

TEST(FrontendLaunchOptionsTest, RequiresPathsForBatchRunner)
{
    const FrontendOptionDefaults defaults{{}, {}, true};
    QCommandLineParser parser;
    parseFrontendArguments(&parser, defaults, {QStringLiteral("frontend")});
    FrontendLaunchOptions options;

    const ActionResult result = readFrontendOptions(parser, QDir::currentPath(), defaults,
                                                    &options);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("missing_argument"));
}

TEST(FrontendLaunchOptionsTest, AppliesControlThenSerialOverride)
{
    TestApplicationController controller;
    const FrontendLaunchOptions options{
        QStringLiteral(HWTEST_APP_TEST_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG),
        QStringLiteral("CONTROL_SERIAL"),
        QStringLiteral("COM87")};

    const ActionResult result = configureController(controller, options);

    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
    EXPECT_EQ(controller.snapshot().controlResourceId, QStringLiteral("CONTROL_SERIAL"));
    EXPECT_EQ(controller.snapshot().providerId, QStringLiteral("qt.serial"));
    EXPECT_EQ(controller.snapshot().serialPortName, QStringLiteral("COM87"));
}

TEST(FrontendLaunchOptionsTest, StopsAfterInvalidControlOverride)
{
    TestApplicationController controller;
    const FrontendLaunchOptions options{
        QStringLiteral(HWTEST_APP_TEST_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG),
        QStringLiteral("CONTROL_UNKNOWN"),
        QStringLiteral("COM88")};

    const ActionResult result = configureController(controller, options);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("control_not_found"));
    EXPECT_EQ(controller.snapshot().controlResourceId, QStringLiteral("CONTROL_SERIAL"));
    EXPECT_EQ(controller.snapshot().serialPortName, QStringLiteral("COM_CHANGE_ME"));
}

} // namespace
} // namespace hwtest::app
