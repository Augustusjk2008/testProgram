#include <algorithm/run_parameter_schema.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

namespace hwtest::algorithm::mbddf {
namespace {

const RunParameterDescriptor* parameter(const RunParameterSchema& schema,
                                        const QString& id)
{
    const auto found = std::find_if(
        schema.parameters.cbegin(), schema.parameters.cend(),
        [&id](const RunParameterDescriptor& item) { return item.id == id; });
    return found == schema.parameters.cend() ? nullptr : &*found;
}

QVariantMap helmDefaults()
{
    return {
        {QStringLiteral("waveform"), 0},
        {QStringLiteral("freq"), 1.0},
        {QStringLiteral("ampl"), 1.8},
        {QStringLiteral("offset"), 0.0},
        {QStringLiteral("start"), 0.0},
        {QStringLiteral("max_freq"), 80.0},
        {QStringLiteral("sweep_duration_s"), 25.0},
        {QStringLiteral("enable"), 15},
    };
}

TEST(RunParameterSchemaTest, HelmSchemaOwnsUiSemanticsWithoutAngleLimits)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.helm_stream"));
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->version, QStringLiteral("1"));
    ASSERT_EQ(schema->parameters.size(), 8);

    const RunParameterDescriptor* waveform = parameter(*schema, QStringLiteral("waveform"));
    ASSERT_NE(waveform, nullptr);
    EXPECT_EQ(waveform->kind, RunParameterKind::Choice);
    EXPECT_EQ(waveform->choices.size(), 5);

    const RunParameterDescriptor* amplitude = parameter(*schema, QStringLiteral("ampl"));
    const RunParameterDescriptor* offset = parameter(*schema, QStringLiteral("offset"));
    ASSERT_NE(amplitude, nullptr);
    ASSERT_NE(offset, nullptr);
    EXPECT_FALSE(amplitude->minimum.isValid());
    EXPECT_FALSE(amplitude->maximum.isValid());
    EXPECT_FALSE(offset->minimum.isValid());
    EXPECT_FALSE(offset->maximum.isValid());

    const RunParameterDescriptor* endFrequency = parameter(*schema, QStringLiteral("max_freq"));
    const RunParameterDescriptor* duration = parameter(*schema, QStringLiteral("sweep_duration_s"));
    ASSERT_NE(endFrequency, nullptr);
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(endFrequency->visibleWhenParameter, QStringLiteral("waveform"));
    EXPECT_EQ(endFrequency->visibleWhenEquals.toInt(), 4);
    EXPECT_EQ(duration->visibleWhenParameter, QStringLiteral("waveform"));
    EXPECT_EQ(duration->visibleWhenEquals.toInt(), 4);
}

TEST(RunParameterSchemaTest, HelmBoardSchemaIsRunScopedAndShowsManualFieldsOnlyInManualMode)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.helm_board_test"));
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->version, QStringLiteral("1"));
    EXPECT_FALSE(schema->persistValues);
    ASSERT_EQ(schema->parameters.size(), 9);

    const RunParameterDescriptor* mode = parameter(
        *schema, QStringLiteral("test_mode"));
    ASSERT_NE(mode, nullptr);
    EXPECT_EQ(mode->kind, RunParameterKind::Choice);
    ASSERT_EQ(mode->choices.size(), 2);
    EXPECT_EQ(mode->defaultValue.toInt(), 0);

    for (int channel = 0; channel < 4; ++channel) {
        const RunParameterDescriptor* duty = parameter(
            *schema,
            QStringLiteral("pwm_duty_percent[%1]").arg(channel));
        const RunParameterDescriptor* direction = parameter(
            *schema,
            QStringLiteral("direction[%1]").arg(channel));
        ASSERT_NE(duty, nullptr);
        ASSERT_NE(direction, nullptr);
        EXPECT_EQ(duty->kind, RunParameterKind::Integer);
        EXPECT_EQ(duty->minimum.toInt(), 0);
        EXPECT_EQ(duty->maximum.toInt(), 100);
        EXPECT_EQ(duty->visibleWhenParameter, QStringLiteral("test_mode"));
        EXPECT_EQ(duty->visibleWhenEquals.toInt(), 1);
        EXPECT_EQ(direction->kind, RunParameterKind::Boolean);
        EXPECT_EQ(direction->visibleWhenParameter, QStringLiteral("test_mode"));
        EXPECT_EQ(direction->visibleWhenEquals.toInt(), 1);
    }

    const auto defaults = normalizeRunParameters(
        QStringLiteral("mbddf.helm_board_test"), {}, {});
    ASSERT_TRUE(defaults.ok()) << defaults.status.error.message.toStdString();
    EXPECT_EQ(defaults.value.value(QStringLiteral("test_mode")).toInt(), 0);
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.helm_board_test"), {},
                     {{QStringLiteral("pwm_duty_percent[0]"), 101}})
                     .ok());
}

