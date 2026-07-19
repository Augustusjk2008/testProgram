#include "qt_serial_control_provider.h"

#include "hal_error_mapper.h"

#include <QIODevice>
#include <QElapsedTimer>
#include <QSerialPort>

#include <utility>

namespace hwtest::hal {

namespace {

HalStatus invalidSerialProperty(const QString& property,
                                const QString& expected)
{
    QVariantMap detail;
    detail.insert(QStringLiteral("property"), property);
    detail.insert(QStringLiteral("expected"), expected);
    return makeError(HalStatusCode::InvalidArgument,
                     QStringLiteral("control.qtSerial.open"),
                     QStringLiteral("Invalid serial control property"),
                     {},
                     {},
                     {},
                     detail);
}

HalStatus serialError(HalStatusCode code,
                      const QString& operation,
                      const QSerialPort& port)
{
    QVariantMap detail;
    detail.insert(QStringLiteral("serialError"), static_cast<int>(port.error()));
    return makeError(code,
                     operation,
                     port.errorString(),
                     {},
                     {},
                     {},
                     detail);
}

bool readRequiredInteger(const QVariantMap& properties,
                         const QString& key,
                         int* value)
{
    const QVariant property = properties.value(key);
    bool converted = false;
    const int parsed = property.toInt(&converted);
    if (!property.isValid() || !converted) {
        return false;
    }
    *value = parsed;
    return true;
}

bool readRequiredText(const QVariantMap& properties,
                      const QString& key,
                      QString* value)
{
    const QVariant property = properties.value(key);
    if (!property.isValid()) {
        return false;
    }
    const QString parsed = property.toString().trimmed();
    if (parsed.isEmpty()) {
        return false;
    }
    *value = parsed;
    return true;
}

bool toDataBits(int value, QSerialPort::DataBits* dataBits)
{
    switch (value) {
    case 5:
        *dataBits = QSerialPort::Data5;
        return true;
    case 6:
        *dataBits = QSerialPort::Data6;
        return true;
    case 7:
        *dataBits = QSerialPort::Data7;
        return true;
    case 8:
        *dataBits = QSerialPort::Data8;
        return true;
    default:
        return false;
    }
}

bool toParity(const QString& value, QSerialPort::Parity* parity)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("none")) {
        *parity = QSerialPort::NoParity;
        return true;
    }
    if (normalized == QStringLiteral("odd")) {
        *parity = QSerialPort::OddParity;
        return true;
    }
    if (normalized == QStringLiteral("even")) {
        *parity = QSerialPort::EvenParity;
        return true;
    }
    if (normalized == QStringLiteral("mark")) {
        *parity = QSerialPort::MarkParity;
        return true;
    }
    if (normalized == QStringLiteral("space")) {
        *parity = QSerialPort::SpaceParity;
        return true;
    }
    return false;
}

bool toStopBits(const QVariantMap& properties, QSerialPort::StopBits* stopBits)
{
    const QVariant property = properties.value(QStringLiteral("stopBits"));
    bool converted = false;
    const double value = property.toDouble(&converted);
    if (!property.isValid() || !converted) {
        return false;
    }
    if (value == 1.0) {
        *stopBits = QSerialPort::OneStop;
        return true;
    }
    if (value == 1.5) {
        *stopBits = QSerialPort::OneAndHalfStop;
        return true;
    }
    if (value == 2.0) {
        *stopBits = QSerialPort::TwoStop;
        return true;
    }
    return false;
}

bool toFlowControl(const QString& value, QSerialPort::FlowControl* flowControl)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("none") ||
        normalized == QStringLiteral("noflowcontrol")) {
        *flowControl = QSerialPort::NoFlowControl;
        return true;
    }
    if (normalized == QStringLiteral("hardware") ||
        normalized == QStringLiteral("hardwarecontrol")) {
        *flowControl = QSerialPort::HardwareControl;
        return true;
    }
    if (normalized == QStringLiteral("software") ||
        normalized == QStringLiteral("softwarecontrol")) {
        *flowControl = QSerialPort::SoftwareControl;
        return true;
    }
    return false;
}

int timeoutFor(const OperationOptions& options)
{
    return options.timeoutMs < 0 ? 0 : options.timeoutMs;
}

} // namespace

QtSerialControlProvider::QtSerialControlProvider() = default;

QtSerialControlProvider::~QtSerialControlProvider() = default;

