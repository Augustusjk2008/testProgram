#include "hal/hal_adapter_abi.h"

#include "fake_nidaqmx_control.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

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
      "settings":{
        "deviceName":"DevFixture",
        "expectedProductType":"USB-6259",
        "serialNumber":"62590001",
        "timeoutSeconds":0.25
      },
      "hardware":{
        "devices":[
          {"alias":"ni6259_stimulus","model":"USB-6259","serialNumber":"62590001"}
        ],
        "resources":{
          "OUT0":{"device":"ni6259_stimulus","module":"digital","direction":"output","physicalIndex":0,"properties":{"portNumber":0,"lineNumber":0}},
          "OUT1":{"device":"ni6259_stimulus","module":"digital","direction":"output","physicalIndex":1,"properties":{"portNumber":0,"lineNumber":1}},
          "OUT2":{"device":"ni6259_stimulus","module":"digital","direction":"output","physicalIndex":2,"properties":{"portNumber":0,"lineNumber":2}},
          "IN0":{"device":"ni6259_stimulus","module":"digital","direction":"input","physicalIndex":16,"properties":{"portNumber":1,"lineNumber":0}},
          "IN1":{"device":"ni6259_stimulus","module":"digital","direction":"input","physicalIndex":17,"properties":{"portNumber":1,"lineNumber":1}}
        }
      },
      "safeState":{"OUT0":"Low","OUT1":"High","OUT2":"Low"}
    })json";
}

bool runLifecycleTest(const HalAdapterApiV1& api)
{
    fake_nidaqmx::reset();
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "initialize should validate the configured NI device")) return false;

    int count = 0;
    status = api.enumerateDevices(adapter, nullptr, &count, 1000);
    if (!check(status.code == HAL_ADAPTER_OK && count == 1,
               "enumerate size query should report one configured device")) return false;
    HalAdapterDeviceInfo info{};
    status = api.enumerateDevices(adapter, &info, &count, 1000);
    if (!check(status.code == HAL_ADAPTER_OK &&
                   std::strcmp(info.deviceId, "DevFixture") == 0 &&
                   std::strcmp(info.model, "USB-6259") == 0,
               "enumerate should expose the validated physical device")) return false;

    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "DevFixture", "{}", &device);
    if (!check(status.code == HAL_ADAPTER_OK && device != nullptr,
               "open should create persistent digital tasks")) return false;
    if (!check(fake_nidaqmx::createDoCalls() == 1 &&
                    fake_nidaqmx::createDiCalls() == 1 &&
                    fake_nidaqmx::startCalls() == 2 &&
                    std::strcmp(fake_nidaqmx::lastDoPhysicalChannel(),
                                "DevFixture/port0/line0:2") == 0 &&
                    std::strcmp(fake_nidaqmx::lastDiPhysicalChannel(),
                                "DevFixture/port1/line0:1") == 0,
               "open should create and start one task per configured port bank")) return false;
    if (!check(fake_nidaqmx::writeCalls() == 1 &&
                   fake_nidaqmx::lastWrittenMask() == 0x2u,
               "open should apply the complete configured safe mask")) return false;

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
                   fake_nidaqmx::stopCalls() == 2 &&
                   fake_nidaqmx::clearCalls() == 2 &&
                   fake_nidaqmx::resetDeviceCalls() == 0,
               "close should write safe state before stopping and clearing tasks")) return false;

    status = api.shutdown(adapter);
    return check(status.code == HAL_ADAPTER_OK,
                 "shutdown should release the adapter state");
}

bool runIdentityMismatchTest(const HalAdapterApiV1& api)
{
    const char* badConfig = R"json({
      "settings":{"deviceName":"DevFixture","expectedProductType":"USB-6259","serialNumber":"999"},
      "hardware":{
        "devices":[{"alias":"ni6259_stimulus","model":"USB-6259","serialNumber":"999"}],
        "resources":{"OUT0":{"device":"ni6259_stimulus","module":"digital","direction":"output","physicalIndex":0,"properties":{"portNumber":0,"lineNumber":0}}}
      },
      "safeState":{"OUT0":"Low"}
    })json";
    HalAdapterHandle handle = nullptr;
    const HalAdapterStatus status = api.initialize(badConfig, &handle);
    return check(status.code == HAL_ADAPTER_NOT_FOUND && handle == nullptr,
                 "serial mismatch must fail before opening a task");
}