TEST(RunParameterSchemaTest, HelmNormalizationAllowsUnboundedAnglesAndRejectsUnknownFields)
{
    const QVariantMap overrides{
        {QStringLiteral("ampl"), 250.0},
        {QStringLiteral("offset"), -100.0},
    };
    const auto normalized = normalizeRunParameters(
        QStringLiteral("mbddf.helm_stream"), helmDefaults(), overrides);
    ASSERT_TRUE(normalized.ok()) << normalized.status.error.message.toStdString();
    EXPECT_DOUBLE_EQ(normalized.value.value(QStringLiteral("ampl")).toDouble(), 250.0);
    EXPECT_DOUBLE_EQ(normalized.value.value(QStringLiteral("offset")).toDouble(), -100.0);

    const auto unknown = normalizeRunParameters(
        QStringLiteral("mbddf.helm_stream"), helmDefaults(),
        {{QStringLiteral("mechanical_limit"), 20.5}});
    EXPECT_FALSE(unknown.ok());
    EXPECT_NE(unknown.status.error.message.indexOf(QStringLiteral("mechanical_limit")), -1);

    const auto wrongType = normalizeRunParameters(
        QStringLiteral("mbddf.helm_stream"), helmDefaults(),
        {{QStringLiteral("ampl"), QStringLiteral("2.0")}});
    EXPECT_FALSE(wrongType.ok());

    const auto notRepresentableAsF32 = normalizeRunParameters(
        QStringLiteral("mbddf.helm_stream"), helmDefaults(),
        {{QStringLiteral("ampl"), std::numeric_limits<double>::max()}});
    EXPECT_FALSE(notRepresentableAsF32.ok());
}

TEST(RunParameterSchemaTest, HelmSweepRequiresPositiveFrequenciesAndDuration)
{
    QVariantMap overrides{
        {QStringLiteral("waveform"), 4},
        {QStringLiteral("freq"), 0.0},
    };
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.helm_stream"), helmDefaults(), overrides)
                     .ok());

    overrides.insert(QStringLiteral("freq"), 1.0);
    overrides.insert(QStringLiteral("max_freq"), 0.0);
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.helm_stream"), helmDefaults(), overrides)
                     .ok());

    overrides.insert(QStringLiteral("max_freq"), 80.0);
    overrides.insert(QStringLiteral("sweep_duration_s"), 0.0);
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.helm_stream"), helmDefaults(), overrides)
                     .ok());
}

TEST(RunParameterSchemaTest, HiddenSweepValuesStillRemainProtocolValid)
{
    const auto invalidDuration = normalizeRunParameters(
        QStringLiteral("mbddf.helm_stream"), helmDefaults(),
        {{QStringLiteral("waveform"), 0},
         {QStringLiteral("sweep_duration_s"), 0.0}});
    EXPECT_FALSE(invalidDuration.ok());

    const auto invalidEndFrequency = normalizeRunParameters(
        QStringLiteral("mbddf.helm_stream"), helmDefaults(),
        {{QStringLiteral("waveform"), 3},
         {QStringLiteral("max_freq"), 0.0}});
    EXPECT_FALSE(invalidEndFrequency.ok());
}

