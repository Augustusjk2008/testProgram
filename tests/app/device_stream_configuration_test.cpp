#include <gtest/gtest.h>

#include <app/test_application_controller.h>

#include <biz/test_config_manager.h>

#include "mbddf_algorithm_registry.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace hwtest::app {
namespace {

TEST(DeviceStreamConfigurationTest, ImuDeclaresHostTimestampInterval)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(QStringLiteral(HWTEST_APP_IMU_STREAM_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();

    const QVariantMap stream = loaded.value.executionConfig
                                   .value(QStringLiteral("stream"))
                                   .toMap();
    EXPECT_EQ(stream.value(QStringLiteral("hostTimestampIntervalUs")).toInt(),
              2500);
}

TEST(DeviceStreamConfigurationTest, HelmKeepsAuthoritativeDeviceTimestamps)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(QStringLiteral(HWTEST_APP_HELM_STREAM_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();

    const QVariantMap stream = loaded.value.executionConfig
                                   .value(QStringLiteral("stream"))
                                   .toMap();
    EXPECT_FALSE(stream.contains(QStringLiteral("hostTimestampIntervalUs")));
}

TEST(DeviceStreamConfigurationTest, HelmDoesNotExposePcSelfCheckMeasurements)
{
    QFile file(QStringLiteral(HWTEST_APP_HELM_STREAM_CONFIG));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError);
    const QVariantList measurements = document.object().toVariantMap()
        .value(QStringLiteral("reportFields")).toMap()
        .value(QStringLiteral("measurements")).toList();
    const QStringList removedSelfCheckFields{
        QStringLiteral("self_check"),
        QStringLiteral("self_check_1"),
        QStringLiteral("self_check_2"),
        QStringLiteral("self_check_3"),
        QStringLiteral("self_check_4"),
        QStringLiteral("self_check_combined"),
        QStringLiteral("self_check_reserved"),
        QStringLiteral("self_check_or"),
        QStringLiteral("self_check_or_timeout"),
    };
    QStringList ids;
    for (const QVariant& measurement : measurements) {
        ids.push_back(measurement.toMap().value(QStringLiteral("id")).toString());
    }
    for (const QString& field : removedSelfCheckFields) {
        EXPECT_FALSE(ids.contains(field)) << field.toStdString();
    }
}

TEST(DeviceStreamConfigurationTest, DhIgniteIsRegisteredAsDedicatedExecutor)
{
    const MbdDfAlgorithmRegistration* registration = findMbdDfAlgorithm(
        QStringLiteral("mbddf.dh_ignite_stream"));
    ASSERT_NE(registration, nullptr);
    EXPECT_EQ(registration->requestProfileId,
              QStringLiteral("dh_control_request"));
    EXPECT_EQ(registration->responseProfileId,
              QStringLiteral("dh_control_response"));
    EXPECT_EQ(registration->commandName, QStringLiteral("DH_IGNITE_STREAM"));
}

TEST(DeviceStreamConfigurationTest, DhIgniteConfigIsFiniteNonStoppableDeviceStream)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(
        QStringLiteral(HWTEST_APP_DH_IGNITE_STREAM_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    ASSERT_EQ(loaded.value.steps.size(), 1);
    const hwtest::biz::TestStep& step = loaded.value.steps.first();
    EXPECT_EQ(step.algorithmId, QStringLiteral("mbddf.dh_ignite_stream"));

    QFile file(QStringLiteral(HWTEST_APP_DH_IGNITE_STREAM_CONFIG));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(),
                                                           &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError);
    const QVariantMap root = document.object().toVariantMap();
    const QVariantMap report = root.value(QStringLiteral("reportFields")).toMap();
    EXPECT_EQ(report.value(QStringLiteral("supportedRunModes")).toStringList(),
              QStringList{QStringLiteral("device_stream")});
    EXPECT_FALSE(report.value(QStringLiteral("stoppable"), true).toBool());

    const QVariantMap requestValues = step.parameters
        .value(QStringLiteral("protocol")).toMap()
        .value(QStringLiteral("requestValues")).toMap();
    EXPECT_EQ(requestValues.value(QStringLiteral("power_enable")).toInt(), 0);
    EXPECT_EQ(requestValues.value(QStringLiteral("return_enable")).toInt(), 0);
    EXPECT_EQ(requestValues.value(QStringLiteral("report_count")).toInt(), 50);
    EXPECT_EQ(requestValues.value(QStringLiteral("interval_us")).toInt(), 2500);
    EXPECT_EQ(requestValues.value(QStringLiteral("delay_frames")).toInt(), 5);
    for (int channel = 0; channel < 23; ++channel) {
        EXPECT_FALSE(requestValues
                         .value(QStringLiteral("channel_enabled[%1]").arg(channel))
                         .toBool());
    }

    const QVariantList measurements = report
        .value(QStringLiteral("measurements")).toList();
    QStringList ids;
    for (const QVariant& item : measurements) {
        ids.push_back(item.toMap().value(QStringLiteral("id")).toString());
    }
    // seq is a fixed recorder metadata column and must not be duplicated in
    // the descriptor-driven measurement columns.
    EXPECT_FALSE(ids.contains(QStringLiteral("seq")));
    EXPECT_TRUE(ids.contains(QStringLiteral("frame_index")));
    EXPECT_TRUE(ids.contains(QStringLiteral("ignition_phase")));
    for (int channel = 0; channel < 23; ++channel) {
        EXPECT_TRUE(ids.contains(QStringLiteral("dh_status.ch%1").arg(channel)));
        EXPECT_TRUE(ids.contains(QStringLiteral("telemetry[%1]").arg(channel)));
    }

    TestApplicationController controller;
    const ActionResult configured = controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_DH_IGNITE_STREAM_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG));
    ASSERT_TRUE(configured.ok) << configured.message.toStdString();
    const TestDescriptor& descriptor = controller.snapshot().descriptor;
    EXPECT_FALSE(descriptor.stoppable);
    EXPECT_EQ(descriptor.supportedRunModes,
              QVector<QString>{QStringLiteral("device_stream")});
    ASSERT_EQ(descriptor.runParameters.size(), 28);
    EXPECT_EQ(descriptor.runParameterDefaults
                  .value(QStringLiteral("delay_frames")).toInt(),
              5);
}

} // namespace
} // namespace hwtest::app
