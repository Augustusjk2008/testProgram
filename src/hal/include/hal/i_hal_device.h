#pragma once

#include "hal_global.h"
#include "hal_types.h"
#include "i_control_channel.h"

namespace hwtest::hal {

class IAnalogIo;
class ICanFdBus;
class IDigitalIo;
class ISerialBus;

class HWTEST_HAL_EXPORT IHalDevice {
public:
    virtual ~IHalDevice() = default;

    virtual DeviceDescriptor descriptor() const = 0;
    virtual DeviceCapabilities capabilities() const = 0;

    virtual IAnalogIo* analogIo() = 0;
    virtual IDigitalIo* digitalIo() = 0;
    virtual ISerialBus* serialBus() = 0;
    virtual ICanFdBus* canFdBus() = 0;
    virtual IControlChannel* controlChannel() { return nullptr; }
};

} // namespace hwtest::hal
