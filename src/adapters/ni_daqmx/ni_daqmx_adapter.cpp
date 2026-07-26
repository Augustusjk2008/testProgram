#include "hal/hal_adapter_abi.h"

#include <NIDAQmx.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

HalAdapterStatus makeStatus(int code = HAL_ADAPTER_OK,
                            int vendorCode = 0,
                            const std::string& message = {})
{
    HalAdapterStatus result{};
    result.code = code;
    result.vendorCode = vendorCode;
    std::snprintf(result.message,
                  sizeof(result.message),
                  "%s",
                  message.c_str());
    return result;
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

class JsonValue {
public:
    enum class Type { Null, Boolean, Number, String, Object, Array };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;

    const JsonValue* member(const char* key) const
    {
        if (type != Type::Object || key == nullptr) return nullptr;
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const char* text)
        : m_begin(text == nullptr ? "" : text)
        , m_current(m_begin)
    {
    }

    bool parse(JsonValue* output, std::string* error)
    {
        if (output == nullptr) return fail("JSON output is null", error);
        skipSpace();
        if (!parseValue(output, error)) return false;
        skipSpace();
        if (*m_current != '\0') return fail("Unexpected trailing JSON data", error);
        return true;
    }

private:
    bool parseValue(JsonValue* output, std::string* error)
    {
        skipSpace();
        switch (*m_current) {
        case '{': return parseObject(output, error);
        case '[': return parseArray(output, error);
        case '"':
            output->type = JsonValue::Type::String;
            return parseString(&output->string, error);
        case 't': return parseLiteral("true", JsonValue::Type::Boolean, true, output, error);
        case 'f': return parseLiteral("false", JsonValue::Type::Boolean, false, output, error);
        case 'n': return parseLiteral("null", JsonValue::Type::Null, false, output, error);
        default:
            if (*m_current == '-' || std::isdigit(static_cast<unsigned char>(*m_current)) != 0) {
                return parseNumber(output, error);
            }
            return fail("Unexpected JSON token", error);
        }
    }

    bool parseObject(JsonValue* output, std::string* error)
    {
        output->type = JsonValue::Type::Object;
        output->object.clear();
        ++m_current;
        skipSpace();
        if (*m_current == '}') {
            ++m_current;
            return true;
        }
        while (*m_current != '\0') {
            std::string key;
            if (!parseString(&key, error)) return false;
            skipSpace();
            if (*m_current != ':') return fail("JSON object requires ':'", error);
            ++m_current;
            JsonValue value;
            if (!parseValue(&value, error)) return false;
            output->object[key] = std::move(value);
            skipSpace();
            if (*m_current == '}') {
                ++m_current;
                return true;
            }
            if (*m_current != ',') return fail("JSON object requires ','", error);
            ++m_current;
            skipSpace();
        }
        return fail("Unterminated JSON object", error);
    }

    bool parseArray(JsonValue* output, std::string* error)
    {
        output->type = JsonValue::Type::Array;
        output->array.clear();
        ++m_current;
        skipSpace();
        if (*m_current == ']') {
            ++m_current;
            return true;
        }
        while (*m_current != '\0') {
            JsonValue value;
            if (!parseValue(&value, error)) return false;
            output->array.push_back(std::move(value));
            skipSpace();
            if (*m_current == ']') {
                ++m_current;
                return true;
            }
            if (*m_current != ',') return fail("JSON array requires ','", error);
            ++m_current;
            skipSpace();
        }
        return fail("Unterminated JSON array", error);
    }

    bool parseString(std::string* output, std::string* error)
    {
        if (*m_current != '"') return fail("JSON string expected", error);
        ++m_current;
        output->clear();
        while (*m_current != '\0' && *m_current != '"') {
            const unsigned char current = static_cast<unsigned char>(*m_current++);
            if (current < 0x20u) return fail("Control character in JSON string", error);
            if (current != '\\') {
                output->push_back(static_cast<char>(current));
                continue;
            }
            const char escaped = *m_current++;
            switch (escaped) {
            case '"': output->push_back('"'); break;
            case '\\': output->push_back('\\'); break;
            case '/': output->push_back('/'); break;
            case 'b': output->push_back('\b'); break;
            case 'f': output->push_back('\f'); break;
            case 'n': output->push_back('\n'); break;
            case 'r': output->push_back('\r'); break;
            case 't': output->push_back('\t'); break;
            case 'u': {
                unsigned int codePoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const char hex = *m_current++;
                    if (hex >= '0' && hex <= '9') codePoint = codePoint * 16u + (hex - '0');
                    else if (hex >= 'a' && hex <= 'f') codePoint = codePoint * 16u + (hex - 'a' + 10u);
                    else if (hex >= 'A' && hex <= 'F') codePoint = codePoint * 16u + (hex - 'A' + 10u);
                    else return fail("Invalid JSON unicode escape", error);
                }
                output->push_back(codePoint <= 0x7fu ? static_cast<char>(codePoint) : '?');
                break;
            }
            default: return fail("Invalid JSON escape", error);
            }
        }
        if (*m_current != '"') return fail("Unterminated JSON string", error);
        ++m_current;
        return true;
    }

    bool parseNumber(JsonValue* output, std::string* error)
    {
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(m_current, &end);
        if (end == m_current || errno == ERANGE || !std::isfinite(value)) {
            return fail("Invalid JSON number", error);
        }
        m_current = end;
        output->type = JsonValue::Type::Number;
        output->number = value;
        return true;
    }

    bool parseLiteral(const char* literal,
                      JsonValue::Type type,
                      bool boolean,
                      JsonValue* output,
                      std::string* error)
    {
        const std::size_t length = std::strlen(literal);
        if (std::strncmp(m_current, literal, length) != 0) {
            return fail("Invalid JSON literal", error);
        }
        m_current += length;
        output->type = type;
        output->boolean = boolean;
        return true;
    }

    void skipSpace()
    {
        while (std::isspace(static_cast<unsigned char>(*m_current)) != 0) ++m_current;
    }

    bool fail(const char* message, std::string* error) const
    {
        if (error != nullptr) {
            *error = std::string(message) + " at byte " +
                std::to_string(static_cast<long long>(m_current - m_begin));
        }
        return false;
    }

    const char* m_begin = nullptr;
    const char* m_current = nullptr;
};

