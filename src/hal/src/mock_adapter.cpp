#include "mock_adapter.h"

#include "hal_error_mapper.h"

#include <QDateTime>
#include <QRandomGenerator>

namespace hwtest::hal {

namespace {

static qint64 transactionTimestamp()
{
    return static_cast<qint64>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) * 1000;
}

static DigitalLevel parseDigitalLevel(const QVariant& value)
{
    const QString text = value.toString().trimmed().toLower();
    if (text == QStringLiteral("high") || text == QStringLiteral("1")) {
        return DigitalLevel::High;
    }
    if (text == QStringLiteral("low") || text == QStringLiteral("0")) {
        return DigitalLevel::Low;
    }
    return DigitalLevel::Unknown;
}

static double extractDouble(const QVariantMap& map, const QString& key, double fallback)
{
    const QVariant value = map.value(key);
    return value.isValid() ? value.toDouble() : fallback;
}

static bool extractBool(const QVariantMap& map, const QString& key, bool fallback)
{
    const QVariant value = map.value(key);
    return value.isValid() ? value.toBool() : fallback;
}

} // namespace

MockAdapter::MockAdapter() = default;
MockAdapter::~MockAdapter() = default;

QString MockAdapter::adapterId() const
{
    return m_adapterId;
}

HalStatus MockAdapter::initialize(const QVariantMap& config)
{
    m_config = config;
    m_devices.clear();
    m_deviceStateById.clear();
    m_sessionsById.clear();
    m_initialized = true;
    m_adapterId = config.value(QStringLiteral("adapterId"), QStringLiteral("mock.adapter.v1")).toString();
    if (m_adapterId.isEmpty()) {
        m_adapterId = QStringLiteral("mock.adapter.v1");
    }

    ResourceMapper mapper;
    mapper.load(config);

    const QVector<DeviceDescriptor> descriptors = mapper.devices();
    for (const DeviceDescriptor& descriptor : descriptors) {
        DeviceState state;
        state.descriptor = descriptor;
        state.capabilities = mapper.capabilities(descriptor.deviceId);
        ensureDefaultState(state);
        m_devices.push_back(descriptor);
        m_deviceStateById.insert(descriptor.deviceId, state);
    }

    return HalStatus{};
}

HalStatus MockAdapter::shutdown()
{
    m_sessionsById.clear();
    m_deviceStateById.clear();
    m_devices.clear();
    m_initialized = false;
    return HalStatus{};
}

HalResult<QVector<DeviceDescriptor>> MockAdapter::enumerateDevices(const OperationOptions& options)
{
    Q_UNUSED(options)
    HalResult<QVector<DeviceDescriptor>> result;
    if (!m_initialized) {
        result.status = makeError(HalStatusCode::NotInitialized,
                                  QStringLiteral("adapter.enumerateDevices"),
                                  QStringLiteral("Adapter is not initialized"));
        return result;
    }
    result.value = m_devices;
    return result;
}

HalResult<DeviceCapabilities> MockAdapter::queryCapabilities(const DeviceId& deviceId,
                                                             const OperationOptions& options)
{
    Q_UNUSED(options)
    HalResult<DeviceCapabilities> result;
    const DeviceState* state = deviceForId(deviceId);
    if (state == nullptr) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("adapter.queryCapabilities"),
                                  QStringLiteral("Device not found"),
                                  deviceId);
        return result;
    }
    result.value = state->capabilities;
    return result;
}

HalResult<SessionId> MockAdapter::openDevice(const DeviceId& deviceId,
                                            const QVariantMap& openOptions,
                                            const OperationOptions& options)
{
    Q_UNUSED(openOptions)
    Q_UNUSED(options)
    HalResult<SessionId> result;
    DeviceState* state = deviceForId(deviceId);
    if (state == nullptr) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("adapter.openDevice"),
                                  QStringLiteral("Device not found"),
                                  deviceId);
        return result;
    }

    const SessionId sessionId = nextSessionId(deviceId);
    SessionState session;
    session.sessionId = sessionId;
    session.deviceId = deviceId;
    session.openOptions = openOptions;
    m_sessionsById.insert(sessionId, session);
    result.value = sessionId;
    return result;
}

