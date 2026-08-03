#include "hal/hal_adapter_abi.h"
#include "hal/hal_adapter_task_abi.h"

#include "fake/NIDAQmx.h"
#include "fake_nidaqmx_control.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

long long HAL_ADAPTER_CALL nowUs()
{
    return 987654321;
}

void HAL_ADAPTER_CALL logMessage(int, const char*, const char*, const char*)
{
}

bool check(bool condition, const char* message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

const char* validConfig()
{
    return R"json({
      "adapterId":"ni.daqmx",
      "settings":{"timeoutSeconds":0.25}
    })json";
}

const char* validOpenSpec()
{
    return R"json({
      "schema":"hwtest.adapter-device-open",
      "version":1,
      "physicalDeviceId":"PXI1Slot2",
      "device":{
        "deviceId":"pxi6259",
        "adapterId":"ni.daqmx",
        "vendor":"NI",
        "model":"PXI-6259",
        "serialNumber":"62590002"
      },
      "channels":[
        {"resourceId":"OUT0","module":"digital","direction":"output","physicalIndex":0,"properties":{"portNumber":0,"lineNumber":0}},
        {"resourceId":"OUT1","module":"digital","direction":"output","physicalIndex":1,"properties":{"portNumber":0,"lineNumber":1}},
        {"resourceId":"OUT2","module":"digital","direction":"output","physicalIndex":2,"properties":{"portNumber":0,"lineNumber":2}},
        {"resourceId":"IN0","module":"digital","direction":"input","physicalIndex":16,"properties":{"portNumber":1,"lineNumber":0}},
        {"resourceId":"IN1","module":"digital","direction":"input","physicalIndex":17,"properties":{"portNumber":1,"lineNumber":1}},
        {"resourceId":"IN_TIMED0","module":"digital","direction":"input","physicalIndex":18,"properties":{"portNumber":0,"lineNumber":3}},
        {"resourceId":"IN_TIMED1","module":"digital","direction":"input","physicalIndex":19,"properties":{"portNumber":0,"lineNumber":4}},
        {"resourceId":"AI0","module":"analog","direction":"input","physicalIndex":0,"properties":{"channelNumber":0,"terminalConfig":"Differential","minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AI1","module":"analog","direction":"input","physicalIndex":1,"properties":{"channelNumber":1,"terminalConfig":"Differential","minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO0","module":"analog","direction":"output","physicalIndex":0,"properties":{"channelNumber":0,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"CTR0","module":"counter","direction":"input","physicalIndex":0,"properties":{"counterNumber":0}},
        {"resourceId":"CTR1","module":"counter","direction":"output","physicalIndex":1,"properties":{"counterNumber":1}}
      ],
      "taskProfiles":[],
      "safeState":{"OUT0":"Low","OUT1":"High","OUT2":"Low","AO0":0.0}
    })json";
}

void usePxiFixture()
{
    fake_nidaqmx::reset();
    fake_nidaqmx::setDeviceIdentity("PXI1Slot2", "PXI-6259", 62590002u);
}

void usePxi6733Fixture()
{
    fake_nidaqmx::reset();
    fake_nidaqmx::setDeviceIdentity("PXI1Slot3", "PXI-6733", 67330003u);
}

bool runLifecycleTest(const HalAdapterApiV1& api)
{
    usePxiFixture();
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "initialize should validate driver-level settings")) return false;

    int count = 0;
    status = api.enumerateDevices(adapter, nullptr, &count, 1000);
    if (!check(status.code == HAL_ADAPTER_OK && count == 1,
               "enumerate size query should report one configured device")) return false;
    HalAdapterDeviceInfo info{};
    status = api.enumerateDevices(adapter, &info, &count, 1000);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   std::strcmp(info.deviceId, "PXI1Slot2") == 0 &&
                   std::strcmp(info.model, "PXI-6259") == 0,
               "enumerate should expose the PXI device reported by NI-DAQmx")) return false;

    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "PXI1Slot2", validOpenSpec(), &device);
    if (!check(status.code == HAL_ADAPTER_OK && device != nullptr,
               "open should validate the versioned PXI device projection")) return false;
    if (!check(fake_nidaqmx::createDoCalls() == 1 &&
                    fake_nidaqmx::createAoCalls() == 1 &&
                    fake_nidaqmx::createDiCalls() == 0 &&
                    std::strcmp(fake_nidaqmx::lastDoPhysicalChannel(),
                                "PXI1Slot2/port0/line0:2") == 0,
               "open should apply DO and AO safety through short-lived tasks")) return false;
    if (!check(fake_nidaqmx::writeCalls() == 1 &&
                   fake_nidaqmx::lastWrittenMask() == 0x2u,
               "open should apply the complete configured safe mask")) return false;
    int capabilityBytes = 0;
    status = api.getCapabilities(device, nullptr, &capabilityBytes, 1000);
    if (!check(status.code == HAL_ADAPTER_BUFFER_TOO_SMALL && capabilityBytes > 0,
               "capability size query should report the PXI profile")) return false;
    std::vector<char> capabilities(static_cast<std::size_t>(capabilityBytes));
    status = api.getCapabilities(device, capabilities.data(), &capabilityBytes, 1000);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   std::strstr(capabilities.data(), "analog.inputChannels") != nullptr &&
                   std::strstr(capabilities.data(), "reference") != nullptr &&
                   std::strstr(capabilities.data(), "pulseFrequency") != nullptr,
               "capabilities should advertise PXI analog, timing, trigger and counter limits")) return false;

    const int indexes[] = {0, 2};
    const int levels[] = {1, 1};
    status = api.digitalWrite(device, indexes, levels, 2, 1000);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   fake_nidaqmx::writeCalls() == 2 &&
                   fake_nidaqmx::lastWrittenMask() == 0x7u,
               "a batch write should update one complete port mask once")) return false;

    fake_nidaqmx::setInputMask(0x2u);
    const int inputIndexes[] = {16, 17};
    HalAdapterDigitalSample samples[2]{};
    status = api.digitalRead(device, inputIndexes, 2, samples, 1000);
    if (!check(status.code == HAL_ADAPTER_OK && samples[0].level == 0 &&
                   samples[1].level == 1 && samples[1].timestampUs == nowUs() &&
                   fake_nidaqmx::ioContractValid(),
               "digital read should project the configured DI bank")) return false;

    HalAdapterAnalogRange range{-5.0, 5.0, 0};
    HalAdapterAnalogRange milliVoltRange{-5000.0,
                                         5000.0,
                                         HAL_ADAPTER_ANALOG_UNIT_MILLIVOLT};
    if (!check(api.analogConfigure(device, 0, &range, 0, 1000).code == HAL_ADAPTER_OK &&
                   api.analogConfigure(device, 0, &milliVoltRange, 1, 1000).code == HAL_ADAPTER_OK,
               "on-demand AI and AO should accept explicit voltage ranges")) return false;
    const double analogInput[] = {2.25};
    fake_nidaqmx::setAnalogInputBlock(analogInput, 1, 1);
    const int analogIndex[] = {0};
    HalAdapterAnalogSample analogSample{};
    status = api.analogRead(device,
                            analogIndex,
                            1,
                            &analogSample,
                            1,
                            0,
                            1000);
    if (!check(status.code == HAL_ADAPTER_OK && analogSample.value == 2.25 &&
                   analogSample.timestampUs == nowUs(),
               "on-demand AI should return the configured PXI channel")) return false;
    const double analogOutput[] = {1500.0};
    status = api.analogWrite(device,
                             analogIndex,
                             analogOutput,
                             1,
                             HAL_ADAPTER_ANALOG_UNIT_MILLIVOLT,
                             1000);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   fake_nidaqmx::lastAnalogOutputCount() == 1 &&
                   fake_nidaqmx::lastAnalogOutputAt(0) == 1.5,
               "on-demand AO should write the configured PXI channel")) return false;

    fake_nidaqmx::failNextWrite(-200284, "operation timed out");
    status = api.digitalWrite(device, indexes, levels, 2, 1000);
    if (!check(status.code == HAL_ADAPTER_TIMEOUT &&
                   status.vendorCode == -200284,
               "DAQmx timeout should retain the vendor code")) return false;

    fake_nidaqmx::failNextWrite(-88705, "localized vendor diagnostic");
    status = api.digitalWrite(device, indexes, levels, 2, 1000);
    if (!check(status.code == HAL_ADAPTER_DEVICE_DISCONNECTED &&
                   status.vendorCode == -88705,
               "removed hardware should map to device-disconnected")) return false;

    status = api.resetDevice(device, 1000);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   fake_nidaqmx::lastWrittenMask() == 0x2u &&
                   fake_nidaqmx::resetDeviceCalls() == 0,
               "ABI reset should apply safe output without DAQmxResetDevice")) return false;

    status = api.closeDevice(device);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   fake_nidaqmx::lastWrittenMask() == 0x2u &&
                   fake_nidaqmx::clearCalls() >= 8 &&
                   fake_nidaqmx::resetDeviceCalls() == 0,
               "close should reapply every safe output and release short-lived tasks")) return false;

    status = api.shutdown(adapter);
    return check(status.code == HAL_ADAPTER_OK,
                 "shutdown should release the adapter state");
}

