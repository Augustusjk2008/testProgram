#include "c_abi_adapter.h"

#include "hal_error_mapper.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <cstring>

namespace hwtest::hal {

namespace {

qint64 nowUs()
{
    return static_cast<qint64>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) * 1000;
}

void HAL_ADAPTER_CALL hostLog(int, const char*, const char*, const char*)
{
}

long long HAL_ADAPTER_CALL hostNowUs()
{
    return nowUs();
}

QString fixedText(const char* value, int capacity)
{
    if (value == nullptr || capacity <= 0) {
        return {};
    }
    int length = 0;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return QString::fromUtf8(value, length);
}

QByteArray jsonBytes(const QVariantMap& map)
{
    return QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact);
}

QVariantMap jsonMap(const char* data, int bytes)
{
    if (data == nullptr || bytes <= 0) {
        return {};
    }
    while (bytes > 0 && data[bytes - 1] == '\0') --bytes;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(data, bytes), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object().toVariantMap()
        : QVariantMap{};
}

QString expandedPath(QString path)
{
    path = path.trimmed();
    if (path.startsWith(QStringLiteral("${")) && path.endsWith(QLatin1Char('}'))) {
        const QString variable = path.mid(2, path.size() - 3);
        path = qEnvironmentVariable(variable.toUtf8().constData());
    }
    return path;
}

QStringList modulesFromMask(unsigned int mask)
{
    QStringList modules;
    if ((mask & HAL_MODULE_ANALOG) != 0u) modules.push_back(QStringLiteral("analog"));
    if ((mask & HAL_MODULE_DIGITAL) != 0u) modules.push_back(QStringLiteral("digital"));
    if ((mask & HAL_MODULE_SERIAL) != 0u) modules.push_back(QStringLiteral("serial"));
    if ((mask & HAL_MODULE_CANFD) != 0u) modules.push_back(QStringLiteral("canfd"));
    return modules;
}

DigitalLevel digitalLevel(int value)
{
    if (value == 0) return DigitalLevel::Low;
    if (value == 1) return DigitalLevel::High;
    return DigitalLevel::Unknown;
}

int adapterLevel(DigitalLevel value)
{
    switch (value) {
    case DigitalLevel::Low: return 0;
    case DigitalLevel::High: return 1;
    case DigitalLevel::Unknown: return 2;
    }
    return 2;
}

AnalogSample analogSample(const HalAdapterAnalogSample& source)
{
    AnalogSample sample;
    sample.channel = QString::number(source.channelIndex);
    sample.value = source.value;
    sample.unit = static_cast<AnalogUnit>(source.unit);
    sample.timestampUs = source.timestampUs;
    sample.metadata.insert(QStringLiteral("rawCount"), source.rawCount);
    sample.metadata.insert(QStringLiteral("statusFlags"), source.statusFlags);
    return sample;
}

DigitalSample digitalSample(const HalAdapterDigitalSample& source)
{
    DigitalSample sample;
    sample.channel = QString::number(source.channelIndex);
    sample.level = digitalLevel(source.level);
    sample.timestampUs = source.timestampUs;
    sample.metadata.insert(QStringLiteral("statusFlags"), source.statusFlags);
    return sample;
}

HalAdapterCanFdFrame adapterFrame(const CanFdFrame& source)
{
    HalAdapterCanFdFrame frame {};
    frame.id = source.id;
    frame.extendedId = source.extendedId ? 1 : 0;
    frame.fd = source.fd ? 1 : 0;
    frame.bitrateSwitch = source.bitrateSwitch ? 1 : 0;
    frame.remoteRequest = source.remoteRequest ? 1 : 0;
    frame.payloadSize = qMin(source.payload.size(), 64);
    if (frame.payloadSize > 0) {
        std::memcpy(frame.payload, source.payload.constData(), static_cast<size_t>(frame.payloadSize));
    }
    frame.timestampUs = source.timestampUs;
    return frame;
}

CanFdFrame canFrame(const HalAdapterCanFdFrame& source)
{
    CanFdFrame frame;
    frame.id = source.id;
    frame.extendedId = source.extendedId != 0;
    frame.fd = source.fd != 0;
    frame.bitrateSwitch = source.bitrateSwitch != 0;
    frame.remoteRequest = source.remoteRequest != 0;
    frame.payload = QByteArray(reinterpret_cast<const char*>(source.payload),
                               qBound(0, source.payloadSize, 64));
    frame.timestampUs = source.timestampUs;
    frame.metadata.insert(QStringLiteral("statusFlags"), source.statusFlags);
    return frame;
}

} // namespace