HalStatus MockAdapter::closeDevice(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    Q_UNUSED(options)
    if (!m_sessionsById.contains(sessionId)) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.closeDevice"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    m_sessionsById.remove(sessionId);
    return HalStatus{};
}

HalStatus MockAdapter::resetDevice(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.resetDevice"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    ensureDefaultState(*state);
    return HalStatus{};
}

HalStatus MockAdapter::healthCheck(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    Q_UNUSED(options)
    if (deviceForSession(sessionId) == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.healthCheck"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    return HalStatus{};
}

HalStatus MockAdapter::configureAnalog(const SessionId& sessionId,
                                       int physicalIndex,
                                       const AnalogRange& range,
                                       bool output,
                                       const OperationOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.configureAnalog"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    if (output) {
        state->analogOutputs.insert(physicalIndex, range);
    } else {
        state->analogInputs.insert(physicalIndex, range);
    }
    return HalStatus{};
}

HalResult<AnalogSample> MockAdapter::readAnalog(const SessionId& sessionId,
                                                int physicalIndex,
                                                const AnalogReadOptions& options)
{
    HalResult<AnalogSample> result;
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("adapter.readAnalog"),
                                  QStringLiteral("Session not found"),
                                  {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        return result;
    }

    const QVariantMap mockConfig = m_config.value(QStringLiteral("mock")).toMap();
    const bool loopback = extractBool(mockConfig, QStringLiteral("analogLoopback"), true);
    const double defaultValue = extractDouble(mockConfig.value(QStringLiteral("analogDefaults")).toMap(), QString::number(physicalIndex), 0.0);
    double value = defaultValue;
    if (loopback && state->analogOutputValues.contains(physicalIndex)) {
        value = state->analogOutputValues.value(physicalIndex);
    }
    const double noiseAmplitude = extractDouble(mockConfig, QStringLiteral("analogNoiseAmplitude"), 0.0);
    if (noiseAmplitude > 0.0) {
        const double noise = (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0) * noiseAmplitude;
        value += noise;
    }

    QVariantMap metadata;
    metadata.insert(QStringLiteral("loopback"), loopback);
    result.value = makeAnalogSample(sessionId, physicalIndex, value, options, metadata);
    return result;
}

HalResult<QVector<AnalogSample>> MockAdapter::readAnalogBatch(const SessionId& sessionId,
                                                              const QVector<int>& physicalIndexes,
                                                              const AnalogReadOptions& options)
{
    HalResult<QVector<AnalogSample>> result;
    for (int physicalIndex : physicalIndexes) {
        const HalResult<AnalogSample> sample = readAnalog(sessionId, physicalIndex, options);
        if (!sample.ok()) {
            result.status = sample.status;
            return result;
        }
        result.value.push_back(sample.value);
    }
    return result;
}

HalStatus MockAdapter::writeAnalog(const SessionId& sessionId,
                                   int physicalIndex,
                                   double value,
                                   const AnalogWriteOptions& options)
{
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.writeAnalog"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    Q_UNUSED(options)
    state->analogOutputValues.insert(physicalIndex, value);
    return HalStatus{};
}

HalStatus MockAdapter::writeAnalogBatch(const SessionId& sessionId,
                                        const QMap<int, double>& values,
                                        const AnalogWriteOptions& options)
{
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const HalStatus status = writeAnalog(sessionId, it.key(), it.value(), options);
        if (!status.ok()) {
            return status;
        }
    }
    return HalStatus{};
}

HalResult<DigitalSample> MockAdapter::readDigital(const SessionId& sessionId,
                                                  int physicalIndex,
                                                  const OperationOptions& options)
{
    HalResult<DigitalSample> result;
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("adapter.readDigital"),
                                  QStringLiteral("Session not found"),
                                  {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        return result;
    }
    const QVariantMap mockConfig = m_config.value(QStringLiteral("mock")).toMap();
    const bool loopback = extractBool(mockConfig, QStringLiteral("digitalLoopback"), true);
    DigitalLevel level = state->digitalInputs.value(physicalIndex, DigitalLevel::Low);
    if (loopback && state->digitalOutputs.contains(physicalIndex)) {
        level = state->digitalOutputs.value(physicalIndex);
    }
    Q_UNUSED(options)
    QVariantMap metadata;
    metadata.insert(QStringLiteral("loopback"), loopback);
    result.value = makeDigitalSample(sessionId, physicalIndex, level, metadata);
    return result;
}

