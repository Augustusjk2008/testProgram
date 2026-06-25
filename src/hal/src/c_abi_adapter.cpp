#include "c_abi_adapter.h"

namespace hwtest::hal {

CAbiAdapter::CAbiAdapter() = default;
CAbiAdapter::~CAbiAdapter() = default;

QString CAbiAdapter::adapterId() const
{
    return m_mockAdapter.adapterId();
}

HalStatus CAbiAdapter::initialize(const QVariantMap& config)
{
    return m_mockAdapter.initialize(config);
}

HalStatus CAbiAdapter::shutdown()
{
    return m_mockAdapter.shutdown();
}

HalResult<QVector<DeviceDescriptor>> CAbiAdapter::enumerateDevices(const OperationOptions& options)
{
    return m_mockAdapter.enumerateDevices(options);
}

HalResult<DeviceCapabilities> CAbiAdapter::queryCapabilities(const DeviceId& deviceId,
                                                             const OperationOptions& options)
{
    return m_mockAdapter.queryCapabilities(deviceId, options);
}

HalResult<SessionId> CAbiAdapter::openDevice(const DeviceId& deviceId,
                                            const QVariantMap& openOptions,
                                            const OperationOptions& options)
{
    return m_mockAdapter.openDevice(deviceId, openOptions, options);
}

HalStatus CAbiAdapter::closeDevice(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    return m_mockAdapter.closeDevice(sessionId, options);
}

HalStatus CAbiAdapter::resetDevice(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    return m_mockAdapter.resetDevice(sessionId, options);
}

HalStatus CAbiAdapter::healthCheck(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    return m_mockAdapter.healthCheck(sessionId, options);
}

HalStatus CAbiAdapter::configureAnalog(const SessionId& sessionId,
                                       int physicalIndex,
                                       const AnalogRange& range,
                                       bool output,
                                       const OperationOptions& options)
{
    return m_mockAdapter.configureAnalog(sessionId, physicalIndex, range, output, options);
}

HalResult<AnalogSample> CAbiAdapter::readAnalog(const SessionId& sessionId,
                                                int physicalIndex,
                                                const AnalogReadOptions& options)
{
    return m_mockAdapter.readAnalog(sessionId, physicalIndex, options);
}

HalResult<QVector<AnalogSample>> CAbiAdapter::readAnalogBatch(const SessionId& sessionId,
                                                              const QVector<int>& physicalIndexes,
                                                              const AnalogReadOptions& options)
{
    return m_mockAdapter.readAnalogBatch(sessionId, physicalIndexes, options);
}

HalStatus CAbiAdapter::writeAnalog(const SessionId& sessionId,
                                   int physicalIndex,
                                   double value,
                                   const AnalogWriteOptions& options)
{
    return m_mockAdapter.writeAnalog(sessionId, physicalIndex, value, options);
}

HalStatus CAbiAdapter::writeAnalogBatch(const SessionId& sessionId,
                                        const QMap<int, double>& values,
                                        const AnalogWriteOptions& options)
{
    return m_mockAdapter.writeAnalogBatch(sessionId, values, options);
}

HalResult<DigitalSample> CAbiAdapter::readDigital(const SessionId& sessionId,
                                                  int physicalIndex,
                                                  const OperationOptions& options)
{
    return m_mockAdapter.readDigital(sessionId, physicalIndex, options);
}

HalResult<QVector<DigitalSample>> CAbiAdapter::readDigitalBatch(const SessionId& sessionId,
                                                                const QVector<int>& physicalIndexes,
                                                                const OperationOptions& options)
{
    return m_mockAdapter.readDigitalBatch(sessionId, physicalIndexes, options);
}

HalStatus CAbiAdapter::writeDigital(const SessionId& sessionId,
                                    int physicalIndex,
                                    DigitalLevel level,
                                    const DigitalWriteOptions& options)
{
    return m_mockAdapter.writeDigital(sessionId, physicalIndex, level, options);
}

HalStatus CAbiAdapter::writeDigitalBatch(const SessionId& sessionId,
                                         const QMap<int, DigitalLevel>& values,
                                         const DigitalWriteOptions& options)
{
    return m_mockAdapter.writeDigitalBatch(sessionId, values, options);
}

HalResult<DigitalSample> CAbiAdapter::waitDigitalEdge(const SessionId& sessionId,
                                                       int physicalIndex,
                                                       DigitalLevel targetLevel,
                                                       const OperationOptions& options)
{
    return m_mockAdapter.waitDigitalEdge(sessionId, physicalIndex, targetLevel, options);
}

HalStatus CAbiAdapter::openSerial(const SessionId& sessionId,
                                  int physicalIndex,
                                  const SerialConfig& config,
                                  const OperationOptions& options)
{
    return m_mockAdapter.openSerial(sessionId, physicalIndex, config, options);
}

HalStatus CAbiAdapter::closeSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const OperationOptions& options)
{
    return m_mockAdapter.closeSerial(sessionId, physicalIndex, options);
}

HalStatus CAbiAdapter::flushSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const OperationOptions& options)
{
    return m_mockAdapter.flushSerial(sessionId, physicalIndex, options);
}

HalStatus CAbiAdapter::writeSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const QByteArray& data,
                                   const OperationOptions& options)
{
    return m_mockAdapter.writeSerial(sessionId, physicalIndex, data, options);
}

HalResult<QByteArray> CAbiAdapter::readSerial(const SessionId& sessionId,
                                              int physicalIndex,
                                              int maxBytes,
                                              const OperationOptions& options)
{
    return m_mockAdapter.readSerial(sessionId, physicalIndex, maxBytes, options);
}

HalResult<SerialTransactionResult> CAbiAdapter::transactSerial(const SessionId& sessionId,
                                                               int physicalIndex,
                                                               const SerialTransaction& transaction)
{
    return m_mockAdapter.transactSerial(sessionId, physicalIndex, transaction);
}

HalStatus CAbiAdapter::openCan(const SessionId& sessionId,
                               int physicalIndex,
                               const CanFdConfig& config,
                               const OperationOptions& options)
{
    return m_mockAdapter.openCan(sessionId, physicalIndex, config, options);
}

HalStatus CAbiAdapter::closeCan(const SessionId& sessionId,
                                int physicalIndex,
                                const OperationOptions& options)
{
    return m_mockAdapter.closeCan(sessionId, physicalIndex, options);
}

HalStatus CAbiAdapter::setCanFilters(const SessionId& sessionId,
                                     int physicalIndex,
                                     const QVector<CanFdFilter>& filters,
                                     const OperationOptions& options)
{
    return m_mockAdapter.setCanFilters(sessionId, physicalIndex, filters, options);
}

HalStatus CAbiAdapter::sendCan(const SessionId& sessionId,
                               int physicalIndex,
                               const CanFdFrame& frame,
                               const OperationOptions& options)
{
    return m_mockAdapter.sendCan(sessionId, physicalIndex, frame, options);
}

HalResult<CanFdFrame> CAbiAdapter::receiveCan(const SessionId& sessionId,
                                             int physicalIndex,
                                             const OperationOptions& options)
{
    return m_mockAdapter.receiveCan(sessionId, physicalIndex, options);
}

HalResult<QVector<CanFdFrame>> CAbiAdapter::receiveCanBatch(const SessionId& sessionId,
                                                           int physicalIndex,
                                                           int maxFrames,
                                                           const OperationOptions& options)
{
    return m_mockAdapter.receiveCanBatch(sessionId, physicalIndex, maxFrames, options);
}

} // namespace hwtest::hal