CAbiAdapter::CAbiAdapter() = default;

CAbiAdapter::~CAbiAdapter()
{
    shutdown();
}

QString CAbiAdapter::adapterId() const
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    return m_adapterId;
}

HalStatus CAbiAdapter::ensureInitialized(const QString& operation) const
{
    return m_initialized
        ? HalStatus{}
        : makeError(HalStatusCode::NotInitialized,
                    operation,
                    QStringLiteral("C ABI adapter is not initialized"));
}

HalStatus CAbiAdapter::unsupported(const QString& operation,
                                   const DeviceId& deviceId) const
{
    return makeError(HalStatusCode::NotSupported,
                     operation,
                     QStringLiteral("Adapter ABI function is not available"),
                     deviceId);
}

HalAdapterDeviceHandle CAbiAdapter::deviceHandle(const SessionId& sessionId,
                                                 const QString& operation,
                                                 HalStatus* status) const
{
    const auto it = m_devices.constFind(sessionId);
    if (it == m_devices.constEnd()) {
        if (status != nullptr) {
            *status = makeError(HalStatusCode::NotFound,
                                operation,
                                QStringLiteral("Adapter session not found"),
                                {}, {}, {}, {{QStringLiteral("sessionId"), sessionId}});
        }
        return nullptr;
    }
    return it.value();
}

HalStatus CAbiAdapter::initialize(const QVariantMap& config)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    shutdown();
    m_config = config;
    QVariantMap effective = config;
    const QVariantMap legacyAdapter = config.value(QStringLiteral("adapter")).toMap();
    if (!legacyAdapter.isEmpty() &&
        effective.value(QStringLiteral("libraryPath")).toString().trimmed().isEmpty()) {
        effective = legacyAdapter;
    }
    QString libraryPath = effective.value(QStringLiteral("libraryPath")).toString();
    if (libraryPath.trimmed().isEmpty()) {
        libraryPath = config.value(QStringLiteral("adapterLibraryPath")).toString();
    }
    libraryPath = expandedPath(libraryPath);
    if (libraryPath.isEmpty()) {
        return makeError(HalStatusCode::AdapterLoadFailed,
                         QStringLiteral("adapter.initialize"),
                         QStringLiteral("Adapter library path is empty"));
    }

    HalAdapterHostApiV1 host {};
    host.abiVersion = HAL_ADAPTER_ABI_VERSION;
    host.log = &hostLog;
    host.nowUs = &hostNowUs;
    if (!m_loader.load(libraryPath, host, &m_api)) {
        const HalStatusCode code = m_loader.errorString().contains(
                                       QStringLiteral("Missing symbol"),
                                       Qt::CaseInsensitive)
            ? HalStatusCode::AdapterSymbolMissing
            : HalStatusCode::AdapterLoadFailed;
        return makeError(code,
                         QStringLiteral("adapter.initialize"),
                         m_loader.errorString(),
                         {}, {}, {}, {{QStringLiteral("libraryPath"), libraryPath}});
    }
    if (m_api.getInfo == nullptr || m_api.initialize == nullptr ||
        m_api.shutdown == nullptr) {
        m_loader.unload();
        m_api = HalAdapterApiV1{};
        return unsupported(QStringLiteral("adapter.initialize"));
    }

    HalAdapterInfo info {};
    HalStatus status = fromAdapterStatus(m_api.getInfo(&info),
                                         QStringLiteral("adapter.getInfo"));
    if (!status.ok()) {
        m_loader.unload();
        m_api = HalAdapterApiV1{};
        return status;
    }
    m_adapterId = fixedText(info.adapterId, HAL_ADAPTER_MAX_ID);
    if (m_adapterId.isEmpty()) {
        m_adapterId = effective.value(QStringLiteral("adapterId")).toString().trimmed();
    }

    const QByteArray configJson = jsonBytes(effective);
    status = fromAdapterStatus(m_api.initialize(configJson.constData(), &m_handle),
                               QStringLiteral("adapter.initialize"));
    if (!status.ok() || m_handle == nullptr) {
        if (status.ok()) {
            status = makeError(HalStatusCode::AdapterError,
                               QStringLiteral("adapter.initialize"),
                               QStringLiteral("Adapter returned a null handle"));
        }
        m_handle = nullptr;
        m_loader.unload();
        m_api = HalAdapterApiV1{};
        return status;
    }
    m_initialized = true;
    return {};
}

