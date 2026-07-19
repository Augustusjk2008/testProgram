#include <algorithm/mbddf_transport.h>

#include <hal/i_control_channel.h>
#include <hal/i_hal_device.h>
#include <hal/i_serial_bus.h>

#include <QElapsedTimer>
#include <QMutexLocker>

namespace hwtest::algorithm::mbddf {

namespace {

TransportResult failed(const QString& message,
                        TransportResult::Error errorCode = TransportResult::Error::Io)
{
    TransportResult result;
    result.errorCode = errorCode;
    result.error = message;
    return result;
}

hwtest::hal::SerialConfig defaultSerialConfig()
{
    hwtest::hal::SerialConfig config;
    config.baudRate = 614400;
    config.dataBits = 8;
    config.parity = hwtest::hal::SerialParity::Even;
    config.stopBits = hwtest::hal::SerialStopBits::One;
    config.flowControl = hwtest::hal::SerialFlowControl::None;
    return config;
}

bool parseSerialConfig(const QVariantMap& options,
                       hwtest::hal::SerialConfig* config,
                       QString* error)
{
    if (config == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("serial config output is null");
        }
        return false;
    }
    const int baudRate = options.value(QStringLiteral("baudRate"), 614400).toInt();
    const int dataBits = options.value(QStringLiteral("dataBits"), 8).toInt();
    const QString parity = options.value(QStringLiteral("parity"), QStringLiteral("Even"))
                               .toString();
    const int stopBits = options.value(QStringLiteral("stopBits"), 1).toInt();
    const QString flowControl = options.value(QStringLiteral("flowControl"), QStringLiteral("None"))
                                    .toString();
    if (baudRate != 614400 || dataBits != 8 ||
        parity.compare(QStringLiteral("Even"), Qt::CaseInsensitive) != 0 ||
        stopBits != 1 ||
        flowControl.compare(QStringLiteral("None"), Qt::CaseInsensitive) != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("MB_DDF serial settings must be 614400/8E1/no-flow-control");
        }
        return false;
    }
    *config = defaultSerialConfig();
    return true;
}

QString statusMessage(const hwtest::hal::HalStatus& status, const QString& fallback)
{
    return status.error.message.isEmpty() ? fallback : status.error.message;
}

int remainingMs(const QElapsedTimer& timer, int timeoutMs)
{
    return qMax(0, timeoutMs - static_cast<int>(timer.elapsed()));
}

} // namespace

ScriptedByteTransport::ScriptedByteTransport(Handler handler)
    : m_handler(std::move(handler))
{
}

bool ScriptedByteTransport::open(QString* error)
{
    if (!m_handler) {
        if (error != nullptr) {
            *error = QStringLiteral("Scripted byte transport has no handler");
        }
        return false;
    }
    m_open = true;
    return true;
}

TransportResult ScriptedByteTransport::transact(const QByteArray& frame, int timeoutMs)
{
    if (!m_open) {
        return failed(QStringLiteral("Scripted byte transport is not open"));
    }
    m_lastRequest = frame;
    ++m_transactionCount;
    return m_handler(frame, timeoutMs);
}

void ScriptedByteTransport::close()
{
    m_open = false;
}

bool ScriptedByteTransport::isOpen() const noexcept
{
    return m_open;
}

int ScriptedByteTransport::transactionCount() const noexcept
{
    return m_transactionCount;
}

QByteArray ScriptedByteTransport::lastRequest() const
{
    return m_lastRequest;
}

SystemStatusSimulator::SystemStatusSimulator(const ProtocolCatalog* catalog)
    : m_catalog(catalog)
{
}

bool SystemStatusSimulator::open(QString* error)
{
    if (m_catalog == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("System status simulator requires a protocol catalog");
        }
        return false;
    }
    if (m_catalog->findByName(QStringLiteral("system_status_request")) == nullptr ||
        m_catalog->findByName(QStringLiteral("system_status_response")) == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("System status protocol definitions are missing");
        }
        return false;
    }
    m_open = true;
    return true;
}

