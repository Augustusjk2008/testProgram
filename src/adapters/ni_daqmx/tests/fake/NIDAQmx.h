#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef std::int32_t int32;
typedef std::uint32_t uInt32;
typedef std::uint64_t uInt64;
typedef double float64;
typedef uInt32 bool32;
typedef void* TaskHandle;

#define DAQmx_Val_ChanForAllLines 1
#define DAQmx_Val_GroupByChannel 0
#define DAQmx_Val_GroupByScanNumber 1
#define DAQmx_Val_Cfg_Default -1
#define DAQmx_Val_RSE 10083
#define DAQmx_Val_NRSE 10078
#define DAQmx_Val_Diff 10106
#define DAQmx_Val_Volts 10348
#define DAQmx_Val_Hz 10373
#define DAQmx_Val_Rising 10280
#define DAQmx_Val_Falling 10171
#define DAQmx_Val_FiniteSamps 10178
#define DAQmx_Val_ContSamps 10123
#define DAQmx_Val_Low 10214
#define DAQmx_Val_High 10192
#define DAQmx_Val_CountUp 10128
#define DAQmx_Val_DigLvl 10152
#define DAQmx_Val_AnlgLvl 10101
#define DAQmx_Val_AboveLvl 10093
#define DAQmx_Val_BelowLvl 10107

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
int32 DAQmxCreateAIVoltageChan(TaskHandle taskHandle,
                               const char physicalChannel[],
                               const char nameToAssignToChannel[],
                               int32 terminalConfig,
                               float64 minVal,
                               float64 maxVal,
                               int32 units,
                               const char customScaleName[]);
int32 DAQmxCreateAOVoltageChan(TaskHandle taskHandle,
                               const char physicalChannel[],
                               const char nameToAssignToChannel[],
                               float64 minVal,
                               float64 maxVal,
                               int32 units,
                               const char customScaleName[]);
int32 DAQmxCreateCICountEdgesChan(TaskHandle taskHandle,
                                  const char counter[],
                                  const char nameToAssignToChannel[],
                                  int32 edge,
                                  uInt32 initialCount,
                                  int32 countDirection);
int32 DAQmxCreateCOPulseChanFreq(TaskHandle taskHandle,
                                 const char counter[],
                                 const char nameToAssignToChannel[],
                                 int32 units,
                                 int32 idleState,
                                 float64 initialDelay,
                                 float64 frequency,
                                 float64 dutyCycle);
int32 DAQmxCfgSampClkTiming(TaskHandle taskHandle,
                            const char source[],
                            float64 rate,
                            int32 activeEdge,
                            int32 sampleMode,
                            uInt64 sampsPerChanToAcquire);
int32 DAQmxCfgImplicitTiming(TaskHandle taskHandle,
                             int32 sampleMode,
                             uInt64 sampsPerChanToAcquire);
int32 DAQmxCfgInputBuffer(TaskHandle taskHandle, uInt32 numSampsPerChan);
int32 DAQmxCfgOutputBuffer(TaskHandle taskHandle, uInt32 numSampsPerChan);
int32 DAQmxCfgDigEdgeStartTrig(TaskHandle taskHandle,
                               const char triggerSource[],
                               int32 triggerEdge);
int32 DAQmxCfgAnlgEdgeStartTrig(TaskHandle taskHandle,
                                const char triggerSource[],
                                int32 triggerSlope,
                                float64 triggerLevel);
int32 DAQmxCfgDigEdgeRefTrig(TaskHandle taskHandle,
                             const char triggerSource[],
                             int32 triggerEdge,
                             uInt32 pretriggerSamples);
int32 DAQmxCfgAnlgEdgeRefTrig(TaskHandle taskHandle,
                              const char triggerSource[],
                              int32 triggerSlope,
                              float64 triggerLevel,
                              uInt32 pretriggerSamples);
int32 DAQmxSetPauseTrigType(TaskHandle taskHandle, int32 data);
int32 DAQmxSetDigLvlPauseTrigSrc(TaskHandle taskHandle, const char data[]);
int32 DAQmxSetDigLvlPauseTrigWhen(TaskHandle taskHandle, int32 data);
int32 DAQmxSetAnlgLvlPauseTrigSrc(TaskHandle taskHandle, const char data[]);
int32 DAQmxSetAnlgLvlPauseTrigWhen(TaskHandle taskHandle, int32 data);
int32 DAQmxSetAnlgLvlPauseTrigLvl(TaskHandle taskHandle, float64 data);
int32 DAQmxConnectTerms(const char sourceTerminal[],
                        const char destinationTerminal[],
                        int32 signalModifiers);
int32 DAQmxDisconnectTerms(const char sourceTerminal[],
                           const char destinationTerminal[]);
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
int32 DAQmxWriteAnalogF64(TaskHandle taskHandle,
                          int32 numSampsPerChan,
                          bool32 autoStart,
                          float64 timeout,
                          bool32 dataLayout,
                          const float64 writeArray[],
                          int32* sampsPerChanWritten,
                          bool32* reserved);
int32 DAQmxReadAnalogF64(TaskHandle taskHandle,
                         int32 numSampsPerChan,
                         float64 timeout,
                         bool32 fillMode,
                         float64 readArray[],
                         uInt32 arraySizeInSamps,
                         int32* sampsPerChanRead,
                         bool32* reserved);
int32 DAQmxReadCounterU32(TaskHandle taskHandle,
                          int32 numSampsPerChan,
                          float64 timeout,
                          uInt32 readArray[],
                          uInt32 arraySizeInSamps,
                          int32* sampsPerChanRead,
                          bool32* reserved);
int32 DAQmxIsTaskDone(TaskHandle taskHandle, bool32* isTaskDone);
int32 DAQmxGetReadAvailSampPerChan(TaskHandle taskHandle,
                                   uInt32* data);
int32 DAQmxGetWriteSpaceAvail(TaskHandle taskHandle,
                              uInt32* data);
int32 DAQmxGetExtendedErrorInfo(char errorString[], uInt32 bufferSize);
int32 DAQmxGetErrorString(int32 errorCode, char errorString[], uInt32 bufferSize);
int32 DAQmxResetDevice(const char deviceName[]);

#ifdef __cplusplus
}
#endif
