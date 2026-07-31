#include "ni_daqmx_config.h"

#include "hal/hal_adapter_abi.h"
#include "hal/hal_adapter_task_abi.h"

#include <NIDAQmx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using hwtest::adapters::ni_daqmx::ChannelConfig;
using hwtest::adapters::ni_daqmx::ChannelDirection;
using hwtest::adapters::ni_daqmx::ChannelModule;
using hwtest::adapters::ni_daqmx::DeviceConfig;
using hwtest::adapters::ni_daqmx::DeviceModel;
using hwtest::adapters::ni_daqmx::DeviceProfile;
using hwtest::adapters::ni_daqmx::DriverConfig;
using hwtest::adapters::ni_daqmx::deviceProfile;
using hwtest::adapters::ni_daqmx::findDeviceProfile;

HalAdapterStatus makeStatus(int code = HAL_ADAPTER_OK,
                            int vendorCode = 0,
                            const std::string& message = {})
{
    HalAdapterStatus result{};
    result.code = code;
    result.vendorCode = vendorCode;
    std::snprintf(result.message, sizeof(result.message), "%s", message.c_str());
    return result;
}

HalAdapterStatus rememberFirst(HalAdapterStatus first, HalAdapterStatus next)
{
    return first.code == HAL_ADAPTER_OK && next.code != HAL_ADAPTER_OK ? next : first;
}

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> splitDeviceNames(const char* text)
{
    std::vector<std::string> result;
    std::stringstream stream(text == nullptr ? "" : text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim(item);
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

std::string daqErrorText(int32 code)
{
    char message[1024]{};
    if (DAQmxGetExtendedErrorInfo(message, static_cast<uInt32>(sizeof(message))) >= 0 &&
        message[0] != '\0') {
        return trim(message);
    }
    if (DAQmxGetErrorString(code, message, static_cast<uInt32>(sizeof(message))) >= 0 &&
        message[0] != '\0') {
        return trim(message);
    }
    return "NI-DAQmx error " + std::to_string(code);
}

HalAdapterStatus fromDaq(int32 code, const char* operation)
{
    if (code >= 0) return makeStatus();
    const std::string vendor = daqErrorText(code);
    const std::string normalized = lower(vendor);
    int mapped = HAL_ADAPTER_IO_ERROR;
    if (code == -200284 || normalized.find("timed out") != std::string::npos ||
        normalized.find("timeout") != std::string::npos) {
        mapped = HAL_ADAPTER_TIMEOUT;
    } else if (code == -88705 || code == -88708 ||
               normalized.find("removed") != std::string::npos ||
               normalized.find("disconnected") != std::string::npos ||
               normalized.find("not present") != std::string::npos) {
        mapped = HAL_ADAPTER_DEVICE_DISCONNECTED;
    } else if (code == -50103 || normalized.find("reserved") != std::string::npos) {
        mapped = HAL_ADAPTER_BUSY;
    }
    return makeStatus(mapped,
                      static_cast<int>(code),
                      std::string(operation == nullptr ? "NI-DAQmx" : operation) +
                          ": " + vendor);
}

bool serialMatches(const std::string& expected, uInt32 actual)
{
    const std::string text = trim(expected);
    if (text.empty()) return false;
    try {
        std::size_t parsed = 0;
        const bool hexadecimal = text.size() > 2 && text[0] == '0' &&
            (text[1] == 'x' || text[1] == 'X');
        const unsigned long long value = std::stoull(text, &parsed,
                                                     hexadecimal ? 16 : 10);
        return parsed == text.size() && value <= std::numeric_limits<uInt32>::max() &&
            static_cast<uInt32>(value) == actual;
    } catch (...) {
        return false;
    }
}

std::string jsonEscape(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(ch); break;
        }
    }
    return result;
}

void copyText(char* output, std::size_t capacity, const std::string& value)
{
    if (output == nullptr || capacity == 0) return;
    std::snprintf(output, capacity, "%s", value.c_str());
}

int32 edgeValue(int edge)
{
    return edge == HAL_ADAPTER_EDGE_FALLING ? DAQmx_Val_Falling : DAQmx_Val_Rising;
}

int32 sampleModeValue(int mode)
{
    return mode == HAL_ADAPTER_TASK_CONTINUOUS ? DAQmx_Val_ContSamps
                                               : DAQmx_Val_FiniteSamps;
}

int32 terminalConfigValue(const std::string& terminal)
{
    const std::string value = lower(trim(terminal));
    if (value == "differential" || value == "diff") return DAQmx_Val_Diff;
    if (value == "rse") return DAQmx_Val_RSE;
    if (value == "nrse") return DAQmx_Val_NRSE;
    return DAQmx_Val_Cfg_Default;
}

long long nowUs(const HalAdapterHostApiV1& host)
{
    return host.nowUs == nullptr ? 0 : host.nowUs();
}

unsigned int supportedModulesMask(const DeviceProfile& profile)
{
    unsigned int result = 0;
    if (profile.analogInputChannels > 0 || profile.analogOutputChannels > 0) {
        result |= HAL_MODULE_ANALOG;
    }
    if (profile.supportsDigital) result |= HAL_MODULE_DIGITAL;
    if (profile.supportsCounter) result |= HAL_MODULE_COUNTER;
    return result;
}

bool supportsTaskKind(const DeviceProfile& profile, int kind)
{
    switch (kind) {
    case HAL_ADAPTER_TASK_ANALOG_INPUT: return profile.analogInputChannels > 0;
    case HAL_ADAPTER_TASK_ANALOG_OUTPUT: return profile.analogOutputChannels > 0;
    case HAL_ADAPTER_TASK_DIGITAL_INPUT:
    case HAL_ADAPTER_TASK_DIGITAL_OUTPUT: return profile.supportsDigital;
    case HAL_ADAPTER_TASK_COUNTER_INPUT:
    case HAL_ADAPTER_TASK_COUNTER_OUTPUT: return profile.supportsCounter;
    default: return false;
    }
}

bool isKnownTaskKind(int kind)
{
    return kind == HAL_ADAPTER_TASK_ANALOG_INPUT ||
        kind == HAL_ADAPTER_TASK_ANALOG_OUTPUT ||
        kind == HAL_ADAPTER_TASK_DIGITAL_INPUT ||
        kind == HAL_ADAPTER_TASK_DIGITAL_OUTPUT ||
        kind == HAL_ADAPTER_TASK_COUNTER_INPUT ||
        kind == HAL_ADAPTER_TASK_COUNTER_OUTPUT;
}

struct AdapterState;
struct DeviceState;

struct DigitalBank {
    int portNumber = -1;
    int firstLine = -1;
    int lastLine = -1;
    bool output = false;
    uInt32 safeMask = 0;
    uInt32 appliedMask = 0;
};

struct ChannelLocation {
    std::size_t bank = 0;
    unsigned int bit = 0;
};

struct TaskState {
    DeviceState* owner = nullptr;
    TaskHandle native = nullptr;
    int kind = 0;
    int mode = HAL_ADAPTER_TASK_ON_DEMAND;
    int channelCount = 0;
    bool started = false;
    bool overflowed = false;
    bool underflowed = false;
    long long totalSamplesPerChannel = 0;
    std::vector<int> physicalIndexes;
    std::mutex mutex;
};

struct DeviceState {
    AdapterState* owner = nullptr;
    DeviceConfig config;
    std::string productType;
    uInt32 serialNumber = 0;
    std::map<int, ChannelConfig> analogInputs;
    std::map<int, ChannelConfig> analogOutputs;
    std::map<int, ChannelConfig> digitalInputs;
    std::map<int, ChannelConfig> digitalOutputs;
    std::map<int, ChannelConfig> counterInputs;
    std::map<int, ChannelConfig> counterOutputs;
    std::vector<DigitalBank> banks;
    std::map<int, ChannelLocation> inputLocations;
    std::map<int, ChannelLocation> outputLocations;
    std::vector<TaskState*> tasks;
    std::mutex mutex;
};

struct AdapterState {
    DriverConfig config;
    HalAdapterHostApiV1 host{};
    std::vector<DeviceState*> devices;
    std::mutex mutex;
};

HalAdapterHostApiV1 globalHost{};
std::mutex globalHostMutex;

double timeoutSeconds(const AdapterState* adapter, int timeoutMs)
{
    if (timeoutMs > 0) return static_cast<double>(timeoutMs) / 1000.0;
    return adapter == nullptr ? 1.0 : adapter->config.timeoutSeconds;
}

