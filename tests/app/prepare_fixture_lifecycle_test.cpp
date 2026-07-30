#include <app/test_application_controller.h>

#include "support/mbddf_udp_test_peer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace hwtest::app {
namespace {

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const QByteArray& value)
        : m_name(name)
        , m_previous(qgetenv(name))
        , m_existed(qEnvironmentVariableIsSet(name))
    {
        qputenv(m_name.constData(), value);
    }

    ~ScopedEnvironmentVariable()
    {
        if (m_existed) {
            qputenv(m_name.constData(), m_previous);
        } else {
            qunsetenv(m_name.constData());
        }
    }

private:
    QByteArray m_name;
    QByteArray m_previous;
    bool m_existed = false;
};

QCoreApplication& ensureQtApplication()
{
    if (QCoreApplication* existing = QCoreApplication::instance()) {
        return *existing;
    }
    static int argc = 1;
    static char argument[] = "prepare_fixture_lifecycle_test";
    static char* argv[] = {argument, nullptr};
    static QCoreApplication application(argc, argv);
    return application;
}

QString protocolAssetDirectory()
{
    return QDir(QString::fromLatin1(HWTEST_PROJECT_SOURCE_DIR))
        .filePath(QStringLiteral("dut/docs/design/product_protocol_csv"));
}

bool writeNiHalConfig(const QString& sourcePath,
                      const QString& outputPath,
                      const QString& libraryPath,
                      bool removeAdapter,
                      QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = source.errorString();
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    source.close();
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("HAL configuration is not a JSON object");
        return false;
    }

    QJsonObject root = document.object();
    QJsonObject adapters = root.value(QStringLiteral("adapters")).toObject();
    if (removeAdapter) {
        adapters.remove(QStringLiteral("ni.daqmx"));
    } else {
        QJsonObject ni = adapters.value(QStringLiteral("ni.daqmx")).toObject();
        ni.insert(QStringLiteral("libraryPath"), libraryPath);
        adapters.insert(QStringLiteral("ni.daqmx"), ni);
    }
    root.insert(QStringLiteral("adapters"), adapters);

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson();
    const bool written = output.write(json) == json.size();
    if (!written && error != nullptr) *error = output.errorString();
    return written;
}

bool configureDigitalAdapterFixture(const QString& halPath, QString* error)
{
    QFile source(halPath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = source.errorString();
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    source.close();
    if (!document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("HAL fixture configuration is not a JSON object");
        }
        return false;
    }

    QJsonObject root = document.object();
    QJsonObject adapters = root.value(QStringLiteral("adapters")).toObject();
    QJsonObject ni = adapters.value(QStringLiteral("ni.daqmx")).toObject();
    ni.insert(QStringLiteral("libraryPath"),
              QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH));
    adapters.insert(QStringLiteral("ni.daqmx"), ni);
    root.insert(QStringLiteral("adapters"), adapters);

    QJsonObject hardware = root.value(QStringLiteral("hardware")).toObject();
    QJsonArray devices = hardware.value(QStringLiteral("devices")).toArray();
    bool updated = false;
    for (int index = 0; index < devices.size(); ++index) {
        QJsonObject device = devices.at(index).toObject();
        if (device.value(QStringLiteral("alias")).toString() !=
            QStringLiteral("ni6259_stimulus")) {
            continue;
        }
        QJsonObject properties = device.value(QStringLiteral("properties")).toObject();
        QJsonObject vendor = properties.value(QStringLiteral("vendor")).toObject();
        QJsonObject niProperties = vendor.value(QStringLiteral("ni")).toObject();
        niProperties.insert(QStringLiteral("deviceName"), QStringLiteral("fixture_device"));
        vendor.insert(QStringLiteral("ni"), niProperties);
        properties.insert(QStringLiteral("vendor"), vendor);
        device.insert(QStringLiteral("properties"), properties);
        devices.replace(index, device);
        updated = true;
        break;
    }
    if (!updated) {
        if (error != nullptr) {
            *error = QStringLiteral("PXI-6259 fixture device is missing from HAL configuration");
        }
        return false;
    }
    hardware.insert(QStringLiteral("devices"), devices);
    root.insert(QStringLiteral("hardware"), hardware);

    QFile output(halPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson();
    const bool written = output.write(json) == json.size();
    if (!written && error != nullptr) *error = output.errorString();
    return written;
}

void expectPrepareRejectsRequiredDigitalFixture(const QString& testConfigPath,
                                                const QString& halConfigPath,
                                                const QString& expectedCode)
{
    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(testConfigPath, halConfigPath).ok);

    const ActionResult prepared = controller.prepare();

    EXPECT_FALSE(prepared.ok);
    EXPECT_EQ(prepared.code, expectedCode) << prepared.message.toStdString();
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
}