TEST(RunParameterSchemaTest, DhSchemaExposesEnableAndEveryPulseWidth)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.dh_pulse_config"));
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->parameters.size(), 24);
    EXPECT_NE(parameter(*schema, QStringLiteral("config_enable")), nullptr);
    for (int channel = 0; channel < 23; ++channel) {
        const RunParameterDescriptor* width = parameter(
            *schema,
            QStringLiteral("pulse_width[%1]").arg(channel));
        ASSERT_NE(width, nullptr) << channel;
        EXPECT_EQ(width->kind, RunParameterKind::Integer);
        EXPECT_EQ(width->unit, QStringLiteral("ms"));
        EXPECT_EQ(width->minimum.toInt(), 0);
        EXPECT_EQ(width->maximum.toInt(), 65535);
    }
}

TEST(RunParameterSchemaTest, DhIgniteSchemaExposesSafeDefaultsAndEncodingBounds)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.dh_ignite_stream"));
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->version, QStringLiteral("1"));
    ASSERT_EQ(schema->parameters.size(), 28);

    const RunParameterDescriptor* power = parameter(
        *schema, QStringLiteral("power_enable"));
    const RunParameterDescriptor* returnEnable = parameter(
        *schema, QStringLiteral("return_enable"));
    ASSERT_NE(power, nullptr);
    ASSERT_NE(returnEnable, nullptr);
    EXPECT_EQ(power->kind, RunParameterKind::Integer);
    EXPECT_EQ(power->defaultValue.toInt(), 0);
    EXPECT_EQ(power->minimum.toInt(), 0);
    EXPECT_EQ(power->maximum.toInt(), 255);
    EXPECT_EQ(returnEnable->kind, RunParameterKind::Integer);
    EXPECT_EQ(returnEnable->defaultValue.toInt(), 0);
    EXPECT_EQ(returnEnable->minimum.toInt(), 0);
    EXPECT_EQ(returnEnable->maximum.toInt(), 255);

    for (int channel = 0; channel < 23; ++channel) {
        const RunParameterDescriptor* enabled = parameter(
            *schema,
            QStringLiteral("channel_enabled[%1]").arg(channel));
        ASSERT_NE(enabled, nullptr) << channel;
        EXPECT_EQ(enabled->kind, RunParameterKind::Boolean);
        EXPECT_FALSE(enabled->defaultValue.toBool());
    }

    const RunParameterDescriptor* count = parameter(
        *schema, QStringLiteral("report_count"));
    const RunParameterDescriptor* interval = parameter(
        *schema, QStringLiteral("interval_us"));
    const RunParameterDescriptor* delay = parameter(
        *schema, QStringLiteral("delay_frames"));
    ASSERT_NE(count, nullptr);
    ASSERT_NE(interval, nullptr);
    ASSERT_NE(delay, nullptr);
    EXPECT_EQ(delay->label, QStringLiteral("等待帧数"));
    EXPECT_EQ(count->defaultValue.toInt(), 50);
    EXPECT_EQ(interval->defaultValue.toInt(), 2500);
    EXPECT_EQ(delay->defaultValue.toInt(), 5);
    EXPECT_EQ(count->minimum.toInt(), 0);
    EXPECT_EQ(interval->minimum.toInt(), 0);
    EXPECT_EQ(delay->minimum.toInt(), 0);
    EXPECT_EQ(count->maximum.toInt(), 65535);
    EXPECT_EQ(interval->maximum.toInt(), 65535);
    EXPECT_EQ(delay->maximum.toInt(), 65535);
}

TEST(RunParameterSchemaTest, DhIgniteLeavesBusinessValidationToDut)
{
    const auto normalized = normalizeRunParameters(
        QStringLiteral("mbddf.dh_ignite_stream"), {},
        {{QStringLiteral("power_enable"), 2},
         {QStringLiteral("return_enable"), 255},
         {QStringLiteral("report_count"), 0},
         {QStringLiteral("interval_us"), 0},
         {QStringLiteral("delay_frames"), 65535}});
    ASSERT_TRUE(normalized.ok())
        << normalized.status.error.message.toStdString();
    for (int channel = 0; channel < 23; ++channel) {
        EXPECT_FALSE(normalized.value
                         .value(QStringLiteral("channel_enabled[%1]").arg(channel))
                         .toBool());
    }

    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.dh_ignite_stream"), {},
                     {{QStringLiteral("power_enable"), 256}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.dh_ignite_stream"), {},
                     {{QStringLiteral("report_count"), 65536}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.dh_ignite_stream"), {},
                     {{QStringLiteral("channel_enabled[0]"), 1}})
                     .ok());
}

