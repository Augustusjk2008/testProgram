#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>

namespace hwtest::algorithm::mbddf {

enum class Direction {
    Request,
    Response
};

enum class FieldType {
    Bit,
    Const,
    F32,
    Reserved,
    S16,
    S16F,
    S32F,
    U8,
    U16,
    U32
};

// A field uses B1-based coordinates from the CSV. Payload coordinates begin at B4.
struct ProtocolField {
    QString index;
    int startByte = 0;
    int endByte = 0;
    int declaredLength = 0;
    int byteLength = 0;
    FieldType type = FieldType::U8;
    QString nameCn;
    QString nameEn;
    double lsb = 1.0;
    bool hasLsb = false;
    QVariant defaultValue;
    bool isValid = false;
    int bitOffset = 0;
    int bitLength = 0;

    bool isPayloadField(int payloadLength) const noexcept;
    int payloadOffset() const noexcept;
};

struct MessageDefinition {
    QString name;
    Direction direction = Direction::Request;
    QString sourcePath;
    QVector<ProtocolField> fields;
    int payloadLength = 0;
    quint8 typeGroup = 0;
    quint8 subType = 0;

    const ProtocolField* findField(const QString& name) const noexcept;
};

// A catalog owns validated CSV definitions. It has no transport or execution behavior.
class ProtocolCatalog {
public:
    bool loadFromDirectory(const QString& directory, QString* error = nullptr);

    QString directory() const;
    int size() const noexcept;
    const QVector<MessageDefinition>& messages() const noexcept;
    const MessageDefinition* findByName(const QString& name) const noexcept;
    const MessageDefinition* findByCommand(quint8 typeGroup,
                                           quint8 subType,
                                           Direction direction) const noexcept;

private:
    QString m_directory;
    QVector<MessageDefinition> m_messages;
    QHash<QString, int> m_nameIndex;
    QHash<quint32, int> m_commandIndex;
};

quint16 crc16Xmodem(const QByteArray& data) noexcept;

// Encodes and decodes only the B4..payload-end data segment. The CRC is not part of it.
bool encodePayload(const MessageDefinition& definition,
                   const QVariantMap& values,
                   quint16 sequence,
                   QByteArray* payload,
                   QString* error = nullptr);
bool decodePayload(const MessageDefinition& definition,
                   const QByteArray& payload,
                   QVariantMap* values,
                   QString* error = nullptr);

// Encodes and decodes the physical 55 AA + LEN + payload + CRC_LO CRC_HI envelope.
bool encodeFrame(const QByteArray& payload, QByteArray* frame, QString* error = nullptr);
bool decodeFrame(const QByteArray& frame, QByteArray* payload, QString* error = nullptr);

} // namespace hwtest::algorithm::mbddf