bool runCloseFailureStillShutsDownTest(const HalAdapterApiV1& api)
{
    fake_nidaqmx::reset();
    HalAdapterHandle adapter = nullptr;
    HalAdapterStatus status = api.initialize(validConfig(), &adapter);
    if (!check(status.code == HAL_ADAPTER_OK && adapter != nullptr,
               "close-failure setup should initialize the adapter")) return false;
    HalAdapterDeviceHandle device = nullptr;
    status = api.openDevice(adapter, "DevFixture", "{}", &device);
    if (!check(status.code == HAL_ADAPTER_OK && device != nullptr,
               "close-failure setup should open the device")) return false;

    fake_nidaqmx::failNextWrite(-88705, "configured device was removed");
    status = api.closeDevice(device);
    if (!check(status.code == HAL_ADAPTER_DEVICE_DISCONNECTED &&
                   fake_nidaqmx::stopCalls() == 2 &&
                   fake_nidaqmx::clearCalls() == 2,
               "failed safe write must still stop and clear every task")) return false;

    status = api.shutdown(adapter);
    return check(status.code == HAL_ADAPTER_OK,
                 "close failure must consume the device handle before shutdown");
}

bool runMultipleDeviceConfigRejectedTest(const HalAdapterApiV1& api)
{
    const char* multipleDevices = R"json({
      "settings":{"deviceName":"DevFixture","expectedProductType":"USB-6259","serialNumber":"62590001"},
      "hardware":{
        "devices":[
          {"alias":"stimulus_a","model":"USB-6259","serialNumber":"62590001"},
          {"alias":"stimulus_b","model":"USB-6259","serialNumber":"62590002"}
        ],
        "resources":{
          "OUT0":{"device":"stimulus_a","module":"digital","direction":"output","physicalIndex":0,"properties":{"portNumber":0,"lineNumber":0}}
        }
      },
      "safeState":{"OUT0":"Low"}
    })json";
    HalAdapterHandle handle = nullptr;
    const HalAdapterStatus status = api.initialize(multipleDevices, &handle);
    if (handle != nullptr) api.shutdown(handle);
    return check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && handle == nullptr,
                 "one NI adapter instance must reject multiple logical devices");
}

bool runUsb6259PortBoundsRejectedTest(const HalAdapterApiV1& api)
{
    const char* invalidPort = R"json({
      "settings":{"deviceName":"DevFixture","expectedProductType":"USB-6259","serialNumber":"62590001"},
      "hardware":{
        "devices":[{"alias":"stimulus","model":"USB-6259","serialNumber":"62590001"}],
        "resources":{
          "OUT0":{"device":"stimulus","module":"digital","direction":"output","physicalIndex":0,"properties":{"portNumber":1,"lineNumber":8}}
        }
      },
      "safeState":{"OUT0":"Low"}
    })json";
    HalAdapterHandle handle = nullptr;
    const HalAdapterStatus status = api.initialize(invalidPort, &handle);
    if (handle != nullptr) api.shutdown(handle);
    return check(status.code == HAL_ADAPTER_INVALID_ARGUMENT && handle == nullptr,
                 "USB-6259 port1 and port2 must reject lines above 7");
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
    if (!runLifecycleTest(api)) return EXIT_FAILURE;
    if (!runIdentityMismatchTest(api)) return EXIT_FAILURE;
    if (!runCloseFailureStillShutsDownTest(api)) return EXIT_FAILURE;
    if (!runMultipleDeviceConfigRejectedTest(api)) return EXIT_FAILURE;
    if (!runUsb6259PortBoundsRejectedTest(api)) return EXIT_FAILURE;
    std::cout << "NI-DAQmx fake adapter tests passed\n";
    return EXIT_SUCCESS;
}
