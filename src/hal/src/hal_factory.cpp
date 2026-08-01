#include "hal/hal_factory.h"

#include "c_abi_adapter.h"
#include "hal_service.h"

#include <QSerialPortInfo>

#include <algorithm>

namespace hwtest::hal {

IHalService* createHalService(QObject* parent)
{
    return new HalService(parent);
}

void destroyHalService(IHalService* service)
{
    delete service;
}

QVector<SerialPortDescriptor> availableSerialPorts()
{
    QVector<SerialPortDescriptor> result;
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    result.reserve(ports.size());
    for (const QSerialPortInfo& port : ports) {
        const QString portName = port.portName().trimmed();
        if (portName.isEmpty()) {
            continue;
        }
        result.push_back(SerialPortDescriptor{portName,
                                              port.description(),
                                              port.manufacturer(),
                                              port.serialNumber(),
                                              port.systemLocation()});
    }
    std::sort(result.begin(), result.end(), [](const SerialPortDescriptor& left,
                                               const SerialPortDescriptor& right) {
        const int folded = QString::compare(left.portName,
                                            right.portName,
                                            Qt::CaseInsensitive);
        return folded == 0 ? left.portName < right.portName : folded < 0;
    });
    return result;
}

HalResult<QVector<DeviceDescriptor>> enumerateCAbiAdapterDevices(
    const QVariantMap& driverConfig,
    const OperationOptions& options)
{
    CAbiAdapter adapter;
    const HalStatus initialized = adapter.initialize(driverConfig);
    if (!initialized.ok()) {
        return HalResult<QVector<DeviceDescriptor>>{initialized, {}};
    }

    HalResult<QVector<DeviceDescriptor>> result = adapter.enumerateDevices(options);
    const HalStatus stopped = adapter.shutdown();
    if (result.ok() && !stopped.ok()) {
        result.status = stopped;
    }
    return result;
}

} // namespace hwtest::hal
