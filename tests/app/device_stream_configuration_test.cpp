#include <gtest/gtest.h>

#include <biz/test_config_manager.h>

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

} // namespace
} // namespace hwtest::app