bool runIdentityMismatchTest(const HalAdapterApiV1& api)
{
    usePxiFixture();
    const char* badOpenSpec = R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot2",
      "device":{"deviceId":"pxi6259","adapterId":"ni.daqmx","model":"PXI-6259","serialNumber":"999"},
      "channels":[{"resourceId":"OUT0","module":"digital","direction":"output","physicalIndex":0,"properties":{"portNumber":0,"lineNumber":0}}],
      "safeState":{"OUT0":"Low"}
    })json";
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "identity-mismatch setup should initialize the driver")) return false;
    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "PXI1Slot2", badOpenSpec, &device);
    const bool passed = check(status.code == HAL_ADAPTER_NOT_FOUND && device == nullptr,
                              "serial mismatch must fail before any hardware output task");
    api.shutdown(adapter);
    return passed;
}

bool runCloseFailureStillShutsDownTest(const HalAdapterApiV1& api)
{
    usePxiFixture();
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "close-failure setup should initialize the adapter")) return false;
    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "PXI1Slot2", validOpenSpec(), &device);
    if (!check(status.code == HAL_ADAPTER_OK && device != nullptr,
               "close-failure setup should open the device")) return false;

    fake_nidaqmx::failNextWrite(-88705, "configured device was removed");
    status = api.closeDevice(device);
    if (!check(status.code == HAL_ADAPTER_DEVICE_DISCONNECTED &&
                   fake_nidaqmx::clearCalls() >= 4,
               "failed safe write must still apply remaining safety and clear tasks")) return false;

    status = api.shutdown(adapter);
    return check(status.code == HAL_ADAPTER_OK,
                 "close failure must consume the device handle before shutdown");
}

bool runLegacyFullHalConfigRejectedTest(const HalAdapterApiV1& api)
{
    const char* legacyConfig = R"json({
      "settings":{"timeoutSeconds":0.25},
      "hardware":{
        "devices":[{"alias":"pxi6259","model":"PXI-6259","serialNumber":"62590002"}],
        "resources":{
          "OUT0":{"device":"pxi6259","module":"digital","direction":"output","physicalIndex":0,"properties":{"portNumber":0,"lineNumber":0}}
        }
      },
      "safeState":{"OUT0":"Low"}
    })json";
    HalAdapterHandle handle = nullptr;
    const HalAdapterStatus status = api.initialize(legacyConfig, &handle);
    if (handle != nullptr) api.shutdown(handle);
    return check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && handle == nullptr,
                 "legacy whole-HAL JSON must not cross the adapter initialization boundary");
}

bool runAnalogSurfaceTest(const HalAdapterApiV1& api)
{
    HalAdapterInfo info{};
    bool passed = check(api.getInfo(&info).code == HAL_ADAPTER_OK,
                        "adapter info should be available before checking PXI capabilities");
    passed = check((info.supportedModulesMask & HAL_MODULE_ANALOG) != 0u,
                   "PXI-6259 adapter should advertise analog capability") && passed;
    passed = check(api.analogConfigure != nullptr,
                   "PXI-6259 adapter should export analog configure") && passed;
    passed = check(api.analogRead != nullptr,
                   "PXI-6259 adapter should export analog read") && passed;
    passed = check(api.analogWrite != nullptr,
                   "PXI-6259 adapter should export analog write") && passed;
    return passed;
}

const char* pxi6259Config(bool invalidPortLine)
{
    return invalidPortLine
        ? R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot2",
      "device":{"deviceId":"pxi6259","adapterId":"ni.daqmx","model":"PXI-6259","serialNumber":"62590002"},
      "channels":[{"resourceId":"OUT0","module":"digital","direction":"output","physicalIndex":0,"properties":{"portNumber":1,"lineNumber":8}}],
      "safeState":{"OUT0":"Low"}
    })json"
        : validOpenSpec();
}

bool runPxi6259IdentityAndTopologyTest(const HalAdapterApiV1& api)
{
    usePxiFixture();

    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    bool passed = check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
                        "PXI-6259 Fake driver should initialize the adapter");
    if (adapter != nullptr) {
        HalAdapterDeviceInfo info{};
        int count = 1;
        status = api.enumerateDevices(adapter, &info, &count, 1000);
        passed = check(status.code == HAL_ADAPTER_OK &&
                           std::strcmp(info.deviceId, "PXI1Slot2") == 0 &&
                           std::strcmp(info.model, "PXI-6259") == 0,
                       "PXI-6259 identity should round-trip through enumeration") && passed;
        HalAdapterDeviceHandle invalid = nullptr;
        status = api.openDevice(adapter, "PXI1Slot2", pxi6259Config(true), &invalid);
        passed = check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && invalid == nullptr,
                   "PXI-6259 port1 must reject line 8") && passed;
        api.shutdown(adapter);
    }
    return passed;
}

