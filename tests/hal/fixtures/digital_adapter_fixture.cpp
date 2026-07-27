#include "hal/hal_adapter_abi.h"
#include "hal/hal_adapter_task_abi.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <set>
#include <thread>

namespace {

struct AdapterState {};

struct DeviceState {
    std::array<int, 64> levels{};
    bool failClose = false;
    bool slowWrite = false;
    std::atomic<int> activeWrites{0};
};

struct SampleTaskState {
    DeviceState* device = nullptr;
    int kind = HAL_ADAPTER_TASK_ANALOG_INPUT;
    int channelCount = 0;
    bool started = false;
    long long totalSamples = 0;
};

std::mutex closedDevicesMutex;
std::set<const void*> closedDevices;
std::atomic<bool> writeEntered{false};

HalAdapterStatus status(int code = HAL_ADAPTER_OK,
                        int vendorCode = 0,
                        const char* message = "")
{
    HalAdapterStatus result{};
    result.code = code;
    result.vendorCode = vendorCode;
    std::snprintf(result.message, sizeof(result.message), "%s", message);
    return result;
}

HalAdapterStatus HAL_ADAPTER_CALL getInfo(HalAdapterInfo* output)
{
    if (output == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    *output = HalAdapterInfo{};
    std::snprintf(output->adapterId, sizeof(output->adapterId), "fixture.digital.v1");
    std::snprintf(output->vendor, sizeof(output->vendor), "FixtureVendor");
    std::snprintf(output->name, sizeof(output->name), "Digital fixture");
    std::snprintf(output->version, sizeof(output->version), "1.0");
    output->supportedModulesMask = HAL_MODULE_DIGITAL;
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL initialize(const char* configJson,
                                             HalAdapterHandle* output)
{
    if (output == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    if (configJson == nullptr || std::strstr(configJson, "\"hardware\"") != nullptr ||
        std::strstr(configJson, "\"safeState\"") != nullptr) {
        return status(HAL_ADAPTER_INVALID_ARGUMENT,
                      0,
                      "adapter initialization must contain driver settings only");
    }
    *output = new (std::nothrow) AdapterState;
    return *output == nullptr ? status(HAL_ADAPTER_INTERNAL_ERROR) : status();
}

HalAdapterStatus HAL_ADAPTER_CALL shutdown(HalAdapterHandle handle)
{
    delete static_cast<AdapterState*>(handle);
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL enumerate(HalAdapterHandle handle,
                                            HalAdapterDeviceInfo* output,
                                            int* count,
                                            int)
{
    if (handle == nullptr || count == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    if (output == nullptr || *count < 1) {
        *count = 1;
        return status();
    }
    output[0] = HalAdapterDeviceInfo{};
    std::snprintf(output[0].deviceId, sizeof(output[0].deviceId), "fixture_device");
    std::snprintf(output[0].model, sizeof(output[0].model), "Fixture-Digital");
    std::snprintf(output[0].serialNumber, sizeof(output[0].serialNumber), "62590001");
    output[0].supportedModulesMask = HAL_MODULE_DIGITAL;
    std::snprintf(output[0].propertiesJson, sizeof(output[0].propertiesJson), "{\"fixture\":true}");
    *count = 1;
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL openDevice(HalAdapterHandle handle,
                                             const char* deviceId,
                                             const char* optionsJson,
                                             HalAdapterDeviceHandle* output)
{
    if (handle == nullptr || output == nullptr || deviceId == nullptr ||
        std::strcmp(deviceId, "fixture_device") != 0) {
        return status(HAL_ADAPTER_NOT_FOUND, 0, "fixture device not found");
    }
    auto* device = new (std::nothrow) DeviceState;
    if (device != nullptr) {
        device->failClose = optionsJson != nullptr &&
            std::strstr(optionsJson, "\"failClose\":true") != nullptr;
        device->slowWrite = optionsJson != nullptr &&
            std::strstr(optionsJson, "\"slowWrite\":true") != nullptr;
        writeEntered.store(false);
        std::lock_guard<std::mutex> lock(closedDevicesMutex);
        closedDevices.erase(device);
    }
    *output = device;
    return *output == nullptr ? status(HAL_ADAPTER_INTERNAL_ERROR) : status();
}

HalAdapterStatus HAL_ADAPTER_CALL closeDevice(HalAdapterDeviceHandle device)
{
    if (device == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    if (static_cast<DeviceState*>(device)->activeWrites.load() != 0) {
        return status(HAL_ADAPTER_BUSY, 0, "close raced with a digital write");
    }
    {
        std::lock_guard<std::mutex> lock(closedDevicesMutex);
        if (!closedDevices.insert(device).second) {
            return status(HAL_ADAPTER_INTERNAL_ERROR, 0, "device handle closed twice");
        }
    }
    const bool failClose = static_cast<DeviceState*>(device)->failClose;
    delete static_cast<DeviceState*>(device);
    return failClose
        ? status(HAL_ADAPTER_IO_ERROR, 6259, "fixture close failed after consuming handle")
        : status();
}

HalAdapterStatus HAL_ADAPTER_CALL resetDevice(HalAdapterDeviceHandle device, int)
{
    if (device == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    static_cast<DeviceState*>(device)->levels.fill(0);
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL capabilities(HalAdapterDeviceHandle device,
                                               char* output,
                                               int* bytes,
                                               int)
{
    static constexpr char json[] = "{\"supportedModules\":[\"digital\"]}";
    if (device == nullptr || bytes == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    const int required = static_cast<int>(sizeof(json));
    if (output == nullptr || *bytes < required) {
        *bytes = required;
        return status(HAL_ADAPTER_BUFFER_TOO_SMALL);
    }
    std::memcpy(output, json, sizeof(json));
    *bytes = required;
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL digitalRead(HalAdapterDeviceHandle device,
                                              const int* indexes,
                                              int count,
                                              HalAdapterDigitalSample* output,
                                              int)
{
    if (device == nullptr || indexes == nullptr || count < 0 || output == nullptr) {
        return status(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    auto* state = static_cast<DeviceState*>(device);
    for (int i = 0; i < count; ++i) {
        if (indexes[i] < 0 || indexes[i] >= static_cast<int>(state->levels.size())) {
            return status(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        output[i] = HalAdapterDigitalSample{indexes[i], state->levels[indexes[i]], 123456 + i, 0};
    }
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL digitalWrite(HalAdapterDeviceHandle device,
                                               const int* indexes,
                                               const int* levels,
                                               int count,
                                               int)
{
    if (device == nullptr || indexes == nullptr || levels == nullptr || count < 0) {
        return status(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    auto* state = static_cast<DeviceState*>(device);
    ++state->activeWrites;
    struct ActiveWriteReset {
        std::atomic<int>* active = nullptr;
        ~ActiveWriteReset() { --(*active); }
    } activeReset{&state->activeWrites};
    if (state->slowWrite) {
        writeEntered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (int i = 0; i < count; ++i) {
        if (indexes[i] == 63) return status(HAL_ADAPTER_TIMEOUT, 6259, "fixture timeout");
        if (indexes[i] < 0 || indexes[i] >= static_cast<int>(state->levels.size())) {
            return status(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        state->levels[indexes[i]] = levels[i];
    }
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL createTask(HalAdapterDeviceHandle device,
                                             const HalAdapterTaskConfig* config,
                                             HalAdapterTaskHandle* output)
{
    if (device == nullptr || config == nullptr || output == nullptr ||
        config->structSize < static_cast<int>(sizeof(HalAdapterTaskConfig)) ||
        config->channelIndexes == nullptr || config->channelCount <= 0) {
        return status(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    auto* task = new (std::nothrow) SampleTaskState;
    if (task == nullptr) return status(HAL_ADAPTER_INTERNAL_ERROR);
    task->device = static_cast<DeviceState*>(device);
    task->kind = config->kind;
    task->channelCount = config->channelCount;
    *output = task;
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL startTask(HalAdapterTaskHandle handle, int)
{
    auto* task = static_cast<SampleTaskState*>(handle);
    if (task == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    task->started = true;
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL readTask(HalAdapterTaskHandle handle,
                                           HalAdapterTaskBuffer* buffer,
                                           int)
{
    auto* task = static_cast<SampleTaskState*>(handle);
    if (task == nullptr || buffer == nullptr || !task->started ||
        buffer->sampleType != HAL_ADAPTER_SAMPLE_FLOAT64 ||
        buffer->data == nullptr || buffer->samplesPerChannel <= 0) {
        return status(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    const int required = task->channelCount * buffer->samplesPerChannel;
    if (buffer->capacityValues < required) return status(HAL_ADAPTER_BUFFER_TOO_SMALL);
    auto* values = static_cast<double*>(buffer->data);
    for (int channel = 0; channel < task->channelCount; ++channel) {
        for (int sample = 0; sample < buffer->samplesPerChannel; ++sample) {
            values[channel * buffer->samplesPerChannel + sample] =
                static_cast<double>(channel * 10 + sample + 1);
        }
    }
    task->totalSamples += buffer->samplesPerChannel;
    buffer->channelCount = task->channelCount;
    buffer->timestampUs = 987654;
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL writeTask(HalAdapterTaskHandle handle,
                                            const HalAdapterTaskBuffer* buffer,
                                            int,
                                            int)
{
    if (handle == nullptr || buffer == nullptr || buffer->data == nullptr) {
        return status(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL getTaskStatus(HalAdapterTaskHandle handle,
                                                HalAdapterTaskStatusInfo* output,
                                                int)
{
    auto* task = static_cast<SampleTaskState*>(handle);
    if (task == nullptr || output == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    *output = HalAdapterTaskStatusInfo{};
    output->started = task->started ? 1 : 0;
    output->availableSamplesPerChannel = 4;
    output->totalSamplesPerChannel = task->totalSamples;
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL stopTask(HalAdapterTaskHandle handle, int)
{
    auto* task = static_cast<SampleTaskState*>(handle);
    if (task == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    task->started = false;
    return status();
}

HalAdapterStatus HAL_ADAPTER_CALL closeTask(HalAdapterTaskHandle handle)
{
    if (handle == nullptr) return status(HAL_ADAPTER_INVALID_ARGUMENT);
    delete static_cast<SampleTaskState*>(handle);
    return status();
}

} // namespace

extern "C" int HAL_ADAPTER_CALL hal_adapter_get_api_v1(const HalAdapterHostApiV1* host,
                                                       HalAdapterApiV1* output)
{
    if (host == nullptr || output == nullptr || host->abiVersion != HAL_ADAPTER_ABI_VERSION) {
        return -1;
    }
    *output = HalAdapterApiV1{};
    output->abiVersion = HAL_ADAPTER_ABI_VERSION;
    output->structSize = static_cast<int>(sizeof(HalAdapterApiV1));
    output->getInfo = &getInfo;
    output->initialize = &initialize;
    output->shutdown = &shutdown;
    output->enumerateDevices = &enumerate;
    output->openDevice = &openDevice;
    output->closeDevice = &closeDevice;
    output->resetDevice = &resetDevice;
    output->getCapabilities = &capabilities;
    output->digitalRead = &digitalRead;
    output->digitalWrite = &digitalWrite;
    return 0;
}

extern "C" int HAL_ADAPTER_CALL hal_adapter_get_task_api_v1(
    const HalAdapterHostApiV1* host,
    HalAdapterTaskApiV1* output)
{
    if (host == nullptr || output == nullptr ||
        host->abiVersion != HAL_ADAPTER_ABI_VERSION) {
        return -1;
    }
    *output = HalAdapterTaskApiV1{};
    output->abiVersion = HAL_ADAPTER_TASK_ABI_VERSION;
    output->structSize = static_cast<int>(sizeof(HalAdapterTaskApiV1));
    output->createTask = &createTask;
    output->startTask = &startTask;
    output->readTask = &readTask;
    output->writeTask = &writeTask;
    output->getTaskStatus = &getTaskStatus;
    output->stopTask = &stopTask;
    output->closeTask = &closeTask;
    return 0;
}

extern "C" int HAL_ADAPTER_CALL hal_fixture_write_entered()
{
    return writeEntered.load() ? 1 : 0;
}
