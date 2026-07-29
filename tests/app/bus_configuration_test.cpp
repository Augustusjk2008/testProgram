#include <gtest/gtest.h>

#include <app/test_application_controller.h>

#include "mbddf_algorithm_registry.h"

#include <biz/test_config_manager.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace hwtest::app {
namespace {

QVariantMap loadJsonMap(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError)
        << error.errorString().toStdString();
    EXPECT_TRUE(document.isObject());
    return document.object().toVariantMap();
}

TEST(BusConfigurationTest, RegistryContainsLoopAndEchoCommands)
{
    const MbdDfAlgorithmRegistration* loop = findMbdDfAlgorithm(
        QStringLiteral("mbddf.bus_loop"));
    ASSERT_NE(loop, nullptr);
    EXPECT_EQ(loop->requestProfileId, QStringLiteral("bus_loop_test_request"));
    EXPECT_EQ(loop->responseProfileId, QStringLiteral("bus_loop_test_response"));

    const MbdDfAlgorithmRegistration* echo = findMbdDfAlgorithm(
        QStringLiteral("mbddf.bus_echo"));
    ASSERT_NE(echo, nullptr);
    EXPECT_EQ(echo->requestProfileId, QStringLiteral("bus_echo_test_request"));
    EXPECT_EQ(echo->responseProfileId, QStringLiteral("bus_echo_test_response"));
}