std::string digitalPhysical(const DeviceState* device, const DigitalBank& bank)
{
    std::string physical = device->config.physicalDeviceId + "/port" +
        std::to_string(bank.portNumber) + "/line" + std::to_string(bank.firstLine);
    if (bank.lastLine != bank.firstLine) {
        physical += ":" + std::to_string(bank.lastLine);
    }
    return physical;
}

std::string channelPhysical(const DeviceState* device, const ChannelConfig& channel)
{
    std::string suffix;
    switch (channel.module) {
    case ChannelModule::Analog:
        suffix = channel.direction == ChannelDirection::Input ? "/ai" : "/ao";
        suffix += std::to_string(channel.channelNumber);
        break;
    case ChannelModule::Digital:
        suffix = "/port" + std::to_string(channel.portNumber) + "/line" +
            std::to_string(channel.lineNumber);
        break;
    case ChannelModule::Counter:
        suffix = "/ctr" + std::to_string(channel.counterNumber);
        break;
    }
    return device->config.physicalDeviceId + suffix;
}

HalAdapterStatus clearNativeTask(TaskHandle* task, bool* started = nullptr)
{
    if (task == nullptr || *task == nullptr) return makeStatus();
    HalAdapterStatus first = makeStatus();
    if (started == nullptr || *started) {
        first = rememberFirst(first, fromDaq(DAQmxStopTask(*task), "DAQmxStopTask"));
    }
    first = rememberFirst(first, fromDaq(DAQmxClearTask(*task), "DAQmxClearTask"));
    *task = nullptr;
    if (started != nullptr) *started = false;
    return first;
}

HalAdapterStatus executeBankWrite(DeviceState* device,
                                  DigitalBank* bank,
                                  uInt32 mask,
                                  int timeoutMs)
{
    TaskHandle task = nullptr;
    HalAdapterStatus first = fromDaq(DAQmxCreateTask("", &task), "DAQmxCreateTask");
    if (first.code == HAL_ADAPTER_OK) {
        const std::string physical = digitalPhysical(device, *bank);
        first = fromDaq(DAQmxCreateDOChan(task,
                                          physical.c_str(),
                                          "",
                                          DAQmx_Val_ChanForAllLines),
                        "DAQmxCreateDOChan");
    }
    if (first.code == HAL_ADAPTER_OK) {
        int32 written = 0;
        first = fromDaq(DAQmxWriteDigitalU32(task,
                                             1,
                                             1,
                                             timeoutSeconds(device->owner, timeoutMs),
                                             DAQmx_Val_GroupByChannel,
                                             &mask,
                                             &written,
                                             nullptr),
                        "DAQmxWriteDigitalU32");
        if (first.code == HAL_ADAPTER_OK && written != 1) {
            first = makeStatus(HAL_ADAPTER_IO_ERROR,
                               0,
                               "DAQmxWriteDigitalU32 wrote no complete port sample");
        }
    }
    if (task != nullptr) {
        first = rememberFirst(first, fromDaq(DAQmxClearTask(task), "DAQmxClearTask"));
    }
    if (first.code == HAL_ADAPTER_OK) bank->appliedMask = mask;
    return first;
}

HalAdapterStatus executeBankRead(DeviceState* device,
                                 const DigitalBank& bank,
                                 uInt32* mask,
                                 int timeoutMs)
{
    TaskHandle task = nullptr;
    bool started = false;
    HalAdapterStatus first = fromDaq(DAQmxCreateTask("", &task), "DAQmxCreateTask");
    if (first.code == HAL_ADAPTER_OK) {
        const std::string physical = digitalPhysical(device, bank);
        first = fromDaq(DAQmxCreateDIChan(task,
                                          physical.c_str(),
                                          "",
                                          DAQmx_Val_ChanForAllLines),
                        "DAQmxCreateDIChan");
    }
    if (first.code == HAL_ADAPTER_OK) {
        first = fromDaq(DAQmxStartTask(task), "DAQmxStartTask");
        started = first.code == HAL_ADAPTER_OK;
    }
    if (first.code == HAL_ADAPTER_OK) {
        int32 read = 0;
        first = fromDaq(DAQmxReadDigitalU32(task,
                                            1,
                                            timeoutSeconds(device->owner, timeoutMs),
                                            DAQmx_Val_GroupByChannel,
                                            mask,
                                            1,
                                            &read,
                                            nullptr),
                        "DAQmxReadDigitalU32");
        if (first.code == HAL_ADAPTER_OK && read != 1) {
            first = makeStatus(HAL_ADAPTER_IO_ERROR,
                               0,
                               "DAQmxReadDigitalU32 returned no complete port sample");
        }
    }
    return rememberFirst(first, clearNativeTask(&task, &started));
}

HalAdapterStatus executeAnalogWrite(DeviceState* device,
                                    const std::vector<const ChannelConfig*>& channels,
                                    const double* values,
                                    int timeoutMs)
{
    if (channels.empty() || values == nullptr) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    TaskHandle task = nullptr;
    HalAdapterStatus first = fromDaq(DAQmxCreateTask("", &task), "DAQmxCreateTask");
    for (const ChannelConfig* channel : channels) {
        if (first.code != HAL_ADAPTER_OK) break;
        const std::string physical = channelPhysical(device, *channel);
        first = fromDaq(DAQmxCreateAOVoltageChan(task,
                                                 physical.c_str(),
                                                 "",
                                                 channel->minValue,
                                                 channel->maxValue,
                                                 DAQmx_Val_Volts,
                                                 nullptr),
                        "DAQmxCreateAOVoltageChan");
    }
    if (first.code == HAL_ADAPTER_OK) {
        int32 written = 0;
        first = fromDaq(DAQmxWriteAnalogF64(task,
                                            1,
                                            1,
                                            timeoutSeconds(device->owner, timeoutMs),
                                            DAQmx_Val_GroupByChannel,
                                            values,
                                            &written,
                                            nullptr),
                        "DAQmxWriteAnalogF64");
        if (first.code == HAL_ADAPTER_OK && written != 1) {
            first = makeStatus(HAL_ADAPTER_IO_ERROR,
                               0,
                               "DAQmxWriteAnalogF64 wrote no complete sample");
        }
    }
    if (task != nullptr) {
        first = rememberFirst(first, fromDaq(DAQmxClearTask(task), "DAQmxClearTask"));
    }
    return first;
}

void buildChannelMaps(DeviceState* device)
{
    std::map<std::pair<int, bool>, std::vector<ChannelConfig>> grouped;
    for (const ChannelConfig& channel : device->config.channels) {
        if (channel.module == ChannelModule::Analog) {
            (channel.direction == ChannelDirection::Input ? device->analogInputs
                                                          : device->analogOutputs)
                .emplace(channel.physicalIndex, channel);
        } else if (channel.module == ChannelModule::Counter) {
            (channel.direction == ChannelDirection::Input ? device->counterInputs
                                                          : device->counterOutputs)
                .emplace(channel.physicalIndex, channel);
        } else {
            (channel.direction == ChannelDirection::Input ? device->digitalInputs
                                                          : device->digitalOutputs)
                .emplace(channel.physicalIndex, channel);
            grouped[{channel.portNumber, channel.direction == ChannelDirection::Output}]
                .push_back(channel);
        }
    }

    for (auto& entry : grouped) {
        auto& channels = entry.second;
        std::sort(channels.begin(), channels.end(), [](const ChannelConfig& left,
                                                       const ChannelConfig& right) {
            return left.lineNumber < right.lineNumber;
        });
        std::size_t first = 0;
        while (first < channels.size()) {
            std::size_t last = first;
            while (last + 1 < channels.size() &&
                   channels[last + 1].lineNumber == channels[last].lineNumber + 1) {
                ++last;
            }
            DigitalBank bank;
            bank.portNumber = entry.first.first;
            bank.output = entry.first.second;
            bank.firstLine = channels[first].lineNumber;
            bank.lastLine = channels[last].lineNumber;
            const std::size_t bankIndex = device->banks.size();
            for (std::size_t index = first; index <= last; ++index) {
                const ChannelConfig& channel = channels[index];
                const unsigned int bit = static_cast<unsigned int>(
                    channel.lineNumber - bank.firstLine);
                if (bank.output) {
                    device->outputLocations[channel.physicalIndex] = {bankIndex, bit};
                    if (channel.safeHigh) bank.safeMask |= uInt32{1} << bit;
                } else {
                    device->inputLocations[channel.physicalIndex] = {bankIndex, bit};
                }
            }
            bank.appliedMask = bank.safeMask;
            device->banks.push_back(bank);
            first = last + 1;
        }
    }
}

