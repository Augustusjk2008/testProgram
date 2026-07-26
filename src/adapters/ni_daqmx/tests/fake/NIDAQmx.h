#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef std::int32_t int32;
typedef std::uint32_t uInt32;
typedef double float64;
typedef uInt32 bool32;
typedef void* TaskHandle;

#define DAQmx_Val_ChanForAllLines 1
#define DAQmx_Val_GroupByChannel 0

int32 DAQmxGetSysDevNames(char data[], uInt32 bufferSize);
int32 DAQmxGetDevProductType(const char device[], char data[], uInt32 bufferSize);
int32 DAQmxGetDevSerialNum(const char device[], uInt32* data);
int32 DAQmxCreateTask(const char taskName[], TaskHandle* taskHandle);
int32 DAQmxCreateDOChan(TaskHandle taskHandle,
                        const char lines[],
                        const char nameToAssignToLines[],
                        int32 lineGrouping);
int32 DAQmxCreateDIChan(TaskHandle taskHandle,
                        const char lines[],
                        const char nameToAssignToLines[],
                        int32 lineGrouping);
int32 DAQmxStartTask(TaskHandle taskHandle);
int32 DAQmxStopTask(TaskHandle taskHandle);
int32 DAQmxClearTask(TaskHandle taskHandle);
int32 DAQmxWriteDigitalU32(TaskHandle taskHandle,
                           int32 numSampsPerChan,
                           bool32 autoStart,
                           float64 timeout,
                           bool32 dataLayout,
                           const uInt32 writeArray[],
                           int32* sampsPerChanWritten,
                           bool32* reserved);
int32 DAQmxReadDigitalU32(TaskHandle taskHandle,
                          int32 numSampsPerChan,
                          float64 timeout,
                          bool32 fillMode,
                          uInt32 readArray[],
                          uInt32 arraySizeInSamps,
                          int32* sampsPerChanRead,
                          bool32* reserved);
int32 DAQmxGetExtendedErrorInfo(char errorString[], uInt32 bufferSize);
int32 DAQmxGetErrorString(int32 errorCode, char errorString[], uInt32 bufferSize);
int32 DAQmxResetDevice(const char deviceName[]);

#ifdef __cplusplus
}
#endif
