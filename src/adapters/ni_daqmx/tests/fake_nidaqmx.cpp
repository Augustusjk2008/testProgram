#include "fake/NIDAQmx.h"

#include "fake_nidaqmx_control.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

enum class TaskKind {
    None,
    DigitalInput,
    DigitalOutput,
    AnalogInput,
    AnalogOutput,
    CounterInput,
    CounterOutput
};

struct FakeTask {
    TaskKind kind = TaskKind::None;
    bool started = false;
    bool done = false;
    std::string physicalChannel;
    int channelCount = 0;
    int terminalConfig = DAQmx_Val_Cfg_Default;
    double minValue = 0.0;
    double maxValue = 0.0;
    std::string sampleClockSource;
    double sampleClockRate = 0.0;
    int sampleClockEdge = 0;
    int sampleMode = 0;
    uInt64 samplesPerChannel = 0;
    uInt32 inputBufferSamples = 0;
    uInt32 outputBufferSamples = 0;
    std::string digitalTriggerSource;
    std::string analogTriggerSource;
    int digitalTriggerEdge = 0;
    int analogTriggerSlope = 0;
    double analogTriggerLevel = 0.0;
    uInt32 referencePretriggerSamples = 0;
    int pauseTriggerType = 0;
    std::string pauseTriggerSource;
    int pauseTriggerWhen = 0;
    double pauseTriggerLevel = 0.0;
    std::size_t digitalReadOffset = 0;
    unsigned int digitalInputGeneration = 0;
    std::size_t analogReadOffset = 0;
    unsigned int analogInputGeneration = 0;
    std::size_t counterReadOffset = 0;
    unsigned int counterInputGeneration = 0;
};

struct Failure {
    int32 code = 0;
    std::string message;
};

struct FakeState {
    std::string deviceName = "PXI1Slot2";
    std::string productType = "PXI-6259";
    uInt32 serialNumber = 62590002u;

    int createDo = 0;
    int createDi = 0;
    int createAi = 0;
    int createAo = 0;
    int createCounterInput = 0;
    int createCounterOutput = 0;
    int starts = 0;
    int stops = 0;
    int clears = 0;
    int writes = 0;
    int analogReads = 0;
    int analogWrites = 0;
    int counterReads = 0;
    int sampleClockConfigurations = 0;
    int digitalStartTriggerConfigurations = 0;
    int analogStartTriggerConfigurations = 0;
    int digitalReferenceTriggerConfigurations = 0;
    int analogReferenceTriggerConfigurations = 0;
    int pauseTriggerConfigurations = 0;
    int resets = 0;

    uInt32 inputMask = 0;
    uInt32 lastMask = 0;
    std::vector<uInt32> digitalInput;
    int digitalInputChannels = 1;
    int digitalInputSamplesPerChannel = 0;
    unsigned int digitalInputGeneration = 1;
    int nextDigitalReadSamplesPerChannel = -1;
    std::vector<uInt32> lastDigitalOutput;
    std::vector<double> analogInput;
    int analogInputChannels = 0;
    int analogInputSamplesPerChannel = 0;
    unsigned int analogInputGeneration = 1;
    int nextAnalogReadSamplesPerChannel = -1;
    std::vector<uInt32> counterInput;
    unsigned int counterInputGeneration = 1;
    int nextCounterReadSamples = -1;
    uInt32 writeSpaceAvailable = 1024;
    std::vector<double> lastAnalogOutput;

    std::string lastDoPhysicalChannel;
    std::string lastDiPhysicalChannel;
    std::string lastAiPhysicalChannel;
    std::string lastAoPhysicalChannel;
    std::string lastCounterPhysicalChannel;
    int lastAnalogTerminalConfig = DAQmx_Val_Cfg_Default;
    double lastAnalogMinimum = 0.0;
    double lastAnalogMaximum = 0.0;
    std::string lastSampleClockSource;
    double lastSampleClockRate = 0.0;
    int lastSampleMode = 0;
    uInt64 lastSamplesPerChannel = 0;

    std::map<fake_nidaqmx::Operation, Failure> failures;
    std::vector<fake_nidaqmx::Call> calls;
    std::string errorMessage;
    bool ioContractValid = true;
};

FakeState state;

int32 copyText(const char* source, char* output, uInt32 capacity)
{
    if (source == nullptr) return -1;
    const std::size_t required = std::strlen(source) + 1;
    if (output == nullptr || capacity == 0) return static_cast<int32>(required);
    if (capacity < required) return -2;
    std::memcpy(output, source, required);
    return 0;
}

void record(fake_nidaqmx::Call call)
{
    state.calls.push_back(call);
}

int32 consumeFailure(fake_nidaqmx::Operation operation)
{
    const auto it = state.failures.find(operation);
    if (it == state.failures.end()) return 0;
    const Failure failure = it->second;
    state.failures.erase(it);
    state.errorMessage = failure.message.empty()
        ? "fake NI-DAQmx injected error"
        : failure.message;
    return failure.code;
}

FakeTask* taskFrom(TaskHandle handle)
{
    return static_cast<FakeTask*>(handle);
}

bool taskMatches(const FakeTask* task, TaskKind expected)
{
    return task != nullptr && task->kind == expected;
}