TEST(BusConfigurationTest, LoopConfigIsSingleAndContainsNoNetworkOrSpiPath)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(QStringLiteral(HWTEST_APP_BUS_LOOP_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    ASSERT_EQ(loaded.value.steps.size(), 1);
    const hwtest::biz::TestStep& step = loaded.value.steps.first();
    EXPECT_EQ(step.algorithmId, QStringLiteral("mbddf.bus_loop"));
    EXPECT_EQ(step.timeoutMs, 5000);
    const QVariantMap requestValues = step.parameters
        .value(QStringLiteral("protocol")).toMap()
        .value(QStringLiteral("requestValues")).toMap();
    EXPECT_EQ(requestValues.value(QStringLiteral("link_id")).toInt(), 0);
    EXPECT_EQ(requestValues.value(QStringLiteral("total_count")).toInt(), 1000);

    const QVariantMap json = loadJsonMap(QStringLiteral(HWTEST_APP_BUS_LOOP_CONFIG));
    const QStringList modes = json.value(QStringLiteral("reportFields")).toMap()
                                  .value(QStringLiteral("supportedRunModes")).toStringList();
    EXPECT_EQ(modes, QStringList{QStringLiteral("single")});
    const QByteArray serialized = QJsonDocument::fromVariant(json).toJson(
        QJsonDocument::Compact).toLower();
    EXPECT_FALSE(serialized.contains("network"));
    EXPECT_FALSE(serialized.contains("spi"));
}

TEST(BusConfigurationTest, EchoConfigIsPcPeriodicWithFixedCompactPayload)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(QStringLiteral(HWTEST_APP_BUS_ECHO_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    ASSERT_EQ(loaded.value.steps.size(), 1);
    const hwtest::biz::TestStep& step = loaded.value.steps.first();
    EXPECT_EQ(step.algorithmId, QStringLiteral("mbddf.bus_echo"));
    EXPECT_EQ(step.timeoutMs, 5000);
    const QVariantMap requestValues = step.parameters
        .value(QStringLiteral("protocol")).toMap()
        .value(QStringLiteral("requestValues")).toMap();
    EXPECT_EQ(requestValues.value(QStringLiteral("data[0]")).toInt(), 0x4D);
    EXPECT_EQ(requestValues.value(QStringLiteral("data[1]")).toInt(), 0x42);
    EXPECT_EQ(requestValues.value(QStringLiteral("data[2]")).toInt(), 0x31);

    const QVariantMap json = loadJsonMap(QStringLiteral(HWTEST_APP_BUS_ECHO_CONFIG));
    const QVariantMap report = json.value(QStringLiteral("reportFields")).toMap();
    EXPECT_EQ(report.value(QStringLiteral("supportedRunModes")).toStringList(),
              QStringList{QStringLiteral("pc_periodic")});
    const QVariantList measurements = report.value(QStringLiteral("measurements")).toList();
    EXPECT_LE(measurements.size(), 6);
    for (const QVariant& item : measurements) {
        EXPECT_FALSE(item.toMap().value(QStringLiteral("id")).toString()
                         .startsWith(QStringLiteral("data[")));
    }

    const QVariantMap busEcho = loaded.value.executionConfig
        .value(QStringLiteral("transport")).toMap()
        .value(QStringLiteral("busEcho")).toMap();
    EXPECT_EQ(busEcho.value(QStringLiteral("payloadBytes")).toInt(), 114);
    const QVariantMap mapping = busEcho.value(QStringLiteral("resourceByLink")).toMap();
    EXPECT_EQ(mapping.keys(),
              QStringList({QStringLiteral("0"), QStringLiteral("1"),
                           QStringLiteral("3")}));

    TestApplicationController controller;
    const ActionResult configured = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_BUS_ECHO_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(configured.ok) << configured.message.toStdString();
    const TestDescriptor& descriptor = controller.snapshot().descriptor;
    EXPECT_EQ(descriptor.supportedRunModes,
              QVector<QString>{QStringLiteral("pc_periodic")});
    ASSERT_EQ(descriptor.runParameters.size(), 1);
    EXPECT_EQ(descriptor.runParameters.first().id, QStringLiteral("link_id"));
    ASSERT_EQ(descriptor.runParameters.first().choices.size(), 3);
    EXPECT_EQ(descriptor.runParameters.first().choices.at(0).value.toInt(), 0);
    EXPECT_EQ(descriptor.runParameters.first().choices.at(1).value.toInt(), 1);
    EXPECT_EQ(descriptor.runParameters.first().choices.at(2).value.toInt(), 3);
}

TEST(BusConfigurationTest, AuxiliaryEchoPortsAreConfiguredButNotSelectableControls)
{
    const QVariantMap hal = loadJsonMap(QStringLiteral(HWTEST_APP_HAL_CONFIG));
    const QVariantMap resources = hal.value(QStringLiteral("hardware")).toMap()
                                      .value(QStringLiteral("resources")).toMap();
    const QStringList auxiliaryIds{
        QStringLiteral("BUS_ECHO_COM1"),
        QStringLiteral("BUS_ECHO_COM2"),
        QStringLiteral("BUS_ECHO_COM4"),
    };
    for (const QString& id : auxiliaryIds) {
        const QVariantMap resource = resources.value(id).toMap();
        EXPECT_EQ(resource.value(QStringLiteral("module")).toString(),
                  QStringLiteral("control"));
        EXPECT_EQ(resource.value(QStringLiteral("providerId")).toString(),
                  QStringLiteral("qt.serial"));
        const QVariantMap properties = resource.value(QStringLiteral("properties")).toMap();
        EXPECT_EQ(properties.value(QStringLiteral("role")).toString(),
                  QStringLiteral("auxiliary-link"));
        EXPECT_FALSE(properties.value(QStringLiteral("portName")).toString().isEmpty());
        EXPECT_EQ(properties.value(QStringLiteral("baudRate")).toInt(), 614400);
        EXPECT_EQ(properties.value(QStringLiteral("parity")).toString(),
                  QStringLiteral("Even"));
    }

    TestApplicationController controller;
    const ActionResult loaded = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_BUS_LOOP_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(loaded.ok) << loaded.message.toStdString();
    const QVector<ControlResource> controls = controller.availableControls();
    ASSERT_EQ(controls.size(), 2);
    EXPECT_EQ(controls.at(0).resourceId, QStringLiteral("CONTROL_NETWORK"));
    EXPECT_EQ(controls.at(1).resourceId, QStringLiteral("CONTROL_SERIAL"));
}

} // namespace
} // namespace hwtest::app
