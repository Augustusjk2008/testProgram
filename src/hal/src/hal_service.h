#pragma once

#include "hal/hal_types.h"
#include "hal/i_hal_service.h"

#include "c_abi_adapter.h"
#include "hal_device.h"
#include "resource_mapper.h"

#include <memory>

namespace hwtest::hal {

class HalService final : public IHalService {
public:
    explicit HalService(QObject* parent = nullptr);
    ~HalService() override;

    HalStatus initialize(const QVariantMap& halConfig) override;
    HalStatus shutdown() override;

    HalResult<QVector<DeviceDescriptor>> scanDevices(const OperationOptions& options) override;
    HalResult<DeviceCapabilities> queryCapabilities(const DeviceId& deviceId,
                                                    const OperationOptions& options) override;

    HalResult<SessionId> openDevice(const DeviceId& deviceId,
                                    const OperationOptions& options) override;
    HalStatus closeDevice(const SessionId& sessionId,
                          const OperationOptions& options) override;
    HalStatus resetDevice(const SessionId& sessionId,
                          const OperationOptions& options) override;
    HalStatus healthCheck(const SessionId& sessionId,
                          const OperationOptions& options) override;

    HalResult<IHalDevice*> device(const SessionId& sessionId) override;

private:
    struct SessionEntry {
        std::shared_ptr<HalDevice> device;
        DeviceDescriptor descriptor;
    };

    void emitLogEvent(const HalLogEvent& event);
    void emitLog(const QString& level,
                 const QString& category,
                 const QString& message,
                 const QVariantMap& context = {});
    void emitOperationLog(const QString& operation,
                          const OperationOptions& options,
                          qint64 durationMs,
                          const HalStatus& status,
                          const DeviceId& deviceId = {},
                          const SessionId& sessionId = {},
                          const QVariantMap& context = {});

    std::unique_ptr<HardwareAdapter> createBackend(const QVariantMap& halConfig);
    HalDevice* sessionDevice(const SessionId& sessionId);
    const HalDevice* sessionDevice(const SessionId& sessionId) const;

    QVariantMap m_config;
    ResourceMapper m_mapper;
    std::unique_ptr<HardwareAdapter> m_backend;
    QHash<SessionId, SessionEntry> m_sessions;
    bool m_initialized = false;
};

} // namespace hwtest::hal
