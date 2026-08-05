#include "web_socket_frontend_server.h"

#include "support/websocket_test_client.h"
#include "support/mbddf_udp_test_peer.h"
#include "support/application_snapshot_test_utils.h"

#include <app/tui_shell.h>

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace hwtest::app::web {
namespace {

FrontendLaunchOptions launchOptions(const QString& halConfigPath = {})
{
    return FrontendLaunchOptions{QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                 halConfigPath.isEmpty()
                                     ? QStringLiteral(HWTEST_APP_HAL_CONFIG)
                                     : halConfigPath,
                                 {},
                                 {}};
}

QString requestText(const QString& id,
                    const QString& action,
                    const QJsonObject& params = {})
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("v"), 1},
                                  {QStringLiteral("type"),
                                   QStringLiteral("request")},
                                  {QStringLiteral("id"), id},
                                  {QStringLiteral("action"), action},
                                  {QStringLiteral("params"), params}})
            .toJson(QJsonDocument::Compact));
}

void connectClient(WebSocketFrontendServer* server,
                   test::WebSocketTestClient* client)
{
    ASSERT_TRUE(server->listen());
    ASSERT_TRUE(client->connectTo(server->webSocketUrl()))
        << client->errorString().toStdString();
    ASSERT_TRUE(client->waitForMessageCount(2));
}

QJsonObject sendAndWait(test::WebSocketTestClient* client,
                        const QString& id,
                        const QString& action,
                        const QJsonObject& params = {})
{
    EXPECT_GT(client->sendText(requestText(id, action, params)), 0);
    QJsonObject reply;
    EXPECT_TRUE(client->waitForReply(id, &reply))
        << client->events().join(QStringLiteral(" | ")).toStdString();
    return reply;
}

QVector<SerialPortInfo> testSerialPorts()
{
    return {
        SerialPortInfo{QStringLiteral("COM41"), QStringLiteral("primary"), {}, {}, {}},
        SerialPortInfo{QStringLiteral("COM42"), QStringLiteral("auxiliary"), {}, {}, {}},
    };
}

bool writeJsonObject(const QString& path, const QJsonObject& value)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(QJsonDocument(value).toJson(QJsonDocument::Indented)) >= 0;
}

QString fileTimestampFromMetadata(const QString& timestamp)
{
    QString date = timestamp.left(10);
    QString time = timestamp.mid(11, 8);
    date.remove(QLatin1Char('-'));
    time.remove(QLatin1Char(':'));
    return date + QLatin1Char('_') + time + QLatin1Char('_') + timestamp.mid(20, 6);
}

QJsonObject niAdapterHalConfig(const QString& libraryPath)
{
    QJsonObject adapter{
        {QStringLiteral("adapterId"), QStringLiteral("ni.daqmx")},
        {QStringLiteral("libraryPath"), libraryPath},
        {QStringLiteral("settings"),
         QJsonObject{{QStringLiteral("timeoutSeconds"), 0.25}}},
    };
    QJsonObject adapters;
    adapters.insert(QStringLiteral("ni.daqmx"), adapter);
    return QJsonObject{{QStringLiteral("adapters"), adapters}};
}

FrontendLaunchOptions hardwareOptionsLaunchOptions(const QString& halConfigPath,
                                                    const QString& configurationDirectory)
{
    return FrontendLaunchOptions{QStringLiteral(HWTEST_APP_TEST_CONFIG),
                                 halConfigPath,
                                 {},
                                 {},
                                 {},
                                 configurationDirectory};
}

TEST(WebSocketControllerIntegrationTest, QueuesLoadAndReturnsCachedSnapshot)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    ASSERT_GT(client.sendText(requestText(QStringLiteral("load-1"),
                                          QStringLiteral("load"))),
              0);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));

    QJsonObject loadReply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("load-1"), &loadReply));
    EXPECT_TRUE(loadReply.value(QStringLiteral("ok")).toBool())
        << loadReply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));

    const QJsonObject snapshotReply = sendAndWait(
        &client, QStringLiteral("snapshot-1"), QStringLiteral("snapshot"));
    ASSERT_TRUE(snapshotReply.value(QStringLiteral("ok")).toBool());
    const QJsonObject data = snapshotReply.value(QStringLiteral("data")).toObject();
    EXPECT_GE(data.value(QStringLiteral("seq")).toInt(), 1);
    EXPECT_EQ(data.value(QStringLiteral("snapshot"))
                  .toObject()
                  .value(QStringLiteral("phase"))
                  .toString(),
              QStringLiteral("configured"));
}

