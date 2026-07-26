#include "fake/NIDAQmx.h"

#include "fake_nidaqmx_control.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct FakeTask {
    bool output = false;
    bool started = false;
    std::string physicalChannel;
};

struct FakeState {
    int createDo = 0;
    int createDi = 0;
    int starts = 0;
    int stops = 0;
    int clears = 0;
    int writes = 0;
    int resets = 0;
    uInt32 inputMask = 0;
    uInt32 lastMask = 0;
    int32 nextWriteError = 0;
    std::string errorMessage;
    std::string lastDoPhysicalChannel;
    std::string lastDiPhysicalChannel;
    bool ioContractValid = true;
};

FakeState state;

int32 copyText(const char* source, char* output, uInt32 capacity)
{
    if (source == nullptr) return -1;
    const std::size_t required = std::strlen(source) + 1;
    if (output == nullptr || capacity == 0) {
        return static_cast<int32>(required);
    }
    if (capacity < required) return -2;
    std::memcpy(output, source, required);
    return 0;
}

} // namespace

namespace fake_nidaqmx {

void reset()
{
    state = FakeState{};
}

void setInputMask(std::uint32_t mask)
{
    state.inputMask = mask;
}

void failNextWrite(std::int32_t code, const char* message)
{
    state.nextWriteError = code;
    state.errorMessage = message == nullptr ? "fake NI-DAQmx error" : message;
}

int createDoCalls() { return state.createDo; }
int createDiCalls() { return state.createDi; }
int startCalls() { return state.starts; }
int stopCalls() { return state.stops; }
int clearCalls() { return state.clears; }
int writeCalls() { return state.writes; }
int resetDeviceCalls() { return state.resets; }
std::uint32_t lastWrittenMask() { return state.lastMask; }
const char* lastDoPhysicalChannel() { return state.lastDoPhysicalChannel.c_str(); }
const char* lastDiPhysicalChannel() { return state.lastDiPhysicalChannel.c_str(); }
bool ioContractValid() { return state.ioContractValid; }

} // namespace fake_nidaqmx

extern "C" {

int32 DAQmxGetSysDevNames(char data[], uInt32 bufferSize)
{
    return copyText("DevFixture", data, bufferSize);
}

int32 DAQmxGetDevProductType(const char device[], char data[], uInt32 bufferSize)
{
    if (device == nullptr || std::strcmp(device, "DevFixture") != 0) return -200220;
    return copyText("USB-6259", data, bufferSize);
}

int32 DAQmxGetDevSerialNum(const char device[], uInt32* data)
{
    if (device == nullptr || data == nullptr ||
        std::strcmp(device, "DevFixture") != 0) {
        return -200220;
    }
    *data = 62590001u;
    return 0;
}

int32 DAQmxCreateTask(const char*, TaskHandle* taskHandle)
{
    if (taskHandle == nullptr) return -1;
    *taskHandle = new FakeTask;
    return 0;
}

int32 DAQmxCreateDOChan(TaskHandle taskHandle,
                        const char lines[],
                        const char*,
                        int32 lineGrouping)
{
    if (taskHandle == nullptr || lines == nullptr) return -1;
    if (lineGrouping != DAQmx_Val_ChanForAllLines) {
        state.ioContractValid = false;
        return -1;
    }
    auto* task = static_cast<FakeTask*>(taskHandle);
    task->output = true;
    task->physicalChannel = lines;
    state.lastDoPhysicalChannel = lines;
    ++state.createDo;
    return 0;
}

int32 DAQmxCreateDIChan(TaskHandle taskHandle,
                        const char lines[],
                        const char*,
                        int32 lineGrouping)
{
    if (taskHandle == nullptr || lines == nullptr) return -1;
    if (lineGrouping != DAQmx_Val_ChanForAllLines) {
        state.ioContractValid = false;
        return -1;
    }
    auto* task = static_cast<FakeTask*>(taskHandle);
    task->output = false;
    task->physicalChannel = lines;
    state.lastDiPhysicalChannel = lines;
    ++state.createDi;
    return 0;
}

int32 DAQmxStartTask(TaskHandle taskHandle)
{
    if (taskHandle == nullptr) return -1;
    static_cast<FakeTask*>(taskHandle)->started = true;
    ++state.starts;
    return 0;
}

int32 DAQmxStopTask(TaskHandle taskHandle)
{
    if (taskHandle == nullptr) return -1;
    static_cast<FakeTask*>(taskHandle)->started = false;
    ++state.stops;
    return 0;
}

int32 DAQmxClearTask(TaskHandle taskHandle)
{
    if (taskHandle == nullptr) return -1;
    delete static_cast<FakeTask*>(taskHandle);
    ++state.clears;
    return 0;
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
    const auto* task = static_cast<FakeTask*>(taskHandle);
    if (taskHandle == nullptr || numSampsPerChan != 1 || writeArray == nullptr ||
        task == nullptr || !task->output || !task->started || autoStart != 0 ||
        dataLayout != DAQmx_Val_GroupByChannel) {
        state.ioContractValid = false;
        return -1;
    }
    ++state.writes;
    if (state.nextWriteError < 0) {
        const int32 result = state.nextWriteError;
        state.nextWriteError = 0;
        return result;
    }
    state.lastMask = writeArray[0];
    if (sampsPerChanWritten != nullptr) *sampsPerChanWritten = 1;
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
    const auto* task = static_cast<FakeTask*>(taskHandle);
    if (taskHandle == nullptr || numSampsPerChan != 1 ||
        readArray == nullptr || arraySizeInSamps < 1) {
        state.ioContractValid = false;
        return -1;
    }
    if (task == nullptr || task->output || !task->started ||
        fillMode != DAQmx_Val_GroupByChannel) {
        state.ioContractValid = false;
        return -1;
    }
    readArray[0] = state.inputMask;
    if (sampsPerChanRead != nullptr) *sampsPerChanRead = 1;
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