HalStatus CAbiAdapter::shutdown()
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus firstError;
    const auto sessions = m_devices.keys();
    for (const SessionId& sessionId : sessions) {
        const HalStatus status = closeDevice(sessionId, OperationOptions{});
        if (!status.ok() && firstError.ok()) firstError = status;
    }
    if (m_handle != nullptr && m_api.shutdown != nullptr) {
        const HalStatus status = fromAdapterStatus(m_api.shutdown(m_handle),
                                                   QStringLiteral("adapter.shutdown"));
        if (!status.ok() && firstError.ok()) firstError = status;
    }
    m_devices.clear();
    m_deviceIds.clear();
    m_handle = nullptr;
    m_initialized = false;
    m_adapterId.clear();
    m_config.clear();
    m_api = HalAdapterApiV1{};
    m_loader.unload();
    return firstError;
}

HalResult<QVector<DeviceDescriptor>> CAbiAdapter::enumerateDevices(
    const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<QVector<DeviceDescriptor>> result;
    result.status = ensureInitialized(QStringLiteral("adapter.enumerateDevices"));
    if (!result.ok()) return result;
    if (m_api.enumerateDevices == nullptr) {
        result.status = unsupported(QStringLiteral("adapter.enumerateDevices"));
        return result;
    }

    int count = 0;
    HalAdapterStatus adapterStatus = m_api.enumerateDevices(
        m_handle, nullptr, &count, options.timeoutMs);
    if (adapterStatus.code != HAL_ADAPTER_OK &&
        adapterStatus.code != HAL_ADAPTER_BUFFER_TOO_SMALL) {
        result.status = fromAdapterStatus(adapterStatus,
                                          QStringLiteral("adapter.enumerateDevices"));
        return result;
    }
    if (count <= 0) return result;
    QVector<HalAdapterDeviceInfo> devices(count);
    adapterStatus = m_api.enumerateDevices(
        m_handle, devices.data(), &count, options.timeoutMs);
    result.status = fromAdapterStatus(adapterStatus,
                                      QStringLiteral("adapter.enumerateDevices"));
    if (!result.ok()) return result;
    count = qBound(0, count, devices.size());
    result.value.reserve(count);
    for (int index = 0; index < count; ++index) {
        const HalAdapterDeviceInfo& source = devices.at(index);
        DeviceDescriptor descriptor;
        descriptor.deviceId = fixedText(source.deviceId, HAL_ADAPTER_MAX_ID);
        descriptor.adapterId = m_adapterId;
        descriptor.model = fixedText(source.model, HAL_ADAPTER_MAX_TEXT);
        descriptor.serialNumber = fixedText(source.serialNumber, HAL_ADAPTER_MAX_TEXT);
        descriptor.location = fixedText(source.location, HAL_ADAPTER_MAX_TEXT);
        descriptor.firmwareVersion = fixedText(source.firmwareVersion, HAL_ADAPTER_MAX_TEXT);
        descriptor.properties = jsonMap(source.propertiesJson,
                                        static_cast<int>(sizeof(source.propertiesJson)));
        descriptor.properties.insert(QStringLiteral("supportedModules"),
                                     modulesFromMask(source.supportedModulesMask));
        result.value.push_back(descriptor);
    }
    return result;
}

