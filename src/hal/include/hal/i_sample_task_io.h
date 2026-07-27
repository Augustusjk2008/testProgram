#pragma once

#include "hal_global.h"
#include "hal_types.h"

namespace hwtest::hal {

using SampleTaskId = QString;

enum class SampleTaskKind {
    AnalogInput,
    AnalogOutput,
    DigitalInput,
    DigitalOutput,
    CounterInput,
    CounterOutput
};

enum class SampleTaskMode { OnDemand, Finite, Continuous };
enum class SampleValueType { Float64, UInt32 };
enum class SignalEdge { Rising, Falling };
enum class TriggerType { None, DigitalEdge, AnalogEdge, DigitalLevel };
enum class TriggerRole { Start, Reference, Pause };
enum class CounterMode { CountEdges, PulseFrequency };

struct SampleClockConfig {
    double rateHz = 0.0;
    QString source;
    SignalEdge edge = SignalEdge::Rising;
};

struct SampleTriggerConfig {
    TriggerType type = TriggerType::None;
    TriggerRole role = TriggerRole::Start;
    QString source;
    SignalEdge edge = SignalEdge::Rising;
    double level = 0.0;
    int referencePretriggerSamples = 0;
};

struct CounterTaskConfig {
    CounterMode mode = CounterMode::CountEdges;
    double minValue = 0.0;
    double maxValue = 4294967295.0;
    double frequencyHz = 1000.0;
    double dutyCycle = 0.5;
    SignalEdge edge = SignalEdge::Rising;
};

struct SampleTaskConfig {
    SampleTaskKind kind = SampleTaskKind::AnalogInput;
    SampleTaskMode mode = SampleTaskMode::OnDemand;
    QVector<ResourceId> channels;
    SampleClockConfig clock;
    SampleTriggerConfig trigger;
    CounterTaskConfig counter;
    double sampleRateHz = 0.0;
    int samplesPerChannel = 1;
    int bufferSamplesPerChannel = 0;
};

struct SampleTaskBlock {
    SampleValueType sampleType = SampleValueType::Float64;
    int channelCount = 0;
    int samplesPerChannel = 0;
    QVector<double> analogValues;
    QVector<quint32> integerValues;
    qint64 timestampUs = 0;
    QVariantMap metadata;
};

struct SampleTaskStatus {
    bool started = false;
    bool done = false;
    int availableSamplesPerChannel = 0;
    qint64 totalSamplesPerChannel = 0;
    bool overflowed = false;
    bool underflowed = false;
};

class HWTEST_HAL_EXPORT ISampleTaskIo {
public:
    virtual ~ISampleTaskIo() = default;

    virtual HalResult<SampleTaskId> createTask(const SampleTaskConfig& config,
                                               const OperationOptions& options) = 0;
    virtual HalStatus startTask(const SampleTaskId& taskId,
                                const OperationOptions& options) = 0;
    virtual HalResult<SampleTaskBlock> readTask(const SampleTaskId& taskId,
                                                int maxSamplesPerChannel,
                                                const OperationOptions& options) = 0;
    virtual HalStatus writeTask(const SampleTaskId& taskId,
                                const SampleTaskBlock& block,
                                const OperationOptions& options) = 0;
    virtual HalResult<SampleTaskStatus> taskStatus(const SampleTaskId& taskId,
                                                   const OperationOptions& options) = 0;
    virtual HalStatus stopTask(const SampleTaskId& taskId,
                               const OperationOptions& options) = 0;
    virtual HalStatus closeTask(const SampleTaskId& taskId,
                                const OperationOptions& options) = 0;
};

} // namespace hwtest::hal

Q_DECLARE_METATYPE(hwtest::hal::SampleTaskId)
Q_DECLARE_METATYPE(hwtest::hal::SampleTaskConfig)
Q_DECLARE_METATYPE(hwtest::hal::SampleTaskBlock)
Q_DECLARE_METATYPE(hwtest::hal::SampleTaskStatus)
