#pragma once

#include <cstdint>

namespace fake_nidaqmx {

enum class Operation {
    CreateTask,
    CreateDigitalOutputChannel,
    CreateDigitalInputChannel,
    CreateAnalogInputChannel,
    CreateAnalogOutputChannel,
    CreateCounterInputChannel,
    CreateCounterOutputChannel,
    ConfigureSampleClock,
    ConfigureImplicitTiming,
    ConfigureInputBuffer,
    ConfigureOutputBuffer,
    ConfigureDigitalStartTrigger,
    ConfigureAnalogStartTrigger,
    ConfigureDigitalReferenceTrigger,
    ConfigureAnalogReferenceTrigger,
    ConfigurePauseTrigger,
    ConnectTerms,
    DisconnectTerms,
    StartTask,
    StopTask,
    ClearTask,
    ReadDigital,
    WriteDigital,
    ReadAnalog,
    WriteAnalog,
    ReadCounter,
    QueryTaskDone,
    QueryReadAvailable,
    QueryWriteSpace
};

enum class Call {
    CreateTask,
    CreateDigitalOutputChannel,
    CreateDigitalInputChannel,
    CreateAnalogInputChannel,
    CreateAnalogOutputChannel,
    CreateCounterInputChannel,
    CreateCounterOutputChannel,
    ConfigureSampleClock,
    ConfigureImplicitTiming,
    ConfigureInputBuffer,
    ConfigureOutputBuffer,
    ConfigureDigitalStartTrigger,
    ConfigureAnalogStartTrigger,
    ConfigureDigitalReferenceTrigger,
    ConfigureAnalogReferenceTrigger,
    ConfigurePauseTrigger,
    ConnectTerms,
    DisconnectTerms,
    StartTask,
    StopTask,
    ClearTask,
    ReadDigital,
    WriteDigital,
    ReadAnalog,
    WriteAnalog,
    ReadCounter,
    QueryTaskDone,
    QueryReadAvailable,
    QueryWriteSpace
};

void reset();
void setDeviceIdentity(const char* deviceName,
                       const char* productType,
                       std::uint32_t serialNumber);
void setInputMask(std::uint32_t mask);
void setDigitalInputBlock(const std::uint32_t* values, int samplesPerChannel);
void setDigitalInputChannelsBlock(const std::uint32_t* values,
                                  int channelCount,
                                  int samplesPerChannel);
void setAnalogInputBlock(const double* values,
                         int channelCount,
                         int samplesPerChannel);
void setCounterInputSamples(const std::uint32_t* values, int sampleCount);
void setNextDigitalReadSamplesPerChannel(int samplesPerChannel);
void setNextAnalogReadSamplesPerChannel(int samplesPerChannel);
void setNextCounterReadSamples(int samples);
void setWriteSpaceAvailable(std::uint32_t samplesPerChannel);
void failNext(Operation operation, std::int32_t code, const char* message);
void failNextWrite(std::int32_t code, const char* message);
void clearCallLog();

int createDoCalls();
int createDiCalls();
int createAiCalls();
int createAoCalls();
int createCounterInputCalls();
int createCounterOutputCalls();
int startCalls();
int stopCalls();
int clearCalls();
int writeCalls();
int readAnalogCalls();
int writeAnalogCalls();
int readCounterCalls();
int configureSampleClockCalls();
int configureDigitalStartTriggerCalls();
int configureAnalogStartTriggerCalls();
int configureDigitalReferenceTriggerCalls();
int configureAnalogReferenceTriggerCalls();
int configurePauseTriggerCalls();
int resetDeviceCalls();
std::uint32_t lastWrittenMask();
int lastAnalogTerminalConfig();
double lastAnalogMinimum();
double lastAnalogMaximum();
const char* lastSampleClockSource();
double lastSampleClockRate();
int lastSampleMode();
std::uint64_t lastSamplesPerChannel();
int lastAnalogOutputCount();
double lastAnalogOutputAt(int index);
int lastDigitalOutputCount();
std::uint32_t lastDigitalOutputAt(int index);
const char* lastDoPhysicalChannel();
const char* lastDiPhysicalChannel();
const char* lastAiPhysicalChannel();
const char* lastAoPhysicalChannel();
const char* lastCounterPhysicalChannel();
bool ioContractValid();
int firstCallIndex(Call call);
int lastCallIndex(Call call);
bool callsContainInOrder(const Call* expected, int count);

} // namespace fake_nidaqmx
