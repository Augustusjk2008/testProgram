#pragma once

#include "adapter_loader.h"
#include "hardware_adapter.h"

#include <QHash>

#include <mutex>

namespace hwtest::hal {

class CAbiAdapter final : public HardwareAdapter {
public:
    CAbiAdapter();
    ~CAbiAdapter() override;

    QString adapterId() const override;

    HalStatus initialize(const QVariantMap& config) override;
    HalStatus shutdown() override;

    HalResult<QVector<DeviceDescriptor>> enumerateDevices(const OperationOptions& options) override;
    HalResult<DeviceCapabilities> queryCapabilities(const DeviceId& deviceId,
                                                    const OperationOptions& options) override;

    HalResult<SessionId> openDevice(const DeviceId& deviceId,
                                    const QVariantMap& openOptions,
                                    const OperationOptions& options) override;
    HalStatus closeDevice(const SessionId& sessionId,
                          const OperationOptions& options) override;
    HalStatus resetDevice(const SessionId& sessionId,
                          const OperationOptions& options) override;
    HalStatus healthCheck(const SessionId& sessionId,
                          const OperationOptions& options) override;

    HalStatus configureAnalog(const SessionId& sessionId,
                              int physicalIndex,
                              const AnalogRange& range,
                              bool output,
                              const OperationOptions& options) override;
    HalResult<AnalogSample> readAnalog(const SessionId& sessionId,
                                       int physicalIndex,
                                       const AnalogReadOptions& options) override;
    HalResult<QVector<AnalogSample>> readAnalogBatch(const SessionId& sessionId,
                                                     const QVector<int>& physicalIndexes,
                                                     const AnalogReadOptions& options) override;
    HalStatus writeAnalog(const SessionId& sessionId,
                          int physicalIndex,
                          double value,
                          const AnalogWriteOptions& options) override;
    HalStatus writeAnalogBatch(const SessionId& sessionId,
                               const QMap<int, double>& values,
                               const AnalogWriteOptions& options) override;

    HalResult<DigitalSample> readDigital(const SessionId& sessionId,
                                         int physicalIndex,
                                         const OperationOptions& options) override;
    HalResult<QVector<DigitalSample>> readDigitalBatch(const SessionId& sessionId,
                                                       const QVector<int>& physicalIndexes,
                                                       const OperationOptions& options) override;
    HalStatus writeDigital(const SessionId& sessionId,
                           int physicalIndex,
                           DigitalLevel level,
                           const DigitalWriteOptions& options) override;
    HalStatus writeDigitalBatch(const SessionId& sessionId,
                                const QMap<int, DigitalLevel>& values,
                                const DigitalWriteOptions& options) override;
    HalResult<DigitalSample> waitDigitalEdge(const SessionId& sessionId,
                                             int physicalIndex,
                                             DigitalLevel targetLevel,
                                             const OperationOptions& options) override;

    HalStatus openSerial(const SessionId& sessionId,
                         int physicalIndex,
                         const SerialConfig& config,
                         const OperationOptions& options) override;
    HalStatus closeSerial(const SessionId& sessionId,
                          int physicalIndex,
                          const OperationOptions& options) override;
    HalStatus flushSerial(const SessionId& sessionId,
                          int physicalIndex,
                          const OperationOptions& options) override;
    HalStatus writeSerial(const SessionId& sessionId,
                          int physicalIndex,
                          const QByteArray& data,
                          const OperationOptions& options) override;
    HalResult<QByteArray> readSerial(const SessionId& sessionId,
                                     int physicalIndex,
                                     int maxBytes,
                                     const OperationOptions& options) override;
    HalResult<SerialTransactionResult> transactSerial(const SessionId& sessionId,
                                                      int physicalIndex,
                                                      const SerialTransaction& transaction) override;

    HalStatus openCan(const SessionId& sessionId,
                      int physicalIndex,
                      const CanFdConfig& config,
                      const OperationOptions& options) override;
    HalStatus closeCan(const SessionId& sessionId,
                       int physicalIndex,
                       const OperationOptions& options) override;
    HalStatus setCanFilters(const SessionId& sessionId,
                            int physicalIndex,
                            const QVector<CanFdFilter>& filters,
                            const OperationOptions& options) override;
    HalStatus sendCan(const SessionId& sessionId,
                      int physicalIndex,
                      const CanFdFrame& frame,
                      const OperationOptions& options) override;
    HalResult<CanFdFrame> receiveCan(const SessionId& sessionId,
                                     int physicalIndex,
                                     const OperationOptions& options) override;
    HalResult<QVector<CanFdFrame>> receiveCanBatch(const SessionId& sessionId,
                                                   int physicalIndex,
                                                   int maxFrames,
                                                   const OperationOptions& options) override;

private:
    HalStatus ensureInitialized(const QString& operation) const;
    HalAdapterDeviceHandle deviceHandle(const SessionId& sessionId,
                                        const QString& operation,
                                        HalStatus* status = nullptr) const;
    HalStatus unsupported(const QString& operation,
                          const DeviceId& deviceId = {}) const;

    AdapterLoader m_loader;
    HalAdapterApiV1 m_api {};
    HalAdapterHandle m_handle = nullptr;
    QHash<SessionId, HalAdapterDeviceHandle> m_devices;
    QHash<SessionId, DeviceId> m_deviceIds;
    QVariantMap m_config;
    QString m_adapterId;
    quint64 m_sessionCounter = 0;
    bool m_initialized = false;
    mutable std::recursive_mutex m_apiMutex;
};

} // namespace hwtest::hal