HalAdapterStatus applySafeOutputs(DeviceState* device, int timeoutMs)
{
    HalAdapterStatus first = makeStatus();
    for (DigitalBank& bank : device->banks) {
        if (bank.output) {
            first = rememberFirst(first,
                                  executeBankWrite(device,
                                                   &bank,
                                                   bank.safeMask,
                                                   timeoutMs));
        }
    }
    std::vector<const ChannelConfig*> channels;
    std::vector<double> values;
    channels.reserve(device->analogOutputs.size());
    values.reserve(device->analogOutputs.size());
    for (const auto& entry : device->analogOutputs) {
        channels.push_back(&entry.second);
        values.push_back(entry.second.safeAnalog);
    }
    if (!channels.empty()) {
        first = rememberFirst(first,
                              executeAnalogWrite(device,
                                                 channels,
                                                 values.data(),
                                                 timeoutMs));
    }
    return first;
}

HalAdapterStatus stopTaskNative(TaskState* task)
{
    if (task == nullptr || task->native == nullptr || !task->started) return makeStatus();
    const HalAdapterStatus status = fromDaq(DAQmxStopTask(task->native), "DAQmxStopTask");
    task->started = false;
    return status;
}

HalAdapterStatus destroyTask(TaskState* task, bool removeFromOwner)
{
    if (task == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    if (removeFromOwner && task->owner != nullptr) {
        std::lock_guard<std::mutex> deviceLock(task->owner->mutex);
        auto& tasks = task->owner->tasks;
        tasks.erase(std::remove(tasks.begin(), tasks.end(), task), tasks.end());
    }
    HalAdapterStatus first;
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        first = stopTaskNative(task);
        if (task->native != nullptr) {
            first = rememberFirst(first,
                                  fromDaq(DAQmxClearTask(task->native), "DAQmxClearTask"));
            task->native = nullptr;
        }
    }
    delete task;
    return first;
}

HalAdapterStatus stopAllTasks(DeviceState* device)
{
    HalAdapterStatus first = makeStatus();
    std::vector<TaskState*> tasks;
    {
        std::lock_guard<std::mutex> lock(device->mutex);
        tasks = device->tasks;
    }
    for (TaskState* task : tasks) {
        std::lock_guard<std::mutex> lock(task->mutex);
        first = rememberFirst(first, stopTaskNative(task));
    }
    return first;
}

HalAdapterStatus closeAllTasks(DeviceState* device)
{
    std::vector<TaskState*> tasks;
    {
        std::lock_guard<std::mutex> lock(device->mutex);
        tasks.swap(device->tasks);
    }
    HalAdapterStatus first = makeStatus();
    for (TaskState* task : tasks) {
        first = rememberFirst(first, destroyTask(task, false));
    }
    return first;
}

bool discoverIdentity(const std::string& deviceName,
                      std::string* product,
                      uInt32* serial,
                      HalAdapterStatus* status)
{
    char names[4096]{};
    *status = fromDaq(DAQmxGetSysDevNames(names, static_cast<uInt32>(sizeof(names))),
                      "DAQmxGetSysDevNames");
    if (status->code != HAL_ADAPTER_OK) return false;
    const auto devices = splitDeviceNames(names);
    if (std::find(devices.begin(), devices.end(), deviceName) == devices.end()) {
        *status = makeStatus(HAL_ADAPTER_NOT_FOUND,
                             0,
                             "Configured NI device is not present: " + deviceName);
        return false;
    }
    char model[HAL_ADAPTER_MAX_TEXT]{};
    *status = fromDaq(DAQmxGetDevProductType(deviceName.c_str(),
                                             model,
                                             static_cast<uInt32>(sizeof(model))),
                      "DAQmxGetDevProductType");
    if (status->code != HAL_ADAPTER_OK) return false;
    *product = trim(model);
    *status = fromDaq(DAQmxGetDevSerialNum(deviceName.c_str(), serial),
                      "DAQmxGetDevSerialNum");
    return status->code == HAL_ADAPTER_OK;
}

const ChannelConfig* taskChannel(DeviceState* device, int kind, int physicalIndex)
{
    const std::map<int, ChannelConfig>* channels = nullptr;
    switch (kind) {
    case HAL_ADAPTER_TASK_ANALOG_INPUT: channels = &device->analogInputs; break;
    case HAL_ADAPTER_TASK_ANALOG_OUTPUT: channels = &device->analogOutputs; break;
    case HAL_ADAPTER_TASK_DIGITAL_INPUT: channels = &device->digitalInputs; break;
    case HAL_ADAPTER_TASK_DIGITAL_OUTPUT: channels = &device->digitalOutputs; break;
    case HAL_ADAPTER_TASK_COUNTER_INPUT: channels = &device->counterInputs; break;
    case HAL_ADAPTER_TASK_COUNTER_OUTPUT: channels = &device->counterOutputs; break;
    default: return nullptr;
    }
    const auto found = channels->find(physicalIndex);
    return found == channels->end() ? nullptr : &found->second;
}

HalAdapterStatus configureTaskChannels(TaskState* task,
                                       const HalAdapterTaskConfig* config)
{
    DeviceState* device = task->owner;
    std::set<int> unique;
    for (int index = 0; index < config->channelCount; ++index) {
        const int physicalIndex = config->channelIndexes[index];
        if (!unique.insert(physicalIndex).second) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                              0,
                              "Sample task channel indexes must be unique");
        }
        const ChannelConfig* channel = taskChannel(device, config->kind, physicalIndex);
        if (channel == nullptr) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "Sample task channel is not projected for the requested direction");
        }
        const std::string physical = channelPhysical(device, *channel);
        int32 code = 0;
        switch (config->kind) {
        case HAL_ADAPTER_TASK_ANALOG_INPUT:
            code = DAQmxCreateAIVoltageChan(task->native,
                                            physical.c_str(),
                                            "",
                                            terminalConfigValue(channel->terminalConfig),
                                            channel->minValue,
                                            channel->maxValue,
                                            DAQmx_Val_Volts,
                                            nullptr);
            break;
        case HAL_ADAPTER_TASK_ANALOG_OUTPUT:
            code = DAQmxCreateAOVoltageChan(task->native,
                                            physical.c_str(),
                                            "",
                                            channel->minValue,
                                            channel->maxValue,
                                            DAQmx_Val_Volts,
                                            nullptr);
            break;
        case HAL_ADAPTER_TASK_DIGITAL_INPUT:
            code = DAQmxCreateDIChan(task->native,
                                     physical.c_str(),
                                     "",
                                     DAQmx_Val_ChanForAllLines);
            break;
        case HAL_ADAPTER_TASK_DIGITAL_OUTPUT:
            code = DAQmxCreateDOChan(task->native,
                                     physical.c_str(),
                                     "",
                                     DAQmx_Val_ChanForAllLines);
            break;
        case HAL_ADAPTER_TASK_COUNTER_INPUT:
            if (config->channelCount != 1 ||
                config->counterMode != HAL_ADAPTER_COUNTER_COUNT_EDGES) {
                return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                                  0,
                                  "Counter input v1 supports one edge-count channel per task");
            }
            code = DAQmxCreateCICountEdgesChan(task->native,
                                               physical.c_str(),
                                               "",
                                               edgeValue(config->counterEdge),
                                               0,
                                               DAQmx_Val_CountUp);
            break;
        case HAL_ADAPTER_TASK_COUNTER_OUTPUT:
            if (config->channelCount != 1 ||
                config->counterMode != HAL_ADAPTER_COUNTER_PULSE_FREQUENCY ||
                config->counterFrequencyHz <= 0.0 ||
                config->counterFrequencyHz > 20000000.0 ||
                config->counterDutyCycle <= 0.0 || config->counterDutyCycle >= 1.0) {
                return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                                  0,
                                  "Counter output requires one pulse-frequency channel and duty cycle in (0,1)");
            }
            code = DAQmxCreateCOPulseChanFreq(task->native,
                                              physical.c_str(),
                                              "",
                                              DAQmx_Val_Hz,
                                              DAQmx_Val_Low,
                                              0.0,
                                              config->counterFrequencyHz,
                                              config->counterDutyCycle);
            break;
        default:
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT, 0, "Unknown sample task kind");
        }
        const HalAdapterStatus status = fromDaq(code, "DAQmxCreateChannel");
        if (status.code != HAL_ADAPTER_OK) return status;
        task->physicalIndexes.push_back(physicalIndex);
    }
    return makeStatus();
}