HalResult<QVector<DigitalSample>> MockAdapter::readDigitalBatch(const SessionId& sessionId,
                                                                const QVector<int>& physicalIndexes,
                                                                const OperationOptions& options)
{
    HalResult<QVector<DigitalSample>> result;
    for (int physicalIndex : physicalIndexes) {
        const HalResult<DigitalSample> sample = readDigital(sessionId, physicalIndex, options);
        if (!sample.ok()) {
            result.status = sample.status;
            return result;
        }
        result.value.push_back(sample.value);
    }
    return result;
}

HalStatus MockAdapter::writeDigital(const SessionId& sessionId,
                                    int physicalIndex,
                                    DigitalLevel level,
                                    const DigitalWriteOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.writeDigital"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    state->digitalOutputs.insert(physicalIndex, level);
    return HalStatus{};
}

HalStatus MockAdapter::writeDigitalBatch(const SessionId& sessionId,
                                         const QMap<int, DigitalLevel>& values,
                                         const DigitalWriteOptions& options)
{
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const HalStatus status = writeDigital(sessionId, it.key(), it.value(), options);
        if (!status.ok()) {
            return status;
        }
    }
    return HalStatus{};
}

HalResult<DigitalSample> MockAdapter::waitDigitalEdge(const SessionId& sessionId,
                                                      int physicalIndex,
                                                      DigitalLevel targetLevel,
                                                      const OperationOptions& options)
{
    HalResult<DigitalSample> result = readDigital(sessionId, physicalIndex, options);
    if (!result.ok()) {
        return result;
    }
    result.value.level = targetLevel;
    return result;
}

HalStatus MockAdapter::openSerial(const SessionId& sessionId,
                                  int physicalIndex,
                                  const SerialConfig& config,
                                  const OperationOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.openSerial"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    state->serialConfigs.insert(physicalIndex, config);
    return HalStatus{};
}

HalStatus MockAdapter::closeSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const OperationOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.closeSerial"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    state->serialConfigs.remove(physicalIndex);
    state->serialBuffers.remove(physicalIndex);
    return HalStatus{};
}

HalStatus MockAdapter::flushSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const OperationOptions& options)
{
    return closeSerial(sessionId, physicalIndex, options);
}

HalStatus MockAdapter::writeSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const QByteArray& data,
                                   const OperationOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.writeSerial"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    state->serialBuffers[physicalIndex].append(data);
    return HalStatus{};
}

HalResult<QByteArray> MockAdapter::readSerial(const SessionId& sessionId,
                                              int physicalIndex,
                                              int maxBytes,
                                              const OperationOptions& options)
{
    Q_UNUSED(options)
    HalResult<QByteArray> result;
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("adapter.readSerial"),
                                  QStringLiteral("Session not found"),
                                  {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        return result;
    }

    const QVariantMap mockConfig = m_config.value(QStringLiteral("mock")).toMap();
    const bool echo = extractBool(mockConfig, QStringLiteral("serialEcho"), true);
    QByteArray& buffer = state->serialBuffers[physicalIndex];
    if (echo && buffer.isEmpty()) {
        buffer.append(QByteArrayLiteral("mock-serial"));
    }
    result.value = buffer.left(maxBytes > 0 ? maxBytes : buffer.size());
    buffer.remove(0, result.value.size());
    return result;
}

HalResult<SerialTransactionResult> MockAdapter::transactSerial(const SessionId& sessionId,
                                                              int physicalIndex,
                                                              const SerialTransaction& transaction)
{
    HalResult<SerialTransactionResult> result;
    const HalStatus writeStatus = writeSerial(sessionId, physicalIndex, transaction.tx, transaction.op);
    if (!writeStatus.ok()) {
        result.status = writeStatus;
        return result;
    }
    const HalResult<QByteArray> readResult = readSerial(sessionId, physicalIndex, transaction.readMaxBytes, transaction.op);
    if (!readResult.ok()) {
        result.status = readResult.status;
        return result;
    }
    result.value.rx = readResult.value;
    result.value.txTimestampUs = transaction.op.timeoutMs;
    result.value.rxTimestampUs = transaction.op.timeoutMs + 1;
    result.value.metadata.insert(QStringLiteral("expectedPrefix"), QString::fromUtf8(transaction.expectedPrefix));
    return result;
}