HalResult<DeviceCapabilities> CAbiAdapter::queryCapabilities(
    const DeviceId& deviceId,
    const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<DeviceCapabilities> result;
    result.status = ensureInitialized(QStringLiteral("adapter.queryCapabilities"));
    if (!result.ok()) return result;
    if (m_api.getCapabilities == nullptr || m_api.openDevice == nullptr ||
        m_api.closeDevice == nullptr) {
        result.status = unsupported(QStringLiteral("adapter.queryCapabilities"), deviceId);
        return result;
    }

    HalAdapterDeviceHandle device = nullptr;
    QString physicalDeviceId = m_config.value(QStringLiteral("settings")).toMap()
                                   .value(QStringLiteral("deviceName")).toString().trimmed();
    if (physicalDeviceId.isEmpty()) physicalDeviceId = deviceId;
    const QByteArray id = physicalDeviceId.toUtf8();
    HalAdapterStatus adapterStatus = m_api.openDevice(m_handle, id.constData(), "{}", &device);
    result.status = fromAdapterStatus(adapterStatus,
                                      QStringLiteral("adapter.queryCapabilities"),
                                      deviceId);
    if (!result.ok()) return result;

    int bytes = 0;
    adapterStatus = m_api.getCapabilities(device, nullptr, &bytes, options.timeoutMs);
    if (adapterStatus.code != HAL_ADAPTER_OK &&
        adapterStatus.code != HAL_ADAPTER_BUFFER_TOO_SMALL) {
        result.status = fromAdapterStatus(adapterStatus,
                                          QStringLiteral("adapter.queryCapabilities"),
                                          deviceId);
        m_api.closeDevice(device);
        return result;
    }
    QByteArray buffer(qMax(1, bytes), '\0');
    adapterStatus = m_api.getCapabilities(device, buffer.data(), &bytes, options.timeoutMs);
    const HalAdapterStatus closeStatus = m_api.closeDevice(device);
    result.status = fromAdapterStatus(adapterStatus,
                                      QStringLiteral("adapter.queryCapabilities"),
                                      deviceId);
    if (!result.ok()) return result;
    const HalStatus closed = fromAdapterStatus(closeStatus,
                                               QStringLiteral("adapter.closeDevice"),
                                               deviceId);
    if (!closed.ok()) {
        result.status = closed;
        return result;
    }

    const QVariantMap object = jsonMap(buffer.constData(), qBound(0, bytes, buffer.size()));
    result.value.device.deviceId = deviceId;
    result.value.device.adapterId = m_adapterId;
    for (const QVariant& module :
         object.value(QStringLiteral("supportedModules")).toList()) {
        const QString name = module.toString().trimmed();
        if (!name.isEmpty()) result.value.supportedModules.push_back(name);
    }
    result.value.limits = object.value(QStringLiteral("limits")).toMap();
    for (const QVariant& item : object.value(QStringLiteral("channels")).toList()) {
        const QVariantMap map = item.toMap();
        result.value.channels.push_back(ChannelDescriptor{
            map.value(QStringLiteral("resourceId")).toString(),
            map.value(QStringLiteral("module")).toString(),
            map.value(QStringLiteral("direction")).toString(),
            map.value(QStringLiteral("physicalIndex"), -1).toInt(),
            map.value(QStringLiteral("properties")).toMap()});
    }
    return result;
}

HalResult<SessionId> CAbiAdapter::openDevice(const DeviceId& deviceId,
                                             const QVariantMap& openOptions,
                                             const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    Q_UNUSED(options)
    HalResult<SessionId> result;
    result.status = ensureInitialized(QStringLiteral("adapter.openDevice"));
    if (!result.ok()) return result;
    if (m_api.openDevice == nullptr) {
        result.status = unsupported(QStringLiteral("adapter.openDevice"), deviceId);
        return result;
    }

    QString physicalDeviceId = m_config.value(QStringLiteral("settings")).toMap()
                                   .value(QStringLiteral("deviceName")).toString().trimmed();
    if (physicalDeviceId.isEmpty()) physicalDeviceId = deviceId;
    const QByteArray physicalId = physicalDeviceId.toUtf8();
    const QByteArray optionsJson = jsonBytes(openOptions);
    HalAdapterDeviceHandle device = nullptr;
    result.status = fromAdapterStatus(
        m_api.openDevice(m_handle, physicalId.constData(), optionsJson.constData(), &device),
        QStringLiteral("adapter.openDevice"),
        deviceId);
    if (!result.ok()) return result;
    if (device == nullptr) {
        result.status = makeError(HalStatusCode::AdapterError,
                                  QStringLiteral("adapter.openDevice"),
                                  QStringLiteral("Adapter returned a null device handle"),
                                  deviceId);
        return result;
    }
    result.value = QStringLiteral("cabi:%1:%2")
                       .arg(m_adapterId)
                       .arg(++m_sessionCounter);
    m_devices.insert(result.value, device);
    m_deviceIds.insert(result.value, deviceId);
    return result;
}

