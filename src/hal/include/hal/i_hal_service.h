#pragma once

#include "hal_global.h"
#include "hal_types.h"

#include <QObject>

namespace hwtest::hal {

class IHalDevice;

class HWTEST_HAL_EXPORT IHalService : public QObject {
    Q_OBJECT

public:
    explicit IHalService(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    ~IHalService() override = default;

    virtual HalStatus initialize(const QVariantMap& halConfig) = 0;
    virtual HalStatus shutdown() = 0;

    virtual HalResult<QVector<DeviceDescriptor>> scanDevices(const OperationOptions& options) = 0;
    virtual HalResult<DeviceCapabilities> queryCapabilities(const DeviceId& deviceId,
                                                            const OperationOptions& options) = 0;

    virtual HalResult<SessionId> openDevice(const DeviceId& deviceId,
                                            const OperationOptions& options) = 0;
    virtual HalStatus closeDevice(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;
    virtual HalStatus resetDevice(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;
    virtual HalStatus healthCheck(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;

    virtual HalResult<IHalDevice*> device(const SessionId& sessionId) = 0;

signals:
    void deviceChanged(const DeviceDescriptor& device, const QString& state);
    void hardwareEvent(const QString& eventType, const QVariantMap& payload);
    void logProduced(const HalLogEvent& event);
    void logMessage(const QString& level,
                    const QString& category,
                    const QString& message,
                    const QVariantMap& context);
};

} // namespace hwtest::hal