bool jsonString(const JsonValue* value, std::string* output)
{
    if (value == nullptr || output == nullptr || value->type != JsonValue::Type::String) {
        return false;
    }
    *output = value->string;
    return true;
}

bool jsonInteger(const JsonValue* value, int* output)
{
    if (value == nullptr || output == nullptr || value->type != JsonValue::Type::Number ||
        std::floor(value->number) != value->number ||
        value->number < static_cast<double>(std::numeric_limits<int>::min()) ||
        value->number > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    *output = static_cast<int>(value->number);
    return true;
}

struct ChannelConfig {
    std::string resourceId;
    int physicalIndex = -1;
    int portNumber = -1;
    int lineNumber = -1;
    bool output = false;
    bool safeHigh = false;
};

struct AdapterConfig {
    std::string deviceName;
    std::string expectedProductType;
    std::string serialNumber;
    double timeoutSeconds = 1.0;
    std::vector<ChannelConfig> channels;
};

bool parseConfig(const char* json, AdapterConfig* config, std::string* error)
{
    JsonValue root;
    if (!JsonParser(json).parse(&root, error) || root.type != JsonValue::Type::Object) {
        if (error != nullptr && error->empty()) *error = "Adapter config must be a JSON object";
        return false;
    }
    const JsonValue* settings = root.member("settings");
    if (settings == nullptr || settings->type != JsonValue::Type::Object ||
        !jsonString(settings->member("deviceName"), &config->deviceName) ||
        !jsonString(settings->member("expectedProductType"), &config->expectedProductType) ||
        !jsonString(settings->member("serialNumber"), &config->serialNumber)) {
        if (error != nullptr) {
            *error = "settings.deviceName, expectedProductType and serialNumber are required strings";
        }
        return false;
    }
    config->deviceName = trim(config->deviceName);
    config->expectedProductType = trim(config->expectedProductType);
    config->serialNumber = trim(config->serialNumber);
    if (config->deviceName.empty() || config->expectedProductType.empty() ||
        config->serialNumber.empty() ||
        lower(config->serialNumber) == "configure_me") {
        if (error != nullptr) *error = "NI device identity settings must be explicitly configured";
        return false;
    }
    const JsonValue* timeout = settings->member("timeoutSeconds");
    if (timeout != nullptr) {
        if (timeout->type != JsonValue::Type::Number || timeout->number <= 0.0 ||
            !std::isfinite(timeout->number)) {
            if (error != nullptr) *error = "settings.timeoutSeconds must be positive";
            return false;
        }
        config->timeoutSeconds = timeout->number;
    }

    const JsonValue* hardware = root.member("hardware");
    const JsonValue* devices = hardware == nullptr ? nullptr : hardware->member("devices");
    const JsonValue* resources = hardware == nullptr ? nullptr : hardware->member("resources");
    const JsonValue* safeState = root.member("safeState");
    if (hardware == nullptr || hardware->type != JsonValue::Type::Object ||
        devices == nullptr || devices->type != JsonValue::Type::Array ||
        devices->array.size() != 1 ||
        resources == nullptr || resources->type != JsonValue::Type::Object ||
        safeState == nullptr || safeState->type != JsonValue::Type::Object) {
        if (error != nullptr) {
            *error = "One hardware device, hardware.resources and safeState objects are required";
        }
        return false;
    }

    const JsonValue& configuredDevice = devices->array.front();
    std::string logicalDeviceId;
    std::string configuredModel;
    std::string configuredSerial;
    if (configuredDevice.type != JsonValue::Type::Object ||
        !jsonString(configuredDevice.member("alias"), &logicalDeviceId) ||
        !jsonString(configuredDevice.member("model"), &configuredModel) ||
        !jsonString(configuredDevice.member("serialNumber"), &configuredSerial)) {
        if (error != nullptr) {
            *error = "The NI hardware device requires alias, model and serialNumber strings";
        }
        return false;
    }
    logicalDeviceId = trim(logicalDeviceId);
    configuredModel = trim(configuredModel);
    configuredSerial = trim(configuredSerial);
    if (logicalDeviceId.empty() ||
        lower(configuredModel) != lower(config->expectedProductType) ||
        lower(configuredSerial) != lower(config->serialNumber)) {
        if (error != nullptr) {
            *error = "NI hardware device identity must match adapter settings";
        }
        return false;
    }

    std::set<int> physicalIndexes;
    std::set<std::pair<int, int>> physicalLines;
    for (const auto& entry : resources->object) {
        const JsonValue& resource = entry.second;
        if (resource.type != JsonValue::Type::Object) continue;
        std::string module;
        if (!jsonString(resource.member("module"), &module) || lower(trim(module)) != "digital") {
            continue;
        }
        std::string direction;
        std::string resourceDevice;
        int physicalIndex = -1;
        const JsonValue* properties = resource.member("properties");
        int portNumber = -1;
        int lineNumber = -1;
        if (!jsonString(resource.member("device"), &resourceDevice) ||
            trim(resourceDevice) != logicalDeviceId ||
            !jsonString(resource.member("direction"), &direction) ||
            !jsonInteger(resource.member("physicalIndex"), &physicalIndex) ||
            properties == nullptr || properties->type != JsonValue::Type::Object ||
            !jsonInteger(properties->member("portNumber"), &portNumber) ||
            !jsonInteger(properties->member("lineNumber"), &lineNumber)) {
            if (error != nullptr) {
                *error = "Every digital resource must belong to the configured NI device and provide direction, physicalIndex, portNumber and lineNumber";
            }
            return false;
        }
        direction = lower(trim(direction));
        const bool usb6259 = lower(config->expectedProductType) == "usb-6259";
        const bool validUsb6259Line = !usb6259 ||
            (portNumber == 0 && lineNumber >= 0 && lineNumber <= 31) ||
            ((portNumber == 1 || portNumber == 2) &&
             lineNumber >= 0 && lineNumber <= 7);
        if ((direction != "output" && direction != "input") ||
            physicalIndex < 0 || portNumber < 0 || lineNumber < 0 ||
            lineNumber > 31 || !validUsb6259Line) {
            if (error != nullptr) *error = "Digital direction or physical channel is invalid";
            return false;
        }
        if (!physicalIndexes.insert(physicalIndex).second ||
            !physicalLines.insert({portNumber, lineNumber}).second) {
            if (error != nullptr) *error = "Digital physicalIndex and port/line mappings must be unique";
            return false;
        }
        ChannelConfig channel;
        channel.resourceId = entry.first;
        channel.physicalIndex = physicalIndex;
        channel.portNumber = portNumber;
        channel.lineNumber = lineNumber;
        channel.output = direction == "output";
        if (channel.output) {
            std::string level;
            if (!jsonString(safeState->member(entry.first.c_str()), &level)) {
                if (error != nullptr) *error = "Every digital output requires a safeState";
                return false;
            }
            level = lower(trim(level));
            if (level != "low" && level != "high") {
                if (error != nullptr) *error = "Digital safeState must be Low or High";
                return false;
            }
            channel.safeHigh = level == "high";
        }
        config->channels.push_back(channel);
    }
    if (config->channels.empty()) {
        if (error != nullptr) *error = "At least one digital resource is required";
        return false;
    }

    std::map<std::pair<int, bool>, std::vector<int>> linesByBank;
    for (const ChannelConfig& channel : config->channels) {
        linesByBank[{channel.portNumber, channel.output}].push_back(channel.lineNumber);
    }
    for (auto& entry : linesByBank) {
        std::sort(entry.second.begin(), entry.second.end());
        for (std::size_t index = 1; index < entry.second.size(); ++index) {
            if (entry.second[index] != entry.second[index - 1] + 1) {
                if (error != nullptr) {
                    *error = "Each NI digital port bank must use a contiguous line range";
                }
                return false;
            }
        }
    }
    return true;
}

std::string daqErrorText(int32 code)
{
    char message[2048]{};
    if (DAQmxGetExtendedErrorInfo(message, static_cast<uInt32>(sizeof(message))) >= 0 &&
        message[0] != '\0') {
        return message;
    }
    message[0] = '\0';
    if (DAQmxGetErrorString(code, message, static_cast<uInt32>(sizeof(message))) >= 0 &&
        message[0] != '\0') {
        return message;
    }
    return std::string("NI-DAQmx error ") + std::to_string(code);
}

HalAdapterStatus fromDaq(int32 code, const char* operation)
{
    if (code >= 0) return makeStatus();
    const std::string detail = daqErrorText(code);
    const std::string normalized = lower(detail);
    int mapped = HAL_ADAPTER_IO_ERROR;
    if (code == -200284 || normalized.find("timeout") != std::string::npos ||
        normalized.find("timed out") != std::string::npos) {
        mapped = HAL_ADAPTER_TIMEOUT;
    } else if (code == -88705 ||
               normalized.find("disconnected") != std::string::npos ||
               normalized.find("removed") != std::string::npos ||
               normalized.find("not present") != std::string::npos ||
               normalized.find("not active") != std::string::npos) {
        mapped = HAL_ADAPTER_DEVICE_DISCONNECTED;
    } else if (code == -200220 || normalized.find("not found") != std::string::npos ||
               normalized.find("invalid device") != std::string::npos) {
        mapped = HAL_ADAPTER_NOT_FOUND;
    }
    return makeStatus(mapped,
                      code,
                      std::string(operation == nullptr ? "NI-DAQmx" : operation) +
                          ": " + detail);
}

bool containsDevice(const std::string& devices, const std::string& expected)
{
    std::size_t start = 0;
    while (start <= devices.size()) {
        const std::size_t comma = devices.find(',', start);
        const std::string item = trim(devices.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        if (item == expected) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

bool serialMatches(const std::string& expected, uInt32 actual)
{
    const std::string normalized = lower(trim(expected));
    if (normalized == std::to_string(actual)) return true;
    char hexadecimal[32]{};
    std::snprintf(hexadecimal, sizeof(hexadecimal), "%X", actual);
    std::string expectedHex = normalized;
    if (expectedHex.rfind("0x", 0) == 0) expectedHex.erase(0, 2);
    return lower(hexadecimal) == expectedHex;
}

HalAdapterHostApiV1 globalHost{};
std::mutex globalHostMutex;

struct Bank {
    int portNumber = -1;
    int firstLine = -1;
    int lastLine = -1;
    bool output = false;
    TaskHandle task = nullptr;
    bool started = false;
    uInt32 safeMask = 0;
    uInt32 appliedMask = 0;
};

struct ChannelLocation {
    std::size_t bank = 0;
    unsigned int bit = 0;
};

struct AdapterState;

struct DeviceState {
    AdapterState* owner = nullptr;
    std::vector<Bank> banks;
    std::map<int, ChannelLocation> outputs;
    std::map<int, ChannelLocation> inputs;
    std::mutex mutex;
};

struct AdapterState {
    AdapterConfig config;
    HalAdapterHostApiV1 host{};
    std::string productType;
    uInt32 serialNumber = 0;
    std::vector<DeviceState*> devices;
    std::mutex mutex;
};

double timeoutSeconds(const AdapterState* adapter, int timeoutMs)
{
    if (timeoutMs > 0) return static_cast<double>(timeoutMs) / 1000.0;
    return adapter == nullptr ? 1.0 : adapter->config.timeoutSeconds;
}

HalAdapterStatus writeBank(AdapterState* adapter,
                           Bank* bank,
                           uInt32 mask,
                           int timeoutMs)
{
    int32 written = 0;
    const int32 code = DAQmxWriteDigitalU32(
        bank->task,
        1,
        0,
        timeoutSeconds(adapter, timeoutMs),
        DAQmx_Val_GroupByChannel,
        &mask,
        &written,
        nullptr);
    HalAdapterStatus status = fromDaq(code, "DAQmxWriteDigitalU32");
    if (status.code == HAL_ADAPTER_OK && written != 1) {
        status = makeStatus(HAL_ADAPTER_IO_ERROR,
                            0,
                            "DAQmxWriteDigitalU32 wrote no complete port sample");
    }
    if (status.code == HAL_ADAPTER_OK) bank->appliedMask = mask;
    return status;
}

HalAdapterStatus rememberFirst(HalAdapterStatus first, HalAdapterStatus next)
{
    return first.code == HAL_ADAPTER_OK && next.code != HAL_ADAPTER_OK ? next : first;
}

HalAdapterStatus cleanupDevice(DeviceState* device, bool writeSafe, int timeoutMs)
{
    HalAdapterStatus first = makeStatus();
    if (device == nullptr) return first;
    std::lock_guard<std::mutex> lock(device->mutex);
    for (Bank& bank : device->banks) {
        if (writeSafe && bank.output && bank.task != nullptr) {
            first = rememberFirst(first,
                                  writeBank(device->owner,
                                            &bank,
                                            bank.safeMask,
                                            timeoutMs));
        }
        if (bank.started && bank.task != nullptr) {
            first = rememberFirst(first,
                                  fromDaq(DAQmxStopTask(bank.task),
                                          "DAQmxStopTask"));
            bank.started = false;
        }
        if (bank.task != nullptr) {
            first = rememberFirst(first,
                                  fromDaq(DAQmxClearTask(bank.task),
                                          "DAQmxClearTask"));
            bank.task = nullptr;
        }
    }
    return first;
}

HalAdapterStatus HAL_ADAPTER_CALL adapterGetInfo(HalAdapterInfo* output)
{
    if (output == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    *output = HalAdapterInfo{};
    std::snprintf(output->adapterId, sizeof(output->adapterId), "ni.daqmx");
    std::snprintf(output->vendor, sizeof(output->vendor), "National Instruments");
    std::snprintf(output->name, sizeof(output->name), "HWTest NI-DAQmx Adapter");
    std::snprintf(output->version, sizeof(output->version), "1.0.0");
    output->supportedModulesMask = HAL_MODULE_DIGITAL;
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
        auto state = std::make_unique<AdapterState>();
        std::string error;
        if (!parseConfig(configJson, &state->config, &error)) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT, 0, error);
        }
        {
            std::lock_guard<std::mutex> lock(globalHostMutex);
            state->host = globalHost;
        }

        char devices[4096]{};
        HalAdapterStatus status = fromDaq(
            DAQmxGetSysDevNames(devices, static_cast<uInt32>(sizeof(devices))),
            "DAQmxGetSysDevNames");
        if (status.code != HAL_ADAPTER_OK) return status;
        if (!containsDevice(devices, state->config.deviceName)) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "Configured NI device is not present: " +
                                  state->config.deviceName);
        }

        char product[HAL_ADAPTER_MAX_TEXT]{};
        status = fromDaq(
            DAQmxGetDevProductType(state->config.deviceName.c_str(),
                                   product,
                                   static_cast<uInt32>(sizeof(product))),
            "DAQmxGetDevProductType");
        if (status.code != HAL_ADAPTER_OK) return status;
        state->productType = trim(product);
        if (lower(state->productType) != lower(state->config.expectedProductType)) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "NI product type mismatch; expected '" +
                                  state->config.expectedProductType + "' but found '" +
                                  state->productType + "'");
        }

        status = fromDaq(
            DAQmxGetDevSerialNum(state->config.deviceName.c_str(),
                                 &state->serialNumber),
            "DAQmxGetDevSerialNum");
        if (status.code != HAL_ADAPTER_OK) return status;
        if (!serialMatches(state->config.serialNumber, state->serialNumber)) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "NI serial number does not match the deployment configuration");
        }
        *output = state.release();
        return makeStatus();
    } catch (const std::exception& exception) {
        return makeStatus(HAL_ADAPTER_INTERNAL_ERROR, 0, exception.what());
    } catch (...) {
        return makeStatus(HAL_ADAPTER_INTERNAL_ERROR,
                          0,
                          "Unexpected exception while initializing NI-DAQmx adapter");
    }
}

