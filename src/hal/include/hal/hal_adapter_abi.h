#pragma once

#include "hal_global.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_ADAPTER_ABI_VERSION 1
#define HAL_ADAPTER_MAX_TEXT 256
#define HAL_ADAPTER_MAX_ID 128

#ifdef _MSC_VER
#define HAL_ADAPTER_CALL __stdcall
#else
#define HAL_ADAPTER_CALL
#endif

typedef void* HalAdapterHandle;
typedef void* HalAdapterDeviceHandle;

typedef enum HalAdapterStatusCode {
    HAL_ADAPTER_OK = 0,
    HAL_ADAPTER_INVALID_ARGUMENT = 1,
    HAL_ADAPTER_NOT_FOUND = 2,
    HAL_ADAPTER_NOT_SUPPORTED = 3,
    HAL_ADAPTER_BUSY = 4,
    HAL_ADAPTER_TIMEOUT = 5,
    HAL_ADAPTER_IO_ERROR = 6,
    HAL_ADAPTER_PROTOCOL_ERROR = 7,
    HAL_ADAPTER_DEVICE_DISCONNECTED = 8,
    HAL_ADAPTER_BUFFER_TOO_SMALL = 9,
    HAL_ADAPTER_INTERNAL_ERROR = 100
} HalAdapterStatusCode;

typedef struct HalAdapterStatus {
    int code;
    int vendorCode;
    char message[HAL_ADAPTER_MAX_TEXT];
} HalAdapterStatus;

typedef void (HAL_ADAPTER_CALL *HalAdapterLogFn)(int level,
                                                 const char* category,
                                                 const char* message,
                                                 const char* jsonContext);

typedef long long (HAL_ADAPTER_CALL *HalAdapterNowUsFn)();

typedef struct HalAdapterHostApiV1 {
    int abiVersion;
    HalAdapterLogFn log;
    HalAdapterNowUsFn nowUs;
} HalAdapterHostApiV1;

typedef struct HalAdapterInfo {
    char adapterId[HAL_ADAPTER_MAX_ID];
    char vendor[HAL_ADAPTER_MAX_TEXT];
    char name[HAL_ADAPTER_MAX_TEXT];
    char version[HAL_ADAPTER_MAX_TEXT];
    unsigned int supportedModulesMask;
    unsigned int flags;
} HalAdapterInfo;

typedef struct HalAdapterDeviceInfo {
    char deviceId[HAL_ADAPTER_MAX_ID];
    char model[HAL_ADAPTER_MAX_TEXT];
    char serialNumber[HAL_ADAPTER_MAX_TEXT];
    char location[HAL_ADAPTER_MAX_TEXT];
    char firmwareVersion[HAL_ADAPTER_MAX_TEXT];
    unsigned int supportedModulesMask;
    char propertiesJson[2048];
} HalAdapterDeviceInfo;

#define HAL_MODULE_ANALOG     0x00000001u
#define HAL_MODULE_DIGITAL    0x00000002u
#define HAL_MODULE_SERIAL     0x00000004u
#define HAL_MODULE_CANFD      0x00000008u

typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterGetInfoFn)(HalAdapterInfo* outInfo);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterInitializeFn)(const char* configJson,
                                                                    HalAdapterHandle* outHandle);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterShutdownFn)(HalAdapterHandle handle);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterEnumerateFn)(HalAdapterHandle handle,
                                                                    HalAdapterDeviceInfo* outDevices,
                                                                    int* inoutCount,
                                                                    int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterOpenDeviceFn)(HalAdapterHandle handle,
                                                                    const char* deviceId,
                                                                    const char* openOptionsJson,
                                                                    HalAdapterDeviceHandle* outDevice);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterCloseDeviceFn)(HalAdapterDeviceHandle device);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterResetDeviceFn)(HalAdapterDeviceHandle device,
                                                                     int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterGetCapabilitiesFn)(HalAdapterDeviceHandle device,
                                                                         char* outJson,
                                                                         int* inoutBytes,
                                                                         int timeoutMs);
typedef struct HalAdapterAnalogRange {
    double minValue;
    double maxValue;
    int unit;
} HalAdapterAnalogRange;

typedef struct HalAdapterAnalogSample {
    int channelIndex;
    double value;
    int unit;
    int rawCount;
    long long timestampUs;
    int statusFlags;
} HalAdapterAnalogSample;

typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterAnalogConfigureFn)(HalAdapterDeviceHandle device,
                                                                         int channelIndex,
                                                                         const HalAdapterAnalogRange* range,
                                                                         int isOutput,
                                                                         int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterAnalogReadFn)(HalAdapterDeviceHandle device,
                                                                    const int* channelIndexes,
                                                                    int channelCount,
                                                                    HalAdapterAnalogSample* outSamples,
                                                                    int sampleCountPerChannel,
                                                                    int sampleRateHz,
                                                                    int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterAnalogWriteFn)(HalAdapterDeviceHandle device,
                                                                     const int* channelIndexes,
                                                                     const double* values,
                                                                     int channelCount,
                                                                     int unit,
                                                                     int timeoutMs);