TEST(WebSocketControllerIntegrationTest,
     ConfigCatalogListsDocumentsAndConfigDocumentUsesOnlyDocumentId)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject catalogReply = sendAndWait(
        &client, QStringLiteral("config-catalog"), QStringLiteral("configCatalog"));
    ASSERT_TRUE(catalogReply.value(QStringLiteral("ok")).toBool())
        << catalogReply.value(QStringLiteral("message")).toString().toStdString();
    const QJsonObject catalog = catalogReply.value(QStringLiteral("data")).toObject();
    const QJsonArray items = catalog.value(QStringLiteral("items")).toArray();
    ASSERT_FALSE(items.isEmpty());
    EXPECT_FALSE(catalog.value(QStringLiteral("revision")).toString().isEmpty());
    const QString documentId = QStringLiteral("test-config-catalog");

    const QJsonObject pathReply = sendAndWait(
        &client,
        QStringLiteral("config-document-path"),
        QStringLiteral("configDocument"),
        QJsonObject{{QStringLiteral("path"), documentId}});
    EXPECT_FALSE(pathReply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(pathReply.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));

    const QJsonObject documentReply = sendAndWait(
        &client,
        QStringLiteral("config-document"),
        QStringLiteral("configDocument"),
        QJsonObject{{QStringLiteral("documentId"), documentId}});
    ASSERT_TRUE(documentReply.value(QStringLiteral("ok")).toBool())
        << documentReply.value(QStringLiteral("message")).toString().toStdString();
    const QJsonObject document = documentReply.value(QStringLiteral("data")).toObject();
    EXPECT_EQ(document.value(QStringLiteral("documentId")).toString(), documentId);
    EXPECT_FALSE(document.value(QStringLiteral("revision")).toString().isEmpty());
    EXPECT_TRUE(document.value(QStringLiteral("value")).isObject());
}

TEST(WebSocketControllerIntegrationTest,
     HardwareOptionsRejectsParametersAndProjectsDetectedNiDevice)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString halPath = directory.filePath(QStringLiteral("mbddf_pc_hal.json"));
    ASSERT_TRUE(writeJsonObject(
        halPath,
        niAdapterHalConfig(QStringLiteral(HAL_TEST_NI_DAQMX_ADAPTER_FIXTURE_PATH))));

    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(
        &controller, hardwareOptionsLaunchOptions(halPath, directory.path()), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject invalid = sendAndWait(
        &client,
        QStringLiteral("hardware-options-params"),
        QStringLiteral("hardwareOptions"),
        QJsonObject{{QStringLiteral("unexpected"), true}});
    EXPECT_FALSE(invalid.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(invalid.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));

    const QJsonObject reply = sendAndWait(
        &client, QStringLiteral("hardware-options"), QStringLiteral("hardwareOptions"));
    ASSERT_TRUE(reply.value(QStringLiteral("ok")).toBool())
        << reply.value(QStringLiteral("message")).toString().toStdString();
    const QJsonObject data = reply.value(QStringLiteral("data")).toObject();
    EXPECT_EQ(data.value(QStringLiteral("state")).toString(), QStringLiteral("available"));
    EXPECT_TRUE(data.value(QStringLiteral("allowManualEntry")).toBool());
    const QJsonArray devices = data.value(QStringLiteral("devices")).toArray();
    ASSERT_EQ(devices.size(), 1);
    const QJsonObject device = devices.first().toObject();
    EXPECT_EQ(device.value(QStringLiteral("deviceName")).toString(),
              QStringLiteral("PXI1Slot2"));
    EXPECT_EQ(device.value(QStringLiteral("deviceId")).toString(),
              QStringLiteral("PXI1Slot2"));
    EXPECT_EQ(device.value(QStringLiteral("model")).toString(),
              QStringLiteral("PXI-6259"));
    EXPECT_EQ(device.value(QStringLiteral("serialNumber")).toString(),
              QStringLiteral("62590002"));
    EXPECT_TRUE(device.value(QStringLiteral("supportedModules"))
                    .toArray()
                    .contains(QStringLiteral("analog")));
    EXPECT_TRUE(device.value(QStringLiteral("supportedModules"))
                    .toArray()
                    .contains(QStringLiteral("digital")));
    EXPECT_TRUE(device.value(QStringLiteral("supportedModules"))
                    .toArray()
                    .contains(QStringLiteral("counter")));
}