HalStatus CAbiAdapter::closeDevice(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    Q_UNUSED(options)
    HalStatus lookup;
    const HalAdapterDeviceHandle device = deviceHandle(
        sessionId, QStringLiteral("adapter.closeDevice"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.closeDevice == nullptr) return unsupported(QStringLiteral("adapter.closeDevice"));
    const DeviceId deviceId = m_deviceIds.value(sessionId);
    const HalStatus status = fromAdapterStatus(m_api.closeDevice(device),
                                               QStringLiteral("adapter.closeDevice"),
                                               deviceId);
    // ABI close consumes the native handle even when cleanup reports an error.
    m_devices.remove(sessionId);
    m_deviceIds.remove(sessionId);
    return status;
}

HalStatus CAbiAdapter::resetDevice(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const HalAdapterDeviceHandle device = deviceHandle(
        sessionId, QStringLiteral("adapter.resetDevice"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.resetDevice == nullptr) return unsupported(QStringLiteral("adapter.resetDevice"));
    return fromAdapterStatus(m_api.resetDevice(device, options.timeoutMs),
                             QStringLiteral("adapter.resetDevice"),
                             m_deviceIds.value(sessionId));
}

HalStatus CAbiAdapter::healthCheck(const SessionId& sessionId,
                                   const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    Q_UNUSED(options)
    HalStatus status;
    deviceHandle(sessionId, QStringLiteral("adapter.healthCheck"), &status);
    return status;
}

HalStatus CAbiAdapter::configureAnalog(const SessionId& sessionId,
                                       int physicalIndex,
                                       const AnalogRange& range,
                                       bool output,
                                       const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.configureAnalog"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.analogConfigure == nullptr) return unsupported(QStringLiteral("adapter.configureAnalog"));
    const HalAdapterAnalogRange native{range.minValue, range.maxValue, static_cast<int>(range.unit)};
    return fromAdapterStatus(m_api.analogConfigure(device,
                                                   physicalIndex,
                                                   &native,
                                                   output ? 1 : 0,
                                                   options.timeoutMs),
                             QStringLiteral("adapter.configureAnalog"),
                             m_deviceIds.value(sessionId));
}

HalResult<AnalogSample> CAbiAdapter::readAnalog(const SessionId& sessionId,
                                                int physicalIndex,
                                                const AnalogReadOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<AnalogSample> result;
    const auto batch = readAnalogBatch(sessionId, QVector<int>{physicalIndex}, options);
    result.status = batch.status;
    if (batch.ok() && !batch.value.isEmpty()) result.value = batch.value.first();
    return result;
}

HalResult<QVector<AnalogSample>> CAbiAdapter::readAnalogBatch(
    const SessionId& sessionId,
    const QVector<int>& physicalIndexes,
    const AnalogReadOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<QVector<AnalogSample>> result;
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.readAnalog"), &lookup);
    if (device == nullptr) { result.status = lookup; return result; }
    if (m_api.analogRead == nullptr) {
        result.status = unsupported(QStringLiteral("adapter.readAnalog"));
        return result;
    }
    if (physicalIndexes.isEmpty()) return result;
    const int perChannel = qMax(1, options.sampleCount);
    QVector<HalAdapterAnalogSample> samples(physicalIndexes.size() * perChannel);
    result.status = fromAdapterStatus(m_api.analogRead(device,
                                                       physicalIndexes.constData(),
                                                       physicalIndexes.size(),
                                                       samples.data(),
                                                       perChannel,
                                                       options.sampleRateHz,
                                                       options.op.timeoutMs),
                                      QStringLiteral("adapter.readAnalog"),
                                      m_deviceIds.value(sessionId));
    if (!result.ok()) return result;
    result.value.reserve(physicalIndexes.size());
    for (int index = 0; index < physicalIndexes.size(); ++index) {
        result.value.push_back(analogSample(samples.at(index * perChannel)));
    }
    return result;
}

HalStatus CAbiAdapter::writeAnalog(const SessionId& sessionId,
                                   int physicalIndex,
                                   double value,
                                   const AnalogWriteOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    return writeAnalogBatch(sessionId, QMap<int, double>{{physicalIndex, value}}, options);
}

HalStatus CAbiAdapter::writeAnalogBatch(const SessionId& sessionId,
                                        const QMap<int, double>& values,
                                        const AnalogWriteOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.writeAnalog"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.analogWrite == nullptr) return unsupported(QStringLiteral("adapter.writeAnalog"));
    QVector<int> indexes;
    QVector<double> nativeValues;
    indexes.reserve(values.size());
    nativeValues.reserve(values.size());
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        indexes.push_back(it.key());
        nativeValues.push_back(it.value());
    }
    return fromAdapterStatus(m_api.analogWrite(device,
                                               indexes.constData(),
                                               nativeValues.constData(),
                                               indexes.size(),
                                               static_cast<int>(options.range.unit),
                                               options.op.timeoutMs),
                             QStringLiteral("adapter.writeAnalog"),
                             m_deviceIds.value(sessionId));
}

HalResult<DigitalSample> CAbiAdapter::readDigital(const SessionId& sessionId,
                                                  int physicalIndex,
                                                  const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<DigitalSample> result;
    const auto batch = readDigitalBatch(sessionId, QVector<int>{physicalIndex}, options);
    result.status = batch.status;
    if (batch.ok() && !batch.value.isEmpty()) result.value = batch.value.first();
    return result;
}

HalResult<QVector<DigitalSample>> CAbiAdapter::readDigitalBatch(
    const SessionId& sessionId,
    const QVector<int>& physicalIndexes,
    const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<QVector<DigitalSample>> result;
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.readDigital"), &lookup);
    if (device == nullptr) { result.status = lookup; return result; }
    if (m_api.digitalRead == nullptr) {
        result.status = unsupported(QStringLiteral("adapter.readDigital"));
        return result;
    }
    if (physicalIndexes.isEmpty()) return result;
    QVector<HalAdapterDigitalSample> samples(physicalIndexes.size());
    result.status = fromAdapterStatus(m_api.digitalRead(device,
                                                        physicalIndexes.constData(),
                                                        physicalIndexes.size(),
                                                        samples.data(),
                                                        options.timeoutMs),
                                      QStringLiteral("adapter.readDigital"),
                                      m_deviceIds.value(sessionId));
    if (!result.ok()) return result;
    result.value.reserve(samples.size());
    for (const auto& sample : samples) result.value.push_back(digitalSample(sample));
    return result;
}

HalStatus CAbiAdapter::writeDigital(const SessionId& sessionId,
                                    int physicalIndex,
                                    DigitalLevel level,
                                    const DigitalWriteOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    return writeDigitalBatch(sessionId,
                             QMap<int, DigitalLevel>{{physicalIndex, level}},
                             options);
}

HalStatus CAbiAdapter::writeDigitalBatch(const SessionId& sessionId,
                                         const QMap<int, DigitalLevel>& values,
                                         const DigitalWriteOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.writeDigital"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.digitalWrite == nullptr) return unsupported(QStringLiteral("adapter.writeDigital"));
    QVector<int> indexes;
    QVector<int> levels;
    indexes.reserve(values.size());
    levels.reserve(values.size());
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        indexes.push_back(it.key());
        levels.push_back(adapterLevel(it.value()));
    }
    return fromAdapterStatus(m_api.digitalWrite(device,
                                                indexes.constData(),
                                                levels.constData(),
                                                indexes.size(),
                                                options.op.timeoutMs),
                             QStringLiteral("adapter.writeDigital"),
                             m_deviceIds.value(sessionId));
}

