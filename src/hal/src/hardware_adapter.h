#pragma once

#include "resource_mapper.h"

namespace hwtest::hal {

class HardwareAdapter {
public:
    virtual ~HardwareAdapter() = default;

    virtual QString adapterId() const = 0;

    virtual HalStatus initialize(const QVariantMap& config) = 0;
    virtual HalStatus shutdown() = 0;

    virtual HalResult<QVector<DeviceDescriptor>> enumerateDevices(const OperationOptions& options) = 0;
    virtual HalResult<DeviceCapabilities> queryCapabilities(const DeviceId& deviceId,
                                                            const OperationOptions& options) = 0;

    virtual HalResult<SessionId> openDevice(const DeviceId& deviceId,
                                            const QVariantMap& openOptions,
                                            const OperationOptions& options) = 0;
    virtual HalStatus closeDevice(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;
    virtual HalStatus resetDevice(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;
    virtual HalStatus healthCheck(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;

    virtual HalStatus configureAnalog(const SessionId& sessionId,
                                      int physicalIndex,
                                      const AnalogRange& range,
                                      bool output,
                                      const OperationOptions& options) = 0;
    virtual HalResult<AnalogSample> readAnalog(const SessionId& sessionId,
                                               int physicalIndex,
                                               const AnalogReadOptions& options) = 0;
    virtual HalResult<QVector<AnalogSample>> readAnalogBatch(const SessionId& sessionId,
                                                             const QVector<int>& physicalIndexes,
                                                             const AnalogReadOptions& options) = 0;
    virtual HalStatus writeAnalog(const SessionId& sessionId,
                                  int physicalIndex,
                                  double value,
                                  const AnalogWriteOptions& options) = 0;
    virtual HalStatus writeAnalogBatch(const SessionId& sessionId,
                                       const QMap<int, double>& values,
                                       const AnalogWriteOptions& options) = 0;

    virtual HalResult<DigitalSample> readDigital(const SessionId& sessionId,
                                                 int physicalIndex,
                                                 const OperationOptions& options) = 0;
    virtual HalResult<QVector<DigitalSample>> readDigitalBatch(const SessionId& sessionId,
                                                               const QVector<int>& physicalIndexes,
                                                               const OperationOptions& options) = 0;
    virtual HalStatus writeDigital(const SessionId& sessionId,
                                   int physicalIndex,
                                   DigitalLevel level,
                                   const DigitalWriteOptions& options) = 0;
    virtual HalStatus writeDigitalBatch(const SessionId& sessionId,
                                        const QMap<int, DigitalLevel>& values,
                                        const DigitalWriteOptions& options) = 0;
    virtual HalResult<DigitalSample> waitDigitalEdge(const SessionId& sessionId,
                                                     int physicalIndex,
                                                     DigitalLevel targetLevel,
                                                     const OperationOptions& options) = 0;

    virtual HalStatus openSerial(const SessionId& sessionId,
                                 int physicalIndex,
                                 const SerialConfig& config,
                                 const OperationOptions& options) = 0;
    virtual HalStatus closeSerial(const SessionId& sessionId,
                                  int physicalIndex,
                                  const OperationOptions& options) = 0;
    virtual HalStatus flushSerial(const SessionId& sessionId,
                                  int physicalIndex,
                                  const OperationOptions& options) = 0;
    virtual HalStatus writeSerial(const SessionId& sessionId,
                                  int physicalIndex,
                                  const QByteArray& data,
                                  const OperationOptions& options) = 0;
    virtual HalResult<QByteArray> readSerial(const SessionId& sessionId,
                                             int physicalIndex,
                                             int maxBytes,
                                             const OperationOptions& options) = 0;
    virtual HalResult<SerialTransactionResult> transactSerial(const SessionId& sessionId,
                                                              int physicalIndex,
                                                              const SerialTransaction& transaction) = 0;

    virtual HalStatus openCan(const SessionId& sessionId,
                              int physicalIndex,
                              const CanFdConfig& config,
                              const OperationOptions& options) = 0;
    virtual HalStatus closeCan(const SessionId& sessionId,
                               int physicalIndex,
                               const OperationOptions& options) = 0;
    virtual HalStatus setCanFilters(const SessionId& sessionId,
                                    int physicalIndex,
                                    const QVector<CanFdFilter>& filters,
                                    const OperationOptions& options) = 0;
    virtual HalStatus sendCan(const SessionId& sessionId,
                              int physicalIndex,
                              const CanFdFrame& frame,
                              const OperationOptions& options) = 0;
    virtual HalResult<CanFdFrame> receiveCan(const SessionId& sessionId,
                                             int physicalIndex,
                                             const OperationOptions& options) = 0;
    virtual HalResult<QVector<CanFdFrame>> receiveCanBatch(const SessionId& sessionId,
                                                           int physicalIndex,
                                                           int maxFrames,
                                                           const OperationOptions& options) = 0;
};

} // namespace hwtest::hal
