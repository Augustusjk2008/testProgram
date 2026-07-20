#pragma once

#include <algorithm/mbddf_protocol.h>
#include <algorithm/mbddf_transport.h>

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QUdpSocket>

#include <limits>

namespace hwtest::app::test {

class MbddfUdpTestPeer final {
public:
    bool bind(QString* error = nullptr)
    {
        clearError(error);
        m_request.clear();
        m_sender = QHostAddress();
        m_senderPort = 0;
        m_hasRequest = false;

        if (!m_socket.bind(QHostAddress(QHostAddress::LocalHost), 0)) {
            return fail(error, m_socket.errorString());
        }
        return true;
    }

    quint16 localPort() const
    {
        return m_socket.localPort();
    }

    bool writeHalConfig(const QString& sourceConfigPath,
                        QTemporaryDir* directory,
                        QString* configPath,
                        QString* error = nullptr) const
    {
        clearError(error);
        if (directory == nullptr || !directory->isValid()) {
            return fail(error, QStringLiteral("A valid temporary directory is required"));
        }
        if (configPath == nullptr) {
            return fail(error, QStringLiteral("An output HAL configuration path is required"));
        }

        QFile source(sourceConfigPath);
        if (!source.open(QIODevice::ReadOnly)) {
            return fail(error,
                        QStringLiteral("Cannot read HAL configuration %1: %2")
                            .arg(sourceConfigPath, source.errorString()));
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(source.readAll(), &parseError);
        if (!document.isObject()) {
            return fail(error,
                        QStringLiteral("HAL configuration %1 is not a JSON object: %2")
                            .arg(sourceConfigPath, parseError.errorString()));
        }

        QVariantMap root = document.object().toVariantMap();
        root.remove(QStringLiteral("logging"));

        QVariantMap control = root.value(QStringLiteral("control")).toMap();
        control.insert(QStringLiteral("resourceId"), QStringLiteral("CONTROL_NETWORK"));
        root.insert(QStringLiteral("control"), control);

        QVariantMap hardware = root.value(QStringLiteral("hardware")).toMap();
        QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
        QVariantMap network = resources.value(QStringLiteral("CONTROL_NETWORK")).toMap();
        QVariantMap properties = network.value(QStringLiteral("properties")).toMap();
        properties.insert(QStringLiteral("localAddress"), QStringLiteral("127.0.0.1"));
        properties.insert(QStringLiteral("localPort"), 0);
        properties.insert(QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1"));
        properties.insert(QStringLiteral("remotePort"), static_cast<int>(localPort()));
        network.insert(QStringLiteral("properties"), properties);
        resources.insert(QStringLiteral("CONTROL_NETWORK"), network);
        hardware.insert(QStringLiteral("resources"), resources);
        root.insert(QStringLiteral("hardware"), hardware);

        const QString path = directory->filePath(QStringLiteral("udp-hal.json"));
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return fail(error,
                        QStringLiteral("Cannot write HAL configuration %1: %2")
                            .arg(path, output.errorString()));
        }

        const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(root)).toJson();
        if (output.write(json) != json.size()) {
            return fail(error,
                        QStringLiteral("Cannot write HAL configuration %1: %2")
                            .arg(path, output.errorString()));
        }

        *configPath = path;
        return true;
    }

    bool waitForRequest(int timeoutMs, QString* error = nullptr)
    {
        clearError(error);
        m_request.clear();
        m_sender = QHostAddress();
        m_senderPort = 0;
        m_hasRequest = false;

        if (!m_socket.hasPendingDatagrams() && !m_socket.waitForReadyRead(timeoutMs)) {
            return fail(error, m_socket.errorString());
        }

        const qint64 size = m_socket.pendingDatagramSize();
        if (size <= 0) {
            return fail(error, QStringLiteral("UDP peer received an empty datagram"));
        }
        if (size > std::numeric_limits<int>::max()) {
            return fail(error, QStringLiteral("UDP peer received an oversized datagram"));
        }

        m_request.resize(static_cast<int>(size));
        const qint64 received = m_socket.readDatagram(m_request.data(),
                                                      m_request.size(),
                                                      &m_sender,
                                                      &m_senderPort);
        if (received != size) {
            m_request.clear();
            return fail(error, m_socket.errorString());
        }

        m_hasRequest = true;
        return true;
    }

    bool replyToLastRequest(QString* error = nullptr)
    {
        clearError(error);
        if (!m_hasRequest) {
            return fail(error, QStringLiteral("UDP peer has not received a request"));
        }

        hwtest::algorithm::mbddf::ProtocolCatalog catalog;
        if (!catalog.loadFromDirectory(qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR"), error)) {
            return false;
        }

        hwtest::algorithm::mbddf::SystemStatusSimulator simulator(&catalog);
        simulator.setResponseValues({
            {QStringLiteral("status"), 0},
            {QStringLiteral("err_code"), 0},
            {QStringLiteral("cpu_usage"), 12.5},
            {QStringLiteral("mem_usage"), 25.0},
            {QStringLiteral("power_on_sec"), 99u},
        });
        if (!simulator.open(error)) {
            return false;
        }

        const hwtest::algorithm::mbddf::TransportResult result =
            simulator.transact(m_request, 1000);
        simulator.close();
        if (!result.ok) {
            return fail(error, result.error);
        }
        if (result.frame == m_request) {
            return fail(error,
                        QStringLiteral("SYSTEM_STATUS response unexpectedly matches its request"));
        }

        const qint64 written = m_socket.writeDatagram(result.frame, m_sender, m_senderPort);
        if (written != result.frame.size()) {
            return fail(error, m_socket.errorString());
        }
        return true;
    }

private:
    static void clearError(QString* error)
    {
        if (error != nullptr) {
            error->clear();
        }
    }

    static bool fail(QString* error, const QString& message)
    {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    }

    QUdpSocket m_socket;
    QByteArray m_request;
    QHostAddress m_sender;
    quint16 m_senderPort = 0;
    bool m_hasRequest = false;
};

} // namespace hwtest::app::test
