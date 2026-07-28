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

} // namespace
} // namespace hwtest::algorithm::mbddf
