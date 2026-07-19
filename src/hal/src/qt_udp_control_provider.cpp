#include "qt_udp_control_provider.h"

#include "hal_error_mapper.h"

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QUdpSocket>

#include <utility>

namespace hwtest::hal {

namespace {

HalStatus invalidUdpProperty(const QString& property,
                             const QString& expected)
{
    QVariantMap detail;
    detail.insert(QStringLiteral("property"), property);
    detail.insert(QStringLiteral("expected"), expected);
    return makeError(HalStatusCode::InvalidArgument,
                     QStringLiteral("control.qtUdp.open"),
                     QStringLiteral("Invalid UDP control property"),
                     {},
                     {},
                     {},
                     detail);
}

HalStatus socketError(HalStatusCode code,
                      const QString& operation,
                      const QUdpSocket& socket)
{
    QVariantMap detail;
    detail.insert(QStringLiteral("socketError"), static_cast<int>(socket.error()));
    return makeError(code,
                     operation,
                     socket.errorString(),
                     {},
                     {},
                     {},
                     detail);
}

bool readRequiredAddress(const QVariantMap& properties,
                         const QString& key,
                         QHostAddress* address)
{
    const QVariant property = properties.value(key);
    if (!property.isValid()) {
        return false;
    }
    const QString text = property.toString().trimmed();
    return !text.isEmpty() && address->setAddress(text);
}

bool readPort(const QVariantMap& properties,
              const QString& key,
              bool required,
              quint16* port)
{
    const QVariant property = properties.value(key);
    if (!property.isValid()) {
        if (required) {
            return false;
        }
        *port = 0;
        return true;
    }
    bool converted = false;
    const int parsed = property.toInt(&converted);
    const int minimum = required ? 1 : 0;
    if (!converted || parsed < minimum || parsed > 65535) {
        return false;
    }
    *port = static_cast<quint16>(parsed);
    return true;
}

int timeoutFor(const OperationOptions& options)
{
    return options.timeoutMs < 0 ? 0 : options.timeoutMs;
}

} // namespace

QtUdpControlProvider::QtUdpControlProvider() = default;

QtUdpControlProvider::~QtUdpControlProvider() = default;

HalStatus QtUdpControlProvider::open(const QVariantMap& properties,
                                     const OperationOptions& options)
{
    Q_UNUSED(options)
    if (m_socket != nullptr && m_socket->state() == QAbstractSocket::BoundState) {
        return makeError(HalStatusCode::InvalidState,
                         QStringLiteral("control.qtUdp.open"),
                         QStringLiteral("UDP control channel is already open"));
    }

    QHostAddress remoteAddress;
    if (!readRequiredAddress(properties, QStringLiteral("remoteAddress"), &remoteAddress)) {
        return invalidUdpProperty(QStringLiteral("remoteAddress"), QStringLiteral("a valid IP address"));
    }
    quint16 remotePort = 0;
    if (!readPort(properties, QStringLiteral("remotePort"), true, &remotePort)) {
        return invalidUdpProperty(QStringLiteral("remotePort"), QStringLiteral("an integer from 1 to 65535"));
    }

    QHostAddress localAddress = remoteAddress.protocol() == QAbstractSocket::IPv6Protocol
        ? QHostAddress(QHostAddress::AnyIPv6)
        : QHostAddress(QHostAddress::AnyIPv4);
    const QVariant localAddressProperty = properties.value(QStringLiteral("localAddress"));
    if (localAddressProperty.isValid() && !localAddressProperty.toString().trimmed().isEmpty() &&
        !localAddress.setAddress(localAddressProperty.toString().trimmed())) {
        return invalidUdpProperty(QStringLiteral("localAddress"), QStringLiteral("a valid IP address"));
    }
    if (localAddress.protocol() != remoteAddress.protocol()) {
        return invalidUdpProperty(QStringLiteral("localAddress"),
                                  QStringLiteral("an address in the remote address family"));
    }

    quint16 localPort = 0;
    if (!readPort(properties, QStringLiteral("localPort"), false, &localPort)) {
        return invalidUdpProperty(QStringLiteral("localPort"), QStringLiteral("an integer from 0 to 65535"));
    }

    auto socket = std::make_unique<QUdpSocket>();
    if (!socket->bind(localAddress, localPort)) {
        return socketError(HalStatusCode::IoError,
                           QStringLiteral("control.qtUdp.open"),
                           *socket);
    }

    m_remoteAddress = remoteAddress;
    m_remotePort = remotePort;
    m_socket = std::move(socket);
    return {};
}

HalStatus QtUdpControlProvider::close(const OperationOptions& options)
{
    Q_UNUSED(options)
    if (m_socket != nullptr) {
        m_socket->close();
        m_socket.reset();
    }
    m_remoteAddress = QHostAddress();
    m_remotePort = 0;
    return {};
}

HalStatus QtUdpControlProvider::write(const QByteArray& data,
                                      const OperationOptions& options)
{
    Q_UNUSED(options)
    if (m_socket == nullptr || m_socket->state() != QAbstractSocket::BoundState) {
        return makeError(HalStatusCode::InvalidState,
                         QStringLiteral("control.qtUdp.write"),
                         QStringLiteral("UDP control channel is not open"));
    }

    const qint64 written = m_socket->writeDatagram(data, m_remoteAddress, m_remotePort);
    if (written != data.size()) {
        return socketError(HalStatusCode::IoError,
                           QStringLiteral("control.qtUdp.write"),
                           *m_socket);
    }
    return {};
}

HalResult<QByteArray> QtUdpControlProvider::read(int maxBytes,
                                                  const OperationOptions& options)
{
    HalResult<QByteArray> result;
    if (maxBytes <= 0) {
        result.status = makeError(HalStatusCode::InvalidArgument,
                                  QStringLiteral("control.qtUdp.read"),
                                  QStringLiteral("Maximum read size must be positive"));
        return result;
    }
    if (m_socket == nullptr || m_socket->state() != QAbstractSocket::BoundState) {
        result.status = makeError(HalStatusCode::InvalidState,
                                  QStringLiteral("control.qtUdp.read"),
                                  QStringLiteral("UDP control channel is not open"));
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    const int timeoutMs = timeoutFor(options);

    while (true) {
        if (!m_socket->hasPendingDatagrams()) {
            const int remaining = qMax(0, timeoutMs - static_cast<int>(timer.elapsed()));
            if (remaining == 0 || !m_socket->waitForReadyRead(remaining)) {
                const HalStatusCode code = m_socket->error() == QAbstractSocket::SocketTimeoutError ||
                        m_socket->error() == QAbstractSocket::UnknownSocketError
                    ? HalStatusCode::Timeout
                    : HalStatusCode::IoError;
                result.status = socketError(code, QStringLiteral("control.qtUdp.read"), *m_socket);
                return result;
            }
        }

        const qint64 datagramSize = m_socket->pendingDatagramSize();
        if (datagramSize < 0) {
            result.status = socketError(HalStatusCode::IoError,
                                        QStringLiteral("control.qtUdp.read"),
                                        *m_socket);
            return result;
        }

        QByteArray datagram(static_cast<int>(datagramSize), Qt::Uninitialized);
        QHostAddress senderAddress;
        quint16 senderPort = 0;
        const qint64 read = m_socket->readDatagram(datagram.data(),
                                                   datagram.size(),
                                                   &senderAddress,
                                                   &senderPort);
        if (read < 0) {
            result.status = socketError(HalStatusCode::IoError,
                                        QStringLiteral("control.qtUdp.read"),
                                        *m_socket);
            return result;
        }
        if (senderAddress != m_remoteAddress || senderPort != m_remotePort) {
            continue;
        }
        if (datagramSize > static_cast<qint64>(maxBytes)) {
            QVariantMap detail;
            detail.insert(QStringLiteral("maxBytes"), maxBytes);
            detail.insert(QStringLiteral("datagramBytes"), datagramSize);
            result.status = makeError(HalStatusCode::BufferTooSmall,
                                      QStringLiteral("control.qtUdp.read"),
                                      QStringLiteral("UDP datagram exceeds the requested read size"),
                                      {},
                                      {},
                                      {},
                                      detail);
            return result;
        }

        datagram.resize(static_cast<int>(read));
        result.value = std::move(datagram);
        return result;
    }
}

} // namespace hwtest::hal