const char* pxi6733OpenSpec()
{
    return R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot3",
      "device":{"deviceId":"pxi6733","adapterId":"ni.daqmx","model":"PXI-6733","serialNumber":"67330003"},
      "channels":[
        {"resourceId":"AO0","module":"analog","direction":"output","physicalIndex":0,"properties":{"channelNumber":0,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO1","module":"analog","direction":"output","physicalIndex":1,"properties":{"channelNumber":1,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO2","module":"analog","direction":"output","physicalIndex":2,"properties":{"channelNumber":2,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO3","module":"analog","direction":"output","physicalIndex":3,"properties":{"channelNumber":3,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO4","module":"analog","direction":"output","physicalIndex":4,"properties":{"channelNumber":4,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO5","module":"analog","direction":"output","physicalIndex":5,"properties":{"channelNumber":5,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO6","module":"analog","direction":"output","physicalIndex":6,"properties":{"channelNumber":6,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO7","module":"analog","direction":"output","physicalIndex":7,"properties":{"channelNumber":7,"minValue":-10.0,"maxValue":10.0}}
      ],
      "safeState":{"AO0":0.0,"AO1":0.0,"AO2":0.0,"AO3":0.0,"AO4":0.0,"AO5":0.0,"AO6":0.0,"AO7":0.0}
    })json";
}

bool lastAnalogOutputsAreZero(int expectedCount)
{
    if (fake_nidaqmx::lastAnalogOutputCount() != expectedCount) return false;
    for (int index = 0; index < expectedCount; ++index) {
        if (fake_nidaqmx::lastAnalogOutputAt(index) != 0.0) return false;
    }
    return true;
}

bool runPxi6733ProfileTest(const HalAdapterApiV1& api,
                           const HalAdapterTaskApiV1& taskApi)
{
    usePxi6733Fixture();
    HalAdapterInfo adapterInfo{};
    bool passed = check(api.getInfo(&adapterInfo).code == HAL_ADAPTER_OK &&
                            std::strstr(adapterInfo.name, "PXI-6259") == nullptr,
                        "adapter information must not hard-code the PXI-6259 model");

    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "PXI-6733 Fake driver should initialize the adapter")) return false;

    HalAdapterDeviceInfo info{};
    int count = 1;
    status = api.enumerateDevices(adapter, &info, &count, 1000);
    passed = check(status.code == HAL_ADAPTER_OK &&
                       std::strcmp(info.model, "PXI-6733") == 0 &&
                       (info.supportedModulesMask & HAL_MODULE_ANALOG) != 0u &&
                       (info.supportedModulesMask & (HAL_MODULE_DIGITAL | HAL_MODULE_COUNTER)) == 0u,
                   "PXI-6733 enumeration should advertise its analog-only profile") && passed;

    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "PXI1Slot3", pxi6733OpenSpec(), &device);
    passed = check(status.code == HAL_ADAPTER_OK && device != nullptr,
                   "PXI-6733 AO0..AO7 projection should open") && passed;
    if (device != nullptr) {
        passed = check(fake_nidaqmx::createAoCalls() == 8 &&
                           fake_nidaqmx::createAiCalls() == 0 &&
                           fake_nidaqmx::createDiCalls() == 0 &&
                           fake_nidaqmx::createDoCalls() == 0 &&
                           fake_nidaqmx::createCounterInputCalls() == 0 &&
                           fake_nidaqmx::createCounterOutputCalls() == 0 &&
                           std::strcmp(fake_nidaqmx::lastAoPhysicalChannel(),
                                       "PXI1Slot3/ao7") == 0,
                       "PXI-6733 open should expose only AO0..AO7") && passed;
        passed = check(lastAnalogOutputsAreZero(8),
                       "PXI-6733 open should write zero to every AO safe state") && passed;

        int capabilityBytes = 0;
        status = api.getCapabilities(device, nullptr, &capabilityBytes, 1000);
        std::vector<char> capabilities(static_cast<std::size_t>(capabilityBytes));
        if (status.code == HAL_ADAPTER_BUFFER_TOO_SMALL) {
            status = api.getCapabilities(device, capabilities.data(), &capabilityBytes, 1000);
        }
        passed = check(status.code == HAL_ADAPTER_OK &&
                           std::strstr(capabilities.data(), "\"supportedModules\":[\"analog\"]") != nullptr &&
                           std::strstr(capabilities.data(), "\"analog.outputChannels\":8") != nullptr &&
                           std::strstr(capabilities.data(), "\"analog.outputResolutionBits\":16") != nullptr &&
                           std::strstr(capabilities.data(), "\"analog.outputMaxRateHz\":1000000") != nullptr &&
                           std::strstr(capabilities.data(), "\"sampleModes\":[\"onDemand\"]") != nullptr &&
                           std::strstr(capabilities.data(), "digital") == nullptr &&
                           std::strstr(capabilities.data(), "counter") == nullptr &&
                           std::strstr(capabilities.data(), "analog.input") == nullptr,
                       "PXI-6733 capabilities should be AO-only, 16-bit, 1 MS/s and on-demand") && passed;

        const int outputIndexes[] = {0, 7};
        const double outputValues[] = {1.25, -2.5};
        status = api.analogWrite(device,
                                 outputIndexes,
                                 outputValues,
                                 2,
                                 HAL_ADAPTER_ANALOG_UNIT_VOLT,
                                 1000);
        passed = check(status.code == HAL_ADAPTER_OK &&
                           fake_nidaqmx::lastAnalogOutputCount() == 2 &&
                           fake_nidaqmx::lastAnalogOutputAt(0) == 1.25 &&
                           fake_nidaqmx::lastAnalogOutputAt(1) == -2.5,
                       "PXI-6733 should support static on-demand writes on AO0 and AO7") && passed;

        const int analogIndex[] = {0};
        HalAdapterAnalogSample analogSample{};
        HalAdapterDigitalSample digitalSample{};
        passed = check(api.analogRead(device,
                                      analogIndex,
                                      1,
                                      &analogSample,
                                      1,
                                      0,
                                      1000).code == HAL_ADAPTER_NOT_SUPPORTED &&
                           api.digitalRead(device, analogIndex, 1, &digitalSample, 1000).code ==
                               HAL_ADAPTER_NOT_SUPPORTED &&
                           api.digitalWrite(device, analogIndex, analogIndex, 1, 1000).code ==
                               HAL_ADAPTER_NOT_SUPPORTED,
                       "PXI-6733 must reject unprojected AI and DIO resources") && passed;

        HalAdapterTaskConfig taskConfig{};
        taskConfig.structSize = static_cast<int>(sizeof(taskConfig));
        taskConfig.kind = HAL_ADAPTER_TASK_ANALOG_OUTPUT;
        taskConfig.mode = HAL_ADAPTER_TASK_ON_DEMAND;
        taskConfig.channelIndexes = analogIndex;
        taskConfig.channelCount = 1;
        taskConfig.samplesPerChannel = 1;
        HalAdapterTaskHandle task = nullptr;
        status = taskApi.createTask(device, &taskConfig, &task);
        passed = check(status.code == HAL_ADAPTER_OK && task != nullptr,
                       "PXI-6733 should permit an on-demand AO task") && passed;
        if (task != nullptr) {
            double taskValue = 0.5;
            HalAdapterTaskBuffer buffer{};
            buffer.structSize = static_cast<int>(sizeof(buffer));
            buffer.sampleType = HAL_ADAPTER_SAMPLE_FLOAT64;
            buffer.data = &taskValue;
            buffer.capacityValues = 1;
            buffer.channelCount = 1;
            buffer.samplesPerChannel = 1;
            passed = check(taskApi.writeTask(task, &buffer, 1, 1000).code == HAL_ADAPTER_OK,
                           "PXI-6733 on-demand AO task should write one static sample") && passed;
            passed = check(taskApi.closeTask(task).code == HAL_ADAPTER_OK,
                           "PXI-6733 on-demand AO task should close safely") && passed;
        }

        taskConfig.mode = HAL_ADAPTER_TASK_FINITE;
        taskConfig.sampleRateHz = 1000000.0;
        task = nullptr;
        passed = check(taskApi.createTask(device, &taskConfig, &task).code == HAL_ADAPTER_NOT_SUPPORTED &&
                           task == nullptr,
                       "PXI-6733 must not open unverified finite AO sampling") && passed;
        taskConfig.kind = HAL_ADAPTER_TASK_ANALOG_INPUT;
        taskConfig.mode = HAL_ADAPTER_TASK_ON_DEMAND;
        taskConfig.sampleRateHz = 0.0;
        task = nullptr;
        passed = check(taskApi.createTask(device, &taskConfig, &task).code == HAL_ADAPTER_NOT_SUPPORTED &&
                           task == nullptr,
                       "PXI-6733 must reject AI task creation") && passed;
        taskConfig.kind = HAL_ADAPTER_TASK_DIGITAL_INPUT;
        task = nullptr;
        passed = check(taskApi.createTask(device, &taskConfig, &task).code == HAL_ADAPTER_NOT_SUPPORTED &&
                           task == nullptr,
                       "PXI-6733 must reject DIO task creation") && passed;

        passed = check(api.resetDevice(device, 1000).code == HAL_ADAPTER_OK &&
                           lastAnalogOutputsAreZero(8),
                       "PXI-6733 reset should restore zero AO safe states") && passed;
        passed = check(api.closeDevice(device).code == HAL_ADAPTER_OK &&
                           lastAnalogOutputsAreZero(8),
                       "PXI-6733 close should restore zero AO safe states") && passed;
    }

    const char* unknownModel = R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot3",
      "device":{"deviceId":"unknown","adapterId":"ni.daqmx","model":"PXI-9999","serialNumber":"67330003"},
      "channels":[{"resourceId":"AO0","module":"analog","direction":"output","physicalIndex":0,"properties":{"channelNumber":0,"minValue":-10.0,"maxValue":10.0}}],
      "safeState":{"AO0":0.0}
    })json";
    usePxi6733Fixture();
    HalAdapterDeviceHandle unknown = nullptr;
    status = api.openDevice(adapter, "PXI1Slot3", unknownModel, &unknown);
    passed = check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && unknown == nullptr &&
                       fake_nidaqmx::createAoCalls() == 0,
                   "unknown NI models must fail closed before any output write") && passed;

    const char* unsupportedAi = R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot3",
      "device":{"deviceId":"pxi6733","adapterId":"ni.daqmx","model":"PXI-6733","serialNumber":"67330003"},
      "channels":[{"resourceId":"AI0","module":"analog","direction":"input","physicalIndex":0,"properties":{"channelNumber":0,"minValue":-10.0,"maxValue":10.0}}],
      "safeState":{}
    })json";
    HalAdapterDeviceHandle unsupported = nullptr;
    status = api.openDevice(adapter, "PXI1Slot3", unsupportedAi, &unsupported);
    passed = check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && unsupported == nullptr,
                   "PXI-6733 configuration must reject AI projections") && passed;

    const char* unsupportedDi = R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot3",
      "device":{"deviceId":"pxi6733","adapterId":"ni.daqmx","model":"PXI-6733","serialNumber":"67330003"},
      "channels":[{"resourceId":"DI0","module":"digital","direction":"input","physicalIndex":0,"properties":{"portNumber":0,"lineNumber":0}}],
      "safeState":{}
    })json";
    status = api.openDevice(adapter, "PXI1Slot3", unsupportedDi, &unsupported);
    passed = check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && unsupported == nullptr,
                   "PXI-6733 configuration must reject DIO projections") && passed;

    const char* nonZeroSafeState = R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot3",
      "device":{"deviceId":"pxi6733","adapterId":"ni.daqmx","model":"PXI-6733","serialNumber":"67330003"},
      "channels":[{"resourceId":"AO0","module":"analog","direction":"output","physicalIndex":0,"properties":{"channelNumber":0,"minValue":-10.0,"maxValue":10.0}}],
      "safeState":{"AO0":1.0}
    })json";
    HalAdapterDeviceHandle nonZero = nullptr;
    status = api.openDevice(adapter, "PXI1Slot3", nonZeroSafeState, &nonZero);
    passed = check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && nonZero == nullptr,
                   "PXI-6733 configuration must require a zero AO safeState") && passed;
    if (nonZero != nullptr) api.closeDevice(nonZero);

    passed = check(api.shutdown(adapter).code == HAL_ADAPTER_OK,
                   "PXI-6733 adapter should shut down") && passed;
    return passed;
}