TEST(RunParameterSchemaTest, DoWriteSchemaExposesSafeSixteenChannelMask)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.do_write"));
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->version, QStringLiteral("1"));
    ASSERT_EQ(schema->parameters.size(), 16);

    for (int channel = 0; channel < 16; ++channel) {
        const RunParameterDescriptor* enabled = parameter(
            *schema, QStringLiteral("channel_enabled[%1]").arg(channel));
        ASSERT_NE(enabled, nullptr) << channel;
        EXPECT_EQ(enabled->kind, RunParameterKind::Boolean);
        EXPECT_EQ(enabled->defaultValue.toBool(), channel == 3 || channel == 4);
    }

    const auto defaults = normalizeRunParameters(
        QStringLiteral("mbddf.do_write"), {}, {});
    ASSERT_TRUE(defaults.ok()) << defaults.status.error.message.toStdString();
    for (int channel = 0; channel < 16; ++channel) {
        EXPECT_EQ(defaults.value
                      .value(QStringLiteral("channel_enabled[%1]").arg(channel))
                      .toBool(),
                  channel == 3 || channel == 4);
    }

    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.do_write"), {},
                     {{QStringLiteral("channel_enabled[5]"), true}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.do_write"), {},
                     {{QStringLiteral("channel_enabled[6]"), true}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.do_write"), {},
                     {{QStringLiteral("channel_enabled[0]"), 1}})
                     .ok());
}

TEST(RunParameterSchemaTest, BusLoopExposesOnlyTestableComLinksAndBoundedCount)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.bus_loop"));
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->version, QStringLiteral("1"));
    ASSERT_EQ(schema->parameters.size(), 2);

    const RunParameterDescriptor* link = parameter(*schema, QStringLiteral("link_id"));
    ASSERT_NE(link, nullptr);
    ASSERT_EQ(link->kind, RunParameterKind::Choice);
    ASSERT_EQ(link->choices.size(), 3);
    EXPECT_EQ(link->choices.at(0).value.toInt(), 0);
    EXPECT_EQ(link->choices.at(1).value.toInt(), 1);
    EXPECT_EQ(link->choices.at(2).value.toInt(), 3);

    const RunParameterDescriptor* count = parameter(
        *schema, QStringLiteral("total_count"));
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->label, QStringLiteral("循环次数"));
    EXPECT_EQ(count->kind, RunParameterKind::Integer);
    EXPECT_EQ(count->minimum.toInt(), 1);
    EXPECT_EQ(count->maximum.toInt(), 100000);

    EXPECT_TRUE(normalizeRunParameters(
                    QStringLiteral("mbddf.bus_loop"), {},
                    {{QStringLiteral("link_id"), 3},
                     {QStringLiteral("total_count"), 100000}})
                    .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.bus_loop"), {},
                     {{QStringLiteral("link_id"), 2}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.bus_loop"), {},
                     {{QStringLiteral("total_count"), 0}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.bus_loop"), {},
                     {{QStringLiteral("total_count"), 100001}})
                     .ok());
}

TEST(RunParameterSchemaTest, BusEchoExposesOnlyTheSelectedComLink)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.bus_echo"));
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->version, QStringLiteral("1"));
    ASSERT_EQ(schema->parameters.size(), 1);
    const RunParameterDescriptor* link = parameter(*schema, QStringLiteral("link_id"));
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->kind, RunParameterKind::Choice);

    EXPECT_TRUE(normalizeRunParameters(
                    QStringLiteral("mbddf.bus_echo"), {},
                    {{QStringLiteral("link_id"), 0}})
                    .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.bus_echo"), {},
                     {{QStringLiteral("link_id"), 2}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.bus_echo"), {},
                     {{QStringLiteral("payload_hex"), QStringLiteral("4D4231")}})
                      .ok());
}