TransportResult SystemStatusSimulator::transact(const QByteArray& frame, int timeoutMs)
{
    Q_UNUSED(timeoutMs)
    if (!m_open || m_catalog == nullptr) {
        return failed(QStringLiteral("System status simulator is not open"));
    }
    m_lastRequest = frame;
    ++m_transactionCount;

    if (m_fault == Fault::Timeout) {
        return failed(QStringLiteral("simulated timeout"), TransportResult::Error::Timeout);
    }

    QString error;
    QByteArray requestPayload;
    if (!decodeFrame(frame, &requestPayload, &error)) {
        return failed(QStringLiteral("simulator rejected request: %1").arg(error));
    }
    if (requestPayload.size() < 3) {
        return failed(QStringLiteral("simulator received a payload shorter than the command header"));
    }
    const MessageDefinition* request =
        m_catalog->findByCommand(static_cast<quint8>(requestPayload.at(1)),
                                 static_cast<quint8>(requestPayload.at(2)),
                                 Direction::Request);
    if (request == nullptr || request->name != QStringLiteral("system_status_request")) {
        return failed(QStringLiteral("simulator received an unsupported request"));
    }

    QVariantMap requestValues;
    if (!decodePayload(*request, requestPayload, &requestValues, &error)) {
        return failed(QStringLiteral("simulator could not decode request: %1").arg(error));
    }

    const MessageDefinition* response = m_catalog->findByName(QStringLiteral("system_status_response"));
    QVariantMap values = m_responseValues;
    if (!values.contains(QStringLiteral("status"))) {
        values.insert(QStringLiteral("status"), 0);
    }
    if (!values.contains(QStringLiteral("err_code"))) {
        values.insert(QStringLiteral("err_code"), 0);
    }

    QByteArray responsePayload;
    if (!encodePayload(*response,
                       values,
                       static_cast<quint16>(requestValues.value(QStringLiteral("seq")).toUInt()),
                       &responsePayload,
                       &error)) {
        return failed(QStringLiteral("simulator could not encode response: %1").arg(error));
    }

    QByteArray responseFrame;
    if (!encodeFrame(responsePayload, &responseFrame, &error)) {
        return failed(QStringLiteral("simulator could not frame response: %1").arg(error));
    }
    if (m_fault == Fault::BadCrc && !responseFrame.isEmpty()) {
        responseFrame[responseFrame.size() - 1] =
            static_cast<char>(responseFrame.at(responseFrame.size() - 1) ^ 0xFF);
    } else if (m_fault == Fault::InvalidResponse && responseFrame.size() > 5) {
        responseFrame[4] = static_cast<char>(0x7F);
    }

    TransportResult result;
    result.ok = true;
    result.frame = responseFrame;
    return result;
}

void SystemStatusSimulator::close()
{
    m_open = false;
}

void SystemStatusSimulator::setResponseValues(const QVariantMap& values)
{
    m_responseValues = values;
}

void SystemStatusSimulator::setFault(Fault fault)
{
    m_fault = fault;
}

bool SystemStatusSimulator::isOpen() const noexcept
{
    return m_open;
}

int SystemStatusSimulator::transactionCount() const noexcept
{
    return m_transactionCount;
}

QByteArray SystemStatusSimulator::lastRequest() const
{
    return m_lastRequest;
}

HalControlTransport::HalControlTransport(hwtest::hal::IHalDevice* device,
                                         hwtest::hal::ResourceId resourceId)
    : m_device(device)
    , m_resourceId(std::move(resourceId))
{
}

