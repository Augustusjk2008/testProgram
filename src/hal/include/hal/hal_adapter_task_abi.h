#pragma once

#include "hal_adapter_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_ADAPTER_TASK_ABI_VERSION 1

typedef void* HalAdapterTaskHandle;

typedef enum HalAdapterTaskKind {
    HAL_ADAPTER_TASK_ANALOG_INPUT = 1,
    HAL_ADAPTER_TASK_ANALOG_OUTPUT = 2,
    HAL_ADAPTER_TASK_DIGITAL_INPUT = 3,
    HAL_ADAPTER_TASK_DIGITAL_OUTPUT = 4,
    HAL_ADAPTER_TASK_COUNTER_INPUT = 5,
    HAL_ADAPTER_TASK_COUNTER_OUTPUT = 6
} HalAdapterTaskKind;

typedef enum HalAdapterTaskMode {
    HAL_ADAPTER_TASK_ON_DEMAND = 0,
    HAL_ADAPTER_TASK_FINITE = 1,
    HAL_ADAPTER_TASK_CONTINUOUS = 2
} HalAdapterTaskMode;

typedef enum HalAdapterSampleType {
    HAL_ADAPTER_SAMPLE_FLOAT64 = 1,
    HAL_ADAPTER_SAMPLE_UINT32 = 2
} HalAdapterSampleType;

typedef enum HalAdapterSignalEdge {
    HAL_ADAPTER_EDGE_RISING = 0,
    HAL_ADAPTER_EDGE_FALLING = 1
} HalAdapterSignalEdge;

typedef enum HalAdapterTriggerType {
    HAL_ADAPTER_TRIGGER_NONE = 0,
    HAL_ADAPTER_TRIGGER_DIGITAL_EDGE = 1,
    HAL_ADAPTER_TRIGGER_ANALOG_EDGE = 2,
    HAL_ADAPTER_TRIGGER_DIGITAL_LEVEL = 3
} HalAdapterTriggerType;

typedef enum HalAdapterTriggerRole {
    HAL_ADAPTER_TRIGGER_START = 0,
    HAL_ADAPTER_TRIGGER_REFERENCE = 1,
    HAL_ADAPTER_TRIGGER_PAUSE = 2
} HalAdapterTriggerRole;

typedef enum HalAdapterCounterMode {
    HAL_ADAPTER_COUNTER_COUNT_EDGES = 1,
    HAL_ADAPTER_COUNTER_PULSE_FREQUENCY = 2
} HalAdapterCounterMode;

typedef struct HalAdapterTaskConfig {
    int structSize;
    int kind;
    int mode;
    const int* channelIndexes;
    int channelCount;
    double sampleRateHz;
    int samplesPerChannel;
    int bufferSamplesPerChannel;
    const char* sampleClockSource;
    int sampleClockEdge;
    int triggerType;
    int triggerRole;
    const char* triggerSource;
    int triggerEdge;
    double triggerLevel;
    int counterMode;
    double counterMinValue;
    double counterMaxValue;
    double counterFrequencyHz;
    double counterDutyCycle;
    int counterEdge;
    int referencePretriggerSamples;
} HalAdapterTaskConfig;

typedef struct HalAdapterTaskBuffer {
    int structSize;
    int sampleType;
    void* data;
    int capacityValues;
    int channelCount;
    int samplesPerChannel;
    long long timestampUs;
    int statusFlags;
} HalAdapterTaskBuffer;

typedef struct HalAdapterTaskStatusInfo {
    int started;
    int done;
    int availableSamplesPerChannel;
    long long totalSamplesPerChannel;
    int overflowed;
    int underflowed;
} HalAdapterTaskStatusInfo;

typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterTaskCreateFn)(
    HalAdapterDeviceHandle device,
    const HalAdapterTaskConfig* config,
    HalAdapterTaskHandle* outTask);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterTaskStartFn)(
    HalAdapterTaskHandle task,
    int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterTaskReadFn)(
    HalAdapterTaskHandle task,
    HalAdapterTaskBuffer* inoutBuffer,
    int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterTaskWriteFn)(
    HalAdapterTaskHandle task,
    const HalAdapterTaskBuffer* buffer,
    int autoStart,
    int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterTaskGetStatusFn)(
    HalAdapterTaskHandle task,
    HalAdapterTaskStatusInfo* outStatus,
    int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterTaskStopFn)(
    HalAdapterTaskHandle task,
    int timeoutMs);
/* A close call always consumes the task handle, including cleanup failures. */
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterTaskCloseFn)(
    HalAdapterTaskHandle task);

typedef struct HalAdapterTaskApiV1 {
    int abiVersion;
    int structSize;
    HalAdapterTaskCreateFn createTask;
    HalAdapterTaskStartFn startTask;
    HalAdapterTaskReadFn readTask;
    HalAdapterTaskWriteFn writeTask;
    HalAdapterTaskGetStatusFn getTaskStatus;
    HalAdapterTaskStopFn stopTask;
    HalAdapterTaskCloseFn closeTask;
} HalAdapterTaskApiV1;

typedef int (HAL_ADAPTER_CALL *HalAdapterGetTaskApiV1Fn)(
    const HalAdapterHostApiV1* host,
    HalAdapterTaskApiV1* outApi);

int HAL_ADAPTER_CALL hal_adapter_get_task_api_v1(
    const HalAdapterHostApiV1* host,
    HalAdapterTaskApiV1* outApi);

#ifdef __cplusplus
}
#endif
