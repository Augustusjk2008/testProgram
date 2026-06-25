#pragma once

#include "hardware_adapter.h"

#include <QHash>
#include <QQueue>
#include <QRandomGenerator>

namespace hwtest::hal {

class MockAdapter final : public HardwareAdapter {
public:
    MockAdapter();
    ~MockAdapter() override;

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
    struct DeviceState {
        DeviceDescriptor descriptor;
        DeviceCapabilities capabilities;
        QHash<int, AnalogRange> analogInputs;
        QHash<int, AnalogRange> analogOutputs;
        QHash<int, double> analogOutputValues;
        QHash<int, DigitalLevel> digitalInputs;
        QHash<int, DigitalLevel> digitalOutputs;
        QHash<int, SerialConfig> serialConfigs;
        QHash<int, QByteArray> serialBuffers;
        QHash<int, CanFdConfig> canConfigs;
        QHash<int, QQueue<CanFdFrame>> canReceiveQueues;
    };

    struct SessionState {
        QString sessionId;
        QString deviceId;
        QVariantMap openOptions;
    };

    DeviceState* deviceForSession(const SessionId& sessionId);
    const DeviceState* deviceForSession(const SessionId& sessionId) const;
    DeviceState* deviceForId(const DeviceId& deviceId);
    const DeviceState* deviceForId(const DeviceId& deviceId) const;
    SessionState* sessionForId(const SessionId& sessionId);
    const SessionState* sessionForId(const SessionId& sessionId) const;
    bool ensureSession(const SessionId& sessionId, const QString& operation, HalStatus* status) const;
    static AnalogSample makeAnalogSample(const SessionId& sessionId,
                                         int physicalIndex,
                                         double value,
                                         const AnalogReadOptions& options,
                                         const QVariantMap& metadata = {});
    static DigitalSample makeDigitalSample(const SessionId& sessionId,
                                          int physicalIndex,
                                          DigitalLevel level,
                                          const QVariantMap& metadata = {});
    void ensureDefaultState(DeviceState& state);
    QString nextSessionId(const DeviceId& deviceId) const;

    QVariantMap m_config;
    QVector<DeviceDescriptor> m_devices;
    QHash<QString, DeviceState> m_deviceStateById;
    QHash<QString, SessionState> m_sessionsById;
    QString m_adapterId = QStringLiteral("mock.adapter.v1");
    bool m_initialized = false;
    mutable quint64 m_sessionCounter = 0;
};

} // namespace hwtest::hal