bool HalControlTransport::configure(const QVariantMap& options, QString* error)
{
    if (m_open) {
        if (error != nullptr) {
            *error = QStringLiteral("HAL control transport must be closed before reconfiguration");
        }
        return false;
    }

    bool openTimeoutOk = true;
    bool readChunkOk = true;
    const int openTimeoutMs = options.contains(QStringLiteral("openTimeoutMs"))
        ? options.value(QStringLiteral("openTimeoutMs")).toInt(&openTimeoutOk)
        : m_openTimeoutMs;
    const int readChunkBytes = options.contains(QStringLiteral("readChunkBytes"))
        ? options.value(QStringLiteral("readChunkBytes")).toInt(&readChunkOk)
        : m_readChunkBytes;
    if (!openTimeoutOk || openTimeoutMs <= 0 || !readChunkOk || readChunkBytes <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("control openTimeoutMs and readChunkBytes must be positive integers");
        }
        return false;
    }

    m_openTimeoutMs = openTimeoutMs;
    m_readChunkBytes = readChunkBytes;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool HalControlTransport::open(QString* error)
{
    if (m_open) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    if (m_device == nullptr || m_device->controlChannel() == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("HAL control channel is unavailable");
        }
        return false;
    }

    hwtest::hal::OperationOptions options;
    options.timeoutMs = m_openTimeoutMs;
    const hwtest::hal::HalStatus status =
        m_device->controlChannel()->openControl(m_resourceId, options);
    if (!status.ok()) {
        if (error != nullptr) {
            *error = statusMessage(status, QStringLiteral("Unable to open HAL control channel"));
        }
        return false;
    }

    m_receiveBuffer.clear();
    m_open = true;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool HalControlTransport::takeBufferedFrame(QByteArray* frame)
{
    if (frame == nullptr) {
        return false;
    }

    static const QByteArray sync = QByteArray::fromHex("55AA");
    const int syncIndex = m_receiveBuffer.indexOf(sync);
    if (syncIndex < 0) {
        const bool keepPossibleSync = !m_receiveBuffer.isEmpty() &&
            static_cast<quint8>(m_receiveBuffer.back()) == 0x55u;
        const char trailing = keepPossibleSync ? m_receiveBuffer.back() : '\0';
        m_receiveBuffer.clear();
        if (keepPossibleSync) {
            m_receiveBuffer.append(trailing);
        }
        return false;
    }
    if (syncIndex > 0) {
        m_receiveBuffer.remove(0, syncIndex);
    }
    if (m_receiveBuffer.size() < 3) {
        return false;
    }

    const int payloadBytes = static_cast<quint8>(m_receiveBuffer.at(2));
    const int frameBytes = 2 + 1 + payloadBytes + 2;
    if (m_receiveBuffer.size() < frameBytes) {
        return false;
    }

    *frame = m_receiveBuffer.left(frameBytes);
    m_receiveBuffer.remove(0, frameBytes);
    return true;
}

TransportResult HalControlTransport::transact(const QByteArray& frame, int timeoutMs)
{
    if (!m_open || m_device == nullptr || m_device->controlChannel() == nullptr) {
        return failed(QStringLiteral("HAL control transport is not open"));
    }
    if (timeoutMs <= 0) {
        return failed(QStringLiteral("HAL control transaction deadline is invalid"),
                      TransportResult::Error::Timeout);
    }

    QElapsedTimer timer;
    timer.start();

    hwtest::hal::OperationOptions options;
    options.timeoutMs = remainingMs(timer, timeoutMs);
    const hwtest::hal::HalStatus writeStatus =
        m_device->controlChannel()->writeControl(m_resourceId, frame, options);
    if (!writeStatus.ok()) {
        const auto errorCode = writeStatus.code == hwtest::hal::HalStatusCode::Timeout
            ? TransportResult::Error::Timeout
            : TransportResult::Error::Io;
        return failed(statusMessage(writeStatus, QStringLiteral("HAL control write failed")),
                      errorCode);
    }

    QByteArray completeFrame;
    while (!takeBufferedFrame(&completeFrame)) {
        const int remaining = remainingMs(timer, timeoutMs);
        if (remaining <= 0) {
            return failed(QStringLiteral("HAL control read timed out"),
                          TransportResult::Error::Timeout);
        }

        options.timeoutMs = remaining;
        const hwtest::hal::HalResult<QByteArray> readResult =
            m_device->controlChannel()->readControl(m_resourceId, m_readChunkBytes, options);
        if (!readResult.ok()) {
            const auto errorCode = readResult.status.code == hwtest::hal::HalStatusCode::Timeout
                ? TransportResult::Error::Timeout
                : TransportResult::Error::Io;
            return failed(statusMessage(readResult.status,
                                        QStringLiteral("HAL control read failed")),
                          errorCode);
        }
        m_receiveBuffer.append(readResult.value);
    }

    TransportResult result;
    result.ok = true;
    result.frame = completeFrame;
    return result;
}

