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
} // namespace
} // namespace hwtest::app
