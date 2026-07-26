#include <algorithm/di_stimulus_controller.h>

#include <hal/i_digital_io.h>
#include <hal/i_hal_device.h>

#include <QDateTime>
#include <QSet>

#include <algorithm>

namespace hwtest::algorithm::mbddf {

namespace {

hwtest::hal::HalStatus failure(hwtest::hal::HalStatusCode code,
                               const QString& operation,
                               const QString& message,
                               const QVariantMap& detail = {})
{
    hwtest::hal::HalStatus status;
    status.code = code;
    status.error.code = code;
    status.error.operation = operation;
    status.error.message = message;
    status.error.detail = detail;
    return status;
}

qint64 currentTimestampUs()
{
    return static_cast<qint64>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) * 1000;
}

hwtest::hal::DigitalLevel parseLevel(const QVariant& value)
{
    const QString text = value.toString().trimmed();
    if (text.compare(QStringLiteral("High"), Qt::CaseInsensitive) == 0 || text == QStringLiteral("1")) {
        return hwtest::hal::DigitalLevel::High;
    }
    if (text.compare(QStringLiteral("Low"), Qt::CaseInsensitive) == 0 || text == QStringLiteral("0")) {
        return hwtest::hal::DigitalLevel::Low;
    }
    return hwtest::hal::DigitalLevel::Unknown;
}

hwtest::hal::DigitalLevel inverse(hwtest::hal::DigitalLevel level)
{
    return level == hwtest::hal::DigitalLevel::High
        ? hwtest::hal::DigitalLevel::Low
        : hwtest::hal::DigitalLevel::High;
}

} // namespace

DiStimulusController::DiStimulusController(hwtest::hal::IHalDevice* device)
    : m_device(device)
{
}

hwtest::hal::HalStatus DiStimulusController::configure(const QVariantMap& executionConfig)
{
    m_state = {};
    if (m_device == nullptr || m_device->digitalIo() == nullptr) {
        return failure(hwtest::hal::HalStatusCode::NotSupported,
                       QStringLiteral("diStimulus.configure"),
                       QStringLiteral("HAL digital output is unavailable"));
    }
    const QVariantMap stimulus = executionConfig.value(QStringLiteral("digitalStimulus")).toMap();
    const QVariantList configuredChannels = stimulus.value(QStringLiteral("channels")).toList();
    if (configuredChannels.isEmpty() || configuredChannels.size() > 64) {
        return failure(hwtest::hal::HalStatusCode::InvalidArgument,
                       QStringLiteral("diStimulus.configure"),
                       QStringLiteral("digitalStimulus.channels must contain 1..64 entries"));
    }
    bool settlingOk = false;
    const int settlingMs = stimulus.value(QStringLiteral("settlingMs"), 0).toInt(&settlingOk);
    if (!settlingOk || settlingMs < 0 || settlingMs > 60000) {
        return failure(hwtest::hal::HalStatusCode::InvalidArgument,
                       QStringLiteral("diStimulus.configure"),
                       QStringLiteral("digitalStimulus.settlingMs must be in the range 0..60000"));
    }

    QSet<QString> switchIds;
    QSet<QString> resourceIds;
    QSet<int> dutBits;
    QVector<DiStimulusChannel> channels;
    channels.reserve(configuredChannels.size());
    for (int index = 0; index < configuredChannels.size(); ++index) {
        const QVariantMap map = configuredChannels.at(index).toMap();
        DiStimulusChannel channel;
        channel.switchId = map.value(QStringLiteral("switchId")).toString().trimmed();
        channel.resourceId = map.value(QStringLiteral("resourceId")).toString().trimmed();
        channel.label = map.value(QStringLiteral("label"), channel.switchId).toString().trimmed();
        bool bitOk = false;
        channel.dutBit = map.value(QStringLiteral("dutBit")).toInt(&bitOk);
        channel.activeLevel = parseLevel(map.value(QStringLiteral("activeLevel")));
        if (channel.switchId.isEmpty() || channel.resourceId.isEmpty() ||
            !bitOk || channel.dutBit < 0 || channel.dutBit > 63 ||
            channel.activeLevel == hwtest::hal::DigitalLevel::Unknown) {
            return failure(hwtest::hal::HalStatusCode::InvalidArgument,
                           QStringLiteral("diStimulus.configure"),
                           QStringLiteral("Invalid digital stimulus channel at index %1").arg(index));
        }
        if (switchIds.contains(channel.switchId) ||
            resourceIds.contains(channel.resourceId) ||
            dutBits.contains(channel.dutBit)) {
            return failure(hwtest::hal::HalStatusCode::InvalidArgument,
                           QStringLiteral("diStimulus.configure"),
                           QStringLiteral("Digital stimulus switchId, resourceId and dutBit must be unique"));
        }
        switchIds.insert(channel.switchId);
        resourceIds.insert(channel.resourceId);
        dutBits.insert(channel.dutBit);
        channels.push_back(channel);
    }

    m_state.configured = true;
    m_state.channels = channels;
    m_state.settlingMs = settlingMs;
    return {};
}