bool runPxiDeploymentGateAndBoundaryTest(const HalAdapterApiV1& api)
{
    usePxiFixture();
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "PXI boundary setup should initialize the driver")) return false;

    const char* placeholder = R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot2",
      "device":{"deviceId":"pxi6259","adapterId":"ni.daqmx","model":"PXI-6259","serialNumber":"CONFIGURE_ME"},
      "channels":[{"resourceId":"AO3","module":"analog","direction":"output","physicalIndex":3,"properties":{"channelNumber":3,"minValue":-10.0,"maxValue":10.0}}],
      "safeState":{"AO3":0.0}
    })json";
    fake_nidaqmx::clearCallLog();
    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "PXI1Slot2", placeholder, &device);
    bool passed = check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && device == nullptr &&
                            fake_nidaqmx::createAoCalls() == 0,
                        "CONFIGURE_ME must stop open before any hardware output call");

    const char* boundary = R"json({
      "schema":"hwtest.adapter-device-open","version":1,"physicalDeviceId":"PXI1Slot2",
      "device":{"deviceId":"pxi6259","adapterId":"ni.daqmx","model":"PXI-6259","serialNumber":"62590002"},
      "channels":[
        {"resourceId":"AI31","module":"analog","direction":"input","physicalIndex":31,"properties":{"channelNumber":31,"terminalConfig":"RSE","minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"AO3","module":"analog","direction":"output","physicalIndex":3,"properties":{"channelNumber":3,"minValue":-10.0,"maxValue":10.0}},
        {"resourceId":"DIO47","module":"digital","direction":"input","physicalIndex":47,"properties":{"portNumber":2,"lineNumber":7}},
        {"resourceId":"CTR1","module":"counter","direction":"input","physicalIndex":1,"properties":{"counterNumber":1}}
      ],
      "safeState":{"AO3":0.0}
    })json";
    status = api.openDevice(adapter, "PXI1Slot2", boundary, &device);
    passed = check(status.code == HAL_ADAPTER_OK && device != nullptr,
                   "PXI-6259 upper AI/AO/DIO/counter channel boundaries should be accepted") &&
        passed;
    if (device != nullptr) {
        passed = check(api.closeDevice(device).code == HAL_ADAPTER_OK,
                       "PXI boundary device should close safely") && passed;
    }
    passed = check(api.shutdown(adapter).code == HAL_ADAPTER_OK,
                   "PXI boundary adapter should shut down") && passed;
    return passed;
}

bool runFakeFiniteAiTaskContractTest()
{
    fake_nidaqmx::reset();
    fake_nidaqmx::setDeviceIdentity("PXI1Slot2", "PXI-6259", 62590002u);
    fake_nidaqmx::clearCallLog();

    TaskHandle task = nullptr;
    if (!check(DAQmxCreateTask("finite-ai", &task) == 0,
               "Fake should create an AI task")) return false;
    if (!check(DAQmxCreateAIVoltageChan(task,
                                         "PXI1Slot2/ai0:1",
                                         "",
                                         DAQmx_Val_Diff,
                                         -10.0,
                                         10.0,
                                         DAQmx_Val_Volts,
                                         nullptr) == 0,
               "Fake should record AI range and terminal configuration")) return false;
    if (!check(DAQmxCfgSampClkTiming(task,
                                     "/PXI1Slot2/PFI0",
                                     20000.0,
                                     DAQmx_Val_Rising,
                                     DAQmx_Val_FiniteSamps,
                                     4) == 0 &&
                   DAQmxCfgInputBuffer(task, 16) == 0 &&
                   DAQmxCfgDigEdgeStartTrig(task,
                                            "/PXI1Slot2/PFI1",
                                            DAQmx_Val_Rising) == 0,
               "Fake should configure finite external-clock AI with a digital trigger")) return false;
    if (!check(fake_nidaqmx::lastAnalogTerminalConfig() == DAQmx_Val_Diff &&
                   fake_nidaqmx::lastAnalogMinimum() == -10.0 &&
                   fake_nidaqmx::lastAnalogMaximum() == 10.0 &&
                   std::strcmp(fake_nidaqmx::lastSampleClockSource(),
                               "/PXI1Slot2/PFI0") == 0 &&
                   fake_nidaqmx::lastSampleClockRate() == 20000.0 &&
                   fake_nidaqmx::lastSampleMode() == DAQmx_Val_FiniteSamps &&
                   fake_nidaqmx::lastSamplesPerChannel() == 4u,
               "Fake should retain exact AI range and finite clock settings")) return false;

    const double input[] = {1.0, 2.0, 3.0, 4.0, 11.0, 12.0, 13.0, 14.0};
    fake_nidaqmx::setAnalogInputBlock(input, 2, 4);
    if (!check(DAQmxStartTask(task) == 0,
               "Fake should start a finite AI task")) return false;
    uInt32 available = 0;
    if (!check(DAQmxGetReadAvailSampPerChan(task, &available) == 0 && available == 4,
               "Fake should report queued finite samples")) return false;

    double output[8]{};
    int32 read = 0;
    if (!check(DAQmxReadAnalogF64(task,
                                  4,
                                  0.25,
                                  DAQmx_Val_GroupByChannel,
                                  output,
                                  8,
                                  &read,
                                  nullptr) == 0 &&
                   read == 4 && output[0] == 1.0 && output[3] == 4.0 &&
                   output[4] == 11.0 && output[7] == 14.0,
               "Fake should preserve channel-major finite AI blocks")) return false;

    fake_nidaqmx::setAnalogInputBlock(input, 2, 4);
    fake_nidaqmx::setNextAnalogReadSamplesPerChannel(2);
    std::memset(output, 0, sizeof(output));
    read = 0;
    if (!check(DAQmxReadAnalogF64(task,
                                  4,
                                  0.25,
                                  DAQmx_Val_GroupByChannel,
                                  output,
                                  8,
                                  &read,
                                  nullptr) == 0 &&
                   read == 2 && output[0] == 1.0 && output[1] == 2.0 &&
                   output[2] == 11.0 && output[3] == 12.0,
               "Fake should model deterministic short AI reads")) return false;

    fake_nidaqmx::setAnalogInputBlock(input, 2, 4);
    fake_nidaqmx::failNext(fake_nidaqmx::Operation::ReadAnalog,
                           -200279,
                           "injected input overflow");
    if (!check(DAQmxReadAnalogF64(task,
                                  1,
                                  0.25,
                                  DAQmx_Val_GroupByChannel,
                                  output,
                                  8,
                                  &read,
                                  nullptr) == -200279,
               "Fake should inject analog input overflow")) return false;

    const fake_nidaqmx::Call expected[] = {
        fake_nidaqmx::Call::CreateTask,
        fake_nidaqmx::Call::CreateAnalogInputChannel,
        fake_nidaqmx::Call::ConfigureSampleClock,
        fake_nidaqmx::Call::ConfigureInputBuffer,
        fake_nidaqmx::Call::ConfigureDigitalStartTrigger,
        fake_nidaqmx::Call::StartTask,
        fake_nidaqmx::Call::ReadAnalog,
        fake_nidaqmx::Call::StopTask,
        fake_nidaqmx::Call::ClearTask
    };
    if (!check(DAQmxStopTask(task) == 0 && DAQmxClearTask(task) == 0,
               "Fake should stop and clear the finite AI task")) return false;
    if (!check(fake_nidaqmx::callsContainInOrder(expected,
                                                   static_cast<int>(sizeof(expected) / sizeof(expected[0]))),
               "Fake should retain finite AI call order")) return false;
    return true;
}

