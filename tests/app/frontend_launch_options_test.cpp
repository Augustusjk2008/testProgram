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
