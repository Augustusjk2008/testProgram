#pragma once

#include <hal/hal_types.h>

#include <QVariantMap>
#include <QVector>

namespace hwtest::hal {
class IHalDevice;
}

namespace hwtest::algorithm::mbddf {

struct DiStimulusChannel {
    QString switchId;
    int dutBit = -1;
    hwtest::hal::ResourceId resourceId;
    QString label;
    hwtest::hal::DigitalLevel activeLevel = hwtest::hal::DigitalLevel::Unknown;
};

struct DiStimulusState {
    bool configured = false;
    QVector<DiStimulusChannel> channels;
    quint64 appliedMask = 0;
    quint64 revision = 0;
    qint64 lastWriteTimestampUs = 0;
    int settlingMs = 0;
    hwtest::hal::HalStatus lastError;
};

class DiStimulusController {
public:
    explicit DiStimulusController(hwtest::hal::IHalDevice* device);

    hwtest::hal::HalStatus configure(const QVariantMap& executionConfig);
    hwtest::hal::HalStatus setDigitalStimulus(const QString& switchId,
                                              bool active,
                                              quint64 expectedRevision);
    hwtest::hal::HalStatus resetDigitalStimulus();
    DiStimulusState state() const;

private:
    hwtest::hal::HalStatus writeMask(quint64 mask, const QString& operation);

    hwtest::hal::IHalDevice* m_device = nullptr;
    DiStimulusState m_state;
};

} // namespace hwtest::algorithm::mbddf
