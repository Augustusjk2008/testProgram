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

RunParameterDescriptor booleanParameter(const QString& id,
                                        const QString& label,
                                        bool defaultValue)
{
    RunParameterDescriptor descriptor;
    descriptor.id = id;
    descriptor.label = label;
    descriptor.kind = RunParameterKind::Boolean;
    descriptor.defaultValue = defaultValue;
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
            QStringLiteral("total_count"), QStringLiteral("循环次数"),
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

const RunParameterSchema& doWriteSchema()
{
    static const RunParameterSchema schema = [] {
        RunParameterSchema result;
        result.version = QStringLiteral("1");
        for (int channel = 0; channel < 16; ++channel) {
            RunParameterDescriptor enabled = booleanParameter(
                QStringLiteral("channel_enabled[%1]").arg(channel),
                QStringLiteral("DO%1").arg(channel),
                channel == 3 || channel == 4);
            if (channel == 3 || channel == 4) {
                enabled.description = QStringLiteral(
                    "低有效电源使能；原始位 1 表示关闭");
            } else if (channel == 5 || channel == 6) {
                enabled.description = QStringLiteral(
                    "保留输出，必须保持为 0");
            }
            result.parameters.push_back(enabled);
        }
        return result;
    }();
    return schema;
}

const RunParameterSchema& serialTestSchema()
{
    static const RunParameterSchema schema = [] {
        RunParameterSchema result;
        result.version = QStringLiteral("1");

        RunParameterDescriptor mode;
        mode.id = QStringLiteral("test_mode");
        mode.label = QStringLiteral("测试方式");
        mode.description = QStringLiteral("回环由 DUT 内部完成；回显经独立 PC 本地串口逐字节返回");
        mode.kind = RunParameterKind::Choice;
        mode.defaultValue = 0;
        mode.choices = {
            {0, QStringLiteral("内部回环")},
            {1, QStringLiteral("PC-DUT 回显")},
        };
        result.parameters.push_back(mode);

        result.parameters.push_back(busLinkParameter());

        RunParameterDescriptor cycleCount = integerParameter(
            QStringLiteral("cycle_count"), QStringLiteral("循环次数"),
            QStringLiteral("次"), 1000, 1, 100000);
        cycleCount.description = QStringLiteral("回环一次下发全部次数；回显逐轮执行");
        result.parameters.push_back(cycleCount);
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

const RunParameterSchema& helmBoardSchema()
{
    static const RunParameterSchema schema = [] {
        RunParameterSchema result;
        result.version = QStringLiteral("1");
        result.persistValues = false;

        RunParameterDescriptor mode;
        mode.id = QStringLiteral("test_mode");
        mode.label = QStringLiteral("测试模式");
        mode.kind = RunParameterKind::Choice;
        mode.defaultValue = 0;
        mode.choices = {
            {0, QStringLiteral("自动")},
            {1, QStringLiteral("手动")},
        };
        result.parameters.push_back(mode);

        for (int channel = 0; channel < 4; ++channel) {
            RunParameterDescriptor duty = integerParameter(
                QStringLiteral("pwm_duty_percent[%1]").arg(channel),
                QStringLiteral("舵%1 PWM 占空比").arg(channel + 1),
                QStringLiteral("%"), 0, 0, 100);
            duty.visibleWhenParameter = QStringLiteral("test_mode");
            duty.visibleWhenEquals = 1;
            result.parameters.push_back(duty);

            RunParameterDescriptor direction = booleanParameter(
                QStringLiteral("direction[%1]").arg(channel),
                QStringLiteral("舵%1 方向").arg(channel + 1), false);
            direction.visibleWhenParameter = QStringLiteral("test_mode");
            direction.visibleWhenEquals = 1;
            result.parameters.push_back(direction);
        }
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

const RunParameterSchema& dhIgniteSchema()
{
    static const RunParameterSchema schema = [] {
        RunParameterSchema result;
        result.version = QStringLiteral("1");

        RunParameterDescriptor power = integerParameter(
            QStringLiteral("power_enable"), QStringLiteral("点火电源使能"),
            QString{}, 0, 0, 255);
        power.description = QStringLiteral("DUT 校验业务取值是否为 0 或 1");
        result.parameters.push_back(power);

        RunParameterDescriptor returnEnable = integerParameter(
            QStringLiteral("return_enable"), QStringLiteral("点火回线使能"),
            QString{}, 0, 0, 255);
        returnEnable.description = QStringLiteral("DUT 校验业务取值是否为 0 或 1");
        result.parameters.push_back(returnEnable);

        for (int channel = 0; channel < 23; ++channel) {
            RunParameterDescriptor enabled = booleanParameter(
                QStringLiteral("channel_enabled[%1]").arg(channel),
                QStringLiteral("DH%1 点火通道").arg(channel), false);
            enabled.description = QStringLiteral("编码到 channel[0] bit%1").arg(channel);
            result.parameters.push_back(enabled);
        }

        RunParameterDescriptor count = integerParameter(
            QStringLiteral("report_count"), QStringLiteral("回告总帧数"),
            QStringLiteral("帧"), 50, 0, 65535);
        count.description = QStringLiteral("业务范围由 DUT 校验");
        result.parameters.push_back(count);

        RunParameterDescriptor interval = integerParameter(
            QStringLiteral("interval_us"), QStringLiteral("采样起点最小间隔"),
            QStringLiteral("us"), 2500, 0, 65535);
        interval.description = QStringLiteral("业务范围由 DUT 校验；PC 用于估算时间轴");
        result.parameters.push_back(interval);

        RunParameterDescriptor delay = integerParameter(
            QStringLiteral("delay_frames"), QStringLiteral("等待帧数"),
            QStringLiteral("帧"), 5, 0, 65535);
        delay.description = QStringLiteral("DUT 在基线帧完成后、下一帧采样前点火");
        result.parameters.push_back(delay);
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
    if (algorithmId == QStringLiteral("mbddf.helm_board_test")) {
        return &helmBoardSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.dh_pulse_config")) {
        return &dhSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.dh_ignite_stream")) {
        return &dhIgniteSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.do_write")) {
        return &doWriteSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.bus_loop")) {
        return &busLoopSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.bus_echo")) {
        return &busEchoSchema();
    }
    if (algorithmId == QStringLiteral("mbddf.serial_test")) {
        return &serialTestSchema();
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
    if (algorithmId == QStringLiteral("mbddf.do_write")) {
        for (const int channel : {5, 6}) {
            const QString id = QStringLiteral("channel_enabled[%1]").arg(channel);
            if (values.value(id).toBool()) {
                return failure(ErrorCode::ParameterRangeError,
                               QStringLiteral("Run parameter '%1' must remain false")
                                   .arg(id));
            }
        }
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