HalAdapterStatus configureTaskTiming(TaskState* task,
                                     const HalAdapterTaskConfig* config)
{
    const DeviceProfile& profile = deviceProfile(task->owner->config.modelKind);
    if (profile.analogOutputOnDemandOnly &&
        config->kind == HAL_ADAPTER_TASK_ANALOG_OUTPUT &&
        config->mode != HAL_ADAPTER_TASK_ON_DEMAND) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) +
                              " supports on-demand analog output only");
    }
    if (config->mode == HAL_ADAPTER_TASK_ON_DEMAND) return makeStatus();
    if (config->mode != HAL_ADAPTER_TASK_FINITE &&
        config->mode != HAL_ADAPTER_TASK_CONTINUOUS) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT, 0, "Unknown sample task mode");
    }
    const uInt64 samples = static_cast<uInt64>(std::max(1, config->samplesPerChannel));
    HalAdapterStatus status;
    const bool counterInputWithSampleClock =
        config->kind == HAL_ADAPTER_TASK_COUNTER_INPUT &&
        config->sampleClockSource != nullptr &&
        !trim(config->sampleClockSource).empty();
    if (counterInputWithSampleClock) {
        if (config->sampleRateHz <= 0.0 || config->sampleRateHz > 20000000.0) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                              0,
                              "Clocked counter input requires a rate hint within 20 MHz");
        }
        status = fromDaq(DAQmxCfgSampClkTiming(task->native,
                                              config->sampleClockSource,
                                              config->sampleRateHz,
                                              edgeValue(config->sampleClockEdge),
                                              sampleModeValue(config->mode),
                                              samples),
                         "DAQmxCfgSampClkTiming");
    } else if (config->kind == HAL_ADAPTER_TASK_COUNTER_INPUT ||
               config->kind == HAL_ADAPTER_TASK_COUNTER_OUTPUT) {
        status = fromDaq(DAQmxCfgImplicitTiming(task->native,
                                               sampleModeValue(config->mode),
                                               samples),
                         "DAQmxCfgImplicitTiming");
    } else {
        if (config->sampleRateHz <= 0.0) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                              0,
                              "Finite and continuous sampled tasks require a positive sample rate");
        }
        double maximumRateHz = 0.0;
        if (config->kind == HAL_ADAPTER_TASK_ANALOG_INPUT) {
            maximumRateHz = config->channelCount == 1
                ? 1250000.0
                : 1000000.0 / static_cast<double>(config->channelCount);
        } else if (config->kind == HAL_ADAPTER_TASK_ANALOG_OUTPUT) {
            maximumRateHz = config->channelCount <= 2
                ? 2860000.0
                : (config->channelCount == 3 ? 1540000.0 : 1250000.0);
        } else {
            maximumRateHz = 10000000.0;
            for (const int physicalIndex : task->physicalIndexes) {
                const ChannelConfig* channel = taskChannel(task->owner,
                                                           config->kind,
                                                           physicalIndex);
                if (channel == nullptr || channel->portNumber != 0) {
                    return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                                      0,
                                      std::string(profile.name) +
                                          " hardware-timed DIO is available on port0; "
                                          "port1/2 are static/PFI lines");
                }
            }
        }
        if (config->sampleRateHz > maximumRateHz) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                              0,
                              "Requested sample rate exceeds the " +
                                  std::string(profile.name) + " subsystem limit");
        }
        status = fromDaq(DAQmxCfgSampClkTiming(task->native,
                                              config->sampleClockSource == nullptr
                                                  ? ""
                                                  : config->sampleClockSource,
                                              config->sampleRateHz,
                                              edgeValue(config->sampleClockEdge),
                                              sampleModeValue(config->mode),
                                              samples),
                         "DAQmxCfgSampClkTiming");
    }
    if (status.code != HAL_ADAPTER_OK) return status;

    const int bufferSamples = config->bufferSamplesPerChannel > 0
        ? config->bufferSamplesPerChannel
        : (config->mode == HAL_ADAPTER_TASK_CONTINUOUS
               ? std::max(2, config->samplesPerChannel)
               : 0);
    if (bufferSamples > 0) {
        const bool input = config->kind == HAL_ADAPTER_TASK_ANALOG_INPUT ||
            config->kind == HAL_ADAPTER_TASK_DIGITAL_INPUT ||
            config->kind == HAL_ADAPTER_TASK_COUNTER_INPUT;
        status = input
            ? fromDaq(DAQmxCfgInputBuffer(task->native,
                                          static_cast<uInt32>(bufferSamples)),
                      "DAQmxCfgInputBuffer")
            : fromDaq(DAQmxCfgOutputBuffer(task->native,
                                           static_cast<uInt32>(bufferSamples)),
                      "DAQmxCfgOutputBuffer");
    }
    return status;
}

HalAdapterStatus configureTaskTrigger(TaskState* task,
                                      const HalAdapterTaskConfig* config)
{
    if (config->triggerType == HAL_ADAPTER_TRIGGER_NONE) return makeStatus();
    if (config->triggerSource == nullptr || trim(config->triggerSource).empty()) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                          0,
                          "Triggered tasks require a trigger source");
    }
    if (config->triggerRole == HAL_ADAPTER_TRIGGER_START) {
        if (config->triggerType == HAL_ADAPTER_TRIGGER_DIGITAL_EDGE) {
            return fromDaq(DAQmxCfgDigEdgeStartTrig(task->native,
                                                   config->triggerSource,
                                                   edgeValue(config->triggerEdge)),
                           "DAQmxCfgDigEdgeStartTrig");
        }
        if (config->triggerType == HAL_ADAPTER_TRIGGER_ANALOG_EDGE) {
            return fromDaq(DAQmxCfgAnlgEdgeStartTrig(task->native,
                                                    config->triggerSource,
                                                    edgeValue(config->triggerEdge),
                                                    config->triggerLevel),
                           "DAQmxCfgAnlgEdgeStartTrig");
        }
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          "Start triggers support digital-edge or analog-edge sources");
    }
    if (config->triggerRole == HAL_ADAPTER_TRIGGER_REFERENCE) {
        const bool input = config->kind == HAL_ADAPTER_TASK_ANALOG_INPUT ||
            config->kind == HAL_ADAPTER_TASK_DIGITAL_INPUT ||
            config->kind == HAL_ADAPTER_TASK_COUNTER_INPUT;
        if (!input) {
            return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                              0,
                              "Reference triggers apply to acquisition tasks");
        }
        const int configuredPretrigger = config->referencePretriggerSamples > 0
            ? config->referencePretriggerSamples
            : std::max(1, config->samplesPerChannel / 2);
        if (config->mode == HAL_ADAPTER_TASK_FINITE &&
            configuredPretrigger >= config->samplesPerChannel) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                              0,
                              "Reference pretrigger samples must be smaller than the finite record");
        }
        const uInt32 pretrigger = static_cast<uInt32>(configuredPretrigger);
        if (config->triggerType == HAL_ADAPTER_TRIGGER_DIGITAL_EDGE) {
            return fromDaq(DAQmxCfgDigEdgeRefTrig(task->native,
                                                 config->triggerSource,
                                                 edgeValue(config->triggerEdge),
                                                 pretrigger),
                           "DAQmxCfgDigEdgeRefTrig");
        }
        if (config->triggerType == HAL_ADAPTER_TRIGGER_ANALOG_EDGE) {
            return fromDaq(DAQmxCfgAnlgEdgeRefTrig(task->native,
                                                  config->triggerSource,
                                                  edgeValue(config->triggerEdge),
                                                  config->triggerLevel,
                                                  pretrigger),
                           "DAQmxCfgAnlgEdgeRefTrig");
        }
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          "Reference triggers support digital-edge or analog-edge sources");
    }
    if (config->triggerRole == HAL_ADAPTER_TRIGGER_PAUSE) {
        HalAdapterStatus status;
        if (config->triggerType == HAL_ADAPTER_TRIGGER_DIGITAL_EDGE ||
            config->triggerType == HAL_ADAPTER_TRIGGER_DIGITAL_LEVEL) {
            status = fromDaq(DAQmxSetPauseTrigType(task->native, DAQmx_Val_DigLvl),
                             "DAQmxSetPauseTrigType");
            if (status.code == HAL_ADAPTER_OK) {
                status = fromDaq(DAQmxSetDigLvlPauseTrigSrc(task->native,
                                                            config->triggerSource),
                                 "DAQmxSetDigLvlPauseTrigSrc");
            }
            if (status.code == HAL_ADAPTER_OK) {
                status = fromDaq(DAQmxSetDigLvlPauseTrigWhen(
                                     task->native,
                                     config->triggerEdge == HAL_ADAPTER_EDGE_FALLING
                                         ? DAQmx_Val_Low
                                         : DAQmx_Val_High),
                                 "DAQmxSetDigLvlPauseTrigWhen");
            }
            return status;
        }
        if (config->triggerType == HAL_ADAPTER_TRIGGER_ANALOG_EDGE) {
            status = fromDaq(DAQmxSetPauseTrigType(task->native, DAQmx_Val_AnlgLvl),
                             "DAQmxSetPauseTrigType");
            if (status.code == HAL_ADAPTER_OK) {
                status = fromDaq(DAQmxSetAnlgLvlPauseTrigSrc(task->native,
                                                             config->triggerSource),
                                 "DAQmxSetAnlgLvlPauseTrigSrc");
            }
            if (status.code == HAL_ADAPTER_OK) {
                status = fromDaq(DAQmxSetAnlgLvlPauseTrigWhen(
                                     task->native,
                                     config->triggerEdge == HAL_ADAPTER_EDGE_FALLING
                                         ? DAQmx_Val_BelowLvl
                                         : DAQmx_Val_AboveLvl),
                                 "DAQmxSetAnlgLvlPauseTrigWhen");
            }
            if (status.code == HAL_ADAPTER_OK) {
                status = fromDaq(DAQmxSetAnlgLvlPauseTrigLvl(task->native,
                                                             config->triggerLevel),
                                 "DAQmxSetAnlgLvlPauseTrigLvl");
            }
            return status;
        }
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          "Pause triggers support digital-level or analog-level sources");
    }
    return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT, 0, "Unknown trigger role");
}