bool configureTask(FakeTask* task, TaskKind kind, const char* physicalChannel)
{
    if (task == nullptr || physicalChannel == nullptr || *physicalChannel == '\0' ||
        (task->kind != TaskKind::None && task->kind != kind)) {
        state.ioContractValid = false;
        return false;
    }
    task->kind = kind;
    task->physicalChannel = physicalChannel;
    return true;
}

int channelCount(const char* physicalChannel)
{
    if (physicalChannel == nullptr || *physicalChannel == '\0') return 0;
    const char* colon = std::strrchr(physicalChannel, ':');
    if (colon == nullptr) return 1;
    char* end = nullptr;
    const long last = std::strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0') return 1;
    const char* firstDigits = colon;
    while (firstDigits > physicalChannel &&
           std::isdigit(static_cast<unsigned char>(firstDigits[-1])) != 0) {
        --firstDigits;
    }
    const long first = std::strtol(firstDigits, nullptr, 10);
    return last >= first && last - first < 1024 ? static_cast<int>(last - first + 1) : 1;
}

int32 invalidContract()
{
    state.ioContractValid = false;
    state.errorMessage = "fake NI-DAQmx contract violation";
    return -1;
}

int availableAnalogSamples(const FakeTask* task)
{
    if (task == nullptr || state.analogInputSamplesPerChannel <= 0 ||
        task->analogReadOffset >= static_cast<std::size_t>(state.analogInputSamplesPerChannel)) {
        return 0;
    }
    return state.analogInputSamplesPerChannel - static_cast<int>(task->analogReadOffset);
}

int availableDigitalSamples(const FakeTask* task)
{
    if (task == nullptr) return 0;
    if (state.digitalInputSamplesPerChannel <= 0) return 1;
    if (task->digitalReadOffset >=
        static_cast<std::size_t>(state.digitalInputSamplesPerChannel)) {
        return 0;
    }
    return state.digitalInputSamplesPerChannel -
        static_cast<int>(task->digitalReadOffset);
}

void synchronizeDigitalInput(FakeTask* task)
{
    if (task != nullptr && task->digitalInputGeneration != state.digitalInputGeneration) {
        task->digitalReadOffset = 0;
        task->digitalInputGeneration = state.digitalInputGeneration;
    }
}

void synchronizeAnalogInput(FakeTask* task)
{
    if (task != nullptr && task->analogInputGeneration != state.analogInputGeneration) {
        task->analogReadOffset = 0;
        task->analogInputGeneration = state.analogInputGeneration;
    }
}

void synchronizeCounterInput(FakeTask* task)
{
    if (task != nullptr && task->counterInputGeneration != state.counterInputGeneration) {
        task->counterReadOffset = 0;
        task->counterInputGeneration = state.counterInputGeneration;
    }
}

int availableCounterSamples(const FakeTask* task)
{
    if (task == nullptr || task->counterReadOffset >= state.counterInput.size()) return 0;
    return static_cast<int>(state.counterInput.size() - task->counterReadOffset);
}

} // namespace