HalResult<DigitalSample> CAbiAdapter::waitDigitalEdge(
    const SessionId& sessionId,
    int physicalIndex,
    DigitalLevel targetLevel,
    const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<DigitalSample> result;
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.waitDigitalEdge"), &lookup);
    if (device == nullptr) { result.status = lookup; return result; }
    if (m_api.digitalWaitEdge == nullptr) {
        result.status = unsupported(QStringLiteral("adapter.waitDigitalEdge"));
        return result;
    }
    HalAdapterDigitalSample sample {};
    result.status = fromAdapterStatus(m_api.digitalWaitEdge(device,
                                                             physicalIndex,
                                                             adapterLevel(targetLevel),
                                                             &sample,
                                                             options.timeoutMs),
                                      QStringLiteral("adapter.waitDigitalEdge"),
                                      m_deviceIds.value(sessionId));
    if (result.ok()) result.value = digitalSample(sample);
    return result;
}

HalStatus CAbiAdapter::openSerial(const SessionId& sessionId,
                                  int physicalIndex,
                                  const SerialConfig& config,
                                  const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.openSerial"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.serialOpen == nullptr) return unsupported(QStringLiteral("adapter.openSerial"));
    HalAdapterSerialConfig native {};
    native.baudRate = config.baudRate;
    native.dataBits = config.dataBits;
    native.parity = static_cast<int>(config.parity);
    native.stopBits = static_cast<int>(config.stopBits);
    native.flowControl = static_cast<int>(config.flowControl);
    const QByteArray optionsJson = jsonBytes(config.vendorOptions);
    std::strncpy(native.optionsJson, optionsJson.constData(), sizeof(native.optionsJson) - 1);
    return fromAdapterStatus(m_api.serialOpen(device, physicalIndex, &native, options.timeoutMs),
                             QStringLiteral("adapter.openSerial"),
                             m_deviceIds.value(sessionId));
}