HalAdapterStatus HAL_ADAPTER_CALL adapterCloseDevice(HalAdapterDeviceHandle handle);

HalAdapterStatus HAL_ADAPTER_CALL adapterGetInfo(HalAdapterInfo* output)
{
    if (output == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    *output = HalAdapterInfo{};
    copyText(output->adapterId, sizeof(output->adapterId), "ni.daqmx");
    copyText(output->vendor, sizeof(output->vendor), "National Instruments");
    copyText(output->name, sizeof(output->name), "HWTest NI-DAQmx Adapter");
    copyText(output->version, sizeof(output->version), "2.1.0");
    output->supportedModulesMask = HAL_MODULE_ANALOG | HAL_MODULE_DIGITAL |
        HAL_MODULE_COUNTER;
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterInitialize(const char* configJson,
                                                     HalAdapterHandle* output)
{
    if (configJson == nullptr || output == nullptr) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                          0,
                          "Adapter config and output handle are required");
    }
    *output = nullptr;
    try {
        auto adapter = std::make_unique<AdapterState>();
        std::string error;
        if (!hwtest::adapters::ni_daqmx::parseDriverConfig(configJson,
                                                           &adapter->config,
                                                           &error)) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT, 0, error);
        }
        {
            std::lock_guard<std::mutex> lock(globalHostMutex);
            adapter->host = globalHost;
        }
        char devices[4096]{};
        const HalAdapterStatus status = fromDaq(
            DAQmxGetSysDevNames(devices, static_cast<uInt32>(sizeof(devices))),
            "DAQmxGetSysDevNames");
        if (status.code != HAL_ADAPTER_OK) return status;
        *output = adapter.release();
        return makeStatus();
    } catch (const std::exception& exception) {
        return makeStatus(HAL_ADAPTER_INTERNAL_ERROR, 0, exception.what());
    } catch (...) {
        return makeStatus(HAL_ADAPTER_INTERNAL_ERROR,
                          0,
                          "Unexpected exception while initializing NI-DAQmx adapter");
    }
}

HalAdapterStatus HAL_ADAPTER_CALL adapterShutdown(HalAdapterHandle handle)
{
    auto* adapter = static_cast<AdapterState*>(handle);
    if (adapter == nullptr) return makeStatus();
    HalAdapterStatus first = makeStatus();
    while (true) {
        DeviceState* device = nullptr;
        {
            std::lock_guard<std::mutex> lock(adapter->mutex);
            if (adapter->devices.empty()) break;
            device = adapter->devices.back();
        }
        first = rememberFirst(first, adapterCloseDevice(device));
    }
    delete adapter;
    return first;
}

HalAdapterStatus HAL_ADAPTER_CALL adapterEnumerate(HalAdapterHandle handle,
                                                   HalAdapterDeviceInfo* output,
                                                   int* count,
                                                   int)
{
    auto* adapter = static_cast<AdapterState*>(handle);
    if (adapter == nullptr || count == nullptr || *count < 0) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    char names[4096]{};
    HalAdapterStatus status = fromDaq(
        DAQmxGetSysDevNames(names, static_cast<uInt32>(sizeof(names))),
        "DAQmxGetSysDevNames");
    if (status.code != HAL_ADAPTER_OK) return status;
    const std::vector<std::string> devices = splitDeviceNames(names);
    const int capacity = output == nullptr ? 0 : *count;
    *count = static_cast<int>(devices.size());
    for (int index = 0; output != nullptr && index < capacity &&
         index < static_cast<int>(devices.size()); ++index) {
        std::string product;
        uInt32 serial = 0;
        HalAdapterStatus identity;
        if (!discoverIdentity(devices[static_cast<std::size_t>(index)],
                              &product,
                              &serial,
                              &identity)) {
            return identity;
        }
        output[index] = HalAdapterDeviceInfo{};
        copyText(output[index].deviceId,
                 sizeof(output[index].deviceId),
                 devices[static_cast<std::size_t>(index)]);
        copyText(output[index].model, sizeof(output[index].model), product);
        copyText(output[index].serialNumber,
                 sizeof(output[index].serialNumber),
                 std::to_string(serial));
        const DeviceProfile* profile = findDeviceProfile(product);
        output[index].supportedModulesMask = profile == nullptr
            ? 0u
            : supportedModulesMask(*profile);
        copyText(output[index].propertiesJson,
                 sizeof(output[index].propertiesJson),
                 std::string("{\"driver\":\"NI-DAQmx\",\"bus\":\"PXI\",\"profile\":\"") +
                     (profile == nullptr ? "unsupported" : profile->name) + "\"}");
    }
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterOpenDevice(HalAdapterHandle handle,
                                                    const char* deviceId,
                                                    const char* openOptionsJson,
                                                    HalAdapterDeviceHandle* output)
{
    auto* adapter = static_cast<AdapterState*>(handle);
    if (adapter == nullptr || deviceId == nullptr || openOptionsJson == nullptr ||
        output == nullptr) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    *output = nullptr;
    try {
        auto device = std::make_unique<DeviceState>();
        device->owner = adapter;
        std::string error;
        if (!hwtest::adapters::ni_daqmx::parseDeviceOpenSpec(openOptionsJson,
                                                             &device->config,
                                                             &error)) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT, 0, error);
        }
        if (device->config.physicalDeviceId != trim(deviceId)) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "Requested NI device does not match projected physicalDeviceId");
        }
        HalAdapterStatus identity;
        if (!discoverIdentity(device->config.physicalDeviceId,
                              &device->productType,
                              &device->serialNumber,
                              &identity)) {
            return identity;
        }
        if (lower(device->productType) != lower(device->config.model)) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "NI product type mismatch; expected '" + device->config.model +
                                  "' but found '" + device->productType + "'");
        }
        if (!serialMatches(device->config.serialNumber, device->serialNumber)) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "NI serial number does not match the deployment configuration");
        }
        buildChannelMaps(device.get());
        const HalAdapterStatus safe = applySafeOutputs(device.get(), 0);
        if (safe.code != HAL_ADAPTER_OK) return safe;
        {
            std::lock_guard<std::mutex> lock(adapter->mutex);
            adapter->devices.push_back(device.get());
        }
        *output = device.release();
        return makeStatus();
    } catch (const std::exception& exception) {
        return makeStatus(HAL_ADAPTER_INTERNAL_ERROR, 0, exception.what());
    } catch (...) {
        return makeStatus(HAL_ADAPTER_INTERNAL_ERROR,
                          0,
                          "Unexpected exception while opening NI device");
    }
}

HalAdapterStatus HAL_ADAPTER_CALL adapterCloseDevice(HalAdapterDeviceHandle handle)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    HalAdapterStatus first = closeAllTasks(device);
    first = rememberFirst(first, applySafeOutputs(device, 0));
    AdapterState* adapter = device->owner;
    if (adapter != nullptr) {
        std::lock_guard<std::mutex> lock(adapter->mutex);
        adapter->devices.erase(std::remove(adapter->devices.begin(),
                                           adapter->devices.end(),
                                           device),
                               adapter->devices.end());
    }
    delete device;
    return first;
}