namespace fake_nidaqmx {

void reset()
{
    state = FakeState{};
}

void setDeviceIdentity(const char* deviceName,
                       const char* productType,
                       std::uint32_t serialNumber)
{
    state.deviceName = deviceName == nullptr ? "" : deviceName;
    state.productType = productType == nullptr ? "" : productType;
    state.serialNumber = serialNumber;
}

void setInputMask(std::uint32_t mask)
{
    state.inputMask = mask;
    state.digitalInput.clear();
    state.digitalInputChannels = 1;
    state.digitalInputSamplesPerChannel = 0;
    ++state.digitalInputGeneration;
    if (state.digitalInputGeneration == 0) ++state.digitalInputGeneration;
}

void setDigitalInputBlock(const std::uint32_t* values, int samplesPerChannel)
{
    setDigitalInputChannelsBlock(values, 1, samplesPerChannel);
}

void setDigitalInputChannelsBlock(const std::uint32_t* values,
                                  int channelCount,
                                  int samplesPerChannel)
{
    state.digitalInput.clear();
    state.digitalInputChannels = 1;
    state.digitalInputSamplesPerChannel = 0;
    ++state.digitalInputGeneration;
    if (state.digitalInputGeneration == 0) ++state.digitalInputGeneration;
    if (values == nullptr || channelCount <= 0 || samplesPerChannel < 0) return;
    state.digitalInput.assign(values, values + channelCount * samplesPerChannel);
    state.digitalInputChannels = channelCount;
    state.digitalInputSamplesPerChannel = samplesPerChannel;
}

void setAnalogInputBlock(const double* values,
                         int channelCount,
                         int samplesPerChannel)
{
    state.analogInput.clear();
    state.analogInputChannels = 0;
    state.analogInputSamplesPerChannel = 0;
    ++state.analogInputGeneration;
    if (state.analogInputGeneration == 0) ++state.analogInputGeneration;
    if (values == nullptr || channelCount <= 0 || samplesPerChannel < 0) return;
    const std::size_t total = static_cast<std::size_t>(channelCount) *
        static_cast<std::size_t>(samplesPerChannel);
    state.analogInput.assign(values, values + total);
    state.analogInputChannels = channelCount;
    state.analogInputSamplesPerChannel = samplesPerChannel;
}

void setCounterInputSamples(const std::uint32_t* values, int sampleCount)
{
    state.counterInput.clear();
    ++state.counterInputGeneration;
    if (state.counterInputGeneration == 0) ++state.counterInputGeneration;
    if (values == nullptr || sampleCount < 0) return;
    state.counterInput.assign(values, values + sampleCount);
}

void setNextDigitalReadSamplesPerChannel(int samplesPerChannel)
{
    state.nextDigitalReadSamplesPerChannel = samplesPerChannel;
}

void setNextAnalogReadSamplesPerChannel(int samplesPerChannel)
{
    state.nextAnalogReadSamplesPerChannel = samplesPerChannel;
}

void setNextCounterReadSamples(int samples)
{
    state.nextCounterReadSamples = samples;
}

void setWriteSpaceAvailable(std::uint32_t samplesPerChannel)
{
    state.writeSpaceAvailable = samplesPerChannel;
}

void failNext(Operation operation, std::int32_t code, const char* message)
{
    state.failures[operation] = Failure{code,
                                         message == nullptr
                                             ? "fake NI-DAQmx error"
                                             : message};
}

void failNextWrite(std::int32_t code, const char* message)
{
    failNext(Operation::WriteDigital, code, message);
}

void clearCallLog()
{
    state.calls.clear();
}

int createDoCalls() { return state.createDo; }
int createDiCalls() { return state.createDi; }
int createAiCalls() { return state.createAi; }
int createAoCalls() { return state.createAo; }
int createCounterInputCalls() { return state.createCounterInput; }
int createCounterOutputCalls() { return state.createCounterOutput; }
int startCalls() { return state.starts; }
int stopCalls() { return state.stops; }
int clearCalls() { return state.clears; }
int writeCalls() { return state.writes; }
int readAnalogCalls() { return state.analogReads; }
int writeAnalogCalls() { return state.analogWrites; }
int readCounterCalls() { return state.counterReads; }
int configureSampleClockCalls() { return state.sampleClockConfigurations; }
int configureDigitalStartTriggerCalls() { return state.digitalStartTriggerConfigurations; }
int configureAnalogStartTriggerCalls() { return state.analogStartTriggerConfigurations; }
int configureDigitalReferenceTriggerCalls() { return state.digitalReferenceTriggerConfigurations; }
int configureAnalogReferenceTriggerCalls() { return state.analogReferenceTriggerConfigurations; }
int configurePauseTriggerCalls() { return state.pauseTriggerConfigurations; }
int resetDeviceCalls() { return state.resets; }
std::uint32_t lastWrittenMask() { return state.lastMask; }
int lastAnalogTerminalConfig() { return state.lastAnalogTerminalConfig; }
double lastAnalogMinimum() { return state.lastAnalogMinimum; }
double lastAnalogMaximum() { return state.lastAnalogMaximum; }
const char* lastSampleClockSource() { return state.lastSampleClockSource.c_str(); }
double lastSampleClockRate() { return state.lastSampleClockRate; }
int lastSampleMode() { return state.lastSampleMode; }
std::uint64_t lastSamplesPerChannel() { return state.lastSamplesPerChannel; }
int lastAnalogOutputCount() { return static_cast<int>(state.lastAnalogOutput.size()); }
double lastAnalogOutputAt(int index)
{
    return index >= 0 && index < static_cast<int>(state.lastAnalogOutput.size())
        ? state.lastAnalogOutput.at(static_cast<std::size_t>(index))
        : 0.0;
}
int lastDigitalOutputCount() { return static_cast<int>(state.lastDigitalOutput.size()); }
std::uint32_t lastDigitalOutputAt(int index)
{
    return index >= 0 && index < static_cast<int>(state.lastDigitalOutput.size())
        ? state.lastDigitalOutput.at(static_cast<std::size_t>(index))
        : 0u;
}
const char* lastDoPhysicalChannel() { return state.lastDoPhysicalChannel.c_str(); }
const char* lastDiPhysicalChannel() { return state.lastDiPhysicalChannel.c_str(); }
const char* lastAiPhysicalChannel() { return state.lastAiPhysicalChannel.c_str(); }
const char* lastAoPhysicalChannel() { return state.lastAoPhysicalChannel.c_str(); }
const char* lastCounterPhysicalChannel() { return state.lastCounterPhysicalChannel.c_str(); }
bool ioContractValid() { return state.ioContractValid; }

int firstCallIndex(Call call)
{
    const auto it = std::find(state.calls.begin(), state.calls.end(), call);
    return it == state.calls.end() ? -1 : static_cast<int>(it - state.calls.begin());
}

int lastCallIndex(Call call)
{
    const auto it = std::find(state.calls.rbegin(), state.calls.rend(), call);
    return it == state.calls.rend()
        ? -1
        : static_cast<int>(state.calls.size() - 1 - (it - state.calls.rbegin()));
}

bool callsContainInOrder(const Call* expected, int count)
{
    if (expected == nullptr || count < 0) return false;
    int next = 0;
    for (const Call actual : state.calls) {
        if (next < count && actual == expected[next]) ++next;
    }
    return next == count;
}

} // namespace fake_nidaqmx