HalStatus CAbiAdapter::closeSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.closeSerial"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.serialClose == nullptr) return unsupported(QStringLiteral("adapter.closeSerial"));
    return fromAdapterStatus(m_api.serialClose(device, physicalIndex, options.timeoutMs),
                             QStringLiteral("adapter.closeSerial"),
                             m_deviceIds.value(sessionId));
}

HalStatus CAbiAdapter::flushSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    Q_UNUSED(sessionId)
    Q_UNUSED(physicalIndex)
    Q_UNUSED(options)
    return unsupported(QStringLiteral("adapter.flushSerial"));
}

HalStatus CAbiAdapter::writeSerial(const SessionId& sessionId,
                                   int physicalIndex,
                                   const QByteArray& data,
                                   const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.writeSerial"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.serialWrite == nullptr) return unsupported(QStringLiteral("adapter.writeSerial"));
    int written = 0;
    HalStatus status = fromAdapterStatus(m_api.serialWrite(
        device,
        physicalIndex,
        reinterpret_cast<const unsigned char*>(data.constData()),
        data.size(),
        &written,
        options.timeoutMs),
        QStringLiteral("adapter.writeSerial"),
        m_deviceIds.value(sessionId));
    if (status.ok() && written != data.size()) {
        status = makeError(HalStatusCode::IoError,
                           QStringLiteral("adapter.writeSerial"),
                           QStringLiteral("Adapter reported a partial serial write"),
                           m_deviceIds.value(sessionId), {}, {},
                           {{QStringLiteral("written"), written},
                            {QStringLiteral("requested"), data.size()}});
    }
    return status;
}

HalResult<QByteArray> CAbiAdapter::readSerial(const SessionId& sessionId,
                                              int physicalIndex,
                                              int maxBytes,
                                              const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<QByteArray> result;
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.readSerial"), &lookup);
    if (device == nullptr) { result.status = lookup; return result; }
    if (m_api.serialRead == nullptr) {
        result.status = unsupported(QStringLiteral("adapter.readSerial"));
        return result;
    }
    if (maxBytes <= 0) {
        result.status = makeError(HalStatusCode::InvalidArgument,
                                  QStringLiteral("adapter.readSerial"),
                                  QStringLiteral("maxBytes must be positive"));
        return result;
    }
    result.value.resize(maxBytes);
    int bytes = maxBytes;
    result.status = fromAdapterStatus(m_api.serialRead(
        device,
        physicalIndex,
        reinterpret_cast<unsigned char*>(result.value.data()),
        &bytes,
        options.timeoutMs),
        QStringLiteral("adapter.readSerial"),
        m_deviceIds.value(sessionId));
    if (result.ok()) result.value.resize(qBound(0, bytes, maxBytes));
    else result.value.clear();
    return result;
}

HalResult<SerialTransactionResult> CAbiAdapter::transactSerial(
    const SessionId& sessionId,
    int physicalIndex,
    const SerialTransaction& transaction)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<SerialTransactionResult> result;
    result.value.txTimestampUs = nowUs();
    result.status = writeSerial(sessionId, physicalIndex, transaction.tx, transaction.op);
    if (!result.ok()) return result;
    const auto read = readSerial(sessionId,
                                 physicalIndex,
                                 transaction.readMaxBytes,
                                 transaction.op);
    result.status = read.status;
    if (!result.ok()) return result;
    result.value.rx = read.value;
    result.value.rxTimestampUs = nowUs();
    return result;
}

HalStatus CAbiAdapter::openCan(const SessionId& sessionId,
                               int physicalIndex,
                               const CanFdConfig& config,
                               const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.openCan"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.canOpen == nullptr) return unsupported(QStringLiteral("adapter.openCan"));
    HalAdapterCanFdConfig native {};
    native.nominalBitrate = config.nominalBitrate;
    native.dataBitrate = config.dataBitrate;
    native.fdEnabled = config.fdEnabled ? 1 : 0;
    native.bitrateSwitch = config.bitrateSwitch ? 1 : 0;
    native.loopback = config.loopback ? 1 : 0;
    const QByteArray json = jsonBytes(config.vendorOptions);
    std::strncpy(native.optionsJson, json.constData(), sizeof(native.optionsJson) - 1);
    return fromAdapterStatus(m_api.canOpen(device, physicalIndex, &native, options.timeoutMs),
                             QStringLiteral("adapter.openCan"),
                             m_deviceIds.value(sessionId));
}

