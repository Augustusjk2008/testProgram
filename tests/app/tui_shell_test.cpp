#include <app/tui_shell.h>

#include <gtest/gtest.h>

namespace hwtest::app {
namespace {

TEST(TuiShellTest, ParsesTheStagedWorkflowCommands)
{
    EXPECT_EQ(parseTuiCommand(QStringLiteral("load")).type, TuiCommandType::Load);
    const TuiCommand load =
        parseTuiCommand(QStringLiteral("load \"test config.json\" \"hal config.json\""));
    ASSERT_EQ(load.arguments.size(), 2);
    EXPECT_EQ(load.arguments.at(0), QStringLiteral("test config.json"));
    EXPECT_EQ(load.arguments.at(1), QStringLiteral("hal config.json"));
    EXPECT_EQ(parseTuiCommand(QStringLiteral("controls")).type, TuiCommandType::Controls);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("ports")).type, TuiCommandType::Ports);
    const TuiCommand use = parseTuiCommand(QStringLiteral("use CONTROL_NETWORK"));
    ASSERT_EQ(use.arguments.size(), 1);
    EXPECT_EQ(use.arguments.first(), QStringLiteral("CONTROL_NETWORK"));
    const TuiCommand port = parseTuiCommand(QStringLiteral("port COM42"));
    ASSERT_EQ(port.arguments.size(), 1);
    EXPECT_EQ(port.arguments.first(), QStringLiteral("COM42"));
    EXPECT_EQ(parseTuiCommand(QStringLiteral("prepare")).type, TuiCommandType::Prepare);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("run")).type, TuiCommandType::Run);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("pause")).type, TuiCommandType::Pause);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("resume")).type, TuiCommandType::Resume);
    const TuiCommand stop = parseTuiCommand(QStringLiteral("stop 750"));
    ASSERT_EQ(stop.arguments.size(), 1);
    EXPECT_EQ(stop.arguments.first(), QStringLiteral("750"));
    EXPECT_EQ(parseTuiCommand(QStringLiteral("wait 3000")).type, TuiCommandType::Wait);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("result")).type, TuiCommandType::Result);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("disconnect")).type, TuiCommandType::Disconnect);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("quit")).type, TuiCommandType::Quit);
}

TEST(TuiShellTest, RejectsUnknownMalformedAndOutOfRangeCommands)
{
    EXPECT_EQ(parseTuiCommand(QStringLiteral("fire")).type, TuiCommandType::Invalid);
    EXPECT_FALSE(parseTuiCommand(QStringLiteral("fire")).error.isEmpty());
    EXPECT_EQ(parseTuiCommand(QStringLiteral("use")).type, TuiCommandType::Invalid);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("port")).type, TuiCommandType::Invalid);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("wait 0")).type, TuiCommandType::Invalid);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("load \"unterminated")).type,
              TuiCommandType::Invalid);
}

TEST(TuiShellTest, LoadsListsSelectsAndShowsConfigurationThroughTheController)
{
    TestApplicationController controller;
    TuiShell shell(&controller,
                   QStringLiteral(HWTEST_APP_TEST_CONFIG),
                   QStringLiteral(HWTEST_APP_HAL_CONFIG));

    const TuiReply loaded = shell.execute(QStringLiteral("load"));
    ASSERT_FALSE(loaded.lines.isEmpty());
    EXPECT_TRUE(loaded.lines.first().startsWith(QStringLiteral("ok load")));

    const TuiReply controls = shell.execute(QStringLiteral("controls"));
    ASSERT_EQ(controls.lines.size(), 2);
    EXPECT_TRUE(controls.lines.at(0).contains(QStringLiteral("CONTROL_NETWORK")));
    EXPECT_TRUE(controls.lines.at(0).contains(QStringLiteral("qt.udp")));
    EXPECT_TRUE(controls.lines.at(1).contains(QStringLiteral("CONTROL_SERIAL")));

    const TuiReply ports = shell.execute(QStringLiteral("ports"));
    EXPECT_FALSE(ports.lines.isEmpty());

    EXPECT_TRUE(shell.execute(QStringLiteral("port COM42"))
                    .lines.first().startsWith(QStringLiteral("ok port")));
    TuiReply status = shell.execute(QStringLiteral("status"));
    ASSERT_EQ(status.lines.size(), 1);
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("serial=COM42")));

    EXPECT_TRUE(shell.execute(QStringLiteral("use CONTROL_NETWORK"))
                    .lines.first().startsWith(QStringLiteral("ok use")));
    status = shell.execute(QStringLiteral("status"));
    ASSERT_EQ(status.lines.size(), 1);
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("phase=configured")));
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("control=CONTROL_NETWORK")));
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("provider=qt.udp")));

    const TuiReply noResult = shell.execute(QStringLiteral("result"));
    ASSERT_EQ(noResult.lines.size(), 1);
    EXPECT_EQ(noResult.lines.first(), QStringLiteral("result=unavailable"));

    const TuiReply quit = shell.execute(QStringLiteral("quit"));
    EXPECT_TRUE(quit.quit);
    EXPECT_EQ(quit.exitCode, 0);
    ASSERT_EQ(quit.lines.size(), 1);
    EXPECT_EQ(quit.lines.first(), QStringLiteral("ok quit"));
}

TEST(TuiShellTest, AppliesCommandLineControlAndSerialDefaultsWhenLoading)
{
    TestApplicationController controller;
    TuiShell shell(&controller,
                   QStringLiteral(HWTEST_APP_TEST_CONFIG),
                   QStringLiteral(HWTEST_APP_HAL_CONFIG),
                   QStringLiteral("CONTROL_SERIAL"),
                   QStringLiteral("COM43"));

    const TuiReply loaded = shell.execute(QStringLiteral("load"));

    ASSERT_EQ(loaded.lines.size(), 1);
    EXPECT_EQ(loaded.lines.first(), QStringLiteral("ok load"));
    const TuiReply status = shell.execute(QStringLiteral("status"));
    ASSERT_EQ(status.lines.size(), 1);
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("control=CONTROL_SERIAL")));
    EXPECT_TRUE(status.lines.first().contains(QStringLiteral("serial=COM43")));
    EXPECT_TRUE(shell.execute(QStringLiteral("quit")).quit);
}

} // namespace
} // namespace hwtest::app