void HalControlTransport::close()
{
    if (m_open && m_device != nullptr && m_device->controlChannel() != nullptr) {
        hwtest::hal::OperationOptions options;
        options.timeoutMs = m_openTimeoutMs;
        m_device->controlChannel()->closeControl(m_resourceId, options);
    }
    m_receiveBuffer.clear();
    m_open = false;
}

HalSerialTransport::HalSerialTransport(hwtest::hal::IHalDevice* device,
                                       hwtest::hal::ResourceId resourceId)
    : HalSerialTransport(device, std::move(resourceId), defaultSerialConfig())
{
}

HalSerialTransport::HalSerialTransport(hwtest::hal::IHalDevice* device,
                                       hwtest::hal::ResourceId resourceId,
                                       hwtest::hal::SerialConfig serialConfig)
    : m_device(device)
    , m_resourceId(std::move(resourceId))
    , m_serialConfig(serialConfig)
{
}

bool HalSerialTransport::configure(const QVariantMap& options, QString* error)
{
    if (m_open) {
        if (error != nullptr) {
            *error = QStringLiteral("HAL serial transport must be closed before reconfiguration");
        }
        return false;
    }
    return parseSerialConfig(options, &m_serialConfig, error);
}

bool HalSerialTransport::open(QString* error)
{
    if (m_device == nullptr || m_device->serialBus() == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("HAL serial device is unavailable");
        }
        return false;
    }
    hwtest::hal::OperationOptions options;
    options.timeoutMs = 2000;
    const hwtest::hal::HalStatus status =
        m_device->serialBus()->openSerial(m_resourceId, m_serialConfig, options);
    if (!status.ok()) {
        if (error != nullptr) {
            *error = status.error.message;
        }
        return false;
    }
    m_open = true;
    return true;
}

TransportResult HalSerialTransport::transact(const QByteArray& frame, int timeoutMs)
{
    if (!m_open || m_device == nullptr || m_device->serialBus() == nullptr) {
        return failed(QStringLiteral("HAL serial transport is not open"));
    }

    hwtest::hal::SerialTransaction transaction;
    transaction.tx = frame;
    transaction.readMinBytes = 5;
    transaction.readMaxBytes = 260;
    transaction.op.timeoutMs = timeoutMs;
    const hwtest::hal::HalResult<hwtest::hal::SerialTransactionResult> result =
        m_device->serialBus()->transactSerial(m_resourceId, transaction);
    if (!result.ok()) {
        const auto errorCode = result.status.code == hwtest::hal::HalStatusCode::Timeout
            ? TransportResult::Error::Timeout
            : TransportResult::Error::Io;
        return failed(result.status.error.message, errorCode);
    }

    TransportResult transportResult;
    transportResult.ok = true;
    transportResult.frame = result.value.rx;
    return transportResult;
}

void HalSerialTransport::close()
{
    if (m_open && m_device != nullptr && m_device->serialBus() != nullptr) {
        hwtest::hal::OperationOptions options;
        options.timeoutMs = 2000;
        m_device->serialBus()->closeSerial(m_resourceId, options);
    }
    m_open = false;
}

} // namespace hwtest::algorithm::mbddf
