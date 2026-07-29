#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QString>

namespace hwtest::app::web {
namespace {

QString readProjectFile(const QString& relativePath)
{
    QFile file(QDir(QStringLiteral(HWTEST_PROJECT_SOURCE_DIR)).filePath(relativePath));
    if (!file.open(QIODevice::ReadOnly)) {
        ADD_FAILURE() << file.errorString().toStdString();
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

TEST(WebArchitectureTest, UsesOnlyApplicationControllerAndQtWebSockets)
{
    const QString sources =
        readProjectFile(QStringLiteral("src/app/web/web_protocol.h")) +
        readProjectFile(QStringLiteral("src/app/web/web_protocol.cpp")) +
        readProjectFile(QStringLiteral("src/app/web/web_telemetry_batcher.h")) +
        readProjectFile(QStringLiteral("src/app/web/web_telemetry_batcher.cpp")) +
        readProjectFile(QStringLiteral("src/app/web/web_socket_frontend_server.h")) +
        readProjectFile(QStringLiteral("src/app/web/web_socket_frontend_server.cpp")) +
        readProjectFile(QStringLiteral("src/app/web/web_main.cpp"));

    EXPECT_TRUE(sources.contains(QStringLiteral("QWebSocketServer")));
    EXPECT_TRUE(sources.contains(QStringLiteral("TestApplicationController")));
    EXPECT_TRUE(sources.contains(QStringLiteral("bytesToWrite")));
    EXPECT_TRUE(sources.contains(QStringLiteral("bytesWritten")));
    EXPECT_TRUE(sources.contains(QStringLiteral("telemetry_backpressure")));
    for (const QString& forbidden : {
             QStringLiteral("<hal/"),
             QStringLiteral("<biz/"),
             QStringLiteral("<algorithm/"),
             QStringLiteral("QSerialPort"),
             QStringLiteral("QUdpSocket"),
             QStringLiteral("QTcpSocket"),
             QStringLiteral("waitForTerminal(")}) {
        EXPECT_FALSE(sources.contains(forbidden)) << forbidden.toStdString();
    }
}

TEST(WebArchitectureTest, SupportTargetDoesNotLinkHardwareLayers)
{
    const QString cmake = readProjectFile(QStringLiteral("src/app/CMakeLists.txt"));
    const int linkStart = cmake.indexOf(
        QStringLiteral("target_link_libraries(hwtest_web_support"));
    ASSERT_GE(linkStart, 0);
    const int linkEnd = cmake.indexOf(QLatin1Char(')'), linkStart);
    ASSERT_GT(linkEnd, linkStart);
    const QString linkBlock = cmake.mid(linkStart, linkEnd - linkStart + 1);

    EXPECT_TRUE(linkBlock.contains(QStringLiteral("hwtest_app_core")));
    EXPECT_TRUE(linkBlock.contains(QStringLiteral("${HWTEST_QT_CORE_TARGET}")));
    EXPECT_TRUE(linkBlock.contains(QStringLiteral("${HWTEST_QT_WEBSOCKETS_TARGET}")));
    EXPECT_TRUE(cmake.contains(QStringLiteral("web/web_telemetry_batcher.h")));
    EXPECT_TRUE(cmake.contains(QStringLiteral("web/web_telemetry_batcher.cpp")));
    for (const QString& forbidden : {
             QStringLiteral("hwtest_hal"),
             QStringLiteral("hwtest_biz"),
             QStringLiteral("hwtest_algorithm"),
             QStringLiteral("HWTEST_QT_SERIALPORT_TARGET"),
             QStringLiteral("HWTEST_QT_NETWORK_TARGET")}) {
        EXPECT_FALSE(linkBlock.contains(forbidden)) << forbidden.toStdString();
    }
}

} // namespace
} // namespace hwtest::app::web