bool runFakeContinuousAoCounterTaskContractTest()
{
    fake_nidaqmx::reset();
    fake_nidaqmx::clearCallLog();

    if (!check(DAQmxConnectTerms("/PXI1Slot2/PFI2",
                                 "/PXI1Slot2/ao/SampleClock",
                                 0) == 0,
               "Fake should retain an external sample-clock route")) return false;

    TaskHandle ao = nullptr;
    if (!check(DAQmxCreateTask("continuous-ao", &ao) == 0 &&
                   DAQmxCreateAOVoltageChan(ao,
                                             "PXI1Slot2/ao0",
                                             "",
                                             -5.0,
                                             5.0,
                                             DAQmx_Val_Volts,
                                             nullptr) == 0 &&
                   DAQmxCfgSampClkTiming(ao,
                                         "/PXI1Slot2/PFI2",
                                         10000.0,
                                         DAQmx_Val_Rising,
                                         DAQmx_Val_ContSamps,
                                         0) == 0 &&
                   DAQmxCfgOutputBuffer(ao, 32) == 0 &&
                   DAQmxCfgAnlgEdgeStartTrig(ao,
                                             "PXI1Slot2/ai0",
                                             DAQmx_Val_Rising,
                                             1.25) == 0,
               "Fake should configure continuous AO with analog triggering")) return false;
    if (!check(DAQmxStartTask(ao) == 0,
               "Fake should start a continuous AO task")) return false;
    fake_nidaqmx::setWriteSpaceAvailable(24);
    uInt32 writeSpace = 0;
    if (!check(DAQmxGetWriteSpaceAvail(ao, &writeSpace) == 0 && writeSpace == 24,
               "Fake should expose available continuous AO buffer space")) return false;
    const double write[] = {0.5, 1.5, 2.5, 3.5};
    int32 written = 0;
    if (!check(DAQmxWriteAnalogF64(ao,
                                   4,
                                   0,
                                   0.25,
                                   DAQmx_Val_GroupByChannel,
                                   write,
                                   &written,
                                   nullptr) == 0 &&
                   written == 4 && fake_nidaqmx::lastAnalogOutputCount() == 4 &&
                   fake_nidaqmx::lastAnalogOutputAt(3) == 3.5,
               "Fake should preserve continuous AO blocks")) return false;
    if (!check(DAQmxStopTask(ao) == 0 && DAQmxClearTask(ao) == 0,
               "Fake should stop and clear continuous AO")) return false;
    if (!check(DAQmxDisconnectTerms("/PXI1Slot2/PFI2",
                                    "/PXI1Slot2/ao/SampleClock") == 0,
               "Fake should clear an external sample-clock route")) return false;

    TaskHandle counter = nullptr;
    if (!check(DAQmxCreateTask("counter-input", &counter) == 0 &&
                   DAQmxCreateCICountEdgesChan(counter,
                                                "PXI1Slot2/ctr0",
                                                "",
                                                DAQmx_Val_Rising,
                                                0,
                                                DAQmx_Val_CountUp) == 0 &&
                   DAQmxCfgImplicitTiming(counter, DAQmx_Val_FiniteSamps, 3) == 0,
               "Fake should configure finite counter input")) return false;
    const uInt32 counts[] = {7u, 8u, 9u};
    fake_nidaqmx::setCounterInputSamples(counts, 3);
    if (!check(DAQmxStartTask(counter) == 0,
               "Fake should start counter input")) return false;
    uInt32 observed[3]{};
    int32 observedCount = 0;
    if (!check(DAQmxReadCounterU32(counter,
                                   3,
                                   0.25,
                                   observed,
                                   3,
                                   &observedCount,
                                   nullptr) == 0 &&
                   observedCount == 3 && observed[0] == 7u && observed[2] == 9u,
               "Fake should return queued counter blocks")) return false;
    fake_nidaqmx::setCounterInputSamples(counts, 3);
    fake_nidaqmx::setNextCounterReadSamples(2);
    std::memset(observed, 0, sizeof(observed));
    observedCount = 0;
    if (!check(DAQmxReadCounterU32(counter,
                                   3,
                                   0.25,
                                   observed,
                                   3,
                                   &observedCount,
                                   nullptr) == 0 &&
                   observedCount == 2 && observed[0] == 7u && observed[1] == 8u,
               "Fake should reset counter read offsets for a newly queued block")) return false;
    fake_nidaqmx::failNext(fake_nidaqmx::Operation::ReadCounter,
                           -88705,
                           "injected counter disconnect");
    if (!check(DAQmxReadCounterU32(counter,
                                   1,
                                   0.25,
                                   observed,
                                   3,
                                   &observedCount,
                                   nullptr) == -88705,
               "Fake should inject counter disconnect")) return false;
    if (!check(DAQmxStopTask(counter) == 0 && DAQmxClearTask(counter) == 0,
               "Fake should stop and clear counter input")) return false;

    TaskHandle pulse = nullptr;
    if (!check(DAQmxCreateTask("counter-output", &pulse) == 0 &&
                   DAQmxCreateCOPulseChanFreq(pulse,
                                              "PXI1Slot2/ctr1",
                                              "",
                                              DAQmx_Val_Hz,
                                              DAQmx_Val_Low,
                                              0.0,
                                              1000.0,
                                              0.5) == 0 &&
                   DAQmxCfgImplicitTiming(pulse, DAQmx_Val_ContSamps, 0) == 0 &&
                   DAQmxStartTask(pulse) == 0 &&
                   DAQmxStopTask(pulse) == 0 && DAQmxClearTask(pulse) == 0,
               "Fake should model continuous counter output")) return false;

    return check(fake_nidaqmx::createCounterInputCalls() == 1 &&
                     fake_nidaqmx::createCounterOutputCalls() == 1 &&
                     fake_nidaqmx::configureAnalogStartTriggerCalls() == 1,
                 "Fake should retain AO and counter task classifications");
}

bool runFakeTimedDigitalBlockContractTest()
{
    fake_nidaqmx::reset();

    TaskHandle input = nullptr;
    if (!check(DAQmxCreateTask("finite-di", &input) == 0 &&
                   DAQmxCreateDIChan(input,
                                     "PXI1Slot2/port0/line0",
                                     "",
                                     DAQmx_Val_ChanForAllLines) == 0 &&
                   DAQmxCfgSampClkTiming(input,
                                         "/PXI1Slot2/PFI3",
                                         5000.0,
                                         DAQmx_Val_Rising,
                                         DAQmx_Val_FiniteSamps,
                                         3) == 0,
               "Fake should configure finite hardware-timed DI")) return false;
    const uInt32 inputMasks[] = {0x1u, 0x2u, 0x4u};
    fake_nidaqmx::setDigitalInputBlock(inputMasks, 3);
    if (!check(DAQmxStartTask(input) == 0,
               "Fake should start finite DI")) return false;
    uInt32 observed[3]{};
    int32 read = 0;
    if (!check(DAQmxReadDigitalU32(input,
                                   3,
                                   0.25,
                                   DAQmx_Val_GroupByChannel,
                                   observed,
                                   3,
                                   &read,
                                   nullptr) == 0 &&
                   read == 3 && observed[0] == 0x1u && observed[2] == 0x4u,
               "Fake should preserve finite DI blocks")) return false;
    if (!check(DAQmxStopTask(input) == 0 && DAQmxClearTask(input) == 0,
               "Fake should stop and clear finite DI")) return false;

    TaskHandle output = nullptr;
    if (!check(DAQmxCreateTask("continuous-do", &output) == 0 &&
                   DAQmxCreateDOChan(output,
                                     "PXI1Slot2/port0/line0",
                                     "",
                                     DAQmx_Val_ChanForAllLines) == 0 &&
                   DAQmxCfgSampClkTiming(output,
                                         "/PXI1Slot2/PFI4",
                                         5000.0,
                                         DAQmx_Val_Rising,
                                         DAQmx_Val_ContSamps,
                                         0) == 0 &&
                   DAQmxStartTask(output) == 0,
               "Fake should configure and start continuous DO")) return false;
    const uInt32 outputMasks[] = {0x8u, 0x10u, 0x20u};
    int32 written = 0;
    if (!check(DAQmxWriteDigitalU32(output,
                                    3,
                                    0,
                                    0.25,
                                    DAQmx_Val_GroupByChannel,
                                    outputMasks,
                                    &written,
                                    nullptr) == 0 &&
                   written == 3 && fake_nidaqmx::lastDigitalOutputCount() == 3 &&
                   fake_nidaqmx::lastDigitalOutputAt(2) == 0x20u,
               "Fake should preserve continuous DO blocks")) return false;
    return check(DAQmxStopTask(output) == 0 && DAQmxClearTask(output) == 0,
                 "Fake should stop and clear continuous DO");
}

bool runFakeErrorInjectionContractTest()
{
    fake_nidaqmx::reset();

    TaskHandle ai = nullptr;
    if (!check(DAQmxCreateTask("error-ai", &ai) == 0 &&
                   DAQmxCreateAIVoltageChan(ai,
                                             "PXI1Slot2/ai0",
                                             "",
                                             DAQmx_Val_Cfg_Default,
                                             -10.0,
                                             10.0,
                                             DAQmx_Val_Volts,
                                             nullptr) == 0 &&
                   DAQmxStartTask(ai) == 0,
               "Fake error setup should start AI")) return false;
    fake_nidaqmx::failNext(fake_nidaqmx::Operation::ReadAnalog,
                           -200284,
                           "injected analog timeout");
    double value = 0.0;
    int32 read = 0;
    if (!check(DAQmxReadAnalogF64(ai,
                                  1,
                                  0.25,
                                  DAQmx_Val_GroupByChannel,
                                  &value,
                                  1,
                                  &read,
                                  nullptr) == -200284,
               "Fake should inject analog timeout")) return false;
    char error[128]{};
    if (!check(DAQmxGetExtendedErrorInfo(error, sizeof(error)) == 0 &&
                   std::strcmp(error, "injected analog timeout") == 0,
               "Fake should retain injected vendor diagnostics")) return false;
    if (!check(DAQmxStopTask(ai) == 0 && DAQmxClearTask(ai) == 0,
               "Fake should clean up injected AI failure")) return false;

    TaskHandle ao = nullptr;
    if (!check(DAQmxCreateTask("error-ao", &ao) == 0 &&
                   DAQmxCreateAOVoltageChan(ao,
                                             "PXI1Slot2/ao0",
                                             "",
                                             -10.0,
                                             10.0,
                                             DAQmx_Val_Volts,
                                             nullptr) == 0 &&
                   DAQmxStartTask(ao) == 0,
               "Fake error setup should start AO")) return false;
    fake_nidaqmx::failNext(fake_nidaqmx::Operation::WriteAnalog,
                           -200290,
                           "injected output underflow");
    int32 written = 0;
    if (!check(DAQmxWriteAnalogF64(ao,
                                   1,
                                   0,
                                   0.25,
                                   DAQmx_Val_GroupByChannel,
                                   &value,
                                   &written,
                                   nullptr) == -200290,
               "Fake should inject analog underflow")) return false;
    return check(DAQmxStopTask(ao) == 0 && DAQmxClearTask(ao) == 0,
                 "Fake should clean up injected AO failure");
}