HalAdapterStatus HAL_ADAPTER_CALL adapterCloseDevice(HalAdapterDeviceHandle handle);

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
    if (adapter == nullptr || count == nullptr) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    if (output == nullptr || *count < 1) {
        *count = 1;
        return makeStatus();
    }
    output[0] = HalAdapterDeviceInfo{};
    std::snprintf(output[0].deviceId,
                  sizeof(output[0].deviceId),
                  "%s",
                  adapter->config.deviceName.c_str());
    std::snprintf(output[0].model,
                  sizeof(output[0].model),
                  "%s",
                  adapter->productType.c_str());
    std::snprintf(output[0].serialNumber,
                  sizeof(output[0].serialNumber),
                  "%u",
                  adapter->serialNumber);
    output[0].supportedModulesMask = HAL_MODULE_DIGITAL;
    std::snprintf(output[0].propertiesJson,
                  sizeof(output[0].propertiesJson),
                  "{\"driver\":\"NI-DAQmx\",\"deviceName\":\"%s\"}",
                  adapter->config.deviceName.c_str());
    *count = 1;
    return makeStatus();
}

HalAdapterStatus HAL_ADAPTER_CALL adapterOpenDevice(HalAdapterHandle handle,
                                                    const char* deviceId,
                                                    const char*,
                                                    HalAdapterDeviceHandle* output)
{
    auto* adapter = static_cast<AdapterState*>(handle);
    if (adapter == nullptr || deviceId == nullptr || output == nullptr) {
        return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    }
    *output = nullptr;
    if (adapter->config.deviceName != deviceId) {
        return makeStatus(HAL_ADAPTER_NOT_FOUND,
                          0,
                          "Requested NI device does not match configured deviceName");
    }
    try {
        auto device = std::make_unique<DeviceState>();
        device->owner = adapter;
        std::map<std::pair<int, bool>, std::vector<ChannelConfig>> grouped;
        for (const ChannelConfig& channel : adapter->config.channels) {
            grouped[{channel.portNumber, channel.output}].push_back(channel);
        }
        for (auto& entry : grouped) {
            auto& channels = entry.second;
            std::sort(channels.begin(), channels.end(), [](const ChannelConfig& left,
                                                           const ChannelConfig& right) {
                return left.lineNumber < right.lineNumber;
            });
            Bank bank;
            bank.portNumber = entry.first.first;
            bank.output = entry.first.second;
            bank.firstLine = channels.front().lineNumber;
            bank.lastLine = channels.back().lineNumber;
            const std::size_t bankIndex = device->banks.size();
            for (const ChannelConfig& channel : channels) {
                const unsigned int bit = static_cast<unsigned int>(
                    channel.lineNumber - bank.firstLine);
                if (channel.output) {
                    device->outputs[channel.physicalIndex] = {bankIndex, bit};
                    if (channel.safeHigh) bank.safeMask |= (uInt32{1} << bit);
                } else {
                    device->inputs[channel.physicalIndex] = {bankIndex, bit};
                }
            }
            bank.appliedMask = bank.safeMask;
            device->banks.push_back(bank);
        }

        for (Bank& bank : device->banks) {
            HalAdapterStatus status = fromDaq(DAQmxCreateTask("", &bank.task),
                                              "DAQmxCreateTask");
            if (status.code != HAL_ADAPTER_OK) {
                cleanupDevice(device.get(), true, 0);
                return status;
            }
            std::string physical = adapter->config.deviceName + "/port" +
                std::to_string(bank.portNumber) + "/line" +
                std::to_string(bank.firstLine);
            if (bank.lastLine != bank.firstLine) {
                physical += ":" + std::to_string(bank.lastLine);
            }
            const int32 channelCode = bank.output
                ? DAQmxCreateDOChan(bank.task,
                                    physical.c_str(),
                                    "",
                                    DAQmx_Val_ChanForAllLines)
                : DAQmxCreateDIChan(bank.task,
                                    physical.c_str(),
                                    "",
                                    DAQmx_Val_ChanForAllLines);
            status = fromDaq(channelCode,
                             bank.output ? "DAQmxCreateDOChan" : "DAQmxCreateDIChan");
            if (status.code != HAL_ADAPTER_OK) {
                cleanupDevice(device.get(), true, 0);
                return status;
            }
            status = fromDaq(DAQmxStartTask(bank.task), "DAQmxStartTask");
            if (status.code != HAL_ADAPTER_OK) {
                cleanupDevice(device.get(), true, 0);
                return status;
            }
            bank.started = true;
            if (bank.output) {
                status = writeBank(adapter, &bank, bank.safeMask, 0);
                if (status.code != HAL_ADAPTER_OK) {
                    cleanupDevice(device.get(), true, 0);
                    return status;
                }
            }
        }
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
    AdapterState* adapter = device->owner;
    const HalAdapterStatus status = cleanupDevice(device, true, 0);
    if (adapter != nullptr) {
        std::lock_guard<std::mutex> lock(adapter->mutex);
        const auto it = std::find(adapter->devices.begin(),
                                  adapter->devices.end(),
                                  device);
        if (it != adapter->devices.end()) adapter->devices.erase(it);
    }
    delete device;
    return status;
}

HalAdapterStatus HAL_ADAPTER_CALL adapterResetDevice(HalAdapterDeviceHandle handle,
                                                     int timeoutMs)
{
    auto* device = static_cast<DeviceState*>(handle);
    if (device == nullptr) return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT);
    std::lock_guard<std::mutex> lock(device->mutex);
    HalAdapterStatus first = makeStatus();
    for (Bank& bank : device->banks) {
        if (bank.output) {
            first = rememberFirst(first,
                                  writeBank(device->owner,
                                            &bank,
                                            bank.safeMask,
                                            timeoutMs));
        }
    }
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
    const std::string json = std::string("{\"supportedModules\":[\"digital\"],") +
        "\"limits\":{\"digital.outputChannels\":" +
        std::to_string(device->outputs.size()) +
        ",\"digital.inputChannels\":" +
        std::to_string(device->inputs.size()) + "}}";
    const int required = static_cast<int>(json.size() + 1);
    if (output == nullptr || *bytes < required) {
        *bytes = required;
        return makeStatus(HAL_ADAPTER_BUFFER_TOO_SMALL);
    }
    std::memcpy(output, json.c_str(), static_cast<std::size_t>(required));
    *bytes = required;
    return makeStatus();
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
    std::lock_guard<std::mutex> lock(device->mutex);
    std::map<std::size_t, uInt32> staged;
    std::set<int> seen;
    for (int index = 0; index < count; ++index) {
        if ((levels[index] != 0 && levels[index] != 1) ||
            !seen.insert(indexes[index]).second) {
            return makeStatus(HAL_ADAPTER_INVALID_ARGUMENT,
                              0,
                              "Digital writes require unique indexes and Low/High levels");
        }
        const auto found = device->outputs.find(indexes[index]);
        if (found == device->outputs.end()) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "Digital output physicalIndex is not configured");
        }
        const ChannelLocation location = found->second;
        auto inserted = staged.emplace(location.bank,
                                       device->banks.at(location.bank).appliedMask);
        const uInt32 bit = uInt32{1} << location.bit;
        if (levels[index] == 1) inserted.first->second |= bit;
        else inserted.first->second &= ~bit;
    }
    for (const auto& entry : staged) {
        HalAdapterStatus status = writeBank(device->owner,
                                            &device->banks.at(entry.first),
                                            entry.second,
                                            timeoutMs);
        if (status.code != HAL_ADAPTER_OK) return status;
    }
    return makeStatus();
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
    std::lock_guard<std::mutex> lock(device->mutex);
    std::map<std::size_t, uInt32> masks;
    std::vector<ChannelLocation> locations;
    locations.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const auto found = device->inputs.find(indexes[index]);
        if (found == device->inputs.end()) {
            return makeStatus(HAL_ADAPTER_NOT_FOUND,
                              0,
                              "Digital input physicalIndex is not configured");
        }
        locations.push_back(found->second);
        masks.emplace(found->second.bank, 0u);
    }
    for (auto& entry : masks) {
        int32 samplesRead = 0;
        const int32 code = DAQmxReadDigitalU32(
            device->banks.at(entry.first).task,
            1,
            timeoutSeconds(device->owner, timeoutMs),
            DAQmx_Val_GroupByChannel,
            &entry.second,
            1,
            &samplesRead,
            nullptr);
        HalAdapterStatus status = fromDaq(code, "DAQmxReadDigitalU32");
        if (status.code != HAL_ADAPTER_OK) return status;
        if (samplesRead != 1) {
            return makeStatus(HAL_ADAPTER_IO_ERROR,
                              0,
                              "DAQmxReadDigitalU32 returned no complete port sample");
        }
    }
    const long long timestamp = device->owner != nullptr &&
            device->owner->host.nowUs != nullptr
        ? device->owner->host.nowUs()
        : 0;
    for (int index = 0; index < count; ++index) {
        output[index] = HalAdapterDigitalSample{};
        output[index].channelIndex = indexes[index];
        output[index].level =
            (masks.at(locations.at(static_cast<std::size_t>(index)).bank) &
             (uInt32{1} << locations.at(static_cast<std::size_t>(index)).bit)) != 0u
            ? 1
            : 0;
        output[index].timestampUs = timestamp;
    }
    return makeStatus();
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
    output->digitalRead = &adapterDigitalRead;
    output->digitalWrite = &adapterDigitalWrite;
    return 0;
}