TEST(RunParameterSchemaTest, SerialTestUnifiesModeLinkAndCycleCount)
{
    const RunParameterSchema* schema = findRunParameterSchema(
        QStringLiteral("mbddf.serial_test"));
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->version, QStringLiteral("1"));
    ASSERT_EQ(schema->parameters.size(), 3);

    const RunParameterDescriptor* mode = parameter(*schema, QStringLiteral("test_mode"));
    ASSERT_NE(mode, nullptr);
    EXPECT_EQ(mode->kind, RunParameterKind::Choice);
    ASSERT_EQ(mode->choices.size(), 2);
    EXPECT_EQ(mode->choices.at(0).value.toInt(), 0);
    EXPECT_EQ(mode->choices.at(1).value.toInt(), 1);

    const RunParameterDescriptor* link = parameter(*schema, QStringLiteral("link_id"));
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->kind, RunParameterKind::Choice);
    ASSERT_EQ(link->choices.size(), 3);
    EXPECT_EQ(link->choices.at(0).value.toInt(), 0);
    EXPECT_EQ(link->choices.at(1).value.toInt(), 1);
    EXPECT_EQ(link->choices.at(2).value.toInt(), 3);

    const RunParameterDescriptor* cycleCount = parameter(
        *schema, QStringLiteral("cycle_count"));
    ASSERT_NE(cycleCount, nullptr);
    EXPECT_EQ(cycleCount->label, QStringLiteral("循环次数"));
    EXPECT_EQ(cycleCount->kind, RunParameterKind::Integer);
    EXPECT_EQ(cycleCount->defaultValue.toInt(), 1000);
    EXPECT_EQ(cycleCount->minimum.toInt(), 1);
    EXPECT_EQ(cycleCount->maximum.toInt(), 100000);

    EXPECT_TRUE(normalizeRunParameters(
                    QStringLiteral("mbddf.serial_test"), {},
                    {{QStringLiteral("test_mode"), 1},
                     {QStringLiteral("link_id"), 3},
                     {QStringLiteral("cycle_count"), 100000}})
                    .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.serial_test"), {},
                     {{QStringLiteral("test_mode"), 2}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.serial_test"), {},
                     {{QStringLiteral("link_id"), 2}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.serial_test"), {},
                     {{QStringLiteral("cycle_count"), 100001}})
                     .ok());
}

TEST(RunParameterSchemaTest, MemperfAndTimerJitterExposeCurrentProductParameters)
{
    const RunParameterSchema* memory = findRunParameterSchema(
        QStringLiteral("mbddf.memperf"));
    ASSERT_NE(memory, nullptr);
    ASSERT_EQ(memory->parameters.size(), 3);
    const RunParameterDescriptor* memoryType = parameter(
        *memory, QStringLiteral("memperf_type"));
    const RunParameterDescriptor* length = parameter(
        *memory, QStringLiteral("length"));
    const RunParameterDescriptor* seed = parameter(
        *memory, QStringLiteral("seed"));
    ASSERT_NE(memoryType, nullptr);
    ASSERT_NE(length, nullptr);
    ASSERT_NE(seed, nullptr);
    EXPECT_EQ(memoryType->kind, RunParameterKind::Choice);
    EXPECT_EQ(memoryType->choices.size(), 7);
    EXPECT_EQ(length->minimum.toInt(), 1);
    EXPECT_EQ(length->maximum.toInt(), 256 * 1024);
    EXPECT_EQ(seed->minimum.toLongLong(), 0);
    EXPECT_EQ(seed->maximum.toLongLong(), 0xFFFFFFFFLL);

    const RunParameterSchema* timer = findRunParameterSchema(
        QStringLiteral("mbddf.timer_jitter"));
    ASSERT_NE(timer, nullptr);
    ASSERT_EQ(timer->parameters.size(), 1);
    const RunParameterDescriptor* mode = parameter(*timer, QStringLiteral("mode"));
    ASSERT_NE(mode, nullptr);
    EXPECT_EQ(mode->kind, RunParameterKind::Choice);
    EXPECT_EQ(mode->choices.size(), 2);

    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.memperf"), {},
                     {{QStringLiteral("length"), 0}})
                     .ok());
    EXPECT_FALSE(normalizeRunParameters(
                     QStringLiteral("mbddf.timer_jitter"), {},
                     {{QStringLiteral("mode"), 2}})
                     .ok());
}

} // namespace
} // namespace hwtest::algorithm::mbddf