bool runAdapterTaskApiTest(const HalAdapterApiV1& api,
                           const HalAdapterTaskApiV1& taskApi)
{
    usePxiFixture();
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "task API setup should initialize the adapter")) return false;
    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "PXI1Slot2", validOpenSpec(), &device);
    if (!check(status.code == HAL_ADAPTER_OK && device != nullptr,
               "task API setup should open the projected PXI device")) {
        api.shutdown(adapter);
        return false;
    }

    const int aiChannels[] = {0, 1};
    HalAdapterTaskConfig aiConfig{};
    aiConfig.structSize = sizeof(aiConfig);
    aiConfig.kind = HAL_ADAPTER_TASK_ANALOG_INPUT;
    aiConfig.mode = HAL_ADAPTER_TASK_FINITE;
    aiConfig.channelIndexes = aiChannels;
    aiConfig.channelCount = 2;
    aiConfig.sampleRateHz = 20000.0;
    aiConfig.samplesPerChannel = 4;
    aiConfig.bufferSamplesPerChannel = 16;
    aiConfig.sampleClockSource = "/PXI1Slot2/PFI0";
    aiConfig.sampleClockEdge = HAL_ADAPTER_EDGE_RISING;
    aiConfig.triggerType = HAL_ADAPTER_TRIGGER_DIGITAL_EDGE;
    aiConfig.triggerRole = HAL_ADAPTER_TRIGGER_START;
    aiConfig.triggerSource = "/PXI1Slot2/PFI1";
    aiConfig.triggerEdge = HAL_ADAPTER_EDGE_RISING;
    HalAdapterTaskHandle ai = nullptr;
    status = taskApi.createTask(device, &aiConfig, &ai);
    if (!check(status.code == HAL_ADAPTER_OK && ai != nullptr,
               "task API should create finite two-channel AI")) return false;
    const double aiInput[] = {1.0, 2.0, 3.0, 4.0, 11.0, 12.0, 13.0, 14.0};
    fake_nidaqmx::setAnalogInputBlock(aiInput, 2, 4);
    if (!check(taskApi.startTask(ai, 1000).code == HAL_ADAPTER_OK,
               "task API should start finite AI")) return false;
    double aiOutput[8]{};
    HalAdapterTaskBuffer aiBuffer{};
    aiBuffer.structSize = sizeof(aiBuffer);
    aiBuffer.sampleType = HAL_ADAPTER_SAMPLE_FLOAT64;
    aiBuffer.data = aiOutput;
    aiBuffer.capacityValues = 8;
    aiBuffer.channelCount = 2;
    aiBuffer.samplesPerChannel = 4;
    status = taskApi.readTask(ai, &aiBuffer, 1000);
    if (!check(status.code == HAL_ADAPTER_OK && aiBuffer.samplesPerChannel == 4 &&
                   aiOutput[0] == 1.0 && aiOutput[3] == 4.0 &&
                   aiOutput[4] == 11.0 && aiOutput[7] == 14.0 &&
                   aiBuffer.timestampUs == nowUs(),
               "task API should preserve the complete channel-major AI block")) return false;
    HalAdapterTaskStatusInfo aiStatus{};
    if (!check(taskApi.getTaskStatus(ai, &aiStatus, 1000).code == HAL_ADAPTER_OK &&
                   aiStatus.totalSamplesPerChannel == 4,
               "task API should report authoritative AI progress")) return false;
    fake_nidaqmx::setAnalogInputBlock(aiInput, 2, 4);
    fake_nidaqmx::failNext(fake_nidaqmx::Operation::ReadAnalog,
                           -200279,
                           "injected task overflow");
    status = taskApi.readTask(ai, &aiBuffer, 1000);
    if (!check(status.code == HAL_ADAPTER_IO_ERROR && status.vendorCode == -200279 &&
                   taskApi.getTaskStatus(ai, &aiStatus, 1000).code == HAL_ADAPTER_OK &&
                   aiStatus.overflowed == 1,
               "task API should retain and report an NI input overflow")) return false;
    if (!check(taskApi.stopTask(ai, 1000).code == HAL_ADAPTER_OK &&
                   taskApi.closeTask(ai).code == HAL_ADAPTER_OK,
               "task API should stop and close finite AI")) return false;

    aiConfig.triggerRole = HAL_ADAPTER_TRIGGER_REFERENCE;
    aiConfig.referencePretriggerSamples = 2;
    HalAdapterTaskHandle referencedAi = nullptr;
    status = taskApi.createTask(device, &aiConfig, &referencedAi);
    if (!check(status.code == HAL_ADAPTER_OK && referencedAi != nullptr &&
                   fake_nidaqmx::configureDigitalReferenceTriggerCalls() == 1,
               "task API should configure a digital-edge reference trigger")) return false;
    if (!check(taskApi.closeTask(referencedAi).code == HAL_ADAPTER_OK,
               "task API should close reference-triggered AI")) return false;
    aiConfig.triggerType = HAL_ADAPTER_TRIGGER_NONE;
    aiConfig.referencePretriggerSamples = 0;
    aiConfig.sampleRateHz = 500001.0;
    HalAdapterTaskHandle excessiveAi = nullptr;
    status = taskApi.createTask(device, &aiConfig, &excessiveAi);
    if (!check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && excessiveAi == nullptr,
               "two-channel AI should enforce the 1 MS/s aggregate conversion limit")) return false;

    const int aoChannel[] = {0};
    HalAdapterTaskConfig aoConfig{};
    aoConfig.structSize = sizeof(aoConfig);
    aoConfig.kind = HAL_ADAPTER_TASK_ANALOG_OUTPUT;
    aoConfig.mode = HAL_ADAPTER_TASK_CONTINUOUS;
    aoConfig.channelIndexes = aoChannel;
    aoConfig.channelCount = 1;
    aoConfig.sampleRateHz = 10000.0;
    aoConfig.samplesPerChannel = 4;
    aoConfig.bufferSamplesPerChannel = 32;
    aoConfig.sampleClockSource = "";
    aoConfig.triggerType = HAL_ADAPTER_TRIGGER_ANALOG_EDGE;
    aoConfig.triggerRole = HAL_ADAPTER_TRIGGER_START;
    aoConfig.triggerSource = "PXI1Slot2/ai0";
    aoConfig.triggerLevel = 1.25;
    HalAdapterTaskHandle ao = nullptr;
    status = taskApi.createTask(device, &aoConfig, &ao);
    if (!check(status.code == HAL_ADAPTER_OK && ao != nullptr,
               "task API should configure continuous AO")) return false;
    double aoValues[] = {0.5, 1.5, 2.5, 3.5};
    HalAdapterTaskBuffer aoBuffer{};
    aoBuffer.structSize = sizeof(aoBuffer);
    aoBuffer.sampleType = HAL_ADAPTER_SAMPLE_FLOAT64;
    aoBuffer.data = aoValues;
    aoBuffer.capacityValues = 4;
    aoBuffer.channelCount = 1;
    aoBuffer.samplesPerChannel = 4;
    status = taskApi.writeTask(ao, &aoBuffer, 0, 1000);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   fake_nidaqmx::lastAnalogOutputCount() == 4 &&
                   fake_nidaqmx::lastAnalogOutputAt(3) == 3.5,
               "task API should prefill continuous AO blocks before start")) return false;
    if (!check(taskApi.startTask(ao, 1000).code == HAL_ADAPTER_OK,
               "task API should start prefilled continuous AO")) return false;
    fake_nidaqmx::failNext(fake_nidaqmx::Operation::WriteAnalog,
                           -200290,
                           "injected task underflow");
    status = taskApi.writeTask(ao, &aoBuffer, 0, 1000);
    HalAdapterTaskStatusInfo aoStatus{};
    if (!check(status.code == HAL_ADAPTER_IO_ERROR && status.vendorCode == -200290 &&
                   taskApi.getTaskStatus(ao, &aoStatus, 1000).code == HAL_ADAPTER_OK &&
                   aoStatus.underflowed == 1,
               "task API should retain and report an NI output underflow")) return false;
    if (!check(taskApi.stopTask(ao, 1000).code == HAL_ADAPTER_OK &&
                   taskApi.closeTask(ao).code == HAL_ADAPTER_OK,
               "task API should stop and close continuous AO")) return false;

    aoConfig.triggerRole = HAL_ADAPTER_TRIGGER_PAUSE;
    HalAdapterTaskHandle pausedAo = nullptr;
    status = taskApi.createTask(device, &aoConfig, &pausedAo);
    if (!check(status.code == HAL_ADAPTER_OK && pausedAo != nullptr &&
                   fake_nidaqmx::configurePauseTriggerCalls() >= 1,
               "task API should configure an analog-level pause trigger")) return false;
    if (!check(taskApi.closeTask(pausedAo).code == HAL_ADAPTER_OK,
               "task API should close pause-triggered AO")) return false;

    const int doChannels[] = {0, 1};
    HalAdapterTaskConfig doConfig{};
    doConfig.structSize = sizeof(doConfig);
    doConfig.kind = HAL_ADAPTER_TASK_DIGITAL_OUTPUT;
    doConfig.mode = HAL_ADAPTER_TASK_CONTINUOUS;
    doConfig.channelIndexes = doChannels;
    doConfig.channelCount = 2;
    doConfig.sampleRateHz = 5000.0;
    doConfig.samplesPerChannel = 3;
    doConfig.bufferSamplesPerChannel = 12;
    doConfig.sampleClockSource = "/PXI1Slot2/PFI4";
    HalAdapterTaskHandle digital = nullptr;
    status = taskApi.createTask(device, &doConfig, &digital);
    if (!check(status.code == HAL_ADAPTER_OK && digital != nullptr,
               "task API should configure hardware-timed DO")) return false;
    uInt32 doValues[] = {0u, 1u, 0u, 1u, 0u, 1u};
    HalAdapterTaskBuffer doBuffer{};
    doBuffer.structSize = sizeof(doBuffer);
    doBuffer.sampleType = HAL_ADAPTER_SAMPLE_UINT32;
    doBuffer.data = doValues;
    doBuffer.capacityValues = 6;
    doBuffer.channelCount = 2;
    doBuffer.samplesPerChannel = 3;
    status = taskApi.writeTask(digital, &doBuffer, 0, 1000);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   fake_nidaqmx::lastDigitalOutputCount() == 6,
               "task API should prefill all hardware-timed DO channel samples")) return false;
    if (!check(taskApi.startTask(digital, 1000).code == HAL_ADAPTER_OK,
               "task API should start prefilled hardware-timed DO")) return false;
    if (!check(taskApi.stopTask(digital, 1000).code == HAL_ADAPTER_OK &&
                   taskApi.closeTask(digital).code == HAL_ADAPTER_OK,
               "task API should stop and close hardware-timed DO")) return false;
    doConfig.sampleRateHz = 10000001.0;
    HalAdapterTaskHandle excessiveDo = nullptr;
    status = taskApi.createTask(device, &doConfig, &excessiveDo);
    if (!check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && excessiveDo == nullptr,
               "hardware-timed DO should enforce the PXI-6259 10 MHz limit")) return false;

    const int diChannels[] = {18, 19};
    HalAdapterTaskConfig diConfig{};
    diConfig.structSize = sizeof(diConfig);
    diConfig.kind = HAL_ADAPTER_TASK_DIGITAL_INPUT;
    diConfig.mode = HAL_ADAPTER_TASK_FINITE;
    diConfig.channelIndexes = diChannels;
    diConfig.channelCount = 2;
    diConfig.sampleRateHz = 5000.0;
    diConfig.samplesPerChannel = 3;
    diConfig.bufferSamplesPerChannel = 12;
    diConfig.sampleClockSource = "/PXI1Slot2/PFI3";
    HalAdapterTaskHandle digitalInput = nullptr;
    status = taskApi.createTask(device, &diConfig, &digitalInput);
    if (!check(status.code == HAL_ADAPTER_OK && digitalInput != nullptr,
               "task API should configure finite hardware-timed DI")) return false;
    const uInt32 diValues[] = {0u, 1u, 0u, 1u, 1u, 0u};
    fake_nidaqmx::setDigitalInputChannelsBlock(diValues, 2, 3);
    if (!check(taskApi.startTask(digitalInput, 1000).code == HAL_ADAPTER_OK,
               "task API should start finite DI")) return false;
    uInt32 observedDi[6]{};
    HalAdapterTaskBuffer diBuffer{};
    diBuffer.structSize = sizeof(diBuffer);
    diBuffer.sampleType = HAL_ADAPTER_SAMPLE_UINT32;
    diBuffer.data = observedDi;
    diBuffer.capacityValues = 6;
    diBuffer.channelCount = 2;
    diBuffer.samplesPerChannel = 3;
    status = taskApi.readTask(digitalInput, &diBuffer, 1000);
    if (!check(status.code == HAL_ADAPTER_OK && observedDi[1] == 1u &&
                   observedDi[3] == 1u && observedDi[4] == 1u,
               "task API should preserve all hardware-timed DI channel samples")) return false;
    if (!check(taskApi.stopTask(digitalInput, 1000).code == HAL_ADAPTER_OK &&
                   taskApi.closeTask(digitalInput).code == HAL_ADAPTER_OK,
               "task API should stop and close hardware-timed DI")) return false;
    const int staticPfiInput[] = {16};
    diConfig.channelIndexes = staticPfiInput;
    diConfig.channelCount = 1;
    HalAdapterTaskHandle timedPfi = nullptr;
    status = taskApi.createTask(device, &diConfig, &timedPfi);
    if (!check(status.code == HAL_ADAPTER_NOT_SUPPORTED && timedPfi == nullptr,
               "port1/2 PFI lines should reject correlated hardware-timed DIO tasks")) return false;

    const int counterChannel[] = {0};
    HalAdapterTaskConfig counterConfig{};
    counterConfig.structSize = sizeof(counterConfig);
    counterConfig.kind = HAL_ADAPTER_TASK_COUNTER_INPUT;
    counterConfig.mode = HAL_ADAPTER_TASK_FINITE;
    counterConfig.channelIndexes = counterChannel;
    counterConfig.channelCount = 1;
    counterConfig.sampleRateHz = 10000.0;
    counterConfig.samplesPerChannel = 3;
    counterConfig.sampleClockSource = "/PXI1Slot2/PFI5";
    counterConfig.counterMode = HAL_ADAPTER_COUNTER_COUNT_EDGES;
    HalAdapterTaskHandle counter = nullptr;
    status = taskApi.createTask(device, &counterConfig, &counter);
    if (!check(status.code == HAL_ADAPTER_OK && counter != nullptr,
               "task API should create finite edge-count input")) return false;
    const uInt32 counts[] = {7u, 8u, 9u};
    fake_nidaqmx::setCounterInputSamples(counts, 3);
    if (!check(taskApi.startTask(counter, 1000).code == HAL_ADAPTER_OK,
               "task API should start counter input")) return false;
    uInt32 observedCounts[3]{};
    HalAdapterTaskBuffer counterBuffer{};
    counterBuffer.structSize = sizeof(counterBuffer);
    counterBuffer.sampleType = HAL_ADAPTER_SAMPLE_UINT32;
    counterBuffer.data = observedCounts;
    counterBuffer.capacityValues = 3;
    counterBuffer.channelCount = 1;
    counterBuffer.samplesPerChannel = 3;
    status = taskApi.readTask(counter, &counterBuffer, 1000);
    if (!check(status.code == HAL_ADAPTER_OK && observedCounts[0] == 7u &&
                   observedCounts[2] == 9u,
               "task API should return edge-count samples")) return false;
    if (!check(taskApi.stopTask(counter, 1000).code == HAL_ADAPTER_OK &&
                   taskApi.closeTask(counter).code == HAL_ADAPTER_OK,
               "task API should stop and close counter input")) return false;

    const int pulseChannel[] = {1};
    HalAdapterTaskConfig pulseConfig{};
    pulseConfig.structSize = sizeof(pulseConfig);
    pulseConfig.kind = HAL_ADAPTER_TASK_COUNTER_OUTPUT;
    pulseConfig.mode = HAL_ADAPTER_TASK_CONTINUOUS;
    pulseConfig.channelIndexes = pulseChannel;
    pulseConfig.channelCount = 1;
    pulseConfig.samplesPerChannel = 1;
    pulseConfig.counterMode = HAL_ADAPTER_COUNTER_PULSE_FREQUENCY;
    pulseConfig.counterFrequencyHz = 1000.0;
    pulseConfig.counterDutyCycle = 0.5;
    HalAdapterTaskHandle pulse = nullptr;
    status = taskApi.createTask(device, &pulseConfig, &pulse);
    HalAdapterTaskStatusInfo pulseStatus{};
    if (!check(status.code == HAL_ADAPTER_OK && pulse != nullptr &&
                   taskApi.startTask(pulse, 1000).code == HAL_ADAPTER_OK &&
                   taskApi.getTaskStatus(pulse, &pulseStatus, 1000).code == HAL_ADAPTER_OK &&
                   pulseStatus.started == 1 &&
                   taskApi.stopTask(pulse, 1000).code == HAL_ADAPTER_OK &&
                   taskApi.closeTask(pulse).code == HAL_ADAPTER_OK,
               "task API should run continuous counter pulse output")) return false;

    status = api.closeDevice(device);
    if (!check(status.code == HAL_ADAPTER_OK,
               "task API integration should close the device safely")) return false;
    return check(api.shutdown(adapter).code == HAL_ADAPTER_OK,
                 "task API integration should release adapter state");
}