extern "C" {

int32 DAQmxGetSysDevNames(char data[], uInt32 bufferSize)
{
    return copyText(state.deviceName.c_str(), data, bufferSize);
}

int32 DAQmxGetDevProductType(const char device[], char data[], uInt32 bufferSize)
{
    if (device == nullptr || state.deviceName != device) return -200220;
    return copyText(state.productType.c_str(), data, bufferSize);
}

int32 DAQmxGetDevSerialNum(const char device[], uInt32* data)
{
    if (device == nullptr || data == nullptr || state.deviceName != device) return -200220;
    *data = state.serialNumber;
    return 0;
}

int32 DAQmxCreateTask(const char*, TaskHandle* taskHandle)
{
    record(fake_nidaqmx::Call::CreateTask);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::CreateTask);
    if (failure != 0) return failure;
    if (taskHandle == nullptr) return invalidContract();
    *taskHandle = new FakeTask;
    return 0;
}

int32 DAQmxCreateDOChan(TaskHandle taskHandle,
                        const char lines[],
                        const char*,
                        int32 lineGrouping)
{
    record(fake_nidaqmx::Call::CreateDigitalOutputChannel);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::CreateDigitalOutputChannel);
    if (failure != 0) return failure;
    if (lineGrouping != DAQmx_Val_ChanForAllLines ||
        !configureTask(taskFrom(taskHandle), TaskKind::DigitalOutput, lines)) {
        return invalidContract();
    }
    FakeTask* task = taskFrom(taskHandle);
    task->channelCount += 1;
    state.lastDoPhysicalChannel = lines;
    ++state.createDo;
    return 0;
}

int32 DAQmxCreateDIChan(TaskHandle taskHandle,
                        const char lines[],
                        const char*,
                        int32 lineGrouping)
{
    record(fake_nidaqmx::Call::CreateDigitalInputChannel);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::CreateDigitalInputChannel);
    if (failure != 0) return failure;
    if (lineGrouping != DAQmx_Val_ChanForAllLines ||
        !configureTask(taskFrom(taskHandle), TaskKind::DigitalInput, lines)) {
        return invalidContract();
    }
    FakeTask* task = taskFrom(taskHandle);
    task->channelCount += 1;
    state.lastDiPhysicalChannel = lines;
    ++state.createDi;
    return 0;
}

int32 DAQmxCreateAIVoltageChan(TaskHandle taskHandle,
                               const char physicalChannel[],
                               const char*,
                               int32 terminalConfig,
                               float64 minVal,
                               float64 maxVal,
                               int32 units,
                               const char*)
{
    record(fake_nidaqmx::Call::CreateAnalogInputChannel);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::CreateAnalogInputChannel);
    if (failure != 0) return failure;
    if (units != DAQmx_Val_Volts || minVal >= maxVal ||
        !configureTask(taskFrom(taskHandle), TaskKind::AnalogInput, physicalChannel)) {
        return invalidContract();
    }
    FakeTask* task = taskFrom(taskHandle);
    task->channelCount += channelCount(physicalChannel);
    task->terminalConfig = terminalConfig;
    task->minValue = minVal;
    task->maxValue = maxVal;
    state.lastAiPhysicalChannel = physicalChannel;
    state.lastAnalogTerminalConfig = terminalConfig;
    state.lastAnalogMinimum = minVal;
    state.lastAnalogMaximum = maxVal;
    ++state.createAi;
    return 0;
}

int32 DAQmxCreateAOVoltageChan(TaskHandle taskHandle,
                               const char physicalChannel[],
                               const char*,
                               float64 minVal,
                               float64 maxVal,
                               int32 units,
                               const char*)
{
    record(fake_nidaqmx::Call::CreateAnalogOutputChannel);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::CreateAnalogOutputChannel);
    if (failure != 0) return failure;
    if (units != DAQmx_Val_Volts || minVal >= maxVal ||
        !configureTask(taskFrom(taskHandle), TaskKind::AnalogOutput, physicalChannel)) {
        return invalidContract();
    }
    FakeTask* task = taskFrom(taskHandle);
    task->channelCount += channelCount(physicalChannel);
    task->minValue = minVal;
    task->maxValue = maxVal;
    state.lastAoPhysicalChannel = physicalChannel;
    state.lastAnalogMinimum = minVal;
    state.lastAnalogMaximum = maxVal;
    ++state.createAo;
    return 0;
}

int32 DAQmxCreateCICountEdgesChan(TaskHandle taskHandle,
                                  const char counter[],
                                  const char*,
                                  int32 edge,
                                  uInt32,
                                  int32 countDirection)
{
    record(fake_nidaqmx::Call::CreateCounterInputChannel);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::CreateCounterInputChannel);
    if (failure != 0) return failure;
    if ((edge != DAQmx_Val_Rising && edge != DAQmx_Val_Falling) ||
        countDirection != DAQmx_Val_CountUp ||
        !configureTask(taskFrom(taskHandle), TaskKind::CounterInput, counter)) {
        return invalidContract();
    }
    taskFrom(taskHandle)->channelCount = 1;
    state.lastCounterPhysicalChannel = counter;
    ++state.createCounterInput;
    return 0;
}

int32 DAQmxCreateCOPulseChanFreq(TaskHandle taskHandle,
                                 const char counter[],
                                 const char*,
                                 int32 units,
                                 int32 idleState,
                                 float64,
                                 float64 frequency,
                                 float64 dutyCycle)
{
    record(fake_nidaqmx::Call::CreateCounterOutputChannel);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::CreateCounterOutputChannel);
    if (failure != 0) return failure;
    if (units != DAQmx_Val_Hz ||
        (idleState != DAQmx_Val_Low && idleState != DAQmx_Val_High) ||
        frequency <= 0.0 || dutyCycle <= 0.0 || dutyCycle >= 1.0 ||
        !configureTask(taskFrom(taskHandle), TaskKind::CounterOutput, counter)) {
        return invalidContract();
    }
    taskFrom(taskHandle)->channelCount = 1;
    state.lastCounterPhysicalChannel = counter;
    ++state.createCounterOutput;
    return 0;
}

