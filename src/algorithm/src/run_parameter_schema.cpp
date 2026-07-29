#include <algorithm/run_parameter_schema.h>

#include <QMetaType>
#include <QSet>

#include <cmath>

namespace hwtest::algorithm::mbddf {
namespace {

using hwtest::biz::ErrorCode;
using hwtest::biz::Result;
using hwtest::biz::Status;

Result<QVariantMap> failure(ErrorCode code, const QString& message)
{
    Result<QVariantMap> result;
    result.status.code = code;
    result.status.error.code = code;
    result.status.error.message = message;
    result.status.error.operation = QStringLiteral("normalizeRunParameters");
    return result;
}

bool isNumericVariant(const QVariant& value)
{
    switch (value.userType()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        return true;
    default:
        return false;
    }
}

RunParameterDescriptor numberParameter(const QString& id,
                                       const QString& label,
                                       const QString& unit,
                                       const QVariant& defaultValue)
{
    RunParameterDescriptor descriptor;
    descriptor.id = id;
    descriptor.label = label;
    descriptor.kind = RunParameterKind::Number;
    descriptor.unit = unit;
    descriptor.defaultValue = defaultValue;
    return descriptor;
}

RunParameterDescriptor integerParameter(const QString& id,
                                        const QString& label,
                                        const QString& unit,
                                        const QVariant& defaultValue,
                                        const QVariant& minimum,
                                        const QVariant& maximum)
{
    RunParameterDescriptor descriptor;
    descriptor.id = id;
    descriptor.label = label;
    descriptor.kind = RunParameterKind::Integer;
    descriptor.unit = unit;
    descriptor.defaultValue = defaultValue;
    descriptor.minimum = minimum;
    descriptor.maximum = maximum;
    return descriptor;
}

RunParameterDescriptor busLinkParameter()
{
    RunParameterDescriptor descriptor;
    descriptor.id = QStringLiteral("link_id");
    descriptor.label = QStringLiteral("测试串口");
    descriptor.description = QStringLiteral("COM3 为产品控制口，不参与总线测试");
    descriptor.kind = RunParameterKind::Choice;
    descriptor.defaultValue = 0;
    descriptor.choices = {
        {0, QStringLiteral("COM1")},
        {1, QStringLiteral("COM2")},
        {3, QStringLiteral("COM4")},
    };
    return descriptor;
}

const RunParameterSchema& busLoopSchema()
{
    static const RunParameterSchema schema = [] {
        RunParameterSchema result;
        result.version = QStringLiteral("1");
        result.parameters.push_back(busLinkParameter());
        RunParameterDescriptor count = integerParameter(
            QStringLiteral("total_count"), QStringLiteral("内部回环次数"),
            QStringLiteral("次"), 1000, 1, 100000);
        count.description = QStringLiteral("DUT 将所选串口配置为内部 loopback=true");
        result.parameters.push_back(count);
        return result;
    }();
    return schema;
}

const RunParameterSchema& busEchoSchema()
{
    static const RunParameterSchema schema = [] {
        RunParameterSchema result;
        result.version = QStringLiteral("1");
        result.parameters.push_back(busLinkParameter());
        return result;
    }();
    return schema;
}

const RunParameterSchema& helmSchema()
{
    static const RunParameterSchema schema = [] {
        RunParameterSchema result;
        result.version = QStringLiteral("1");

        RunParameterDescriptor waveform;
        waveform.id = QStringLiteral("waveform");
        waveform.label = QStringLiteral("波形");
        waveform.description = QStringLiteral("所有启用舵通道共用同一波形");
        waveform.kind = RunParameterKind::Choice;
        waveform.defaultValue = 0;
        waveform.choices = {
            {0, QStringLiteral("正弦波")},
            {1, QStringLiteral("方波")},
            {2, QStringLiteral("三角波")},
            {3, QStringLiteral("恒值")},
            {4, QStringLiteral("连续对数扫频")},
        };
        result.parameters.push_back(waveform);

        RunParameterDescriptor frequency = numberParameter(
            QStringLiteral("freq"), QStringLiteral("频率"),
            QStringLiteral("Hz"), 1.0);
        frequency.description = QStringLiteral("普通波形频率或扫频起始频率");
        frequency.minimum = 0.0;
        frequency.minimumExclusive = true;
        result.parameters.push_back(frequency);

        RunParameterDescriptor amplitude = numberParameter(
            QStringLiteral("ampl"), QStringLiteral("幅值"),
            QStringLiteral("°"), 1.8);
        amplitude.description = QStringLiteral("测试链路不限制幅值范围");
        result.parameters.push_back(amplitude);

        RunParameterDescriptor offset = numberParameter(
            QStringLiteral("offset"), QStringLiteral("偏置"),
            QStringLiteral("°"), 0.0);
        offset.description = QStringLiteral("测试链路不限制偏置范围");
        result.parameters.push_back(offset);

        RunParameterDescriptor phase = numberParameter(
            QStringLiteral("start"), QStringLiteral("起始相位"),
            QStringLiteral("rad"), 0.0);
        result.parameters.push_back(phase);

        RunParameterDescriptor endFrequency = numberParameter(
            QStringLiteral("max_freq"), QStringLiteral("扫频终止频率"),
            QStringLiteral("Hz"), 80.0);
        endFrequency.minimum = 0.0;
        endFrequency.minimumExclusive = true;
        endFrequency.visibleWhenParameter = QStringLiteral("waveform");
        endFrequency.visibleWhenEquals = 4;
        result.parameters.push_back(endFrequency);

        RunParameterDescriptor duration = numberParameter(
            QStringLiteral("sweep_duration_s"), QStringLiteral("扫频总时长"),
            QStringLiteral("s"), 25.0);
        duration.minimum = 0.0;
        duration.minimumExclusive = true;
        duration.visibleWhenParameter = QStringLiteral("waveform");
        duration.visibleWhenEquals = 4;
        result.parameters.push_back(duration);

        RunParameterDescriptor enable = integerParameter(
            QStringLiteral("enable"), QStringLiteral("通道使能位图"),
            QString{}, 15, 0, 15);
        enable.description = QStringLiteral("bit0..3 分别对应四路舵机");
        result.parameters.push_back(enable);
        return result;
    }();
    return schema;
}

const RunParameterSchema& dhSchema()
{
    static const RunParameterSchema schema = [] {
        RunParameterSchema result;
        result.version = QStringLiteral("1");
        RunParameterDescriptor enabled = integerParameter(
            QStringLiteral("config_enable"), QStringLiteral("配置使能"),
            QString{}, 1, 0, 1);
        result.parameters.push_back(enabled);
        for (int channel = 0; channel < 23; ++channel) {
            RunParameterDescriptor width = integerParameter(
                QStringLiteral("pulse_width[%1]").arg(channel),
                QStringLiteral("DH%1 脉宽").arg(channel),
                QStringLiteral("ms"), channel == 0 ? 80 : 63, 0, 65535);
            result.parameters.push_back(width);
        }
        return result;
    }();
    return schema;
}

Result<QVariantMap> validateValue(const RunParameterDescriptor& descriptor,
                                 const QVariant& value,
                                 const QVariantMap& values)
{
    if (!value.isValid() || value.isNull()) {
        return descriptor.required
            ? failure(ErrorCode::ParameterRangeError,
                      QStringLiteral("Run parameter '%1' is required").arg(descriptor.id))
            : Result<QVariantMap>{Status{}, values};
    }

    double numericValue = 0.0;
    bool numeric = false;
    switch (descriptor.kind) {
    case RunParameterKind::Integer:
    case RunParameterKind::Number:
    case RunParameterKind::Choice:
        if (!isNumericVariant(value)) {
            return failure(ErrorCode::ParameterRangeError,
                           QStringLiteral("Run parameter '%1' must be numeric")
                               .arg(descriptor.id));
        }
        numericValue = value.toDouble(&numeric);
        if (!numeric || !std::isfinite(numericValue)) {
            return failure(ErrorCode::ParameterRangeError,
                           QStringLiteral("Run parameter '%1' must be a finite number")
                               .arg(descriptor.id));
        }
        break;
    case RunParameterKind::Boolean:
        if (value.userType() != QMetaType::Bool) {
            return failure(ErrorCode::ParameterRangeError,
                           QStringLiteral("Run parameter '%1' must be a boolean")
                               .arg(descriptor.id));
        }
        break;
    }

    if (descriptor.kind == RunParameterKind::Integer &&
        std::floor(numericValue) != numericValue) {
        return failure(ErrorCode::ParameterRangeError,
                       QStringLiteral("Run parameter '%1' must be an integer")
                           .arg(descriptor.id));
    }
    if (descriptor.kind == RunParameterKind::Choice) {
        bool matched = false;
        for (const RunParameterChoice& choice : descriptor.choices) {
            if (choice.value == value || choice.value.toDouble() == numericValue) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return failure(ErrorCode::ParameterRangeError,
                           QStringLiteral("Run parameter '%1' is not an allowed choice")
                               .arg(descriptor.id));
        }
    }

    const auto outsideMinimum = [&] {
        if (!descriptor.minimum.isValid()) return false;
        const double minimum = descriptor.minimum.toDouble();
        return descriptor.minimumExclusive ? numericValue <= minimum
                                           : numericValue < minimum;
    };
    const auto outsideMaximum = [&] {
        if (!descriptor.maximum.isValid()) return false;
        const double maximum = descriptor.maximum.toDouble();
        return descriptor.maximumExclusive ? numericValue >= maximum
                                           : numericValue > maximum;
    };
    if (numeric && (outsideMinimum() || outsideMaximum())) {
        return failure(ErrorCode::ParameterRangeError,
                       QStringLiteral("Run parameter '%1' is outside its allowed range")
                           .arg(descriptor.id));
    }
    return {Status{}, values};
}

} // namespace

const RunParameterSchema* findRunParameterSchema(const QString& algorithmId)
{
    if (algorithmId == QStringLiteral("mbddf.helm_stream")) {
        return &helmSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.dh_pulse_config")) {
        return &dhSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.bus_loop")) {
        return &busLoopSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.bus_echo")) {
        return &busEchoSchema();
    }
    return nullptr;
}

hwtest::biz::Result<QVariantMap> normalizeRunParameters(
    const QString& algorithmId,
    const QVariantMap& configuredDefaults,
    const QVariantMap& overrides)
{
    const RunParameterSchema* schema = findRunParameterSchema(algorithmId);
    if (schema == nullptr) {
        if (configuredDefaults.isEmpty() && overrides.isEmpty()) {
            return {Status{}, {}};
        }
        return failure(ErrorCode::CapabilityUnsupported,
                       QStringLiteral("Algorithm '%1' does not expose editable run parameters")
                           .arg(algorithmId));
    }

    QSet<QString> known;
    QVariantMap values;
    for (const RunParameterDescriptor& descriptor : schema->parameters) {
        known.insert(descriptor.id);
        values.insert(descriptor.id,
                      configuredDefaults.contains(descriptor.id)
                          ? configuredDefaults.value(descriptor.id)
                          : descriptor.defaultValue);
    }
    const auto rejectUnknown = [&](const QVariantMap& source) -> Result<QVariantMap> {
        for (auto iterator = source.cbegin(); iterator != source.cend(); ++iterator) {
            if (!known.contains(iterator.key())) {
                return failure(ErrorCode::ParameterRangeError,
                               QStringLiteral("Unknown run parameter '%1'")
                                   .arg(iterator.key()));
            }
        }
        return {Status{}, values};
    };
    Result<QVariantMap> checked = rejectUnknown(configuredDefaults);
    if (!checked.ok()) return checked;
    checked = rejectUnknown(overrides);
    if (!checked.ok()) return checked;
    for (auto iterator = overrides.cbegin(); iterator != overrides.cend(); ++iterator) {
        values.insert(iterator.key(), iterator.value());
    }

    for (const RunParameterDescriptor& descriptor : schema->parameters) {
        const Result<QVariantMap> validated = validateValue(
            descriptor, values.value(descriptor.id), values);
        if (!validated.ok()) return validated;
    }
    if (algorithmId == QStringLiteral("mbddf.helm_stream")) {
        for (const RunParameterDescriptor& descriptor : schema->parameters) {
            if (descriptor.kind != RunParameterKind::Number) continue;
            const float encoded = static_cast<float>(
                values.value(descriptor.id).toDouble());
            if (!std::isfinite(encoded)) {
                return failure(
                    ErrorCode::ParameterRangeError,
                    QStringLiteral("Run parameter '%1' must be representable as a finite F32")
                        .arg(descriptor.id));
            }
        }
    }
    return {Status{}, values};
}

} // namespace hwtest::algorithm::mbddf
