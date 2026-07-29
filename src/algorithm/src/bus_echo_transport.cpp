#include <algorithm/bus_echo_transport.h>

#include <algorithm/mbddf_protocol.h>

#include <QElapsedTimer>
#include <QSet>

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

TransportResult failed(const hwtest::hal::HalStatus& status,
                       const QString& fallback)
{
    const TransportResult::Error errorCode =
        status.code == hwtest::hal::HalStatusCode::Timeout
        ? TransportResult::Error::Timeout
        : TransportResult::Error::Io;
    return failed(status.error.message.isEmpty() ? fallback : status.error.message,
                  errorCode);
}

int remainingMs(const QElapsedTimer& timer, int timeoutMs)
{
    return qMax(0, timeoutMs - static_cast<int>(timer.elapsed()));
}

hwtest::hal::OperationOptions operationOptions(
    int timeoutMs,
    const hwtest::hal::RequestId& requestId)
{
    hwtest::hal::OperationOptions options;
    options.timeoutMs = timeoutMs;
    options.requestId = requestId;
    options.tags.insert(QStringLiteral("role"), QStringLiteral("bus-echo-auxiliary"));
    return options;
}

} // namespace

BusEchoTransport::BusEchoTransport(
    std::unique_ptr<IByteTransport> controlTransport,
    hwtest::hal::IControlChannel* rawChannel,
    hwtest::hal::ResourceId controlResourceId)
    : m_controlTransport(std::move(controlTransport))
    , m_rawChannel(rawChannel)
    , m_controlResourceId(std::move(controlResourceId))
{
}

BusEchoTransport::~BusEchoTransport()
{
    close();
}

void BusEchoTransport::setRequestId(const hwtest::hal::RequestId& requestId)
{
    m_requestId = requestId;
    if (m_controlTransport != nullptr) {
        m_controlTransport->setRequestId(requestId);
    }
}

bool BusEchoTransport::configure(const QVariantMap& options, QString* error)
{
    if (m_open || m_rawOpen) {
        if (error != nullptr) {
            *error = QStringLiteral("BUS_ECHO transport must be closed before reconfiguration");
        }
        return false;
    }
    if (m_controlTransport == nullptr || m_rawChannel == nullptr ||
        m_controlResourceId.trimmed().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("BUS_ECHO requires control and auxiliary HAL channels");
        }
        return false;
    }

    const QVariantMap busEcho = options.value(QStringLiteral("busEcho")).toMap();
    bool payloadBytesOk = false;
    const int payloadBytes = busEcho.value(QStringLiteral("payloadBytes")).toInt(
        &payloadBytesOk);
    if (!payloadBytesOk || payloadBytes != 114) {
        if (error != nullptr) {
            *error = QStringLiteral("BUS_ECHO payloadBytes must be exactly 114");
        }
        return false;
    }

    const QVariantMap configuredMapping =
        busEcho.value(QStringLiteral("resourceByLink")).toMap();
    const QList<int> requiredLinks{0, 1, 3};
    if (configuredMapping.size() != requiredLinks.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("BUS_ECHO resourceByLink must contain only links 0, 1 and 3");
        }
        return false;
    }
    QMap<int, hwtest::hal::ResourceId> mapping;
    QSet<QString> resources;
    for (int linkId : requiredLinks) {
        const QString key = QString::number(linkId);
        const QString resourceId = configuredMapping.value(key).toString().trimmed();
        if (resourceId.isEmpty() || resourceId == m_controlResourceId ||
            resources.contains(resourceId)) {
            if (error != nullptr) {
                *error = QStringLiteral("BUS_ECHO auxiliary resources must be non-empty, unique and different from the control resource");
            }
            return false;
        }
        mapping.insert(linkId, resourceId);
        resources.insert(resourceId);
    }
    for (auto iterator = configuredMapping.cbegin();
         iterator != configuredMapping.cend(); ++iterator) {
        bool linkOk = false;
        const int linkId = iterator.key().toInt(&linkOk);
        if (!linkOk || !mapping.contains(linkId)) {
            if (error != nullptr) {
                *error = QStringLiteral("BUS_ECHO resourceByLink must contain only links 0, 1 and 3");
            }
            return false;
        }
    }

    bool openTimeoutOk = true;
    const int openTimeoutMs = options.contains(QStringLiteral("openTimeoutMs"))
        ? options.value(QStringLiteral("openTimeoutMs")).toInt(&openTimeoutOk)
        : m_openTimeoutMs;
    if (!openTimeoutOk || openTimeoutMs <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("BUS_ECHO openTimeoutMs must be a positive integer");
        }
        return false;
    }
    if (!m_controlTransport->configure(options, error)) {
        return false;
    }

    m_payloadBytes = payloadBytes;
    m_resourceByLink = mapping;
    m_openTimeoutMs = openTimeoutMs;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool BusEchoTransport::open(QString* error)
{
    if (m_open) {
        if (error != nullptr) error->clear();
        return true;
    }
    if (m_controlTransport == nullptr || m_rawChannel == nullptr ||
        m_resourceByLink.size() != 3) {
        if (error != nullptr) {
            *error = QStringLiteral("BUS_ECHO transport is not configured");
        }
        return false;
    }
    if (!m_controlTransport->open(error)) {
        return false;
    }
    m_open = true;
    if (error != nullptr) error->clear();
    return true;
}