bool runSafeShutdownOrderTest(const HalAdapterApiV1& api,
                              const HalAdapterTaskApiV1& taskApi)
{
    usePxiFixture();
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "safe-order setup should initialize the adapter")) return false;
    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "PXI1Slot2", validOpenSpec(), &device);
    if (!check(status.code == HAL_ADAPTER_OK && device != nullptr,
               "safe-order setup should open the device")) {
        api.shutdown(adapter);
        return false;
    }
    const int channel[] = {0};
    HalAdapterTaskConfig config{};
    config.structSize = sizeof(config);
    config.kind = HAL_ADAPTER_TASK_ANALOG_INPUT;
    config.mode = HAL_ADAPTER_TASK_CONTINUOUS;
    config.channelIndexes = channel;
    config.channelCount = 1;
    config.sampleRateHz = 1000.0;
    config.samplesPerChannel = 16;
    HalAdapterTaskHandle task = nullptr;
    status = taskApi.createTask(device, &config, &task);
    if (!check(status.code == HAL_ADAPTER_OK && task != nullptr &&
                   taskApi.startTask(task, 1000).code == HAL_ADAPTER_OK,
               "safe-order setup should start a continuous task")) return false;

    fake_nidaqmx::clearCallLog();
    status = api.closeDevice(device);
    const int firstStop = fake_nidaqmx::firstCallIndex(fake_nidaqmx::Call::StopTask);
    const int firstSafeWrite = fake_nidaqmx::firstCallIndex(fake_nidaqmx::Call::WriteDigital);
    const int lastClear = fake_nidaqmx::lastCallIndex(fake_nidaqmx::Call::ClearTask);
    bool passed = check(status.code == HAL_ADAPTER_OK,
                        "safe-order close should complete") &&
        check(firstStop >= 0 && firstSafeWrite >= 0 && lastClear >= 0 &&
                  firstStop < firstSafeWrite && firstSafeWrite < lastClear,
              "safe shutdown must stop active tasks before safe writes and clear last");
    api.shutdown(adapter);
    return passed;
}