HalStatus CAbiAdapter::closeCan(const SessionId& sessionId,
                                int physicalIndex,
                                const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.closeCan"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.canClose == nullptr) return unsupported(QStringLiteral("adapter.closeCan"));
    return fromAdapterStatus(m_api.canClose(device, physicalIndex, options.timeoutMs),
                             QStringLiteral("adapter.closeCan"),
                             m_deviceIds.value(sessionId));
}

HalStatus CAbiAdapter::setCanFilters(const SessionId& sessionId,
                                     int physicalIndex,
                                     const QVector<CanFdFilter>& filters,
                                     const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.setCanFilters"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.canSetFilters == nullptr) return unsupported(QStringLiteral("adapter.setCanFilters"));
    QVector<HalAdapterCanFdFilter> native;
    native.reserve(filters.size());
    for (const auto& filter : filters) {
        native.push_back(HalAdapterCanFdFilter{filter.id,
                                               filter.mask,
                                               filter.extendedId ? 1 : 0});
    }
    return fromAdapterStatus(m_api.canSetFilters(device,
                                                 physicalIndex,
                                                 native.constData(),
                                                 native.size(),
                                                 options.timeoutMs),
                             QStringLiteral("adapter.setCanFilters"),
                             m_deviceIds.value(sessionId));
}

HalStatus CAbiAdapter::sendCan(const SessionId& sessionId,
                               int physicalIndex,
                               const CanFdFrame& frame,
                               const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.sendCan"), &lookup);
    if (device == nullptr) return lookup;
    if (m_api.canSend == nullptr) return unsupported(QStringLiteral("adapter.sendCan"));
    if (frame.payload.size() > 64) {
        return makeError(HalStatusCode::InvalidArgument,
                         QStringLiteral("adapter.sendCan"),
                         QStringLiteral("CAN FD payload exceeds 64 bytes"));
    }
    const HalAdapterCanFdFrame native = adapterFrame(frame);
    return fromAdapterStatus(m_api.canSend(device,
                                           physicalIndex,
                                           &native,
                                           options.timeoutMs),
                             QStringLiteral("adapter.sendCan"),
                             m_deviceIds.value(sessionId));
}

HalResult<CanFdFrame> CAbiAdapter::receiveCan(const SessionId& sessionId,
                                              int physicalIndex,
                                              const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<CanFdFrame> result;
    const auto batch = receiveCanBatch(sessionId, physicalIndex, 1, options);
    result.status = batch.status;
    if (batch.ok() && !batch.value.isEmpty()) result.value = batch.value.first();
    return result;
}

HalResult<QVector<CanFdFrame>> CAbiAdapter::receiveCanBatch(
    const SessionId& sessionId,
    int physicalIndex,
    int maxFrames,
    const OperationOptions& options)
{
    const std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
    HalResult<QVector<CanFdFrame>> result;
    HalStatus lookup;
    const auto device = deviceHandle(sessionId, QStringLiteral("adapter.receiveCan"), &lookup);
    if (device == nullptr) { result.status = lookup; return result; }
    if (m_api.canReceive == nullptr) {
        result.status = unsupported(QStringLiteral("adapter.receiveCan"));
        return result;
    }
    if (maxFrames <= 0) {
        result.status = makeError(HalStatusCode::InvalidArgument,
                                  QStringLiteral("adapter.receiveCan"),
                                  QStringLiteral("maxFrames must be positive"));
        return result;
    }
    QVector<HalAdapterCanFdFrame> native(maxFrames);
    int count = maxFrames;
    result.status = fromAdapterStatus(m_api.canReceive(device,
                                                       physicalIndex,
                                                       native.data(),
                                                       &count,
                                                       options.timeoutMs),
                                      QStringLiteral("adapter.receiveCan"),
                                      m_deviceIds.value(sessionId));
    if (!result.ok()) return result;
    count = qBound(0, count, maxFrames);
    result.value.reserve(count);
    for (int index = 0; index < count; ++index) {
        result.value.push_back(canFrame(native.at(index)));
    }
    return result;
}

} // namespace hwtest::hal