int32 DAQmxCfgSampClkTiming(TaskHandle taskHandle,
                            const char source[],
                            float64 rate,
                            int32 activeEdge,
                            int32 sampleMode,
                            uInt64 sampsPerChanToAcquire)
{
    record(fake_nidaqmx::Call::ConfigureSampleClock);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ConfigureSampleClock);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || task->kind == TaskKind::None || source == nullptr || rate <= 0.0 ||
        (activeEdge != DAQmx_Val_Rising && activeEdge != DAQmx_Val_Falling) ||
        (sampleMode != DAQmx_Val_FiniteSamps && sampleMode != DAQmx_Val_ContSamps)) {
        return invalidContract();
    }
    task->sampleClockSource = source;
    task->sampleClockRate = rate;
    task->sampleClockEdge = activeEdge;
    task->sampleMode = sampleMode;
    task->samplesPerChannel = sampsPerChanToAcquire;
    state.lastSampleClockSource = source;
    state.lastSampleClockRate = rate;
    state.lastSampleMode = sampleMode;
    state.lastSamplesPerChannel = sampsPerChanToAcquire;
    ++state.sampleClockConfigurations;
    return 0;
}

int32 DAQmxCfgImplicitTiming(TaskHandle taskHandle,
                             int32 sampleMode,
                             uInt64 sampsPerChanToAcquire)
{
    record(fake_nidaqmx::Call::ConfigureImplicitTiming);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ConfigureImplicitTiming);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || task->kind == TaskKind::None ||
        (sampleMode != DAQmx_Val_FiniteSamps && sampleMode != DAQmx_Val_ContSamps)) {
        return invalidContract();
    }
    task->sampleMode = sampleMode;
    task->samplesPerChannel = sampsPerChanToAcquire;
    state.lastSampleMode = sampleMode;
    state.lastSamplesPerChannel = sampsPerChanToAcquire;
    return 0;
}

int32 DAQmxCfgInputBuffer(TaskHandle taskHandle, uInt32 numSampsPerChan)
{
    record(fake_nidaqmx::Call::ConfigureInputBuffer);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ConfigureInputBuffer);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || numSampsPerChan == 0) return invalidContract();
    task->inputBufferSamples = numSampsPerChan;
    return 0;
}

int32 DAQmxCfgOutputBuffer(TaskHandle taskHandle, uInt32 numSampsPerChan)
{
    record(fake_nidaqmx::Call::ConfigureOutputBuffer);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ConfigureOutputBuffer);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || numSampsPerChan == 0) return invalidContract();
    task->outputBufferSamples = numSampsPerChan;
    return 0;
}

int32 DAQmxCfgDigEdgeStartTrig(TaskHandle taskHandle,
                               const char triggerSource[],
                               int32 triggerEdge)
{
    record(fake_nidaqmx::Call::ConfigureDigitalStartTrigger);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ConfigureDigitalStartTrigger);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || triggerSource == nullptr || *triggerSource == '\0' ||
        (triggerEdge != DAQmx_Val_Rising && triggerEdge != DAQmx_Val_Falling)) {
        return invalidContract();
    }
    task->digitalTriggerSource = triggerSource;
    task->digitalTriggerEdge = triggerEdge;
    ++state.digitalStartTriggerConfigurations;
    return 0;
}

int32 DAQmxCfgAnlgEdgeStartTrig(TaskHandle taskHandle,
                                const char triggerSource[],
                                int32 triggerSlope,
                                float64 triggerLevel)
{
    record(fake_nidaqmx::Call::ConfigureAnalogStartTrigger);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ConfigureAnalogStartTrigger);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || triggerSource == nullptr || *triggerSource == '\0' ||
        (triggerSlope != DAQmx_Val_Rising && triggerSlope != DAQmx_Val_Falling)) {
        return invalidContract();
    }
    task->analogTriggerSource = triggerSource;
    task->analogTriggerSlope = triggerSlope;
    task->analogTriggerLevel = triggerLevel;
    ++state.analogStartTriggerConfigurations;
    return 0;
}

int32 DAQmxCfgDigEdgeRefTrig(TaskHandle taskHandle,
                             const char triggerSource[],
                             int32 triggerEdge,
                             uInt32 pretriggerSamples)
{
    record(fake_nidaqmx::Call::ConfigureDigitalReferenceTrigger);
    const int32 failure = consumeFailure(
        fake_nidaqmx::Operation::ConfigureDigitalReferenceTrigger);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || triggerSource == nullptr || *triggerSource == '\0' ||
        pretriggerSamples == 0 ||
        (triggerEdge != DAQmx_Val_Rising && triggerEdge != DAQmx_Val_Falling)) {
        return invalidContract();
    }
    task->digitalTriggerSource = triggerSource;
    task->digitalTriggerEdge = triggerEdge;
    task->referencePretriggerSamples = pretriggerSamples;
    ++state.digitalReferenceTriggerConfigurations;
    return 0;
}

