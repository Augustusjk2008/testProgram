#include "ni_daqmx_config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace hwtest::adapters::ni_daqmx {
namespace {

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

const DeviceProfile kPxi6259Profile{
    DeviceModel::Pxi6259,
    "PXI-6259",
    32,
    4,
    16,
    2860000.0,
    true,
    true,
    false,
};

const DeviceProfile kPxi6733Profile{
    DeviceModel::Pxi6733,
    "PXI-6733",
    0,
    8,
    16,
    1000000.0,
    false,
    false,
    true,
};

class JsonValue {
public:
    enum class Type { Null, Boolean, Number, String, Object, Array };

    const JsonValue* member(const char* key) const
    {
        if (type != Type::Object || key == nullptr) return nullptr;
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
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
        return *m_current == '\0' || fail("Unexpected trailing JSON data", error);
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
            if (!output->object.emplace(std::move(key), std::move(value)).second) {
                return fail("Duplicate JSON object key", error);
            }
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
                    if (*m_current == '\0') {
                        return fail("Truncated JSON unicode escape", error);
                    }
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

bool stringValue(const JsonValue* value, std::string* output)
{
    if (value == nullptr || output == nullptr || value->type != JsonValue::Type::String) {
        return false;
    }
    *output = value->string;
    return true;
}

bool integerValue(const JsonValue* value, int* output)
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

bool optionalNumber(const JsonValue* object,
                    const char* key,
                    double* output,
                    std::string* error)
{
    const JsonValue* value = object == nullptr ? nullptr : object->member(key);
    if (value == nullptr) return true;
    if (value->type != JsonValue::Type::Number || !std::isfinite(value->number)) {
        if (error != nullptr) *error = std::string(key) + " must be a finite number";
        return false;
    }
    *output = value->number;
    return true;
}

bool fail(std::string* error, const std::string& message)
{
    if (error != nullptr) *error = message;
    return false;
}

bool isPlaceholder(const std::string& value)
{
    return lower(trim(value)) == "configure_me";
}

bool parseDirection(const std::string& text, ChannelDirection* direction)
{
    const std::string normalized = lower(trim(text));
    if (normalized == "input") {
        *direction = ChannelDirection::Input;
        return true;
    }
    if (normalized == "output") {
        *direction = ChannelDirection::Output;
        return true;
    }
    return false;
}

bool parseModule(const std::string& text, ChannelModule* module)
{
    const std::string normalized = lower(trim(text));
    if (normalized == "analog") {
        *module = ChannelModule::Analog;
        return true;
    }
    if (normalized == "digital") {
        *module = ChannelModule::Digital;
        return true;
    }
    if (normalized == "counter") {
        *module = ChannelModule::Counter;
        return true;
    }
    return false;
}

bool parseSafeValue(const JsonValue* safeState,
                    ChannelConfig* channel,
                    std::string* error)
{
    const JsonValue* value = safeState == nullptr
        ? nullptr
        : safeState->member(channel->resourceId.c_str());
    if (channel->direction != ChannelDirection::Output) {
        if (value != nullptr) {
            return fail(error, "safeState may contain output resources only");
        }
        return true;
    }
    if (channel->module == ChannelModule::Counter) {
        if (value != nullptr) {
            return fail(error, "Counter outputs stop their pulse task and do not accept scalar safeState");
        }
        return true;
    }
    if (value == nullptr) {
        return fail(error, "Every configured output requires a safeState value");
    }
    channel->hasSafeValue = true;
    if (channel->module == ChannelModule::Digital) {
        if (value->type != JsonValue::Type::String) {
            return fail(error, "Digital safeState must be Low or High");
        }
        const std::string level = lower(trim(value->string));
        if (level != "low" && level != "high") {
            return fail(error, "Digital safeState must be Low or High");
        }
        channel->safeHigh = level == "high";
        return true;
    }
    if (channel->module == ChannelModule::Analog) {
        if (value->type != JsonValue::Type::Number || !std::isfinite(value->number) ||
            value->number < channel->minValue || value->number > channel->maxValue) {
            return fail(error, "Analog safeState must be within the configured output range");
        }
        channel->safeAnalog = value->number;
        return true;
    }
    return false;
}

bool validateTopology(const DeviceProfile& profile,
                      const ChannelConfig& channel,
                      std::string* error)
{
    if (profile.model == DeviceModel::Pxi6733) {
        if (channel.module != ChannelModule::Analog ||
            channel.direction != ChannelDirection::Output) {
            return fail(error, std::string(profile.name) +
                                   " supports analog output channels only");
        }
        if (channel.channelNumber < 0 ||
            channel.channelNumber >= profile.analogOutputChannels) {
            return fail(error, std::string(profile.name) +
                                   " analog output channel must be ao0..ao" +
                                   std::to_string(profile.analogOutputChannels - 1));
        }
        if (!std::isfinite(channel.minValue) || !std::isfinite(channel.maxValue) ||
            channel.minValue >= channel.maxValue || channel.minValue < -10.0 ||
            channel.maxValue > 10.0) {
            return fail(error, std::string(profile.name) +
                                   " analog range must be ordered within -10..10 V");
        }
        return true;
    }
    switch (channel.module) {
    case ChannelModule::Analog:
        if (channel.direction == ChannelDirection::Input) {
            if (channel.channelNumber < 0 || channel.channelNumber > 31) {
                return fail(error, std::string(profile.name) +
                                       " analog input channel must be ai0..ai31");
            }
        } else if (channel.channelNumber < 0 || channel.channelNumber > 3) {
            return fail(error, std::string(profile.name) +
                                   " analog output channel must be ao0..ao3");
        }
        if (!std::isfinite(channel.minValue) || !std::isfinite(channel.maxValue) ||
            channel.minValue >= channel.maxValue || channel.minValue < -10.0 ||
            channel.maxValue > 10.0) {
            return fail(error, std::string(profile.name) +
                                   " analog range must be ordered within -10..10 V");
        }
        return true;
    case ChannelModule::Digital: {
        const int maxLine = channel.portNumber == 0 ? 31 : 7;
        if (channel.portNumber < 0 || channel.portNumber > 2 ||
            channel.lineNumber < 0 || channel.lineNumber > maxLine) {
            return fail(error, std::string(profile.name) +
                                   " digital channel must be port0/line0..31 or port1..2/line0..7");
        }
        return true;
    }
    case ChannelModule::Counter:
        if (channel.counterNumber < 0 || channel.counterNumber > 1) {
            return fail(error, std::string(profile.name) +
                                   " counter channel must be ctr0 or ctr1");
        }
        return true;
    }
    return false;
}

} // namespace

const DeviceProfile* findDeviceProfile(const std::string& model)
{
    const std::string normalized = lower(trim(model));
    if (normalized == lower(kPxi6259Profile.name)) return &kPxi6259Profile;
    if (normalized == lower(kPxi6733Profile.name)) return &kPxi6733Profile;
    return nullptr;
}

const DeviceProfile& deviceProfile(DeviceModel model)
{
    return model == DeviceModel::Pxi6733 ? kPxi6733Profile : kPxi6259Profile;
}

bool parseDriverConfig(const char* json,
                       DriverConfig* config,
                       std::string* error)
{
    if (config == nullptr) return fail(error, "Driver config output is required");
    JsonValue root;
    if (!JsonParser(json).parse(&root, error) || root.type != JsonValue::Type::Object) {
        return fail(error, error != nullptr && !error->empty()
                               ? *error
                               : "Adapter config must be a JSON object");
    }
    DriverConfig parsed;
    if (root.member("hardware") != nullptr || root.member("safeState") != nullptr ||
        root.member("schema") != nullptr) {
        return fail(error,
                    "Adapter initialization accepts driver settings only; device topology belongs to openDevice");
    }
    const JsonValue* settings = root.member("settings");
    if (settings != nullptr && settings->type != JsonValue::Type::Object) {
        return fail(error, "settings must be an object");
    }
    if (settings != nullptr) {
        const JsonValue* timeout = settings->member("timeoutSeconds");
        if (timeout != nullptr) {
            if (timeout->type != JsonValue::Type::Number || !std::isfinite(timeout->number) ||
                timeout->number <= 0.0) {
                return fail(error, "settings.timeoutSeconds must be positive");
            }
            parsed.timeoutSeconds = timeout->number;
        }
    }
    *config = parsed;
    return true;
}

bool parseDeviceOpenSpec(const char* json,
                         DeviceConfig* config,
                         std::string* error)
{
    if (config == nullptr) return fail(error, "Device config output is required");
    JsonValue root;
    if (!JsonParser(json).parse(&root, error) || root.type != JsonValue::Type::Object) {
        return fail(error, error != nullptr && !error->empty()
                               ? *error
                               : "Device open spec must be a JSON object");
    }
    std::string schema;
    int version = 0;
    if (!stringValue(root.member("schema"), &schema) ||
        schema != "hwtest.adapter-device-open" ||
        !integerValue(root.member("version"), &version) || version != 1) {
        return fail(error, "Device open spec requires schema hwtest.adapter-device-open version 1");
    }
    const JsonValue* device = root.member("device");
    const JsonValue* channels = root.member("channels");
    const JsonValue* safeState = root.member("safeState");
    DeviceConfig parsed;
    parsed.version = version;
    std::string adapterId;
    if (device == nullptr || device->type != JsonValue::Type::Object ||
        channels == nullptr || channels->type != JsonValue::Type::Array ||
        safeState == nullptr || safeState->type != JsonValue::Type::Object ||
        !stringValue(root.member("physicalDeviceId"), &parsed.physicalDeviceId) ||
        !stringValue(device->member("deviceId"), &parsed.logicalDeviceId) ||
        !stringValue(device->member("adapterId"), &adapterId) ||
        !stringValue(device->member("model"), &parsed.model) ||
        !stringValue(device->member("serialNumber"), &parsed.serialNumber)) {
        return fail(error, "Device open spec identity, channels and safeState are required");
    }
    parsed.physicalDeviceId = trim(parsed.physicalDeviceId);
    parsed.logicalDeviceId = trim(parsed.logicalDeviceId);
    parsed.model = trim(parsed.model);
    parsed.serialNumber = trim(parsed.serialNumber);
    const DeviceProfile* profile = findDeviceProfile(parsed.model);
    if (parsed.physicalDeviceId.empty() || parsed.logicalDeviceId.empty() ||
        lower(trim(adapterId)) != "ni.daqmx" ||
        parsed.serialNumber.empty() || isPlaceholder(parsed.serialNumber)) {
        return fail(error,
                    "Supported NI device model and deployed serialNumber must be explicit");
    }
    if (profile == nullptr) {
        return fail(error,
                    "Unsupported NI device.model; supported models are PXI-6259 and PXI-6733");
    }
    parsed.model = profile->name;
    parsed.modelKind = profile->model;
    const JsonValue* taskProfiles = root.member("taskProfiles");
    if (taskProfiles != nullptr && taskProfiles->type != JsonValue::Type::Array) {
        return fail(error, "taskProfiles must be an array");
    }
    const JsonValue* properties = device->member("properties");
    if (properties != nullptr) {
        if (properties->type != JsonValue::Type::Object) {
            return fail(error, "device.properties must be an object");
        }
        const JsonValue* vendor = properties->member("vendor");
        const JsonValue* ni = vendor == nullptr ? nullptr : vendor->member("ni");
        const JsonValue* projectedName = ni == nullptr ? nullptr : ni->member("deviceName");
        if (projectedName != nullptr) {
            std::string nestedDeviceName;
            if (!stringValue(projectedName, &nestedDeviceName) ||
                trim(nestedDeviceName) != parsed.physicalDeviceId) {
                return fail(error,
                            "device.properties.vendor.ni.deviceName must match physicalDeviceId");
            }
        }
    }

    std::set<std::string> resources;
    std::set<std::string> physicalChannels;
    for (const JsonValue& value : channels->array) {
        if (value.type != JsonValue::Type::Object) {
            return fail(error, "Each projected channel must be an object");
        }
        ChannelConfig channel;
        std::string module;
        std::string direction;
        if (!stringValue(value.member("resourceId"), &channel.resourceId) ||
            !stringValue(value.member("module"), &module) ||
            !stringValue(value.member("direction"), &direction) ||
            !integerValue(value.member("physicalIndex"), &channel.physicalIndex) ||
            !parseModule(module, &channel.module) ||
            !parseDirection(direction, &channel.direction)) {
            return fail(error, "Projected channel identity, module, direction and physicalIndex are required");
        }
        const JsonValue* projectedDeviceId = value.member("deviceId");
        const JsonValue* projectedAdapterId = value.member("adapterId");
        std::string channelDeviceId;
        std::string channelAdapterId;
        if ((projectedDeviceId != nullptr &&
             (!stringValue(projectedDeviceId, &channelDeviceId) ||
              trim(channelDeviceId) != parsed.logicalDeviceId)) ||
            (projectedAdapterId != nullptr &&
             (!stringValue(projectedAdapterId, &channelAdapterId) ||
              lower(trim(channelAdapterId)) != "ni.daqmx"))) {
            return fail(error, "Projected channel deviceId/adapterId must match the device identity");
        }
        channel.resourceId = trim(channel.resourceId);
        if (channel.resourceId.empty() || !resources.insert(channel.resourceId).second) {
            return fail(error, "Projected resourceId values must be non-empty and unique");
        }
        const JsonValue* properties = value.member("properties");
        if (properties == nullptr || properties->type != JsonValue::Type::Object) {
            return fail(error, "Each NI channel requires a properties object");
        }
        if (channel.module == ChannelModule::Digital) {
            if (!integerValue(properties->member("portNumber"), &channel.portNumber) ||
                !integerValue(properties->member("lineNumber"), &channel.lineNumber)) {
                return fail(error, "Digital properties require portNumber and lineNumber");
            }
        } else if (channel.module == ChannelModule::Analog) {
            channel.channelNumber = channel.physicalIndex;
            const JsonValue* explicitNumber = properties->member("channelNumber");
            if (explicitNumber != nullptr &&
                (!integerValue(explicitNumber, &channel.channelNumber) ||
                 channel.channelNumber != channel.physicalIndex)) {
                return fail(error, "Analog channelNumber must equal physicalIndex");
            }
            const JsonValue* terminal = properties->member("terminalConfig");
            if (terminal != nullptr && !stringValue(terminal, &channel.terminalConfig)) {
                return fail(error, "terminalConfig must be a string");
            }
            if (!optionalNumber(properties, "minValue", &channel.minValue, error) ||
                !optionalNumber(properties, "maxValue", &channel.maxValue, error)) {
                return false;
            }
        } else {
            channel.counterNumber = channel.physicalIndex;
            const JsonValue* explicitNumber = properties->member("counterNumber");
            if (explicitNumber != nullptr &&
                (!integerValue(explicitNumber, &channel.counterNumber) ||
                 channel.counterNumber != channel.physicalIndex)) {
                return fail(error, "Counter counterNumber must equal physicalIndex");
            }
        }
        if (!validateTopology(*profile, channel, error) ||
            !parseSafeValue(safeState, &channel, error)) {
            return false;
        }
        if (profile->model == DeviceModel::Pxi6733 && channel.safeAnalog != 0.0) {
            return fail(error, "PXI-6733 analog output safeState must be zero");
        }
        std::string physicalKey;
        if (channel.module == ChannelModule::Digital) {
            physicalKey = "dio:" + std::to_string(channel.portNumber) + ":" +
                std::to_string(channel.lineNumber);
        } else if (channel.module == ChannelModule::Analog) {
            physicalKey = channel.direction == ChannelDirection::Input ? "ai:" : "ao:";
            physicalKey += std::to_string(channel.channelNumber);
        } else {
            physicalKey = "ctr:" + std::to_string(channel.counterNumber);
        }
        if (!physicalChannels.insert(physicalKey).second) {
            return fail(error, "Projected channels contain a duplicate physical channel");
        }
        parsed.channels.push_back(std::move(channel));
    }
    if (parsed.channels.empty()) {
        return fail(error, "Device open spec must project at least one channel");
    }
    for (const auto& entry : safeState->object) {
        if (resources.find(entry.first) == resources.end()) {
            return fail(error, "safeState contains a resource outside this device projection");
        }
    }
    *config = std::move(parsed);
    return true;
}

} // namespace hwtest::adapters::ni_daqmx