TEST(WebSocketControllerIntegrationTest,
     HardwareOptionsKeepsManualEntryWhenNiIsMissingOrLibraryFails)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString halPath = directory.filePath(QStringLiteral("mbddf_pc_hal.json"));
    ASSERT_TRUE(writeJsonObject(
        halPath, QJsonObject{{QStringLiteral("adapters"), QJsonObject{}}}));

    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(
        &controller, hardwareOptionsLaunchOptions(halPath, directory.path()), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const auto assertManualEntry = [](const QJsonObject& reply,
                                      const QString& expectedState) {
        ASSERT_TRUE(reply.value(QStringLiteral("ok")).toBool())
            << reply.value(QStringLiteral("message")).toString().toStdString();
        const QJsonObject data = reply.value(QStringLiteral("data")).toObject();
        EXPECT_EQ(data.value(QStringLiteral("state")).toString(), expectedState);
        EXPECT_TRUE(data.value(QStringLiteral("allowManualEntry")).toBool());
        EXPECT_TRUE(data.value(QStringLiteral("devices")).toArray().isEmpty());
        EXPECT_FALSE(data.value(QStringLiteral("message")).toString().isEmpty());
    };

    assertManualEntry(
        sendAndWait(&client,
                    QStringLiteral("hardware-options-missing"),
                    QStringLiteral("hardwareOptions")),
        QStringLiteral("unavailable"));

    ASSERT_TRUE(writeJsonObject(
        halPath,
        niAdapterHalConfig(QStringLiteral("Z:/definitely/missing/hwtest_adapter.dll"))));
    assertManualEntry(
        sendAndWait(&client,
                    QStringLiteral("hardware-options-library"),
                    QStringLiteral("hardwareOptions")),
        QStringLiteral("error"));
}

TEST(WebSocketControllerIntegrationTest,
      SaveConfigForwardsExpectedRevisionAndValue)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString testName = QStringLiteral("current.testcfg.json");
    const QString testPath = directory.filePath(testName);
    const QString halPath = directory.filePath(QStringLiteral("mbddf_pc_hal.json"));
    ASSERT_TRUE(QFile::copy(QStringLiteral(HWTEST_APP_TEST_CONFIG), testPath));
    ASSERT_TRUE(QFile::copy(QStringLiteral(HWTEST_APP_HAL_CONFIG), halPath));
    QFile catalogFile(directory.filePath(QStringLiteral("test-config-catalog.json")));
    ASSERT_TRUE(catalogFile.open(QIODevice::WriteOnly));
    const QByteArray catalogBytes = QJsonDocument(QJsonObject{
        {QStringLiteral("schemaVersion"), QStringLiteral("1")},
        {QStringLiteral("entries"),
         QJsonArray{QJsonObject{{QStringLiteral("documentId"), testName},
                                {QStringLiteral("enabled"), true},
                                {QStringLiteral("order"), 0}}}},
    }).toJson(QJsonDocument::Indented);
    ASSERT_EQ(catalogFile.write(catalogBytes), catalogBytes.size());
    catalogFile.close();

    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    FrontendLaunchOptions isolatedOptions{
        testPath, halPath, {}, {}, {}, directory.path()};
    WebSocketFrontendServer server(&controller, isolatedOptions, options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QString documentId = QStringLiteral("test-config-catalog");

    const QJsonObject documentReply = sendAndWait(
        &client,
        QStringLiteral("config-document"),
        QStringLiteral("configDocument"),
        QJsonObject{{QStringLiteral("documentId"), documentId}});
    ASSERT_TRUE(documentReply.value(QStringLiteral("ok")).toBool())
        << documentReply.value(QStringLiteral("message")).toString().toStdString();
    const QJsonObject document = documentReply.value(QStringLiteral("data")).toObject();
    const QJsonValue revision = document.value(QStringLiteral("revision"));
    const QJsonValue value = document.value(QStringLiteral("value"));
    ASSERT_FALSE(revision.toString().isEmpty());
    ASSERT_TRUE(value.isObject());

    const QJsonObject saveReply = sendAndWait(
        &client,
        QStringLiteral("config-save"),
        QStringLiteral("saveConfig"),
        QJsonObject{{QStringLiteral("documentId"), documentId},
                    {QStringLiteral("expectedRevision"), revision},
                    {QStringLiteral("value"), value}});
    ASSERT_TRUE(saveReply.value(QStringLiteral("ok")).toBool())
        << saveReply.value(QStringLiteral("message")).toString().toStdString();
    const QJsonObject saved = saveReply.value(QStringLiteral("data")).toObject();
    EXPECT_EQ(saved.value(QStringLiteral("documentId")).toString(), documentId);
    EXPECT_EQ(saved.value(QStringLiteral("value")), value);
}