int32 DAQmxCfgAnlgEdgeRefTrig(TaskHandle taskHandle,
                              const char triggerSource[],
                              int32 triggerSlope,
                              float64 triggerLevel,
                              uInt32 pretriggerSamples)
{
    record(fake_nidaqmx::Call::ConfigureAnalogReferenceTrigger);
    const int32 failure = consumeFailure(
        fake_nidaqmx::Operation::ConfigureAnalogReferenceTrigger);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || triggerSource == nullptr || *triggerSource == '\0' ||
        pretriggerSamples == 0 ||
        (triggerSlope != DAQmx_Val_Rising && triggerSlope != DAQmx_Val_Falling)) {
        return invalidContract();
    }
    task->analogTriggerSource = triggerSource;
    task->analogTriggerSlope = triggerSlope;
    task->analogTriggerLevel = triggerLevel;
    task->referencePretriggerSamples = pretriggerSamples;
    ++state.analogReferenceTriggerConfigurations;
    return 0;
}

int32 DAQmxSetPauseTrigType(TaskHandle taskHandle, int32 data)
{
    record(fake_nidaqmx::Call::ConfigurePauseTrigger);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ConfigurePauseTrigger);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || (data != DAQmx_Val_DigLvl && data != DAQmx_Val_AnlgLvl)) {
        return invalidContract();
    }
    task->pauseTriggerType = data;
    ++state.pauseTriggerConfigurations;
    return 0;
}

int32 DAQmxSetDigLvlPauseTrigSrc(TaskHandle taskHandle, const char data[])
{
    record(fake_nidaqmx::Call::ConfigurePauseTrigger);
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || task->pauseTriggerType != DAQmx_Val_DigLvl ||
        data == nullptr || *data == '\0') {
        return invalidContract();
    }
    task->pauseTriggerSource = data;
    return 0;
}

int32 DAQmxSetDigLvlPauseTrigWhen(TaskHandle taskHandle, int32 data)
{
    record(fake_nidaqmx::Call::ConfigurePauseTrigger);
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || task->pauseTriggerType != DAQmx_Val_DigLvl ||
        (data != DAQmx_Val_High && data != DAQmx_Val_Low)) {
        return invalidContract();
    }
    task->pauseTriggerWhen = data;
    return 0;
}

int32 DAQmxSetAnlgLvlPauseTrigSrc(TaskHandle taskHandle, const char data[])
{
    record(fake_nidaqmx::Call::ConfigurePauseTrigger);
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || task->pauseTriggerType != DAQmx_Val_AnlgLvl ||
        data == nullptr || *data == '\0') {
        return invalidContract();
    }
    task->pauseTriggerSource = data;
    return 0;
}

int32 DAQmxSetAnlgLvlPauseTrigWhen(TaskHandle taskHandle, int32 data)
{
    record(fake_nidaqmx::Call::ConfigurePauseTrigger);
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || task->pauseTriggerType != DAQmx_Val_AnlgLvl ||
        (data != DAQmx_Val_AboveLvl && data != DAQmx_Val_BelowLvl)) {
        return invalidContract();
    }
    task->pauseTriggerWhen = data;
    return 0;
}

int32 DAQmxSetAnlgLvlPauseTrigLvl(TaskHandle taskHandle, float64 data)
{
    record(fake_nidaqmx::Call::ConfigurePauseTrigger);
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || task->pauseTriggerType != DAQmx_Val_AnlgLvl) {
        return invalidContract();
    }
    task->pauseTriggerLevel = data;
    return 0;
}

int32 DAQmxConnectTerms(const char sourceTerminal[],
                        const char destinationTerminal[],
                        int32)
{
    record(fake_nidaqmx::Call::ConnectTerms);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ConnectTerms);
    if (failure != 0) return failure;
    if (sourceTerminal == nullptr || destinationTerminal == nullptr ||
        *sourceTerminal == '\0' || *destinationTerminal == '\0') {
        return invalidContract();
    }
    return 0;
}

int32 DAQmxDisconnectTerms(const char sourceTerminal[], const char destinationTerminal[])
{
    record(fake_nidaqmx::Call::DisconnectTerms);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::DisconnectTerms);
    if (failure != 0) return failure;
    if (sourceTerminal == nullptr || destinationTerminal == nullptr ||
        *sourceTerminal == '\0' || *destinationTerminal == '\0') {
        return invalidContract();
    }
    return 0;
}

int32 DAQmxStartTask(TaskHandle taskHandle)
{
    record(fake_nidaqmx::Call::StartTask);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::StartTask);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || task->kind == TaskKind::None) return invalidContract();
    task->started = true;
    task->done = false;
    ++state.starts;
    return 0;
}

int32 DAQmxStopTask(TaskHandle taskHandle)
{
    record(fake_nidaqmx::Call::StopTask);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::StopTask);
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr) return invalidContract();
    task->started = false;
    task->done = true;
    ++state.stops;
    return failure;
}

int32 DAQmxClearTask(TaskHandle taskHandle)
{
    record(fake_nidaqmx::Call::ClearTask);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ClearTask);
    if (taskHandle == nullptr) return invalidContract();
    delete taskFrom(taskHandle);
    ++state.clears;
    return failure;
}