HalAdapterStatus HAL_ADAPTER_CALL adapterResetDevice(HalAdapterDeviceHandle handle,
                                                     int timeoutMs)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    HalAdapterStatus first = stopAllTasks(device);
    first = rememberFirst(first, applySafeOutputs(device, timeoutMs));
    return first;
}

HalAdapterStatus HAL_ADAPTER_CALL adapterCapabilities(HalAdapterDeviceHandle handle,
                                                      char* output,
                                                      int* bytes,
                                                      int)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr || bytes == nullptr) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    const DeviceProfile& profile = deviceProfile(device->config.modelKind);
    const std::string json = profile.model == DeviceModel::Pxi6733
        ? "{\"supportedModules\":[\"analog\"],"
          "\"limits\":{"
          "\"analog.outputChannels\":8,\"analog.outputResolutionBits\":16,"
          "\"analog.outputMaxRateHz\":1000000,"
          "\"sampleModes\":[\"onDemand\"]}}"
        : "{\"supportedModules\":[\"analog\",\"digital\",\"counter\"],"
          "\"limits\":{"
          "\"analog.inputChannels\":32,\"analog.differentialInputChannels\":16,"
          "\"analog.inputResolutionBits\":16,\"analog.inputMaxSingleRateHz\":1250000,"
          "\"analog.inputMaxAggregateRateHz\":1000000,\"analog.outputChannels\":4,"
          "\"analog.outputResolutionBits\":16,\"analog.outputMaxRateHz\":2860000,"
          "\"digital.lines\":48,\"digital.port0Lines\":32,"
          "\"digital.hardwareTimedMaxRateHz\":10000000,\"counter.channels\":2,"
          "\"counter.resolutionBits\":32,\"dmaChannels\":6,"
          "\"sampleModes\":[\"onDemand\",\"finite\",\"continuous\"],"
          "\"clockSources\":[\"internal\",\"externalTerminal\"],"
          "\"triggerRoles\":[\"start\",\"reference\",\"pause\"],"
          "\"triggerSources\":[\"digitalEdge\",\"analogEdge\",\"digitalLevel\"],"
          "\"counterModes\":[\"countEdges\",\"pulseFrequency\"]}}";
    const int required = static_cast<int>(json.size() + 1);
    if (output == nullptr || *bytes < required) {
        *bytes = required;
        return makeStatus(HAL_ADAPTER_BUFFER_TOO_SMALL);
    }
    std::memcpy(output, json.c_str(), static_cast<std::size_t>(required));
    *bytes = required;
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterAnalogConfigure(HalAdapterDeviceHandle handle,
                                                         int channelIndex,
                                                         const HalAdapterAnalogRange* range,
                                                         int isOutput,
                                                         int)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr || range == nullptr || !std::isfinite(range->minValue) ||
        !std::isfinite(range->maxValue) || range->minValue >= range->maxValue) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    const DeviceProfile& profile = deviceProfile(device->config.modelKind);
    if (isOutput == 0 && profile.analogInputChannels == 0) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) + " does not expose analog input");
    }
    if (range->unit != HAL_ADAPTER_ANALOG_UNIT_VOLT &&
        range->unit != HAL_ADAPTER_ANALOG_UNIT_MILLIVOLT) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) +
                              " voltage channels accept Volt or MilliVolt ranges");
    }
    const double scale = range->unit == HAL_ADAPTER_ANALOG_UNIT_MILLIVOLT
        ? 0.001
        : 1.0;
    const double minimum = range->minValue * scale;
    const double maximum = range->maxValue * scale;
    if (minimum < -10.0 || maximum > 10.0) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    std::lock_guard<std::mutex> lock(device->mutex);
    auto& channels = isOutput != 0 ? device->analogOutputs : device->analogInputs;
    const auto found = channels.find(channelIndex);
    if (found == channels.end()) return makeStatus(HAL_ADAPTER_NOT_FOUND);
    if (found->second.hasSafeValue &&
        (found->second.safeAnalog < minimum ||
         found->second.safeAnalog > maximum)) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                          0,
                          "Configured analog safe value is outside the requested range");
    }
    found->second.minValue = minimum;
    found->second.maxValue = maximum;
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterAnalogRead(HalAdapterDeviceHandle handle,
                                                    const int* channelIndexes,
                                                    int channelCount,
                                                    HalAdapterAnalogSample* output,
                                                    int samplesPerChannel,
                                                    int sampleRateHz,
                                                    int timeoutMs)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr || channelIndexes == nullptr || output == nullptr ||
        channelCount <= 0 || samplesPerChannel <= 0) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    const DeviceProfile& profile = deviceProfile(device->config.modelKind);
    if (profile.analogInputChannels == 0) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) + " does not expose analog input");
    }
    std::vector<ChannelConfig> channels;
    {
        std::lock_guard<std::mutex> lock(device->mutex);
        std::set<int> unique;
        for (int index = 0; index < channelCount; ++index) {
            const auto found = device->analogInputs.find(channelIndexes[index]);
            if (found == device->analogInputs.end()) return makeStatus(HAL_ADAPTER_NOT_FOUND);
            if (!unique.insert(channelIndexes[index]).second) {
                return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
            }
            channels.push_back(found->second);
        }
    }
    TaskHandle task = nullptr;
    bool started = false;
    HalAdapterStatus first = fromDaq(DAQmxCreateTask("", &task), "DAQmxCreateTask");
    for (const ChannelConfig& channel : channels) {
        if (first.code != HAL_ADAPTER_OK) break;
        const std::string physical = channelPhysical(device, channel);
        first = fromDaq(DAQmxCreateAIVoltageChan(task,
                                                 physical.c_str(),
                                                 "",
                                                 terminalConfigValue(channel.terminalConfig),
                                                 channel.minValue,
                                                 channel.maxValue,
                                                 DAQmx_Val_Volts,
                                                 nullptr),
                        "DAQmxCreateAIVoltageChan");
    }
    if (first.code == HAL_ADAPTER_OK && samplesPerChannel > 1) {
        if (sampleRateHz <= 0) {
            first = makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                               0,
                               "Multi-sample analog reads require a positive sample rate");
        } else {
            first = fromDaq(DAQmxCfgSampClkTiming(task,
                                                  "",
                                                  sampleRateHz,
                                                  DAQmx_Val_Rising,
                                                  DAQmx_Val_FiniteSamps,
                                                  static_cast<uInt64>(samplesPerChannel)),
                            "DAQmxCfgSampClkTiming");
        }
    }
    if (first.code == HAL_ADAPTER_OK) {
        first = fromDaq(DAQmxStartTask(task), "DAQmxStartTask");
        started = first.code == HAL_ADAPTER_OK;
    }
    std::vector<double> values(static_cast<std::size_t>(channelCount * samplesPerChannel));
    int32 read = 0;
    if (first.code == HAL_ADAPTER_OK) {
        first = fromDaq(DAQmxReadAnalogF64(task,
                                           samplesPerChannel,
                                           timeoutSeconds(device->owner, timeoutMs),
                                           DAQmx_Val_GroupByChannel,
                                           values.data(),
                                           static_cast<uInt32>(values.size()),
                                           &read,
                                           nullptr),
                        "DAQmxReadAnalogF64");
        if (first.code == HAL_ADAPTER_OK && read != samplesPerChannel) {
            first = makeStatus(HAL_ADAPTER_IO_ERROR,
                               0,
                               "DAQmxReadAnalogF64 returned a partial block");
        }
    }
    if (first.code == HAL_ADAPTER_OK) {
        const long long timestamp = nowUs(device->owner->host);
        for (int channel = 0; channel < channelCount; ++channel) {
            for (int sample = 0; sample < samplesPerChannel; ++sample) {
                const int offset = channel * samplesPerChannel + sample;
                output[offset] = HalAdapterAnalogSample{};
                output[offset].channelIndex = channelIndexes[channel];
                output[offset].value = values[static_cast<std::size_t>(offset)];
                output[offset].unit = HAL_ADAPTER_ANALOG_UNIT_VOLT;
                output[offset].timestampUs = timestamp;
            }
        }
    }
    return rememberFirst(first, clearNativeTask(&task, &started));
}