HalStatus MockAdapter::openCan(const SessionId& sessionId,
                               int physicalIndex,
                               const CanFdConfig& config,
                               const OperationOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.openCan"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    state->canConfigs.insert(physicalIndex, config);
    state->canReceiveQueues[physicalIndex];
    return HalStatus{};
}

HalStatus MockAdapter::closeCan(const SessionId& sessionId,
                                int physicalIndex,
                                const OperationOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.closeCan"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    state->canConfigs.remove(physicalIndex);
    state->canReceiveQueues.remove(physicalIndex);
    return HalStatus{};
}

HalStatus MockAdapter::setCanFilters(const SessionId& sessionId,
                                     int physicalIndex,
                                     const QVector<CanFdFilter>& filters,
                                     const OperationOptions& options)
{
    Q_UNUSED(options)
    Q_UNUSED(filters)
    if (deviceForSession(sessionId) == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.setCanFilters"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    return HalStatus{};
}

HalStatus MockAdapter::sendCan(const SessionId& sessionId,
                               int physicalIndex,
                               const CanFdFrame& frame,
                               const OperationOptions& options)
{
    Q_UNUSED(options)
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        return makeError(HalStatusCode::NotFound,
                         QStringLiteral("adapter.sendCan"),
                         QStringLiteral("Session not found"),
                         {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    const QVariantMap mockConfig = m_config.value(QStringLiteral("mock")).toMap();
    const bool loopback = extractBool(mockConfig, QStringLiteral("canLoopback"), true);
    if (loopback) {
        CanFdFrame echoed = frame;
        echoed.timestampUs = transactionTimestamp();
        state->canReceiveQueues[physicalIndex].enqueue(echoed);
    }
    return HalStatus{};
}

HalResult<CanFdFrame> MockAdapter::receiveCan(const SessionId& sessionId,
                                              int physicalIndex,
                                              const OperationOptions& options)
{
    Q_UNUSED(options)
    HalResult<CanFdFrame> result;
    DeviceState* state = deviceForSession(sessionId);
    if (state == nullptr) {
        result.status = makeError(HalStatusCode::NotFound,
                                  QStringLiteral("adapter.receiveCan"),
                                  QStringLiteral("Session not found"),
                                  {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        return result;
    }
    QQueue<CanFdFrame>& queue = state->canReceiveQueues[physicalIndex];
    if (queue.isEmpty()) {
        result.status = makeError(HalStatusCode::Timeout,
                                  QStringLiteral("adapter.receiveCan"),
                                  QStringLiteral("No CAN frame available"),
                                  {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        return result;
    }
    result.value = queue.dequeue();
    return result;
}

HalResult<QVector<CanFdFrame>> MockAdapter::receiveCanBatch(const SessionId& sessionId,
                                                            int physicalIndex,
                                                            int maxFrames,
                                                            const OperationOptions& options)
{
    HalResult<QVector<CanFdFrame>> result;
    for (int index = 0; index < maxFrames; ++index) {
        const HalResult<CanFdFrame> frame = receiveCan(sessionId, physicalIndex, options);
        if (!frame.ok()) {
            break;
        }
        result.value.push_back(frame.value);
    }
    if (result.value.isEmpty()) {
        result.status = makeError(HalStatusCode::Timeout,
                                  QStringLiteral("adapter.receiveCanBatch"),
                                  QStringLiteral("No CAN frame available"),
                                  {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
    }
    return result;
}

MockAdapter::DeviceState* MockAdapter::deviceForSession(const SessionId& sessionId)
{
    const SessionState* session = sessionForId(sessionId);
    if (session == nullptr) {
        return nullptr;
    }
    return deviceForId(session->deviceId);
}

const MockAdapter::DeviceState* MockAdapter::deviceForSession(const SessionId& sessionId) const
{
    const SessionState* session = sessionForId(sessionId);
    if (session == nullptr) {
        return nullptr;
    }
    return deviceForId(session->deviceId);
}

MockAdapter::DeviceState* MockAdapter::deviceForId(const DeviceId& deviceId)
{
    const auto it = m_deviceStateById.find(deviceId);
    if (it == m_deviceStateById.end()) {
        return nullptr;
    }
    return &it.value();
}

const MockAdapter::DeviceState* MockAdapter::deviceForId(const DeviceId& deviceId) const
{
    const auto it = m_deviceStateById.constFind(deviceId);
    if (it == m_deviceStateById.constEnd()) {
        return nullptr;
    }
    return &it.value();
}

MockAdapter::SessionState* MockAdapter::sessionForId(const SessionId& sessionId)
{
    const auto it = m_sessionsById.find(sessionId);
    if (it == m_sessionsById.end()) {
        return nullptr;
    }
    return &it.value();
}

const MockAdapter::SessionState* MockAdapter::sessionForId(const SessionId& sessionId) const
{
    const auto it = m_sessionsById.constFind(sessionId);
    if (it == m_sessionsById.constEnd()) {
        return nullptr;
    }
    return &it.value();
}

bool MockAdapter::ensureSession(const SessionId& sessionId, const QString& operation, HalStatus* status) const
{
    if (sessionForId(sessionId) == nullptr) {
        if (status != nullptr) {
            *status = makeError(HalStatusCode::NotFound,
                                operation,
                                QStringLiteral("Session not found"),
                                {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        }
        return false;
    }
    return true;
}

AnalogSample MockAdapter::makeAnalogSample(const SessionId& sessionId,
                                           int physicalIndex,
                                           double value,
                                           const AnalogReadOptions& options,
                                           const QVariantMap& metadata)
{
    AnalogSample sample;
    sample.channel = QStringLiteral("%1:%2").arg(sessionId).arg(physicalIndex);
    sample.value = value;
    sample.unit = options.range.unit;
    sample.timestampUs = transactionTimestamp();
    sample.metadata = metadata;
    return sample;
}

DigitalSample MockAdapter::makeDigitalSample(const SessionId& sessionId,
                                             int physicalIndex,
                                             DigitalLevel level,
                                             const QVariantMap& metadata)
{
    DigitalSample sample;
    sample.channel = QStringLiteral("%1:%2").arg(sessionId).arg(physicalIndex);
    sample.level = level;
    sample.timestampUs = transactionTimestamp();
    sample.metadata = metadata;
    return sample;
}

void MockAdapter::ensureDefaultState(DeviceState& state)
{
    const QVariantMap mockConfig = m_config.value(QStringLiteral("mock")).toMap();
    const QVariantMap analogDefaults = mockConfig.value(QStringLiteral("analogDefaults")).toMap();
    const QVariantMap digitalDefaults = mockConfig.value(QStringLiteral("digitalDefaults")).toMap();

    for (const ChannelDescriptor& channel : state.capabilities.channels) {
        if (channel.module == QStringLiteral("analog")) {
            if (channel.direction == QStringLiteral("input")) {
                const double defaultValue = extractDouble(analogDefaults, QString::number(channel.physicalIndex), 0.0);
                state.analogInputs.insert(channel.physicalIndex,
                                          AnalogRange{defaultValue, defaultValue, AnalogUnit::Volt});
            } else {
                state.analogOutputs.insert(channel.physicalIndex,
                                           AnalogRange{0.0, 5.0, AnalogUnit::Volt});
            }
        } else if (channel.module == QStringLiteral("digital")) {
            state.digitalInputs.insert(channel.physicalIndex,
                                       parseDigitalLevel(digitalDefaults.value(QString::number(channel.physicalIndex), QStringLiteral("Low"))));
        }
    }
}

QString MockAdapter::nextSessionId(const DeviceId& deviceId) const
{
    return QStringLiteral("%1_session_%2").arg(deviceId).arg(++m_sessionCounter);
}

} // namespace hwtest::hal