TransportResult BusEchoTransport::transact(const QByteArray& frame, int timeoutMs)
{
    if (!m_open || m_controlTransport == nullptr || m_rawChannel == nullptr) {
        return failed(QStringLiteral("BUS_ECHO transport is not open"));
    }
    if (timeoutMs <= 0) {
        return failed(QStringLiteral("BUS_ECHO transaction timed out"),
                      TransportResult::Error::Timeout);
    }

    QByteArray requestPayload;
    QString protocolError;
    if (!decodeFrame(frame, &requestPayload, &protocolError) ||
        requestPayload.size() < 6 + m_payloadBytes) {
        return failed(QStringLiteral("Invalid BUS_ECHO request frame: %1")
                          .arg(protocolError));
    }
    const quint8 typeGroup = static_cast<quint8>(requestPayload.at(1));
    const quint8 subType = static_cast<quint8>(requestPayload.at(2));
    if (typeGroup != 0x03u || subType != 0x02u) {
        return failed(QStringLiteral("BUS_ECHO transport received a non-03/02 request"));
    }
    const int linkId = static_cast<quint8>(requestPayload.at(5));
    const auto resource = m_resourceByLink.constFind(linkId);
    if (resource == m_resourceByLink.cend()) {
        return failed(QStringLiteral("BUS_ECHO link %1 is not supported").arg(linkId));
    }
    const QByteArray expectedPayload = requestPayload.mid(6, m_payloadBytes);

    QElapsedTimer timer;
    timer.start();
    const auto timedOptions = [&](int limitMs = -1) {
        const int remaining = remainingMs(timer, timeoutMs);
        return operationOptions(limitMs > 0 ? qMin(remaining, limitMs) : remaining,
                                m_requestId);
    };

    if (m_rawOpen && m_rawResourceId != resource.value()) {
        const hwtest::hal::HalStatus closed = m_rawChannel->closeControl(
            m_rawResourceId, timedOptions(m_openTimeoutMs));
        if (!closed.ok()) {
            return failed(closed, QStringLiteral("Unable to close previous BUS_ECHO auxiliary link"));
        }
        m_rawOpen = false;
        m_rawResourceId.clear();
    }
    if (!m_rawOpen) {
        if (remainingMs(timer, timeoutMs) <= 0) {
            return failed(QStringLiteral("BUS_ECHO auxiliary open timed out"),
                          TransportResult::Error::Timeout);
        }
        const hwtest::hal::HalStatus opened = m_rawChannel->openControl(
            resource.value(), timedOptions(m_openTimeoutMs));
        if (!opened.ok()) {
            return failed(opened, QStringLiteral("Unable to open BUS_ECHO auxiliary link"));
        }
        m_rawOpen = true;
        m_rawResourceId = resource.value();
    }

    const TransportResult written = m_controlTransport->writeFrame(
        frame, remainingMs(timer, timeoutMs));
    if (!written.ok) return written;

    QByteArray received;
    received.reserve(m_payloadBytes);
    while (received.size() < m_payloadBytes) {
        const int remaining = remainingMs(timer, timeoutMs);
        if (remaining <= 0) {
            return failed(QStringLiteral("BUS_ECHO auxiliary read timed out after %1 of %2 bytes")
                              .arg(received.size()).arg(m_payloadBytes),
                          TransportResult::Error::Timeout);
        }
        const hwtest::hal::HalResult<QByteArray> read = m_rawChannel->readControl(
            m_rawResourceId, m_payloadBytes - received.size(), timedOptions());
        if (!read.ok()) {
            return failed(read.status, QStringLiteral("BUS_ECHO auxiliary read failed"));
        }
        received.append(read.value);
    }

    if (remainingMs(timer, timeoutMs) <= 0) {
        return failed(QStringLiteral("BUS_ECHO auxiliary write timed out"),
                      TransportResult::Error::Timeout);
    }
    const hwtest::hal::HalStatus echoed = m_rawChannel->writeControl(
        m_rawResourceId, received, timedOptions());
    if (!echoed.ok()) {
        return failed(echoed, QStringLiteral("BUS_ECHO auxiliary write failed"));
    }

    const TransportResult response = m_controlTransport->readFrame(
        remainingMs(timer, timeoutMs));
    if (!response.ok) return response;
    if (received != expectedPayload) {
        int mismatchIndex = 0;
        while (mismatchIndex < received.size() &&
               received.at(mismatchIndex) == expectedPayload.at(mismatchIndex)) {
            ++mismatchIndex;
        }
        return failed(QStringLiteral("BUS_ECHO payload mismatch at byte %1")
                          .arg(mismatchIndex));
    }
    return response;
}

void BusEchoTransport::close()
{
    if (m_rawOpen && m_rawChannel != nullptr) {
        m_rawChannel->closeControl(
            m_rawResourceId, operationOptions(m_openTimeoutMs, m_requestId));
    }
    m_rawOpen = false;
    m_rawResourceId.clear();
    if (m_controlTransport != nullptr) {
        m_controlTransport->close();
    }
    m_open = false;
}

} // namespace hwtest::algorithm::mbddf