HalAdapterStatus HAL_ADAPTER_CALL adapterAnalogWrite(HalAdapterDeviceHandle handle,
                                                     const int* channelIndexes,
                                                     const double* values,
                                                     int channelCount,
                                                     int unit,
                                                     int timeoutMs)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr || channelIndexes == nullptr || values == nullptr ||
        channelCount <= 0) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    const DeviceProfile& profile = deviceProfile(device->config.modelKind);
    if (profile.analogOutputChannels == 0) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) + " does not expose analog output");
    }
    if (unit != HAL_ADAPTER_ANALOG_UNIT_VOLT &&
        unit != HAL_ADAPTER_ANALOG_UNIT_MILLIVOLT) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) +
                              " voltage channels accept Volt or MilliVolt values");
    }
    const double scale = unit == HAL_ADAPTER_ANALOG_UNIT_MILLIVOLT ? 0.001 : 1.0;
    std::vector<const ChannelConfig*> channels;
    std::vector<double> scaledValues;
    scaledValues.reserve(static_cast<std::size_t>(channelCount));
    std::lock_guard<std::mutex> lock(device->mutex);
    std::set<int> unique;
    for (int index = 0; index < channelCount; ++index) {
        const auto found = device->analogOutputs.find(channelIndexes[index]);
        if (found == device->analogOutputs.end()) return makeStatus(HAL_ADAPTER_NOT_FOUND);
        const double value = values[index] * scale;
        if (!std::isfinite(value) || !unique.insert(channelIndexes[index]).second ||
            value < found->second.minValue || value > found->second.maxValue) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        channels.push_back(&found->second);
        scaledValues.push_back(value);
    }
    return executeAnalogWrite(device, channels, scaledValues.data(), timeoutMs);
}

HalAdapterStatus HAL_ADAPTER_CALL adapterDigitalWrite(HalAdapterDeviceHandle handle,
                                                      const int* indexes,
                                                      const int* levels,
                                                      int count,
                                                      int timeoutMs)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr || indexes == nullptr || levels == nullptr || count <= 0) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    const DeviceProfile& profile = deviceProfile(device->config.modelKind);
    if (!profile.supportsDigital) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) + " does not expose digital I/O");
    }
    std::lock_guard<std::mutex> lock(device->mutex);
    std::map<std::size_t, uInt32> staged;
    std::set<int> unique;
    for (int index = 0; index < count; ++index) {
        if ((levels[index] != 0 && levels[index] != 1) ||
            !unique.insert(indexes[index]).second) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        const auto found = device->outputLocations.find(indexes[index]);
        if (found == device->outputLocations.end()) return makeStatus(HAL_ADAPTER_NOT_FOUND);
        const ChannelLocation location = found->second;
        auto inserted = staged.emplace(location.bank,
                                       device->banks[location.bank].appliedMask);
        const uInt32 bit = uInt32{1} << location.bit;
        if (levels[index] != 0) inserted.first->second |= bit;
        else inserted.first->second &= ~bit;
    }
    HalAdapterStatus first = makeStatus();
    for (const auto& entry : staged) {
        first = rememberFirst(first,
                              executeBankWrite(device,
                                               &device->banks[entry.first],
                                               entry.second,
                                               timeoutMs));
    }
    return first;
}