typedef struct HalAdapterDigitalSample {
    int channelIndex;
    int level;
    long long timestampUs;
    int statusFlags;
} HalAdapterDigitalSample;
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterDigitalReadFn)(HalAdapterDeviceHandle device,
                                                                     const int* channelIndexes,
                                                                     int channelCount,
                                                                     HalAdapterDigitalSample* outSamples,
                                                                     int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterDigitalWriteFn)(HalAdapterDeviceHandle device,
                                                                      const int* channelIndexes,
                                                                      const int* levels,
                                                                      int channelCount,
                                                                      int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterDigitalWaitEdgeFn)(HalAdapterDeviceHandle device,
                                                                         int channelIndex,
                                                                         int targetLevel,
                                                                         HalAdapterDigitalSample* outSample,
                                                                         int timeoutMs);
typedef struct HalAdapterSerialConfig {
    int baudRate;
    int dataBits;
    int parity;
    int stopBits;
    int flowControl;
    char optionsJson[1024];
} HalAdapterSerialConfig;
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterSerialOpenFn)(HalAdapterDeviceHandle device,
                                                                    int portIndex,
                                                                    const HalAdapterSerialConfig* config,
                                                                    int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterSerialCloseFn)(HalAdapterDeviceHandle device,
                                                                     int portIndex,
                                                                     int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterSerialWriteFn)(HalAdapterDeviceHandle device,
                                                                     int portIndex,
                                                                     const unsigned char* data,
                                                                     int bytes,
                                                                     int* outWritten,
                                                                     int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterSerialReadFn)(HalAdapterDeviceHandle device,
                                                                    int portIndex,
                                                                    unsigned char* outData,
                                                                    int* inoutBytes,
                                                                    int timeoutMs);
typedef struct HalAdapterCanFdConfig {
    int nominalBitrate;
    int dataBitrate;
    int fdEnabled;
    int bitrateSwitch;
    int loopback;
    char optionsJson[1024];
} HalAdapterCanFdConfig;
typedef struct HalAdapterCanFdFrame {
    unsigned int id;
    int extendedId;
    int fd;
    int bitrateSwitch;
    int remoteRequest;
    unsigned char payload[64];
    int payloadSize;
    long long timestampUs;
    int statusFlags;
} HalAdapterCanFdFrame;
typedef struct HalAdapterCanFdFilter {
    unsigned int id;
    unsigned int mask;
    int extendedId;
} HalAdapterCanFdFilter;
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterCanOpenFn)(HalAdapterDeviceHandle device,
                                                                 int busIndex,
                                                                 const HalAdapterCanFdConfig* config,
                                                                 int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterCanCloseFn)(HalAdapterDeviceHandle device,
                                                                  int busIndex,
                                                                  int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterCanSetFiltersFn)(HalAdapterDeviceHandle device,
                                                                        int busIndex,
                                                                        const HalAdapterCanFdFilter* filters,
                                                                        int filterCount,
                                                                        int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterCanSendFn)(HalAdapterDeviceHandle device,
                                                                 int busIndex,
                                                                 const HalAdapterCanFdFrame* frame,
                                                                 int timeoutMs);
typedef HalAdapterStatus (HAL_ADAPTER_CALL *HalAdapterCanReceiveFn)(HalAdapterDeviceHandle device,
                                                                    int busIndex,
                                                                    HalAdapterCanFdFrame* outFrames,
                                                                    int* inoutFrameCount,
                                                                    int timeoutMs);
typedef struct HalAdapterApiV1 {
    int abiVersion;
    int structSize;

    HalAdapterGetInfoFn getInfo;
    HalAdapterInitializeFn initialize;
    HalAdapterShutdownFn shutdown;
    HalAdapterEnumerateFn enumerateDevices;

    HalAdapterOpenDeviceFn openDevice;
    HalAdapterCloseDeviceFn closeDevice;
    HalAdapterResetDeviceFn resetDevice;
    HalAdapterGetCapabilitiesFn getCapabilities;

    HalAdapterAnalogConfigureFn analogConfigure;
    HalAdapterAnalogReadFn analogRead;
    HalAdapterAnalogWriteFn analogWrite;

    HalAdapterDigitalReadFn digitalRead;
    HalAdapterDigitalWriteFn digitalWrite;
    HalAdapterDigitalWaitEdgeFn digitalWaitEdge;

    HalAdapterSerialOpenFn serialOpen;
    HalAdapterSerialCloseFn serialClose;
    HalAdapterSerialWriteFn serialWrite;
    HalAdapterSerialReadFn serialRead;

    HalAdapterCanOpenFn canOpen;
    HalAdapterCanCloseFn canClose;
    HalAdapterCanSetFiltersFn canSetFilters;
    HalAdapterCanSendFn canSend;
    HalAdapterCanReceiveFn canReceive;
} HalAdapterApiV1;

typedef int (HAL_ADAPTER_CALL *HalAdapterGetApiV1Fn)(const HalAdapterHostApiV1* host,
                                                     HalAdapterApiV1* outApi);

int HAL_ADAPTER_CALL hal_adapter_get_api_v1(const HalAdapterHostApiV1* host,
                                            HalAdapterApiV1* outApi);

#ifdef __cplusplus
}
#endif