TEST(PrepareFixtureLifecycleTest,
     DigitalInputAndOutputRejectMissingAdapterDuringPrepare)
{
    ensureQtApplication();
    const ScopedEnvironmentVariable protocolAssets(
        "MB_DDF_PROTOCOL_CSV_DIR", protocolAssetDirectory().toUtf8());
    ASSERT_TRUE(QFileInfo(protocolAssetDirectory()).isDir());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString halPath = directory.filePath(QStringLiteral("missing-adapter-hal.json"));
    ASSERT_TRUE(writeNiHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG), halPath, {}, true, &error))
        << error.toStdString();

    expectPrepareRejectsRequiredDigitalFixture(QStringLiteral(HWTEST_APP_DI_CONFIG),
                                                halPath,
                                                QStringLiteral("NotSupported"));
    expectPrepareRejectsRequiredDigitalFixture(QStringLiteral(HWTEST_APP_DO_WRITE_CONFIG),
                                                halPath,
                                                QStringLiteral("NotSupported"));
}

TEST(PrepareFixtureLifecycleTest,
     DigitalInputAndOutputRejectMissingAdapterLibraryDuringPrepare)
{
    ensureQtApplication();
    const ScopedEnvironmentVariable protocolAssets(
        "MB_DDF_PROTOCOL_CSV_DIR", protocolAssetDirectory().toUtf8());
    ASSERT_TRUE(QFileInfo(protocolAssetDirectory()).isDir());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString halPath = directory.filePath(QStringLiteral("missing-library-hal.json"));
    ASSERT_TRUE(writeNiHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG), halPath, {}, false, &error))
        << error.toStdString();

    expectPrepareRejectsRequiredDigitalFixture(QStringLiteral(HWTEST_APP_DI_CONFIG),
                                                halPath,
                                                QStringLiteral("AdapterLoadFailed"));
    expectPrepareRejectsRequiredDigitalFixture(QStringLiteral(HWTEST_APP_DO_WRITE_CONFIG),
                                                halPath,
                                                QStringLiteral("AdapterLoadFailed"));
}

TEST(PrepareFixtureLifecycleTest,
     DigitalInputAndOutputRejectConfigureMeDuringPrepare)
{
    ensureQtApplication();
    const ScopedEnvironmentVariable protocolAssets(
        "MB_DDF_PROTOCOL_CSV_DIR", protocolAssetDirectory().toUtf8());
    ASSERT_TRUE(QFileInfo(protocolAssetDirectory()).isDir());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    const QString halPath = directory.filePath(QStringLiteral("configure-me-hal.json"));
    ASSERT_TRUE(writeNiHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                 halPath,
                                 QString::fromLatin1(HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH),
                                 false,
                                 &error))
        << error.toStdString();

    expectPrepareRejectsRequiredDigitalFixture(QStringLiteral(HWTEST_APP_DI_CONFIG),
                                                halPath,
                                                QStringLiteral("NotFound"));
    expectPrepareRejectsRequiredDigitalFixture(QStringLiteral(HWTEST_APP_DO_WRITE_CONFIG),
                                                halPath,
                                                QStringLiteral("NotFound"));
}

TEST(PrepareFixtureLifecycleTest,
     DoFixtureSurvivesFinishedRunAndSupportsSecondStartUntilShutdown)
{
    ensureQtApplication();
    const ScopedEnvironmentVariable protocolAssets(
        "MB_DDF_PROTOCOL_CSV_DIR", protocolAssetDirectory().toUtf8());
    ASSERT_TRUE(QFileInfo(protocolAssetDirectory()).isDir());

    test::MbddfUdpTestPeer peer;
    QString error;
    ASSERT_TRUE(peer.bind(&error)) << error.toStdString();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString halPath;
    ASSERT_TRUE(peer.writeHalConfig(QStringLiteral(HWTEST_APP_HAL_CONFIG),
                                    &directory, &halPath, &error))
        << error.toStdString();
    ASSERT_TRUE(configureDigitalAdapterFixture(halPath, &error))
        << error.toStdString();

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_DO_WRITE_CONFIG), halPath).ok);
    ASSERT_TRUE(controller.prepare().ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));

    const QVariantMap response{
        {QStringLiteral("status"), 0u},
        {QStringLiteral("err_code"), 0u},
        {QStringLiteral("applied_state[0]"), 0x0018u},
        {QStringLiteral("applied_state[1]"), 0u},
    };
    for (int run = 0; run < 2; ++run) {
        ASSERT_TRUE(controller.start().ok) << controller.snapshot().message.toStdString();
        ASSERT_TRUE(peer.waitForRequest(3000, &error)) << error.toStdString();
        ASSERT_TRUE(peer.replyToLastRequest(QStringLiteral("do_write_response"),
                                            response, &error))
            << error.toStdString();
        ASSERT_TRUE(controller.waitForTerminal(3000).ok)
            << controller.snapshot().message.toStdString();
        EXPECT_EQ(controller.snapshot().phase, QStringLiteral("finished"));
        EXPECT_EQ(controller.snapshot().verdict, QStringLiteral("Pass"));
    }

    ASSERT_TRUE(controller.shutdown().ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("configured"));
}

} // namespace
} // namespace hwtest::app