HalAdapterStatus HAL_ADAPTER_CALL adapterDigitalRead(HalAdapterDeviceHandle handle,
                                                     const int* indexes,
                                                     int count,
                                                     HalAdapterDigitalSample* output,
                                                     int timeoutMs)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr || indexes == nullptr || output == nullptr || count <= 0) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    const DeviceProfile& profile = deviceProfile(device->config.modelKind);
    if (!profile.supportsDigital) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) + " does not expose digital I/O");
    }
    std::lock_guard<std::mutex> lock(device->mutex);
    std::map<std::size_t, uInt32> masks;
    std::vector<ChannelLocation> locations;
    std::set<int> unique;
    for (int index = 0; index < count; ++index) {
        const auto found = device->inputLocations.find(indexes[index]);
        if (found == device->inputLocations.end()) return makeStatus(HAL_ADAPTER_NOT_FOUND);
        if (!unique.insert(indexes[index]).second) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
        locations.push_back(found->second);
        masks.emplace(found->second.bank, 0u);
    }
    for (auto& entry : masks) {
        const HalAdapterStatus status = executeBankRead(device,
                                                        device->banks[entry.first],
                                                        &entry.second,
                                                        timeoutMs);
        if (status.code != HAL_ADAPTER_OK) return status;
    }
    const long long timestamp = nowUs(device->owner->host);
    for (int index = 0; index < count; ++index) {
        output[index] = HalAdapterDigitalSample{};
        output[index].channelIndex = indexes[index];
        output[index].level =
            (masks[locations[static_cast<std::size_t>(index)].bank] &
             (uInt32{1} << locations[static_cast<std::size_t>(index)].bit)) != 0u;
        output[index].timestampUs = timestamp;
    }
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterTaskCreate(HalAdapterDeviceHandle handle,
                                                    const HalAdapterTaskConfig* config,
                                                    HalAdapterTaskHandle* output)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr || config == nullptr || output == nullptr ||
        config->structSize < static_cast<int>(sizeof(HalAdapterTaskConfig)) ||
        config->channelIndexes == nullptr || config->channelCount <= 0 ||
        config->samplesPerChannel <= 0 || config->bufferSamplesPerChannel < 0) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    *output = nullptr;
    const DeviceProfile& profile = deviceProfile(device->config.modelKind);
    if (!isKnownTaskKind(config->kind)) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT, 0, "Unknown sample task kind");
    }
    if (!supportsTaskKind(profile, config->kind)) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) +
                              " does not expose the requested sample task kind");
    }
    if (profile.analogOutputOnDemandOnly &&
        (config->kind != HAL_ADAPTER_TASK_ANALOG_OUTPUT ||
         config->mode != HAL_ADAPTER_TASK_ON_DEMAND ||
         config->triggerType != HAL_ADAPTER_TRIGGER_NONE ||
         config->samplesPerChannel != 1 ||
         config->bufferSamplesPerChannel != 0)) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) +
                              " supports one-sample on-demand analog output tasks only");
    }
    auto task = std::make_unique<TaskState>();
    task->owner = device;
    task->kind = config->kind;
    task->mode = config->mode;
    task->channelCount = config->channelCount;
    HalAdapterStatus status = fromDaq(DAQmxCreateTask("", &task->native), "DAQmxCreateTask");
    if (status.code == HAL_ADAPTER_OK) status = configureTaskChannels(task.get(), config);
    if (status.code == HAL_ADAPTER_OK) status = configureTaskTiming(task.get(), config);
    if (status.code == HAL_ADAPTER_OK) status = configureTaskTrigger(task.get(), config);
    if (status.code != HAL_ADAPTER_OK) {
        if (task->native != nullptr) {
            status = rememberFirst(status,
                                   fromDaq(DAQmxClearTask(task->native), "DAQmxClearTask"));
        }
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(device->mutex);
        device->tasks.push_back(task.get());
    }
    *output = task.release();
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterTaskStart(HalAdapterTaskHandle handle, int)
{
    auto* task = static_cast<TaskState*>(handle);
    if (task == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    std::lock_guard<std::mutex> lock(task->mutex);
    if (task->started) return makeStatus();
    const HalAdapterStatus status = fromDaq(DAQmxStartTask(task->native), "DAQmxStartTask");
    if (status.code == HAL_ADAPTER_OK) task->started = true;
    return status;
}

HalAdapterStatus HAL_ADAPTER_CALL adapterTaskRead(HalAdapterTaskHandle handle,
                                                  HalAdapterTaskBuffer* buffer,
                                                  int timeoutMs)
{
    auto* task = static_cast<TaskState*>(handle);
    if (task == nullptr || buffer == nullptr ||
        buffer->structSize < static_cast<int>(sizeof(HalAdapterTaskBuffer)) ||
        buffer->data == nullptr || buffer->samplesPerChannel <= 0 ||
        buffer->channelCount != task->channelCount ||
        buffer->capacityValues < task->channelCount * buffer->samplesPerChannel) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    std::lock_guard<std::mutex> lock(task->mutex);
    if (!task->started) return makeStatus(HAL_ADAPTER_BUSY, 0, "Sample task is not started");
    int32 read = 0;
    HalAdapterStatus status;
    if (task->kind == HAL_ADAPTER_TASK_ANALOG_INPUT) {
        if (buffer->sampleType != HAL_ADAPTER_SAMPLE_FLOAT64) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        status = fromDaq(DAQmxReadAnalogF64(task->native,
                                           buffer->samplesPerChannel,
                                           timeoutSeconds(task->owner->owner, timeoutMs),
                                           DAQmx_Val_GroupByChannel,
                                           static_cast<double*>(buffer->data),
                                           static_cast<uInt32>(buffer->capacityValues),
                                           &read,
                                           nullptr),
                         "DAQmxReadAnalogF64");
    } else if (task->kind == HAL_ADAPTER_TASK_DIGITAL_INPUT) {
        if (buffer->sampleType != HAL_ADAPTER_SAMPLE_UINT32) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        status = fromDaq(DAQmxReadDigitalU32(task->native,
                                            buffer->samplesPerChannel,
                                            timeoutSeconds(task->owner->owner, timeoutMs),
                                            DAQmx_Val_GroupByChannel,
                                            static_cast<uInt32*>(buffer->data),
                                            static_cast<uInt32>(buffer->capacityValues),
                                            &read,
                                            nullptr),
                         "DAQmxReadDigitalU32");
    } else if (task->kind == HAL_ADAPTER_TASK_COUNTER_INPUT) {
        if (buffer->sampleType != HAL_ADAPTER_SAMPLE_UINT32 || task->channelCount != 1) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        status = fromDaq(DAQmxReadCounterU32(task->native,
                                            buffer->samplesPerChannel,
                                            timeoutSeconds(task->owner->owner, timeoutMs),
                                            static_cast<uInt32*>(buffer->data),
                                            static_cast<uInt32>(buffer->capacityValues),
                                            &read,
                                            nullptr),
                         "DAQmxReadCounterU32");
    } else {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED, 0, "Output tasks cannot be read");
    }
    if (status.code != HAL_ADAPTER_OK) {
        if (status.vendorCode == -200279) task->overflowed = true;
        return status;
    }
    buffer->samplesPerChannel = std::max(0, static_cast<int>(read));
    buffer->timestampUs = nowUs(task->owner->owner->host);
    buffer->statusFlags = task->overflowed ? 1 : 0;
    task->totalSamplesPerChannel += buffer->samplesPerChannel;
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterTaskWrite(HalAdapterTaskHandle handle,
                                                   const HalAdapterTaskBuffer* buffer,
                                                   int autoStart,
                                                   int timeoutMs)
{
    auto* task = static_cast<TaskState*>(handle);
    if (task == nullptr || buffer == nullptr ||
        buffer->structSize < static_cast<int>(sizeof(HalAdapterTaskBuffer)) ||
        buffer->data == nullptr || buffer->samplesPerChannel <= 0 ||
        buffer->channelCount != task->channelCount ||
        buffer->capacityValues < task->channelCount * buffer->samplesPerChannel) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    std::lock_guard<std::mutex> lock(task->mutex);
    const DeviceProfile& profile = deviceProfile(task->owner->config.modelKind);
    if (profile.analogOutputOnDemandOnly &&
        task->kind == HAL_ADAPTER_TASK_ANALOG_OUTPUT &&
        buffer->samplesPerChannel != 1) {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          std::string(profile.name) +
                              " supports one-sample on-demand analog output writes only");
    }
    int32 written = 0;
    HalAdapterStatus status;
    if (task->kind == HAL_ADAPTER_TASK_ANALOG_OUTPUT) {
        if (buffer->sampleType != HAL_ADAPTER_SAMPLE_FLOAT64) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        status = fromDaq(DAQmxWriteAnalogF64(task->native,
                                            buffer->samplesPerChannel,
                                            autoStart != 0,
                                            timeoutSeconds(task->owner->owner, timeoutMs),
                                            DAQmx_Val_GroupByChannel,
                                            static_cast<const double*>(buffer->data),
                                            &written,
                                            nullptr),
                         "DAQmxWriteAnalogF64");
    } else if (task->kind == HAL_ADAPTER_TASK_DIGITAL_OUTPUT) {
        if (buffer->sampleType != HAL_ADAPTER_SAMPLE_UINT32) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
        }
        status = fromDaq(DAQmxWriteDigitalU32(task->native,
                                             buffer->samplesPerChannel,
                                             autoStart != 0,
                                             timeoutSeconds(task->owner->owner, timeoutMs),
                                             DAQmx_Val_GroupByChannel,
                                             static_cast<const uInt32*>(buffer->data),
                                             &written,
                                             nullptr),
                         "DAQmxWriteDigitalU32");
    } else {
        return makeStatus(HAL_ADAPTER_NOT_SUPPORTED,
                          0,
                          "Only analog and digital output tasks accept sample blocks");
    }
    if (status.code != HAL_ADAPTER_OK) {
        if (status.vendorCode == -200290) task->underflowed = true;
        return status;
    }
    if (written != buffer->samplesPerChannel) {
        return makeStatus(HAL_ADAPTER_IO_ERROR, 0, "NI-DAQmx wrote a partial sample block");
    }
    task->totalSamplesPerChannel += written;
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterTaskStatus(HalAdapterTaskHandle handle,
                                                    HalAdapterTaskStatusInfo* output,
                                                    int)
{
    auto* task = static_cast<TaskState*>(handle);
    if (task == nullptr || output == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    std::lock_guard<std::mutex> lock(task->mutex);
    bool32 done = 0;
    HalAdapterStatus status = fromDaq(DAQmxIsTaskDone(task->native, &done),
                                      "DAQmxIsTaskDone");
    if (status.code != HAL_ADAPTER_OK) return status;
    uInt32 available = 0;
    const bool input = task->kind == HAL_ADAPTER_TASK_ANALOG_INPUT ||
        task->kind == HAL_ADAPTER_TASK_DIGITAL_INPUT ||
        task->kind == HAL_ADAPTER_TASK_COUNTER_INPUT;
    if (input) {
        status = fromDaq(DAQmxGetReadAvailSampPerChan(task->native, &available),
                         "DAQmxGetReadAvailSampPerChan");
    } else if (task->kind == HAL_ADAPTER_TASK_COUNTER_OUTPUT) {
        status = makeStatus();
    } else {
        status = fromDaq(DAQmxGetWriteSpaceAvail(task->native, &available),
                         "DAQmxGetWriteSpaceAvail");
    }
    if (status.code != HAL_ADAPTER_OK) return status;
    *output = HalAdapterTaskStatusInfo{};
    output->started = task->started ? 1 : 0;
    output->done = done != 0 ? 1 : 0;
    output->availableSamplesPerChannel = static_cast<int>(std::min<uInt32>(
        available, static_cast<uInt32>(std::numeric_limits<int>::max())));
    output->totalSamplesPerChannel = task->totalSamplesPerChannel;
    output->overflowed = task->overflowed ? 1 : 0;
    output->underflowed = task->underflowed ? 1 : 0;
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterTaskStop(HalAdapterTaskHandle handle, int)
{
    auto* task = static_cast<TaskState*>(handle);
    if (task == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    std::lock_guard<std::mutex> lock(task->mutex);
    return stopTaskNative(task);
}

HalAdapterStatus HAL_ADAPTER_CALL adapterTaskClose(HalAdapterTaskHandle handle)
{
    return destroyTask(static_cast<TaskState*>(handle), true);
}

} // namespace

extern "C" int HAL_ADAPTER_CALL hal_adapter_get_api_v1(
    const HalAdapterHostApiV1* host,
    HalAdapterApiV1* output)
{
    if (host == nullptr || output == nullptr ||
        host->abiVersion != HAL_ADAPTER_ABI_VERSION) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(globalHostMutex);
        globalHost = *host;
    }
    *output = HalAdapterApiV1{};
    output->abiVersion = HAL_ADAPTER_ABI_VERSION;
    output->structSize = static_cast<int>(sizeof(HalAdapterApiV1));
    output->getInfo = &adapterGetInfo;
    output->initialize = &adapterInitialize;
    output->shutdown = &adapterShutdown;
    output->enumerateDevices = &adapterEnumerate;
    output->openDevice = &adapterOpenDevice;
    output->closeDevice = &adapterCloseDevice;
    output->resetDevice = &adapterResetDevice;
    output->getCapabilities = &adapterCapabilities;
    output->analogConfigure = &adapterAnalogConfigure;
    output->analogRead = &adapterAnalogRead;
    output->analogWrite = &adapterAnalogWrite;
    output->digitalRead = &adapterDigitalRead;
    output->digitalWrite = &adapterDigitalWrite;
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
    {
        std::lock_guard<std::mutex> lock(globalHostMutex);
        globalHost = *host;
    }
    *output = HalAdapterTaskApiV1{};
    output->abiVersion = HAL_ADAPTER_TASK_ABI_VERSION;
    output->structSize = static_cast<int>(sizeof(HalAdapterTaskApiV1));
    output->createTask = &adapterTaskCreate;
    output->startTask = &adapterTaskStart;
    output->readTask = &adapterTaskRead;
    output->writeTask = &adapterTaskWrite;
    output->getTaskStatus = &adapterTaskStatus;
    output->stopTask = &adapterTaskStop;
    output->closeTask = &adapterTaskClose;
    return 0;
}
