#pragma once

#include "hal/i_analog_io.h"
#include "hal/i_canfd_bus.h"
#include "hal/i_digital_io.h"
#include "hal/i_hal_device.h"
#include "hal/i_serial_bus.h"

#include "hardware_adapter.h"
#include "resource_mapper.h"
#include "safety_guard.h"

#include <functional>

namespace hwtest::hal {

class HalDevice final : public IHalDevice,
                        public IAnalogIo,
                        public IDigitalIo,
                        public ISerialBus,
                        public ICanFdBus {
public:
    using LogCallback = std::function<void(const HalLogEvent& event)>;

    HalDevice(HardwareAdapter* backend,
              SessionId sessionId,
              DeviceDescriptor descriptor,
              DeviceCapabilities capabilities,
              QVector<ResourceBinding> bindings,
              QVariantMap safeState,
              LogCallback logCallback = {});
    ~HalDevice() override = default;

    DeviceDescriptor descriptor() const override;
    DeviceCapabilities capabilities() const override;

    IAnalogIo* analogIo() override;
    IDigitalIo* digitalIo() override;
    ISerialBus* serialBus() override;
    ICanFdBus* canFdBus() override;

    HalStatus close(const OperationOptions& options = {});
    HalStatus reset(const OperationOptions& options = {});
    HalStatus healthCheck(const OperationOptions& options = {});
    bool isOpen() const;

    HalStatus configureAd(const ResourceId& channel,
                          const AnalogRange& range,
                          const OperationOptions& options) override;
    HalResult<AnalogSample> readAd(const ResourceId& channel,
                                   const AnalogReadOptions& options) override;
    HalResult<QVector<AnalogSample>> readAdBatch(const QVector<ResourceId>& channels,
                                                 const AnalogReadOptions& options) override;
    HalStatus configureDa(const ResourceId& channel,
                          const AnalogRange& range,
                          const OperationOptions& options) override;
    HalStatus writeDa(const ResourceId& channel,
                              double value,
                              const AnalogWriteOptions& options) override;
    HalStatus writeDaBatch(const QMap<ResourceId, double>& values,
                           const AnalogWriteOptions& options) override;

    HalResult<DigitalSample> readDi(const ResourceId& channel,
                                    const OperationOptions& options) override;
    HalResult<QVector<DigitalSample>> readDiBatch(const QVector<ResourceId>& channels,
                                                  const OperationOptions& options) override;
    HalStatus writeDo(const ResourceId& channel,
                      DigitalLevel level,
                      const DigitalWriteOptions& options) override;
    HalStatus writeDoBatch(const QMap<ResourceId, DigitalLevel>& values,
                           const DigitalWriteOptions& options) override;
    HalResult<DigitalSample> waitEdge(const ResourceId& channel,
                                      DigitalLevel targetLevel,
                                      const OperationOptions& options) override;

    HalStatus openSerial(const ResourceId& port,
                         const SerialConfig& config,
                         const OperationOptions& options) override;
    HalStatus closeSerial(const ResourceId& port,
                          const OperationOptions& options) override;
    HalStatus flushSerial(const ResourceId& port,
                          const OperationOptions& options) override;
    HalStatus writeSerial(const ResourceId& port,
                          const QByteArray& data,
                          const OperationOptions& options) override;
    HalResult<QByteArray> readSerial(const ResourceId& port,
                                     int maxBytes,
                                     const OperationOptions& options) override;
    HalResult<SerialTransactionResult> transactSerial(const ResourceId& port,
                                                      const SerialTransaction& transaction) override;

    HalStatus openCan(const ResourceId& bus,
                      const CanFdConfig& config,
                      const OperationOptions& options) override;
    HalStatus closeCan(const ResourceId& bus,
                       const OperationOptions& options) override;
    HalStatus setCanFilters(const ResourceId& bus,
                            const QVector<CanFdFilter>& filters,
                            const OperationOptions& options) override;
    HalStatus sendCan(const ResourceId& bus,
                      const CanFdFrame& frame,
                      const OperationOptions& options) override;
    HalResult<CanFdFrame> receiveCan(const ResourceId& bus,
                                     const OperationOptions& options) override;
    HalResult<QVector<CanFdFrame>> receiveCanBatch(const ResourceId& bus,
                                                   int maxFrames,
                                                   const OperationOptions& options) override;

private:
    struct ChannelInfo {
        ResourceBinding binding;
        ResourceId resourceId;
    };

    const ResourceBinding* bindingFor(const ResourceId& resourceId) const;
    const ResourceBinding* bindingFor(const ResourceId& resourceId,
                                      const QString& module,
                                      const QString& expectedDirection,
                                      HalStatus* status = nullptr) const;
    HalStatus ensureOpen(const QString& operation) const;
    HalStatus ensureModuleBinding(const ResourceId& resourceId,
                                  const QString& module,
                                  const QString& expectedDirection,
                                  const QString& operation,
                                  const ChannelInfo** channel = nullptr) const;
    HalStatus applySafeState();
    void log(const QString& level,
             const QString& category,
             const QString& message,
             const QVariantMap& context = {}) const;
    void emitOperationLog(const QString& operation,
                          const OperationOptions& options,
                          qint64 durationMs,
                          const HalStatus& status,
                          const ResourceBinding* binding = nullptr,
                          const QVariantMap& context = {}) const;
    static QString effectiveDirection(const ResourceBinding& binding);
    static bool directionMatches(const QString& actual, const QString& expected);
    static bool moduleMatches(const QString& actual, const QString& expected);
    static AnalogRange effectiveAnalogRange(const ResourceBinding& binding,
                                            const AnalogRange& requestedRange);
    static DigitalLevel digitalLevelFromVariant(const QVariant& value);
    static QVariantMap channelContext(const ResourceBinding& binding,
                                      const QString& operation);
    HalResult<AnalogSample> rewriteAnalogSample(const ResourceId& resourceId,
                                                const HalResult<AnalogSample>& sample) const;
    HalResult<DigitalSample> rewriteDigitalSample(const ResourceId& resourceId,
                                                  const HalResult<DigitalSample>& sample) const;

    HardwareAdapter* m_backend = nullptr;
    SessionId m_sessionId;
    DeviceDescriptor m_descriptor;
    DeviceCapabilities m_capabilities;
    QVariantMap m_safeState;
    QHash<ResourceId, ResourceBinding> m_bindingsByResourceId;
    QHash<int, ResourceId> m_resourceIdByPhysicalIndex;
    QHash<ResourceId, AnalogRange> m_analogInputRanges;
    QHash<ResourceId, AnalogRange> m_analogOutputRanges;
    QHash<ResourceId, SerialConfig> m_openSerialPorts;
    QHash<ResourceId, CanFdConfig> m_openCanBuses;
    bool m_open = true;
    SafetyGuard m_safetyGuard;
    LogCallback m_logCallback;
};

} // namespace hwtest::hal