TEST(WebSocketControllerIntegrationTest,
     RejectsSaveConfigAfterPreparation)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QString documentId = QStringLiteral("test-config-catalog");
    const QJsonObject documentReply = sendAndWait(
        &client,
        QStringLiteral("config-document"),
        QStringLiteral("configDocument"),
        QJsonObject{{QStringLiteral("documentId"), documentId}});
    ASSERT_TRUE(documentReply.value(QStringLiteral("ok")).toBool())
        << documentReply.value(QStringLiteral("message")).toString().toStdString();
    const QJsonObject document = documentReply.value(QStringLiteral("data")).toObject();
    const QJsonValue revision = document.value(QStringLiteral("revision"));
    const QJsonValue value = document.value(QStringLiteral("value"));
    ASSERT_FALSE(revision.toString().isEmpty());
    ASSERT_TRUE(value.isObject());

    ASSERT_TRUE(sendAndWait(&client, QStringLiteral("load"), QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(sendAndWait(&client, QStringLiteral("prepare"), QStringLiteral("prepare"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QJsonObject rejected = sendAndWait(
        &client,
        QStringLiteral("config-save-prepared"),
        QStringLiteral("saveConfig"),
        QJsonObject{{QStringLiteral("documentId"), documentId},
                    {QStringLiteral("expectedRevision"), revision},
                    {QStringLiteral("value"), value}});
    EXPECT_FALSE(rejected.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));
}

TEST(WebSocketControllerIntegrationTest, MapsControlsPortsAndSelections)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QJsonObject controls = sendAndWait(
        &client, QStringLiteral("controls"), QStringLiteral("controls"));
    const QJsonArray controlArray = controls.value(QStringLiteral("data"))
                                        .toObject()
                                        .value(QStringLiteral("controls"))
                                        .toArray();
    ASSERT_GE(controlArray.size(), 2);
    EXPECT_TRUE(controlArray.first().toObject().contains(QStringLiteral("resourceId")));
    EXPECT_TRUE(controlArray.first().toObject().contains(QStringLiteral("providerId")));

    const QJsonObject ports = sendAndWait(
        &client, QStringLiteral("ports"), QStringLiteral("ports"));
    EXPECT_TRUE(ports.value(QStringLiteral("data"))
                    .toObject()
                    .value(QStringLiteral("ports"))
                    .isArray());

    const QJsonObject selected = sendAndWait(
        &client,
        QStringLiteral("select-network"),
        QStringLiteral("selectControl"),
        QJsonObject{{QStringLiteral("resourceId"),
                     QStringLiteral("CONTROL_NETWORK")}});
    EXPECT_TRUE(selected.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(controller.snapshot().providerId, QStringLiteral("qt.udp"));

    const QJsonObject missing = sendAndWait(
        &client,
        QStringLiteral("missing-port"),
        QStringLiteral("selectSerialPort"));
    EXPECT_FALSE(missing.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(missing.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));
}

TEST(WebSocketControllerIntegrationTest,
     SelectsAnEnumeratedAuxiliarySerialPortForTheUnifiedSerialTest)
{
    const QString serialTestConfig =
        QStringLiteral(HWTEST_PROJECT_SOURCE_DIR) +
        QStringLiteral("/configs/mbddf_serial_test.testcfg.json");
    ASSERT_TRUE(QFileInfo::exists(serialTestConfig));

    TestApplicationController controller(nullptr, &testSerialPorts);
    const QVector<SerialPortInfo> ports = controller.availableSerialPorts();
    WebSocketServerOptions options;
    options.port = 0;
    FrontendLaunchOptions serialOptions = launchOptions();
    serialOptions.testConfigPath = serialTestConfig;
    WebSocketFrontendServer server(&controller, serialOptions, options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QString selectedPort = ports.first().portName;
    const QJsonObject selected = sendAndWait(
        &client,
        QStringLiteral("aux-select"),
        QStringLiteral("selectAuxiliarySerialPort"),
        QJsonObject{{QStringLiteral("portName"),
                     QStringLiteral("  %1  ").arg(selectedPort)}});
    EXPECT_TRUE(selected.value(QStringLiteral("ok")).toBool())
        << selected.value(QStringLiteral("message")).toString().toStdString();

    const QJsonObject snapshot = sendAndWait(
        &client, QStringLiteral("aux-snapshot"), QStringLiteral("snapshot"));
    ASSERT_TRUE(snapshot.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(snapshot.value(QStringLiteral("data"))
                  .toObject()
                  .value(QStringLiteral("snapshot"))
                  .toObject()
                  .value(QStringLiteral("auxiliarySerialPortName"))
                  .toString(),
              selectedPort);
}

TEST(WebSocketControllerIntegrationTest,
     EchoStartRequiresAnAuxiliarySerialPortBeforeOpeningTheControlPort)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }
    const QString serialTestConfig =
        QStringLiteral(HWTEST_PROJECT_SOURCE_DIR) +
        QStringLiteral("/configs/mbddf_serial_test.testcfg.json");
    ASSERT_TRUE(QFileInfo::exists(serialTestConfig));

    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    FrontendLaunchOptions serialOptions = launchOptions();
    serialOptions.testConfigPath = serialTestConfig;
    WebSocketFrontendServer server(&controller, serialOptions, options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("prepare"),
                            QStringLiteral("prepare"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QJsonObject started = sendAndWait(
        &client,
        QStringLiteral("echo-without-aux"),
        QStringLiteral("start"),
        QJsonObject{
            {QStringLiteral("mode"), QStringLiteral("single")},
            {QStringLiteral("algorithmParameters"),
             QJsonObject{{QStringLiteral("test_mode"), 1},
                         {QStringLiteral("link_id"), 0},
                         {QStringLiteral("cycle_count"), 1}}},
        });

    EXPECT_FALSE(started.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(started.value(QStringLiteral("code")).toString(),
              QStringLiteral("auxiliary_serial_port_required"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    EXPECT_TRUE(sendAndWait(&client,
                            QStringLiteral("disconnect"),
                            QStringLiteral("disconnect"))
                    .value(QStringLiteral("ok"))
                    .toBool());
}

TEST(WebSocketControllerIntegrationTest,
     EchoStartRejectsAnAuxiliaryPortThatMatchesThePrimaryControlPort)
{
    if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }
    const QString serialTestConfig =
        QStringLiteral(HWTEST_PROJECT_SOURCE_DIR) +
        QStringLiteral("/configs/mbddf_serial_test.testcfg.json");
    ASSERT_TRUE(QFileInfo::exists(serialTestConfig));

    TestApplicationController controller(nullptr, &testSerialPorts);
    const QVector<SerialPortInfo> ports = controller.availableSerialPorts();
    const QString selectedPort = ports.first().portName;
    WebSocketServerOptions options;
    options.port = 0;
    FrontendLaunchOptions serialOptions = launchOptions();
    serialOptions.testConfigPath = serialTestConfig;
    WebSocketFrontendServer server(&controller, serialOptions, options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(sendAndWait(
                    &client,
                    QStringLiteral("select-primary"),
                    QStringLiteral("selectSerialPort"),
                    QJsonObject{{QStringLiteral("portName"), selectedPort}})
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(sendAndWait(
                    &client,
                    QStringLiteral("select-aux"),
                    QStringLiteral("selectAuxiliarySerialPort"),
                    QJsonObject{{QStringLiteral("portName"), selectedPort}})
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("prepare"),
                            QStringLiteral("prepare"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QJsonObject started = sendAndWait(
        &client,
        QStringLiteral("echo-with-primary-port"),
        QStringLiteral("start"),
        QJsonObject{
            {QStringLiteral("mode"), QStringLiteral("single")},
            {QStringLiteral("algorithmParameters"),
             QJsonObject{{QStringLiteral("test_mode"), 1},
                         {QStringLiteral("link_id"), 0},
                         {QStringLiteral("cycle_count"), 1}}},
        });

    EXPECT_FALSE(started.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(started.value(QStringLiteral("code")).toString(),
              QStringLiteral("auxiliary_serial_conflict"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    EXPECT_TRUE(sendAndWait(&client,
                            QStringLiteral("disconnect"),
                            QStringLiteral("disconnect"))
                    .value(QStringLiteral("ok"))
                    .toBool());
}

TEST(WebSocketControllerIntegrationTest, ReturnsControllerErrorsAndRemainsUsable)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject prepare = sendAndWait(
        &client, QStringLiteral("prepare"), QStringLiteral("prepare"));
    EXPECT_FALSE(prepare.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(prepare.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));

    const QJsonObject stop = sendAndWait(
        &client, QStringLiteral("stop"), QStringLiteral("stop"));
    EXPECT_FALSE(stop.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(stop.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));

    const QJsonObject load = sendAndWait(
        &client, QStringLiteral("load"), QStringLiteral("load"));
    EXPECT_TRUE(load.value(QStringLiteral("ok")).toBool());
}

TEST(WebSocketControllerIntegrationTest, RejectsMistypedContinuousRunParametersAtBoundary)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject reply = sendAndWait(
        &client,
        QStringLiteral("bad-run-options"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), QStringLiteral("fast")},
                    {QStringLiteral("maxCycles"), 2}});

    EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));

    const QJsonObject saveReply = sendAndWait(
        &client,
        QStringLiteral("bad-save-option"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 2},
                    {QStringLiteral("saveData"), QStringLiteral("true")}});
    EXPECT_FALSE(saveReply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saveReply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));

    const QJsonObject parametersReply = sendAndWait(
        &client,
        QStringLiteral("bad-algorithm-parameters"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("single")},
                    {QStringLiteral("algorithmParameters"),
                     QJsonArray{1, 2, 3}}});
    EXPECT_FALSE(parametersReply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(parametersReply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(WebSocketControllerIntegrationTest,
     ValidatesContinuousDataDestinationParametersAtBoundary)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject typeDirectory = sendAndWait(
        &client,
        QStringLiteral("data-directory-type"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 1},
                    {QStringLiteral("saveData"), true},
                    {QStringLiteral("dataDirectory"), 42}});
    EXPECT_FALSE(typeDirectory.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(typeDirectory.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_TRUE(typeDirectory.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("dataDirectory")));
    EXPECT_TRUE(typeDirectory.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("string")));

    const QJsonObject typeFileName = sendAndWait(
        &client,
        QStringLiteral("data-file-name-type"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 1},
                    {QStringLiteral("saveData"), true},
                    {QStringLiteral("dataFileName"), true}});
    EXPECT_FALSE(typeFileName.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(typeFileName.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_TRUE(typeFileName.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("dataFileName")));
    EXPECT_TRUE(typeFileName.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("string")));

    const QJsonObject relativeDirectory = sendAndWait(
        &client,
        QStringLiteral("relative-data-directory"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 1},
                    {QStringLiteral("saveData"), true},
                    {QStringLiteral("dataDirectory"), QStringLiteral("relative-output")}});
    EXPECT_FALSE(relativeDirectory.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(relativeDirectory.value(QStringLiteral("code")).toString(),
              QStringLiteral("ParameterRangeError"));
    EXPECT_TRUE(relativeDirectory.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("absolute")));

    const QJsonObject pathFileName = sendAndWait(
        &client,
        QStringLiteral("path-data-file-name"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 1},
                    {QStringLiteral("saveData"), true},
                    {QStringLiteral("dataFileName"),
                     QStringLiteral("nested/capture.txt")}});
    EXPECT_FALSE(pathFileName.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(pathFileName.value(QStringLiteral("code")).toString(),
              QStringLiteral("ParameterRangeError"));
    EXPECT_TRUE(pathFileName.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("separator")));

    const QJsonObject windowsPathFileName = sendAndWait(
        &client,
        QStringLiteral("windows-path-data-file-name"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 1},
                    {QStringLiteral("saveData"), true},
                    {QStringLiteral("dataFileName"),
                     QStringLiteral("nested\\capture.txt")}});
    EXPECT_FALSE(windowsPathFileName.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(windowsPathFileName.value(QStringLiteral("code")).toString(),
              QStringLiteral("ParameterRangeError"));
    EXPECT_TRUE(windowsPathFileName.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("separator")));

    const QJsonObject driveRelativeDirectory = sendAndWait(
        &client,
        QStringLiteral("drive-relative-data-directory"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 1},
                    {QStringLiteral("saveData"), true},
                    {QStringLiteral("dataDirectory"), QStringLiteral("H:relative-output")}});
    EXPECT_FALSE(driveRelativeDirectory.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(driveRelativeDirectory.value(QStringLiteral("code")).toString(),
              QStringLiteral("ParameterRangeError"));
    EXPECT_TRUE(driveRelativeDirectory.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("absolute")));

#ifdef Q_OS_WIN
    const QJsonObject incompleteUncDirectory = sendAndWait(
        &client,
        QStringLiteral("incomplete-unc-data-directory"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 1},
                    {QStringLiteral("saveData"), true},
                    {QStringLiteral("dataDirectory"), QStringLiteral("//server")}});
    EXPECT_FALSE(incompleteUncDirectory.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(incompleteUncDirectory.value(QStringLiteral("code")).toString(),
              QStringLiteral("ParameterRangeError"));
    EXPECT_TRUE(incompleteUncDirectory.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("absolute")));
#endif

    const QJsonObject wrongExtension = sendAndWait(
        &client,
        QStringLiteral("wrong-data-file-extension"),
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("mode"), QStringLiteral("pc_periodic")},
                    {QStringLiteral("intervalMs"), 10},
                    {QStringLiteral("maxCycles"), 1},
                    {QStringLiteral("saveData"), true},
                    {QStringLiteral("dataFileName"), QStringLiteral("capture.csv")}});
    EXPECT_FALSE(wrongExtension.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(wrongExtension.value(QStringLiteral("code")).toString(),
              QStringLiteral("ParameterRangeError"));
    EXPECT_TRUE(wrongExtension.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral(".txt")));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(WebSocketControllerIntegrationTest, RejectsClientSuppliedConfigurationPaths)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject reply = sendAndWait(
        &client,
        QStringLiteral("load-path"),
        QStringLiteral("load"),
        QJsonObject{{QStringLiteral("testConfigPath"),
                     QStringLiteral("C:/untrusted.json")}});
    EXPECT_FALSE(reply.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(reply.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("empty"));
}

