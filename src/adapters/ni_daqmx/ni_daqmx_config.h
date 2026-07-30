#pragma once

#include <string>
#include <vector>

namespace hwtest::adapters::ni_daqmx {

struct DriverConfig {
    double timeoutSeconds = 1.0;
};

enum class ChannelModule { Analog, Digital, Counter };
enum class ChannelDirection { Input, Output };
enum class DeviceModel { Pxi6259, Pxi6733 };

struct DeviceProfile {
    DeviceModel model = DeviceModel::Pxi6259;
    const char* name = "";
    int analogInputChannels = 0;
    int analogOutputChannels = 0;
    int analogOutputResolutionBits = 0;
    double analogOutputMaxRateHz = 0.0;
    bool supportsDigital = false;
    bool supportsCounter = false;
    bool analogOutputOnDemandOnly = false;
};

struct ChannelConfig {
    std::string resourceId;
    ChannelModule module = ChannelModule::Digital;
    ChannelDirection direction = ChannelDirection::Input;
    int physicalIndex = -1;

    int portNumber = -1;
    int lineNumber = -1;
    int channelNumber = -1;
    int counterNumber = -1;
    std::string terminalConfig;
    double minValue = -10.0;
    double maxValue = 10.0;

    bool hasSafeValue = false;
    bool safeHigh = false;
    double safeAnalog = 0.0;
};

struct DeviceConfig {
    int version = 0;
    std::string logicalDeviceId;
    std::string physicalDeviceId;
    std::string model;
    DeviceModel modelKind = DeviceModel::Pxi6259;
    std::string serialNumber;
    std::vector<ChannelConfig> channels;
};

const DeviceProfile* findDeviceProfile(const std::string& model);
const DeviceProfile& deviceProfile(DeviceModel model);

bool parseDriverConfig(const char* json,
                       DriverConfig* config,
                       std::string* error);

bool parseDeviceOpenSpec(const char* json,
                         DeviceConfig* config,
                         std::string* error);

} // namespace hwtest::adapters::ni_daqmx