HalStatus QtSerialControlProvider::open(const QVariantMap& properties,
                                        const OperationOptions& options)
{
    Q_UNUSED(options)
    if (m_port != nullptr && m_port->isOpen()) {
        return makeError(HalStatusCode::InvalidState,
                         QStringLiteral("control.qtSerial.open"),
                         QStringLiteral("Serial control channel is already open"));
    }

    QString portName;
    if (!readRequiredText(properties, QStringLiteral("portName"), &portName)) {
        return invalidSerialProperty(QStringLiteral("portName"), QStringLiteral("a non-empty port name"));
    }

    int baudRate = 0;
    if (!readRequiredInteger(properties, QStringLiteral("baudRate"), &baudRate) || baudRate <= 0) {
        return invalidSerialProperty(QStringLiteral("baudRate"), QStringLiteral("a positive integer"));
    }

    int dataBitValue = 0;
    QSerialPort::DataBits dataBits;
    if (!readRequiredInteger(properties, QStringLiteral("dataBits"), &dataBitValue) ||
        !toDataBits(dataBitValue, &dataBits)) {
        return invalidSerialProperty(QStringLiteral("dataBits"), QStringLiteral("one of 5, 6, 7, or 8"));
    }

    QString parityText;
    QSerialPort::Parity parity;
    if (!readRequiredText(properties, QStringLiteral("parity"), &parityText) ||
        !toParity(parityText, &parity)) {
        return invalidSerialProperty(QStringLiteral("parity"),
                                     QStringLiteral("one of None, Odd, Even, Mark, or Space"));
    }

    QSerialPort::StopBits stopBits;
    if (!toStopBits(properties, &stopBits)) {
        return invalidSerialProperty(QStringLiteral("stopBits"), QStringLiteral("one of 1, 1.5, or 2"));
    }

    QString flowControlText;
    QSerialPort::FlowControl flowControl;
    if (!readRequiredText(properties, QStringLiteral("flowControl"), &flowControlText) ||
        !toFlowControl(flowControlText, &flowControl)) {
        return invalidSerialProperty(QStringLiteral("flowControl"),
                                     QStringLiteral("one of None, Hardware, or Software"));
    }

    auto port = std::make_unique<QSerialPort>();
    port->setPortName(portName);
    if (!port->setBaudRate(static_cast<qint32>(baudRate)) ||
        !port->setDataBits(dataBits) ||
        !port->setParity(parity) ||
        !port->setStopBits(stopBits) ||
        !port->setFlowControl(flowControl)) {
        return serialError(HalStatusCode::IoError,
                           QStringLiteral("control.qtSerial.open"),
                           *port);
    }
    if (!port->open(QIODevice::ReadWrite)) {
        return serialError(HalStatusCode::IoError,
                           QStringLiteral("control.qtSerial.open"),
                           *port);
    }

    m_port = std::move(port);
    return {};
}

HalStatus QtSerialControlProvider::close(const OperationOptions& options)
{
    Q_UNUSED(options)
    if (m_port != nullptr) {
        m_port->close();
        m_port.reset();
    }
    return {};
}

HalStatus QtSerialControlProvider::write(const QByteArray& data,
                                         const OperationOptions& options)
{
    if (m_port == nullptr || !m_port->isOpen()) {
        return makeError(HalStatusCode::InvalidState,
                         QStringLiteral("control.qtSerial.write"),
                         QStringLiteral("Serial control channel is not open"));
    }

    QElapsedTimer timer;
    timer.start();
    qint64 acceptedBytes = 0;
    const int timeoutMs = timeoutFor(options);
    while (acceptedBytes < data.size()) {
        const qint64 accepted = m_port->write(data.constData() + acceptedBytes,
                                              data.size() - acceptedBytes);
        if (accepted < 0) {
            return serialError(HalStatusCode::IoError,
                               QStringLiteral("control.qtSerial.write"),
                               *m_port);
        }
        acceptedBytes += accepted;

        while (m_port->bytesToWrite() > 0) {
            const int remaining = qMax(0, timeoutMs - static_cast<int>(timer.elapsed()));
            if (remaining == 0 || !m_port->waitForBytesWritten(remaining)) {
                const HalStatusCode code = m_port->error() == QSerialPort::TimeoutError ||
                        m_port->error() == QSerialPort::NoError
                    ? HalStatusCode::Timeout
                    : HalStatusCode::IoError;
                return serialError(code, QStringLiteral("control.qtSerial.write"), *m_port);
            }
        }

        if (accepted == 0 && timer.elapsed() >= timeoutMs) {
            return serialError(HalStatusCode::Timeout,
                               QStringLiteral("control.qtSerial.write"),
                               *m_port);
        }
    }
    return {};
}

HalResult<QByteArray> QtSerialControlProvider::read(int maxBytes,
                                                     const OperationOptions& options)
{
    HalResult<QByteArray> result;
    if (maxBytes <= 0) {
        result.status = makeError(HalStatusCode::InvalidArgument,
                                  QStringLiteral("control.qtSerial.read"),
                                  QStringLiteral("Maximum read size must be positive"));
        return result;
    }
    if (m_port == nullptr || !m_port->isOpen()) {
        result.status = makeError(HalStatusCode::InvalidState,
                                  QStringLiteral("control.qtSerial.read"),
                                  QStringLiteral("Serial control channel is not open"));
        return result;
    }
    if (m_port->bytesAvailable() == 0 && !m_port->waitForReadyRead(timeoutFor(options))) {
        const HalStatusCode code = m_port->error() == QSerialPort::TimeoutError ||
                m_port->error() == QSerialPort::NoError
            ? HalStatusCode::Timeout
            : HalStatusCode::IoError;
        result.status = serialError(code, QStringLiteral("control.qtSerial.read"), *m_port);
        return result;
    }

    result.value = m_port->read(maxBytes);
    if (result.value.isEmpty() && m_port->error() != QSerialPort::NoError) {
        result.status = serialError(HalStatusCode::IoError,
                                    QStringLiteral("control.qtSerial.read"),
                                    *m_port);
    }
    return result;
}

} // namespace hwtest::hal
