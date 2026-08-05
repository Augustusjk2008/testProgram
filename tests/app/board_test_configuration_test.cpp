#include <gtest/gtest.h>

#include <biz/test_config_manager.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>

namespace hwtest::app {
namespace {

QVariantMap loadBoardJson(const QString& path)
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

QVariantMap resource(const QVariantMap& hal, const QString& id)
{
    return hal.value(QStringLiteral("hardware")).toMap()
        .value(QStringLiteral("resources")).toMap()
        .value(id).toMap();
}

void expectResource(const QVariantMap& hal,
                    const QString& id,
                    const QString& device,
                    const QString& direction,
                    int physicalIndex,
                    int connectorPin)
{
    const QVariantMap value = resource(hal, id);
    ASSERT_FALSE(value.isEmpty()) << id.toStdString();
    EXPECT_EQ(value.value(QStringLiteral("device")).toString(), device);
    EXPECT_EQ(value.value(QStringLiteral("adapterId")).toString(),
              QStringLiteral("ni.daqmx"));
    EXPECT_EQ(value.value(QStringLiteral("direction")).toString(), direction);
    EXPECT_EQ(value.value(QStringLiteral("physicalIndex")).toInt(), physicalIndex);
    EXPECT_EQ(value.value(QStringLiteral("properties")).toMap()
                  .value(QStringLiteral("connectorPin")).toInt(),
              connectorPin);
}

TEST(BoardTestConfigurationTest, DoWriteIsSingleShotWithoutRetryAndUses6259Sense)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(QStringLiteral(HWTEST_APP_DO_WRITE_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    ASSERT_EQ(loaded.value.steps.size(), 1);
    const auto& step = loaded.value.steps.first();
    EXPECT_EQ(step.algorithmId, QStringLiteral("mbddf.do_write"));
    EXPECT_EQ(step.retryCount, 0);

    const QVariantMap json = loadBoardJson(QStringLiteral(HWTEST_APP_DO_WRITE_CONFIG));
    EXPECT_EQ(json.value(QStringLiteral("reportFields")).toMap()
                  .value(QStringLiteral("supportedRunModes")).toStringList(),
              QStringList{QStringLiteral("single")});
    const QVariantMap fixture = loaded.value.executionConfig
                                    .value(QStringLiteral("boardFixture")).toMap();
    EXPECT_EQ(fixture.value(QStringLiteral("pxi6259DeviceId")).toString(),
              QStringLiteral("ni6259_stimulus"));
    EXPECT_EQ(fixture.value(QStringLiteral("doSenseResources")).toStringList(),
              QStringList({QStringLiteral("DUT_TX_ENABLE_SENSE"),
                           QStringLiteral("DUT_ATTENUATOR_SENSE")}));
    EXPECT_EQ(fixture.value(QStringLiteral("settlingMs")).toInt(), 100);
}

TEST(BoardTestConfigurationTest, TimerJitterUsesExactEightBucketLabels)
{
    const QVariantList measurements = loadBoardJson(
        QStringLiteral(HWTEST_APP_TIMER_JITTER_CONFIG))
        .value(QStringLiteral("reportFields")).toMap()
        .value(QStringLiteral("measurements")).toList();
    ASSERT_EQ(measurements.size(), 12);

    const QStringList expectedLabels{
        QStringLiteral("抖动桶 [0, 2) µs"),
        QStringLiteral("抖动桶 [2, 4) µs"),
        QStringLiteral("抖动桶 [4, 8) µs"),
        QStringLiteral("抖动桶 [8, 16) µs"),
        QStringLiteral("抖动桶 [16, 32) µs"),
        QStringLiteral("抖动桶 [32, 64) µs"),
        QStringLiteral("抖动桶 [64, 100) µs"),
        QStringLiteral("抖动桶 [100, +∞) µs")};
    for (int index = 0; index < expectedLabels.size(); ++index) {
        const QVariantMap bucket = measurements.at(index + 4).toMap();
        EXPECT_EQ(bucket.value(QStringLiteral("id")).toString(),
                  QStringLiteral("buckets[%1]").arg(index));
        EXPECT_EQ(bucket.value(QStringLiteral("label")).toString(),
                  expectedLabels.at(index));
    }
}

TEST(BoardTestConfigurationTest, DiReadDisplaysSixteenConfiguredBitStates)
{
    const QVariantMap json = loadBoardJson(QStringLiteral(HWTEST_APP_DI_CONFIG));
    const QVariantList measurements = json.value(QStringLiteral("reportFields")).toMap()
                                          .value(QStringLiteral("measurements")).toList();
    ASSERT_EQ(measurements.size(), 4);
    const QVariantList taskMeasurements = json.value(QStringLiteral("reportFields")).toMap()
                                              .value(QStringLiteral("taskMeasurements")).toList();
    ASSERT_EQ(taskMeasurements.size(), 16);

    const QStringList expectedLabels{
        QStringLiteral("DI0 联锁、电气弹动"),
        QStringLiteral("DI1 引信报警"),
        QStringLiteral("DI2 引信起爆指令"),
        QStringLiteral("DI3 锁相环锁定指示"),
        QStringLiteral("DI4"),
        QStringLiteral("DI5"),
        QStringLiteral("DI6"),
        QStringLiteral("DI7"),
        QStringLiteral("DI8 投放允许"),
        QStringLiteral("DI9"),
        QStringLiteral("DI10"),
        QStringLiteral("DI11"),
        QStringLiteral("DI12"),
        QStringLiteral("DI13"),
        QStringLiteral("DI14"),
        QStringLiteral("DI15")};
    for (int bit = 0; bit < expectedLabels.size(); ++bit) {
        const QVariantMap measurement = taskMeasurements.at(bit).toMap();
        EXPECT_EQ(measurement.value(QStringLiteral("id")).toString(),
                  QStringLiteral("di%1").arg(bit));
        EXPECT_EQ(measurement.value(QStringLiteral("label")).toString(),
                  expectedLabels.at(bit));
        EXPECT_EQ(measurement.value(QStringLiteral("sourceId")).toString(),
                  QStringLiteral("di_state[0]"));
        EXPECT_EQ(measurement.value(QStringLiteral("bitIndex")).toInt(), bit);
        EXPECT_TRUE(measurement.value(QStringLiteral("primary")).toBool());
    }

    for (int index = 2; index < measurements.size(); ++index) {
        const QVariantMap source = measurements.at(index).toMap();
        EXPECT_EQ(source.value(QStringLiteral("primary")).toBool(), index == 2);
        EXPECT_TRUE(source.contains(QStringLiteral("taskVisible")));
        EXPECT_FALSE(source.value(QStringLiteral("taskVisible")).toBool());
    }

    const QVariantList channels = json.value(QStringLiteral("executionConfig")).toMap()
                                      .value(QStringLiteral("digitalStimulus")).toMap()
                                      .value(QStringLiteral("channels")).toList();
    ASSERT_EQ(channels.size(), 3);
    EXPECT_EQ(channels.at(0).toMap().value(QStringLiteral("label")).toString(),
              QStringLiteral("DI3 锁相环锁定指示"));
    EXPECT_EQ(channels.at(1).toMap().value(QStringLiteral("label")).toString(),
              QStringLiteral("DI1 引信报警"));
    EXPECT_EQ(channels.at(2).toMap().value(QStringLiteral("label")).toString(),
              QStringLiteral("DI2 引信起爆指令"));
}

TEST(BoardTestConfigurationTest, HelmManualAndAutomaticShareSingleConfig)
{
    hwtest::biz::TestConfigManager manager;
    const auto loaded = manager.load(QStringLiteral(HWTEST_APP_HELM_BOARD_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    ASSERT_EQ(loaded.value.steps.size(), 1);
    EXPECT_EQ(loaded.value.steps.first().algorithmId,
              QStringLiteral("mbddf.helm_board_test"));
    EXPECT_EQ(loaded.value.steps.first().retryCount, 0);

    const QVariantMap json = loadBoardJson(QStringLiteral(HWTEST_APP_HELM_BOARD_CONFIG));
    EXPECT_TRUE(json.value(QStringLiteral("hardwareRequirements")).toList().isEmpty());
    EXPECT_EQ(json.value(QStringLiteral("reportFields")).toMap()
                  .value(QStringLiteral("supportedRunModes")).toStringList(),
              QStringList{QStringLiteral("single")});

    const QVariantMap fixture = loaded.value.executionConfig
                                    .value(QStringLiteral("boardFixture")).toMap();
    EXPECT_EQ(fixture.value(QStringLiteral("pxi6259DeviceId")).toString(),
              QStringLiteral("ni6259_stimulus"));
    EXPECT_EQ(fixture.value(QStringLiteral("pxi6733DeviceId")).toString(),
              QStringLiteral("ni6733_fixture"));
    EXPECT_EQ(fixture.value(QStringLiteral("settlingMs")).toInt(), 100);
    EXPECT_DOUBLE_EQ(fixture.value(QStringLiteral("pwmSampleRateHz")).toDouble(),
                     1250000.0);
    EXPECT_EQ(fixture.value(QStringLiteral("pwmSamplesPerChannel")).toInt(), 7500);
}

TEST(BoardTestConfigurationTest, HalWiringMatchesTables26And27AndFailsClosed)
{
    const QVariantMap hal = loadBoardJson(QStringLiteral(HWTEST_APP_HAL_CONFIG));
    const QVariantList devices = hal.value(QStringLiteral("hardware")).toMap()
                                     .value(QStringLiteral("devices")).toList();
    QVariantMap pxi6259;
    QVariantMap pxi6733;
    for (const QVariant& item : devices) {
        const QVariantMap device = item.toMap();
        if (device.value(QStringLiteral("alias")) == QStringLiteral("ni6259_stimulus")) {
            pxi6259 = device;
        } else if (device.value(QStringLiteral("alias")) == QStringLiteral("ni6733_fixture")) {
            pxi6733 = device;
        }
    }
    ASSERT_FALSE(pxi6259.isEmpty());
    ASSERT_FALSE(pxi6733.isEmpty());
    EXPECT_EQ(pxi6259.value(QStringLiteral("model")).toString(),
              QStringLiteral("PXI-6259"));
    EXPECT_EQ(pxi6733.value(QStringLiteral("model")).toString(),
              QStringLiteral("PXI-6733"));
    EXPECT_EQ(pxi6259.value(QStringLiteral("serialNumber")).toString(),
              QStringLiteral("CONFIGURE_ME"));
    EXPECT_EQ(pxi6733.value(QStringLiteral("serialNumber")).toString(),
              QStringLiteral("CONFIGURE_ME"));

    expectResource(hal, QStringLiteral("DUT_DI3_STIM"),
                   QStringLiteral("ni6259_stimulus"), QStringLiteral("output"), 0, 52);
    expectResource(hal, QStringLiteral("DUT_DI1_STIM"),
                   QStringLiteral("ni6259_stimulus"), QStringLiteral("output"), 1, 17);
    expectResource(hal, QStringLiteral("DUT_DI2_STIM"),
                   QStringLiteral("ni6259_stimulus"), QStringLiteral("output"), 2, 49);
    expectResource(hal, QStringLiteral("DUT_TX_ENABLE_SENSE"),
                   QStringLiteral("ni6259_stimulus"), QStringLiteral("input"), 3, 47);
    expectResource(hal, QStringLiteral("DUT_ATTENUATOR_SENSE"),
                   QStringLiteral("ni6259_stimulus"), QStringLiteral("input"), 4, 19);

    const std::array<int, 4> pwmPins{{68, 33, 65, 30}};
    const std::array<int, 4> directionPins{{28, 60, 25, 57}};
    const std::array<int, 4> feedbackPins{{22, 21, 57, 25}};
    for (int channel = 0; channel < 4; ++channel) {
        const QVariantMap pwm = resource(
            hal, QStringLiteral("HELM_PWM%1_SENSE").arg(channel + 1));
        expectResource(hal, QStringLiteral("HELM_PWM%1_SENSE").arg(channel + 1),
                       QStringLiteral("ni6259_stimulus"), QStringLiteral("input"),
                       channel, pwmPins.at(static_cast<size_t>(channel)));
        EXPECT_EQ(pwm.value(QStringLiteral("properties")).toMap()
                      .value(QStringLiteral("terminalConfig")).toString(),
                  QStringLiteral("RSE"));

        const QVariantMap direction = resource(
            hal, QStringLiteral("HELM_DIR%1_SENSE").arg(channel + 1));
        expectResource(hal, QStringLiteral("HELM_DIR%1_SENSE").arg(channel + 1),
                       QStringLiteral("ni6259_stimulus"), QStringLiteral("input"),
                       channel + 4,
                       directionPins.at(static_cast<size_t>(channel)));
        EXPECT_EQ(direction.value(QStringLiteral("properties")).toMap()
                      .value(QStringLiteral("terminalConfig")).toString(),
                  QStringLiteral("RSE"));

        expectResource(hal, QStringLiteral("HELM_FK%1_STIM").arg(channel + 1),
                       QStringLiteral("ni6733_fixture"), QStringLiteral("output"),
                       channel, feedbackPins.at(static_cast<size_t>(channel)));
    }

    const QVariantMap safe = hal.value(QStringLiteral("safeState")).toMap();
    for (int channel = 0; channel < 4; ++channel) {
        EXPECT_DOUBLE_EQ(safe.value(
            QStringLiteral("HELM_FK%1_STIM").arg(channel + 1)).toDouble(), 0.0);
    }
}

} // namespace
} // namespace hwtest::app