int32 DAQmxWriteDigitalU32(TaskHandle taskHandle,
                           int32 numSampsPerChan,
                           bool32 autoStart,
                           float64,
                           bool32 dataLayout,
                           const uInt32 writeArray[],
                           int32* sampsPerChanWritten,
                           bool32*)
{
    record(fake_nidaqmx::Call::WriteDigital);
    FakeTask* task = taskFrom(taskHandle);
    if (!taskMatches(task, TaskKind::DigitalOutput) || numSampsPerChan <= 0 ||
        writeArray == nullptr || (autoStart != 0 && autoStart != 1) ||
        (dataLayout != DAQmx_Val_GroupByChannel &&
         dataLayout != DAQmx_Val_GroupByScanNumber) ||
        (!task->started && autoStart == 0 && task->sampleMode == 0)) {
        return invalidContract();
    }
    ++state.writes;
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::WriteDigital);
    if (failure != 0) return failure;
    if (autoStart != 0) task->started = true;
    const std::size_t total = static_cast<std::size_t>(task->channelCount) *
        static_cast<std::size_t>(numSampsPerChan);
    state.lastDigitalOutput.assign(writeArray, writeArray + total);
    state.lastMask = writeArray[total - 1];
    if (sampsPerChanWritten != nullptr) *sampsPerChanWritten = numSampsPerChan;
    return 0;
}

int32 DAQmxReadDigitalU32(TaskHandle taskHandle,
                          int32 numSampsPerChan,
                          float64,
                          bool32 fillMode,
                          uInt32 readArray[],
                          uInt32 arraySizeInSamps,
                          int32* sampsPerChanRead,
                          bool32*)
{
    record(fake_nidaqmx::Call::ReadDigital);
    FakeTask* task = taskFrom(taskHandle);
    if (!taskMatches(task, TaskKind::DigitalInput) || numSampsPerChan <= 0 ||
        readArray == nullptr || !task->started ||
        (fillMode != DAQmx_Val_GroupByChannel &&
         fillMode != DAQmx_Val_GroupByScanNumber)) {
        return invalidContract();
    }
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ReadDigital);
    if (failure != 0) return failure;
    synchronizeDigitalInput(task);
    int actual = std::min(numSampsPerChan, availableDigitalSamples(task));
    if (state.nextDigitalReadSamplesPerChannel >= 0) {
        actual = std::min(actual, state.nextDigitalReadSamplesPerChannel);
        state.nextDigitalReadSamplesPerChannel = -1;
    }
    const int channelCount = std::max(1, task->channelCount);
    if (state.digitalInputSamplesPerChannel > 0 &&
        state.digitalInputChannels != channelCount) {
        return invalidContract();
    }
    if (arraySizeInSamps < static_cast<uInt32>(channelCount * actual)) {
        return invalidContract();
    }
    for (int channel = 0; channel < channelCount; ++channel) {
        for (int index = 0; index < actual; ++index) {
            readArray[channel * actual + index] = state.digitalInputSamplesPerChannel == 0
                ? state.inputMask
                : state.digitalInput.at(
                      static_cast<std::size_t>(channel * state.digitalInputSamplesPerChannel) +
                      task->digitalReadOffset + static_cast<std::size_t>(index));
        }
    }
    task->digitalReadOffset += static_cast<std::size_t>(actual);
    if (sampsPerChanRead != nullptr) *sampsPerChanRead = actual;
    return 0;
}

int32 DAQmxWriteAnalogF64(TaskHandle taskHandle,
                          int32 numSampsPerChan,
                          bool32 autoStart,
                          float64,
                          bool32 dataLayout,
                          const float64 writeArray[],
                          int32* sampsPerChanWritten,
                          bool32*)
{
    record(fake_nidaqmx::Call::WriteAnalog);
    FakeTask* task = taskFrom(taskHandle);
    if (!taskMatches(task, TaskKind::AnalogOutput) || numSampsPerChan <= 0 ||
        writeArray == nullptr || (autoStart != 0 && autoStart != 1) ||
        dataLayout != DAQmx_Val_GroupByChannel ||
        (!task->started && autoStart == 0 && task->sampleMode == 0)) {
        return invalidContract();
    }
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::WriteAnalog);
    if (failure != 0) return failure;
    if (autoStart != 0) task->started = true;
    const std::size_t total = static_cast<std::size_t>(task->channelCount) *
        static_cast<std::size_t>(numSampsPerChan);
    state.lastAnalogOutput.assign(writeArray, writeArray + total);
    ++state.analogWrites;
    if (sampsPerChanWritten != nullptr) *sampsPerChanWritten = numSampsPerChan;
    return 0;
}