bool runCompletedFiniteTaskSkipsInterruptedStopTest(
    const HalAdapterApiV1& api,
    const HalAdapterTaskApiV1& taskApi)
{
    usePxiFixture();
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "completed finite stop setup should initialize the adapter")) return false;

    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "PXI1Slot2", validOpenSpec(), &device);
    if (!check(status.code == HAL_ADAPTER_OK && device != nullptr,
               "completed finite stop setup should open the device")) {
        api.shutdown(adapter);
        return false;
    }

    const int channel[] = {0};
    HalAdapterTaskConfig config{};
    config.structSize = sizeof(config);
    config.kind = HAL_ADAPTER_TASK_ANALOG_INPUT;
    config.mode = HAL_ADAPTER_TASK_FINITE;
    config.channelIndexes = channel;
    config.channelCount = 1;
    config.sampleRateHz = 10000.0;
    config.samplesPerChannel = 4;
    config.bufferSamplesPerChannel = 4;
    HalAdapterTaskHandle task = nullptr;
    status = taskApi.createTask(device, &config, &task);
    if (!check(status.code == HAL_ADAPTER_OK && task != nullptr,
               "completed finite stop setup should create the task")) return false;

    const double input[] = {1.0, 2.0, 3.0, 4.0};
    fake_nidaqmx::setAnalogInputBlock(input, 1, 4);
    if (!check(taskApi.startTask(task, 1000).code == HAL_ADAPTER_OK,
               "completed finite stop setup should start the task")) return false;
    double output[4]{};
    HalAdapterTaskBuffer buffer{};
    buffer.structSize = sizeof(buffer);
    buffer.sampleType = HAL_ADAPTER_SAMPLE_FLOAT64;
    buffer.data = output;
    buffer.capacityValues = 4;
    buffer.channelCount = 1;
    buffer.samplesPerChannel = 4;
    if (!check(taskApi.readTask(task, &buffer, 1000).code == HAL_ADAPTER_OK &&
                   buffer.samplesPerChannel == 4,
               "completed finite stop setup should consume the finite acquisition")) {
        return false;
    }

    fake_nidaqmx::clearCallLog();
    fake_nidaqmx::failNext(fake_nidaqmx::Operation::StopTask,
                           -50700,
                           "injected PAL wait interruption");
    const HalAdapterStatus stopped = taskApi.stopTask(task, 1000);
    const bool passed = check(stopped.code == HAL_ADAPTER_OK,
                              "a naturally completed finite task should not surface an interrupted redundant stop") &&
        check(fake_nidaqmx::firstCallIndex(fake_nidaqmx::Call::QueryTaskDone) >= 0,
              "finite stop should query native completion") &&
        check(fake_nidaqmx::firstCallIndex(fake_nidaqmx::Call::StopTask) < 0,
              "finite stop should skip DAQmxStopTask after native completion");

    const bool closed = check(taskApi.closeTask(task).code == HAL_ADAPTER_OK,
                              "completed finite task should still clear normally");
    const bool deviceClosed = check(api.closeDevice(device).code == HAL_ADAPTER_OK,
                                    "completed finite stop test should close the device");
    const bool shutdown = check(api.shutdown(adapter).code == HAL_ADAPTER_OK,
                                "completed finite stop test should shut down the adapter");
    return passed && closed && deviceClosed && shutdown;
}

} // namespace

int main()
{
    HalAdapterHostApiV1 host{};
    host.abiVersion = HAL_ADAPTER_ABI_VERSION;
    host.log = &logMessage;
    host.nowUs = &nowUs;
    HalAdapterApiV1 api{};
    if (!check(hal_adapter_get_api_v1(&host, &api) == 0,
               "ABI entry point should initialize the function table")) return EXIT_FAILURE;
    if (!check(api.abiVersion == HAL_ADAPTER_ABI_VERSION &&
                   api.structSize == static_cast<int>(sizeof(HalAdapterApiV1)),
               "ABI metadata should match v1")) return EXIT_FAILURE;
    HalAdapterTaskApiV1 taskApi{};
    if (!check(hal_adapter_get_task_api_v1(&host, &taskApi) == 0 &&
                   taskApi.abiVersion == HAL_ADAPTER_TASK_ABI_VERSION &&
                   taskApi.structSize == static_cast<int>(sizeof(HalAdapterTaskApiV1)),
               "optional task ABI metadata should match v1")) return EXIT_FAILURE;
    bool passed = true;
    passed = runLifecycleTest(api) && passed;
    passed = runIdentityMismatchTest(api) && passed;
    passed = runCloseFailureStillShutsDownTest(api) && passed;
    passed = runLegacyFullHalConfigRejectedTest(api) && passed;
    passed = runAnalogSurfaceTest(api) && passed;
    passed = runPxi6259IdentityAndTopologyTest(api) && passed;
    passed = runPxi6733ProfileTest(api, taskApi) && passed;
    passed = runPxiDeploymentGateAndBoundaryTest(api) && passed;
    passed = runFakeFiniteAiTaskContractTest() && passed;
    passed = runFakeContinuousAoCounterTaskContractTest() && passed;
    passed = runFakeTimedDigitalBlockContractTest() && passed;
    passed = runFakeErrorInjectionContractTest() && passed;
    passed = runAdapterTaskApiTest(api, taskApi) && passed;
    passed = runCompletedFiniteTaskSkipsInterruptedStopTest(api, taskApi) && passed;
    passed = runSafeShutdownOrderTest(api, taskApi) && passed;
    if (!passed) return EXIT_FAILURE;
    std::cout << "NI-DAQmx fake adapter tests passed\n";
    return EXIT_SUCCESS;
}