hwtest::hal::HalStatus DiStimulusController::setDigitalStimulus(
    const QString& switchId,
    bool active,
    quint64 expectedRevision)
{
    if (!m_state.configured) {
        return failure(hwtest::hal::HalStatusCode::InvalidState,
                       QStringLiteral("diStimulus.set"),
                       QStringLiteral("Digital stimulus is not configured"));
    }
    if (expectedRevision != m_state.revision) {
        return failure(hwtest::hal::HalStatusCode::DataMismatch,
                       QStringLiteral("diStimulus.set"),
                       QStringLiteral("Digital stimulus revision conflict"),
                       {{QStringLiteral("expectedRevision"),
                         static_cast<qulonglong>(expectedRevision)},
                        {QStringLiteral("currentRevision"),
                         static_cast<qulonglong>(m_state.revision)}});
    }
    const auto it = std::find_if(m_state.channels.cbegin(),
                                 m_state.channels.cend(),
                                 [&switchId](const DiStimulusChannel& channel) {
                                     return channel.switchId == switchId;
                                 });
    if (it == m_state.channels.cend()) {
        return failure(hwtest::hal::HalStatusCode::NotFound,
                       QStringLiteral("diStimulus.set"),
                       QStringLiteral("Unknown digital stimulus switch '%1'").arg(switchId));
    }
    const quint64 bit = quint64{1} << it->dutBit;
    const quint64 nextMask = active ? (m_state.appliedMask | bit)
                                    : (m_state.appliedMask & ~bit);
    return writeMask(nextMask, QStringLiteral("diStimulus.set"));
}

hwtest::hal::HalStatus DiStimulusController::resetDigitalStimulus()
{
    if (!m_state.configured) {
        return failure(hwtest::hal::HalStatusCode::InvalidState,
                       QStringLiteral("diStimulus.reset"),
                       QStringLiteral("Digital stimulus is not configured"));
    }
    return writeMask(0, QStringLiteral("diStimulus.reset"));
}

DiStimulusState DiStimulusController::state() const
{
    return m_state;
}

hwtest::hal::HalStatus DiStimulusController::writeMask(quint64 mask,
                                                       const QString& operation)
{
    if (m_device == nullptr || m_device->digitalIo() == nullptr) {
        const auto status = failure(hwtest::hal::HalStatusCode::NotSupported,
                                    operation,
                                    QStringLiteral("HAL digital output is unavailable"));
        m_state.lastError = status;
        return status;
    }
    QMap<hwtest::hal::ResourceId, hwtest::hal::DigitalLevel> values;
    for (const DiStimulusChannel& channel : m_state.channels) {
        const bool active = (mask & (quint64{1} << channel.dutBit)) != 0;
        values.insert(channel.resourceId,
                      active ? channel.activeLevel : inverse(channel.activeLevel));
    }
    hwtest::hal::DigitalWriteOptions options;
    options.verifyAfterWrite = false;
    const hwtest::hal::HalStatus status = m_device->digitalIo()->writeDoBatch(values, options);
    if (!status.ok()) {
        m_state.lastError = status;
        return status;
    }
    m_state.appliedMask = mask;
    ++m_state.revision;
    m_state.lastWriteTimestampUs = currentTimestampUs();
    m_state.lastError = {};
    return {};
}

} // namespace hwtest::algorithm::mbddf