TEST(WebSocketControllerIntegrationTest, ValidatesDigitalStimulusActionWhitelist)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);

    const QJsonObject leaked = sendAndWait(
        &client,
        QStringLiteral("stimulus-leak"),
        QStringLiteral("setDigitalStimulus"),
        QJsonObject{{QStringLiteral("switchId"), QStringLiteral("di0")},
                    {QStringLiteral("active"), true},
                    {QStringLiteral("expectedRevision"), 0},
                    {QStringLiteral("resourceId"), QStringLiteral("DUT_DI0_STIM")}});
    EXPECT_FALSE(leaked.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(leaked.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));

    const QJsonObject wrongType = sendAndWait(
        &client,
        QStringLiteral("stimulus-type"),
        QStringLiteral("setDigitalStimulus"),
        QJsonObject{{QStringLiteral("switchId"), QStringLiteral("di0")},
                    {QStringLiteral("active"), QStringLiteral("true")},
                    {QStringLiteral("expectedRevision"), 0}});
    EXPECT_FALSE(wrongType.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(wrongType.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));

    const QJsonObject resetWithParams = sendAndWait(
        &client,
        QStringLiteral("stimulus-reset"),
        QStringLiteral("resetDigitalStimulus"),
        QJsonObject{{QStringLiteral("adapterId"), QStringLiteral("ni.daqmx")}});
    EXPECT_FALSE(resetWithParams.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resetWithParams.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_envelope"));

    const QJsonObject unavailable = sendAndWait(
        &client,
        QStringLiteral("stimulus-unavailable"),
        QStringLiteral("setDigitalStimulus"),
        QJsonObject{{QStringLiteral("switchId"), QStringLiteral("di0")},
                    {QStringLiteral("active"), true},
                    {QStringLiteral("expectedRevision"), 0}});
    EXPECT_FALSE(unavailable.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(unavailable.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_state"));
    const QJsonObject authoritative = unavailable
        .value(QStringLiteral("data"))
        .toObject()
        .value(QStringLiteral("digitalStimulus"))
        .toObject();
    EXPECT_FALSE(authoritative.isEmpty());
    EXPECT_FALSE(authoritative.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(authoritative.value(QStringLiteral("revision")).toDouble(), 0.0);
}

TEST(WebSocketControllerIntegrationTest, DisconnectShutsDownSessionAndKeepsListening)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient first;
    connectClient(&server, &first);
    ASSERT_TRUE(sendAndWait(&first,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    ASSERT_GT(first.sendText(requestText(QStringLiteral("disconnect"),
                                         QStringLiteral("disconnect"))),
              0);
    QJsonObject reply;
    ASSERT_TRUE(first.waitForReply(QStringLiteral("disconnect"), &reply));
    EXPECT_TRUE(reply.value(QStringLiteral("ok")).toBool())
        << reply.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_TRUE(first.waitForDisconnected());
    EXPECT_TRUE(server.isListening());
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));

    test::WebSocketTestClient second;
    ASSERT_TRUE(second.connectTo(server.webSocketUrl()));
    ASSERT_TRUE(second.waitForMessageCount(2));
    EXPECT_EQ(second.messages()
                  .at(1)
                  .value(QStringLiteral("snapshot"))
                  .toObject()
                  .value(QStringLiteral("phase"))
                  .toString(),
              QStringLiteral("configured"));
    EXPECT_TRUE(sendAndWait(&second,
                            QStringLiteral("reload"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());
}

TEST(WebSocketControllerIntegrationTest, QuitRepliesThenStopsListeningAndRequestsExit)
{
    TestApplicationController controller;
    WebSocketServerOptions options;
    options.port = 0;
    WebSocketFrontendServer server(&controller, launchOptions(), options);
    test::WebSocketTestClient client;
    connectClient(&server, &client);
    ASSERT_TRUE(sendAndWait(&client,
                            QStringLiteral("load"),
                            QStringLiteral("load"))
                    .value(QStringLiteral("ok"))
                    .toBool());

    bool quitRequested = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&server,
                     &WebSocketFrontendServer::quitRequested,
                     &loop,
                     [&] {
                         quitRequested = true;
                         loop.quit();
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    ASSERT_GT(client.sendText(requestText(QStringLiteral("quit"),
                                          QStringLiteral("quit"))),
              0);
    QJsonObject reply;
    ASSERT_TRUE(client.waitForReply(QStringLiteral("quit"), &reply));
    EXPECT_TRUE(reply.value(QStringLiteral("ok")).toBool())
        << reply.value(QStringLiteral("message")).toString().toStdString();
    if (!quitRequested) {
        timeout.start(3000);
        loop.exec();
    }
    EXPECT_TRUE(quitRequested);
    EXPECT_FALSE(server.isListening());
    EXPECT_TRUE(client.waitForDisconnected());
}

class WebSocketUdpIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!QFileInfo(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR")).isDir()) {
            GTEST_SKIP() << "MB_DDF protocol assets are not available";
        }
        ASSERT_TRUE(directory.isValid());
        ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
        ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                        &directory,
                                        &halConfigPath,
                                        &error))
            << error.toStdString();

        WebSocketServerOptions options;
        options.port = 0;
        server = std::make_unique<WebSocketFrontendServer>(
            &controller, launchOptions(halConfigPath), options);
        client = std::make_unique<test::WebSocketTestClient>();
        connectClient(server.get(), client.get());
        ASSERT_TRUE(sendAndWait(client.get(),
                                QStringLiteral("load"),
                                QStringLiteral("load"))
                        .value(QStringLiteral("ok"))
                        .toBool());
        ASSERT_TRUE(sendAndWait(client.get(),
                                QStringLiteral("prepare"),
                                QStringLiteral("prepare"))
                        .value(QStringLiteral("ok"))
                        .toBool());
        ASSERT_TRUE(sendAndWait(client.get(),
                                QStringLiteral("start"),
                                QStringLiteral("start"))
                        .value(QStringLiteral("ok"))
                        .toBool());
        ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
    }

    void disconnectIfNeeded()
    {
        if (client == nullptr ||
            client->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        client->sendText(requestText(QStringLiteral("cleanup"),
                                     QStringLiteral("disconnect")));
        QJsonObject reply;
        client->waitForReply(QStringLiteral("cleanup"), &reply, 5000);
        client->waitForDisconnected(5000);
    }

    TestApplicationController controller;
    test::MbddfUdpTestPeer peer;
    QTemporaryDir directory;
    QString error;
    QString halConfigPath;
    std::unique_ptr<WebSocketFrontendServer> server;
    std::unique_ptr<test::WebSocketTestClient> client;
};

TEST_F(WebSocketUdpIntegrationTest, PassesSystemStatusThroughHalUdp)
{
    ASSERT_TRUE(peer.replyToLastRequest(&error)) << error.toStdString();
    QJsonObject message;
    ASSERT_TRUE(client->waitForSnapshotPhase(QStringLiteral("finished"),
                                             &message,
                                             5000));
    const QJsonObject snapshot = message.value(QStringLiteral("snapshot")).toObject();
    EXPECT_TRUE(snapshot.value(QStringLiteral("hasResult")).toBool());
    EXPECT_EQ(snapshot.value(QStringLiteral("verdict")).toString(),
              QStringLiteral("Pass"));
    EXPECT_TRUE(snapshot.value(QStringLiteral("rawData"))
                    .toObject()
                    .contains(QStringLiteral("responseValues")));
    disconnectIfNeeded();
}

TEST_F(WebSocketUdpIntegrationTest, ReportsTimeoutWithoutPeerResponse)
{
    QJsonObject message;
    ASSERT_TRUE(client->waitForSnapshotPhase(QStringLiteral("error"),
                                             &message,
                                             6000));
    const QJsonObject snapshot = message.value(QStringLiteral("snapshot")).toObject();
    EXPECT_TRUE(snapshot.value(QStringLiteral("hasResult")).toBool());
    EXPECT_EQ(snapshot.value(QStringLiteral("verdict")).toString(),
              QStringLiteral("Error"));
    EXPECT_EQ(snapshot.value(QStringLiteral("errorCode")).toString(),
              QStringLiteral("BusTimeout"));
    disconnectIfNeeded();
}

TEST_F(WebSocketUdpIntegrationTest, StopIsAsyncAndRejectsWritesWhilePending)
{
    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeatCount; });
    heartbeat.start();

    ASSERT_GT(client->sendText(requestText(QStringLiteral("stop"),
                                           QStringLiteral("stop"))),
              0);
    ASSERT_GT(client->sendText(requestText(QStringLiteral("pause"),
                                           QStringLiteral("pause"))),
              0);

    QJsonObject blocked;
    ASSERT_TRUE(client->waitForReply(QStringLiteral("pause"), &blocked, 5000));
    EXPECT_FALSE(blocked.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(blocked.value(QStringLiteral("code")).toString(),
              QStringLiteral("command_in_progress"));

    QJsonObject stopped;
    ASSERT_TRUE(client->waitForReply(QStringLiteral("stop"), &stopped, 5000));
    EXPECT_TRUE(stopped.value(QStringLiteral("ok")).toBool())
        << stopped.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("stopped"));
    EXPECT_GT(heartbeatCount, 0);
    disconnectIfNeeded();
}
} // namespace
} // namespace hwtest::app::web