int32 DAQmxReadAnalogF64(TaskHandle taskHandle,
                         int32 numSampsPerChan,
                         float64,
                         bool32 fillMode,
                         float64 readArray[],
                         uInt32 arraySizeInSamps,
                         int32* sampsPerChanRead,
                         bool32*)
{
    record(fake_nidaqmx::Call::ReadAnalog);
    FakeTask* task = taskFrom(taskHandle);
    if (!taskMatches(task, TaskKind::AnalogInput) || readArray == nullptr ||
        !task->started || (fillMode != DAQmx_Val_GroupByChannel &&
                           fillMode != DAQmx_Val_GroupByScanNumber)) {
        return invalidContract();
    }
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ReadAnalog);
    if (failure != 0) return failure;
    if (state.analogInputChannels != task->channelCount) return invalidContract();
    synchronizeAnalogInput(task);
    const int available = availableAnalogSamples(task);
    const int requested = numSampsPerChan < 0 ? available : numSampsPerChan;
    if (requested < 0) return invalidContract();
    int actual = std::min(requested, available);
    if (state.nextAnalogReadSamplesPerChannel >= 0) {
        actual = std::min(actual, state.nextAnalogReadSamplesPerChannel);
        state.nextAnalogReadSamplesPerChannel = -1;
    }
    const std::size_t required = static_cast<std::size_t>(actual) *
        static_cast<std::size_t>(task->channelCount);
    if (arraySizeInSamps < required) return invalidContract();
    for (int channel = 0; channel < task->channelCount; ++channel) {
        for (int sample = 0; sample < actual; ++sample) {
            const double value = state.analogInput.at(
                static_cast<std::size_t>(channel * state.analogInputSamplesPerChannel) +
                task->analogReadOffset + static_cast<std::size_t>(sample));
            const std::size_t target = fillMode == DAQmx_Val_GroupByChannel
                ? static_cast<std::size_t>(channel * actual + sample)
                : static_cast<std::size_t>(sample * task->channelCount + channel);
            readArray[target] = value;
        }
    }
    task->analogReadOffset += static_cast<std::size_t>(actual);
    if (task->sampleMode == DAQmx_Val_FiniteSamps &&
        task->analogReadOffset >= task->samplesPerChannel) {
        task->done = true;
    }
    ++state.analogReads;
    if (sampsPerChanRead != nullptr) *sampsPerChanRead = actual;
    return 0;
}

int32 DAQmxReadCounterU32(TaskHandle taskHandle,
                          int32 numSampsPerChan,
                          float64,
                          uInt32 readArray[],
                          uInt32 arraySizeInSamps,
                          int32* sampsPerChanRead,
                          bool32*)
{
    record(fake_nidaqmx::Call::ReadCounter);
    FakeTask* task = taskFrom(taskHandle);
    if (!taskMatches(task, TaskKind::CounterInput) || numSampsPerChan <= 0 ||
        readArray == nullptr || !task->started) {
        return invalidContract();
    }
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::ReadCounter);
    if (failure != 0) return failure;
    synchronizeCounterInput(task);
    int actual = std::min(numSampsPerChan, availableCounterSamples(task));
    if (state.nextCounterReadSamples >= 0) {
        actual = std::min(actual, state.nextCounterReadSamples);
        state.nextCounterReadSamples = -1;
    }
    if (arraySizeInSamps < static_cast<uInt32>(actual)) return invalidContract();
    for (int index = 0; index < actual; ++index) {
        readArray[index] = state.counterInput.at(task->counterReadOffset +
                                                 static_cast<std::size_t>(index));
    }
    task->counterReadOffset += static_cast<std::size_t>(actual);
    ++state.counterReads;
    if (sampsPerChanRead != nullptr) *sampsPerChanRead = actual;
    return 0;
}

int32 DAQmxIsTaskDone(TaskHandle taskHandle, bool32* isTaskDone)
{
    record(fake_nidaqmx::Call::QueryTaskDone);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::QueryTaskDone);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || isTaskDone == nullptr) return invalidContract();
    *isTaskDone = (!task->started || task->done) ? 1u : 0u;
    return 0;
}

int32 DAQmxGetReadAvailSampPerChan(TaskHandle taskHandle, uInt32* data)
{
    record(fake_nidaqmx::Call::QueryReadAvailable);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::QueryReadAvailable);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if (task == nullptr || data == nullptr) return invalidContract();
    if (task->kind == TaskKind::DigitalInput) {
        synchronizeDigitalInput(task);
        *data = static_cast<uInt32>(availableDigitalSamples(task));
    } else if (task->kind == TaskKind::AnalogInput) {
        synchronizeAnalogInput(task);
        *data = static_cast<uInt32>(availableAnalogSamples(task));
    }
    else if (task->kind == TaskKind::CounterInput) {
        synchronizeCounterInput(task);
        *data = static_cast<uInt32>(availableCounterSamples(task));
    }
    else return invalidContract();
    return 0;
}

int32 DAQmxGetWriteSpaceAvail(TaskHandle taskHandle, uInt32* data)
{
    record(fake_nidaqmx::Call::QueryWriteSpace);
    const int32 failure = consumeFailure(fake_nidaqmx::Operation::QueryWriteSpace);
    if (failure != 0) return failure;
    FakeTask* task = taskFrom(taskHandle);
    if ((task == nullptr || (task->kind != TaskKind::AnalogOutput &&
                             task->kind != TaskKind::DigitalOutput)) || data == nullptr) {
        return invalidContract();
    }
    *data = task->outputBufferSamples == 0
        ? state.writeSpaceAvailable
        : std::min(task->outputBufferSamples, state.writeSpaceAvailable);
    return 0;
}

int32 DAQmxGetExtendedErrorInfo(char errorString[], uInt32 bufferSize)
{
    const char* text = state.errorMessage.empty()
        ? "fake NI-DAQmx extended error"
        : state.errorMessage.c_str();
    return copyText(text, errorString, bufferSize);
}

int32 DAQmxGetErrorString(int32 errorCode, char errorString[], uInt32 bufferSize)
{
    char text[128]{};
    std::snprintf(text, sizeof(text), "fake NI-DAQmx error %d", errorCode);
    return copyText(text, errorString, bufferSize);
}

int32 DAQmxResetDevice(const char*)
{
    ++state.resets;
    return 0;
}

} // extern "C"
