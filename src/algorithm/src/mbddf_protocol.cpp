#include <algorithm/mbddf_protocol.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>
#include <cstring>
#include <limits>

namespace hwtest::algorithm::mbddf {
namespace {

constexpr int kShortPayloadLength = 48;
constexpr int kLongPayloadLength = 123;
constexpr int kFrameHeaderLength = 3;
constexpr int kFrameCrcLength = 2;
constexpr uchar kSync0 = 0x55;
constexpr uchar kSync1 = 0xAA;
constexpr quint8 kVersion = 0x11;

struct CsvRow {
    QStringList cells;
    int lineNumber = 0;
};

const QStringList& expectedColumns()
{
    static const QStringList columns = {
        QStringLiteral("index"),
        QStringLiteral("length"),
        QStringLiteral("type"),
        QStringLiteral("name_cn"),
        QStringLiteral("name_en"),
        QStringLiteral("lsb"),
        QStringLiteral("default"),
        QStringLiteral("is_valid")};
    return columns;
}

bool fail(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

QString fieldContext(const QString& fileName, int lineNumber)
{
    return QStringLiteral("%1:%2").arg(fileName).arg(lineNumber);
}

bool appendCsvField(const QByteArray& bytes, QStringList* cells, QString* error)
{
    const QString value = QString::fromUtf8(bytes);
    if (value.contains(QChar::ReplacementCharacter)) {
        return fail(error, QStringLiteral("CSV is not valid UTF-8"));
    }
    cells->append(value.trimmed());
    return true;
}

bool parseCsv(const QByteArray& source, QVector<CsvRow>* rows, QString* error)
{
    QByteArray bytes = source;
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        bytes.remove(0, 3);
    }

    QVector<CsvRow> parsed;
    QByteArray field;
    QStringList cells;
    bool inQuotes = false;
    bool quoteClosed = false;
    bool rowHasContent = false;
    int lineNumber = 1;

    const auto finishField = [&]() -> bool {
        if (!appendCsvField(field, &cells, error)) {
            return false;
        }
        field.clear();
        quoteClosed = false;
        return true;
    };
    const auto finishRow = [&]() -> bool {
        if (!rowHasContent) {
            return fail(error, QStringLiteral("CSV contains an empty row at line %1").arg(lineNumber));
        }
        if (!finishField()) {
            return false;
        }
        parsed.append({cells, lineNumber});
        cells.clear();
        rowHasContent = false;
        return true;
    };

    for (int index = 0; index < bytes.size(); ++index) {
        const char character = bytes.at(index);
        if (inQuotes) {
            if (character == '"') {
                if (index + 1 < bytes.size() && bytes.at(index + 1) == '"') {
                    field.append('"');
                    ++index;
                } else {
                    inQuotes = false;
                    quoteClosed = true;
                }
            } else {
                field.append(character);
            }
            continue;
        }

        if (character == '"') {
            if (!field.isEmpty() || quoteClosed) {
                return fail(error,
                            QStringLiteral("CSV quote position is invalid at line %1").arg(lineNumber));
            }
            inQuotes = true;
            rowHasContent = true;
            continue;
        }
        if (quoteClosed && character != ',' && character != '\r' && character != '\n') {
            return fail(error, QStringLiteral("CSV quote is not followed by a separator at line %1").arg(lineNumber));
        }
        if (character == ',') {
            rowHasContent = true;
            if (!finishField()) {
                return false;
            }
            continue;
        }
        if (character == '\r' || character == '\n') {
            if (character == '\r' && index + 1 < bytes.size() && bytes.at(index + 1) == '\n') {
                ++index;
            }
            if (!finishRow()) {
                return false;
            }
            ++lineNumber;
            continue;
        }
        field.append(character);
        rowHasContent = true;
    }

    if (inQuotes) {
        return fail(error, QStringLiteral("CSV has an unterminated quote at line %1").arg(lineNumber));
    }
    if (rowHasContent) {
        if (!finishRow()) {
            return false;
        }
    }
    if (parsed.isEmpty()) {
        return fail(error, QStringLiteral("CSV must not be empty"));
    }
    *rows = parsed;
    return true;
}

bool parseFieldType(const QString& text, FieldType* type)
{
    if (text == QStringLiteral("BIT")) {
        *type = FieldType::Bit;
    } else if (text == QStringLiteral("CONST")) {
        *type = FieldType::Const;
    } else if (text == QStringLiteral("F32")) {
        *type = FieldType::F32;
    } else if (text == QStringLiteral("RESERVED")) {
        *type = FieldType::Reserved;
    } else if (text == QStringLiteral("S16")) {
        *type = FieldType::S16;
    } else if (text == QStringLiteral("S16F")) {
        *type = FieldType::S16F;
    } else if (text == QStringLiteral("S32F")) {
        *type = FieldType::S32F;
    } else if (text == QStringLiteral("U8")) {
        *type = FieldType::U8;
    } else if (text == QStringLiteral("U16")) {
        *type = FieldType::U16;
    } else if (text == QStringLiteral("U32")) {
        *type = FieldType::U32;
    } else {
        return false;
    }
    return true;
}

int fixedByteLength(FieldType type)
{
    switch (type) {
    case FieldType::U8:
        return 1;
    case FieldType::U16:
    case FieldType::S16:
    case FieldType::S16F:
        return 2;
    case FieldType::U32:
    case FieldType::S32F:
    case FieldType::F32:
        return 4;
    case FieldType::Bit:
    case FieldType::Const:
    case FieldType::Reserved:
        return 0;
    }
    return 0;
}

bool parseUnsigned(const QString& text, quint64* value)
{
    bool ok = false;
    const qulonglong parsed = text.toULongLong(&ok, 0);
    if (!ok) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parseFiniteDouble(const QString& text, double* value)
{
    bool ok = false;
    const double parsed = text.toDouble(&ok);
    if (!ok || !std::isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parseDefaultValue(const QString& text, QVariant* value)
{
    if (text.isEmpty()) {
        *value = QVariant();
        return true;
    }

    quint64 integer = 0;
    if (parseUnsigned(text, &integer)) {
        *value = QVariant::fromValue<qulonglong>(integer);
        return true;
    }

    double number = 0.0;
    if (!parseFiniteDouble(text, &number)) {
        return false;
    }
    *value = QVariant(number);
    return true;
}

quint64 maxUnsignedForBits(int bitCount)
{
    if (bitCount >= 64) {
        return std::numeric_limits<quint64>::max();
    }
    return (quint64(1) << bitCount) - 1U;
}

bool variantToFiniteDouble(const QVariant& value, double* result)
{
    bool ok = false;
    const double converted = value.toDouble(&ok);
    if (!ok || !std::isfinite(converted)) {
        return false;
    }
    *result = converted;
    return true;
}

bool variantToUnsignedInteger(const QVariant& value, quint64* result)
{
    double number = 0.0;
    if (!variantToFiniteDouble(value, &number) || number < 0.0 || std::floor(number) != number ||
        number > static_cast<double>(std::numeric_limits<quint64>::max())) {
        return false;
    }
    *result = static_cast<quint64>(number);
    return true;
}

bool defaultAsUnsigned(const ProtocolField& field, quint64* value)
{
    return field.defaultValue.isValid() && variantToUnsignedInteger(field.defaultValue, value);
}

bool parseIndex(const QString& text, int* startByte, int* endByte)
{
    static const QRegularExpression pattern(
        QStringLiteral("^B([1-9][0-9]*)(?:-([1-9][0-9]*))?$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) {
        return false;
    }
    bool startOk = false;
    bool endOk = false;
    const int start = match.captured(1).toInt(&startOk);
    const int end = match.captured(2).isEmpty()
        ? start
        : match.captured(2).toInt(&endOk);
    if (!startOk || (!match.captured(2).isEmpty() && !endOk) || end < start) {
        return false;
    }
    *startByte = start;
    *endByte = end;
    return true;
}

bool parseField(const QString& fileName,
                const CsvRow& row,
                ProtocolField* field,
                QString* error)
{
    if (row.cells.size() != expectedColumns().size()) {
        return fail(error,
                    QStringLiteral("%1: column count must be %2, actual %3")
                        .arg(fieldContext(fileName, row.lineNumber))
                        .arg(expectedColumns().size())
                        .arg(row.cells.size()));
    }

    ProtocolField parsed;
    parsed.index = row.cells.at(0);
    if (!parseIndex(parsed.index, &parsed.startByte, &parsed.endByte)) {
        return fail(error,
                    QStringLiteral("%1: invalid index: %2")
                        .arg(fieldContext(fileName, row.lineNumber), parsed.index));
    }

    bool lengthOk = false;
    parsed.declaredLength = row.cells.at(1).toInt(&lengthOk);
    if (!lengthOk || parsed.declaredLength <= 0) {
        return fail(error,
                    QStringLiteral("%1: length must be a positive integer")
                        .arg(fieldContext(fileName, row.lineNumber)));
    }

    if (!parseFieldType(row.cells.at(2), &parsed.type)) {
        return fail(error,
                    QStringLiteral("%1: unsupported field type %2")
                        .arg(fieldContext(fileName, row.lineNumber), row.cells.at(2)));
    }

    parsed.nameCn = row.cells.at(3);
    parsed.nameEn = row.cells.at(4);
    if (parsed.nameCn.isEmpty() || parsed.nameEn.isEmpty()) {
        return fail(error,
                    QStringLiteral("%1: name_cn and name_en must not be empty")
                        .arg(fieldContext(fileName, row.lineNumber)));
    }

    const QString lsbText = row.cells.at(5);
    if (!lsbText.isEmpty()) {
        if (!parseFiniteDouble(lsbText, &parsed.lsb) || parsed.lsb <= 0.0) {
            return fail(error,
                        QStringLiteral("%1: lsb must be finite and greater than zero")
                            .arg(fieldContext(fileName, row.lineNumber)));
        }
        parsed.hasLsb = true;
    }

    const QString defaultText = row.cells.at(6);
    if (parsed.type == FieldType::Const) {
        quint64 defaultValue = 0;
        if (!parseUnsigned(defaultText, &defaultValue)) {
            return fail(error,
                        QStringLiteral("%1: CONST default must be an unsigned integer literal")
                            .arg(fieldContext(fileName, row.lineNumber)));
        }
        parsed.defaultValue = QVariant::fromValue<qulonglong>(defaultValue);
    } else if (parsed.type == FieldType::Reserved) {
        if (!defaultText.isEmpty() && defaultText != QStringLiteral("0") &&
            defaultText != QStringLiteral("0x0") && defaultText != QStringLiteral("0X0")) {
            return fail(error,
                        QStringLiteral("%1: RESERVED default must be zero")
                            .arg(fieldContext(fileName, row.lineNumber)));
        }
        parsed.defaultValue = QVariant::fromValue<qulonglong>(0);
    } else {
        const bool commonIntegerDefault = parsed.nameEn == QStringLiteral("len") ||
            parsed.nameEn == QStringLiteral("type_group") ||
            parsed.nameEn == QStringLiteral("sub_type");
        if (commonIntegerDefault) {
            quint64 defaultValue = 0;
            if (!parseUnsigned(defaultText, &defaultValue)) {
                return fail(error,
                            QStringLiteral("%1: %2 default must be an unsigned integer literal")
                                .arg(fieldContext(fileName, row.lineNumber), parsed.nameEn));
            }
            parsed.defaultValue = QVariant::fromValue<qulonglong>(defaultValue);
        } else if (!parseDefaultValue(defaultText, &parsed.defaultValue)) {
            return fail(error,
                        QStringLiteral("%1: default must be a finite number")
                            .arg(fieldContext(fileName, row.lineNumber)));
        }
    }

    if ((parsed.nameEn == QStringLiteral("sync[0]") && defaultText != QStringLiteral("0x55")) ||
        (parsed.nameEn == QStringLiteral("sync[1]") && defaultText != QStringLiteral("0xAA")) ||
        (parsed.nameEn == QStringLiteral("version") && defaultText != QStringLiteral("0x11"))) {
        return fail(error,
                    QStringLiteral("%1: MB_DDF sync and version defaults must use canonical literals")
                        .arg(fieldContext(fileName, row.lineNumber)));
    }

    const QString validText = row.cells.at(7);
    if (validText == QStringLiteral("1")) {
        parsed.isValid = true;
    } else if (validText == QStringLiteral("0")) {
        parsed.isValid = false;
    } else {
        return fail(error,
                    QStringLiteral("%1: is_valid must be 0 or 1")
                        .arg(fieldContext(fileName, row.lineNumber)));
    }

    if (parsed.type == FieldType::Bit) {
        if (parsed.startByte != parsed.endByte || parsed.declaredLength > 8) {
            return fail(error,
                        QStringLiteral("%1: BIT must use one byte and have width 1..8")
                            .arg(fieldContext(fileName, row.lineNumber)));
        }
        parsed.byteLength = 1;
        parsed.bitLength = parsed.declaredLength;
    } else {
        const int span = parsed.endByte - parsed.startByte + 1;
        if (span != parsed.declaredLength) {
            return fail(error,
                        QStringLiteral("%1: index and length are inconsistent")
                            .arg(fieldContext(fileName, row.lineNumber)));
        }
        parsed.byteLength = parsed.declaredLength;
    }

    const int fixedLength = fixedByteLength(parsed.type);
    if (fixedLength != 0 && parsed.byteLength != fixedLength) {
        return fail(error,
                    QStringLiteral("%1: fixed-width field length must be %2")
                        .arg(fieldContext(fileName, row.lineNumber))
                        .arg(fixedLength));
    }

    if (parsed.type == FieldType::Const) {
        quint64 defaultValue = 0;
        if (!defaultAsUnsigned(parsed, &defaultValue) || parsed.byteLength > 8 ||
            defaultValue > maxUnsignedForBits(parsed.byteLength * 8)) {
            return fail(error,
                        QStringLiteral("%1: CONST requires an unsigned default fitting the field width")
                            .arg(fieldContext(fileName, row.lineNumber)));
        }
    }
    if (parsed.type == FieldType::Reserved) {
        quint64 defaultValue = 0;
        if (!defaultAsUnsigned(parsed, &defaultValue) || defaultValue != 0) {
            return fail(error,
                        QStringLiteral("%1: RESERVED default must be zero")
                            .arg(fieldContext(fileName, row.lineNumber)));
        }
    }

    *field = parsed;
    return true;
}

bool requireCommonField(const QString& fileName,
                        const QVector<ProtocolField>& fields,
                        int position,
                        const QString& name,
                        const QString& index,
                        FieldType type,
                        QString* error)
{
    if (position >= fields.size()) {
        return fail(error, QStringLiteral("%1: missing common field %2").arg(fileName, name));
    }
    const ProtocolField& field = fields.at(position);
    if (field.nameEn != name || field.index != index || field.type != type) {
        return fail(error,
                    QStringLiteral("%1: common field must be %2 %3")
                        .arg(fileName, index, name));
    }
    return true;
}

bool validateCommonFields(const QString& fileName,
                          const QVector<ProtocolField>& fields,
                          int* payloadLength,
                          quint8* typeGroup,
                          quint8* subType,
                          QString* error)
{
    if (!requireCommonField(fileName, fields, 0, QStringLiteral("sync[0]"), QStringLiteral("B1"),
                            FieldType::Const, error) ||
        !requireCommonField(fileName, fields, 1, QStringLiteral("sync[1]"), QStringLiteral("B2"),
                            FieldType::Const, error) ||
        !requireCommonField(fileName, fields, 2, QStringLiteral("len"), QStringLiteral("B3"),
                            FieldType::U8, error) ||
        !requireCommonField(fileName, fields, 3, QStringLiteral("version"), QStringLiteral("B4"),
                            FieldType::Const, error) ||
        !requireCommonField(fileName, fields, 4, QStringLiteral("type_group"), QStringLiteral("B5"),
                            FieldType::U8, error) ||
        !requireCommonField(fileName, fields, 5, QStringLiteral("sub_type"), QStringLiteral("B6"),
                            FieldType::U8, error) ||
        !requireCommonField(fileName, fields, 6, QStringLiteral("seq"), QStringLiteral("B7-8"),
                            FieldType::U16, error)) {
        return false;
    }

    quint64 value = 0;
    if (!defaultAsUnsigned(fields.at(0), &value) || value != kSync0 ||
        !defaultAsUnsigned(fields.at(1), &value) || value != kSync1 ||
        !defaultAsUnsigned(fields.at(3), &value) || value != kVersion) {
        return fail(error, QStringLiteral("%1: sync or version constant violates MB_DDF protocol").arg(fileName));
    }

    quint64 length = 0;
    if (!defaultAsUnsigned(fields.at(2), &length) ||
        (length != kShortPayloadLength && length != kLongPayloadLength)) {
        return fail(error, QStringLiteral("%1: len must be 48 or 123").arg(fileName));
    }
    quint64 type = 0;
    quint64 sub = 0;
    if (!defaultAsUnsigned(fields.at(4), &type) || type > 0xFF ||
        !defaultAsUnsigned(fields.at(5), &sub) || sub > 0xFF) {
        return fail(error, QStringLiteral("%1: type_group/sub_type must have U8 defaults").arg(fileName));
    }

    *payloadLength = static_cast<int>(length);
    *typeGroup = static_cast<quint8>(type);
    *subType = static_cast<quint8>(sub);
    return true;
}

bool validateLayout(const QString& fileName,
                    QVector<ProtocolField>* fields,
                    int fullFrameLength,
                    QString* error)
{
    int expectedByte = 1;
    int bitByte = 0;
    int bitWidth = 0;

    const auto finishBitGroup = [&]() -> bool {
        if (bitByte == 0) {
            return true;
        }
        if (bitWidth != 8) {
            return fail(error,
                        QStringLiteral("%1: BIT fields at B%2 must cover exactly 8 bits")
                            .arg(fileName)
                            .arg(bitByte));
        }
        ++expectedByte;
        bitByte = 0;
        bitWidth = 0;
        return true;
    };

    for (ProtocolField& field : *fields) {
        if (field.type == FieldType::Bit) {
            if (bitByte == 0) {
                if (field.startByte != expectedByte) {
                    return fail(error,
                                QStringLiteral("%1: field %2 has a non-contiguous index")
                                    .arg(fileName, field.nameEn));
                }
                bitByte = field.startByte;
            } else if (field.startByte != bitByte) {
                if (!finishBitGroup()) {
                    return false;
                }
                if (field.startByte != expectedByte) {
                    return fail(error,
                                QStringLiteral("%1: BIT field %2 has a non-contiguous index")
                                    .arg(fileName, field.nameEn));
                }
                bitByte = field.startByte;
            }
            if (bitWidth + field.bitLength > 8) {
                return fail(error,
                            QStringLiteral("%1: BIT fields at B%2 exceed 8 bits")
                                .arg(fileName)
                                .arg(field.startByte));
            }
            field.bitOffset = bitWidth;
            bitWidth += field.bitLength;
            continue;
        }

        if (!finishBitGroup()) {
            return false;
        }
        if (field.startByte != expectedByte) {
            return fail(error,
                        QStringLiteral("%1: field %2 has a non-contiguous index")
                            .arg(fileName, field.nameEn));
        }
        expectedByte = field.endByte + 1;
    }

    if (!finishBitGroup()) {
        return false;
    }
    if (expectedByte != fullFrameLength + 1) {
        return fail(error,
                    QStringLiteral("%1: fields do not cover through frame byte B%2")
                        .arg(fileName)
                        .arg(fullFrameLength));
    }
    return true;
}

bool isKnownFieldType(FieldType type)
{
    switch (type) {
    case FieldType::Bit:
    case FieldType::Const:
    case FieldType::F32:
    case FieldType::Reserved:
    case FieldType::S16:
    case FieldType::S16F:
    case FieldType::S32F:
    case FieldType::U8:
    case FieldType::U16:
    case FieldType::U32:
        return true;
    }
    return false;
}

bool validateDefinitionForCodec(const MessageDefinition& definition, QString* error)
{
    if (definition.payloadLength != kShortPayloadLength &&
        definition.payloadLength != kLongPayloadLength) {
        return fail(error, QStringLiteral("product payload length must be 48 or 123"));
    }
    if (definition.fields.isEmpty()) {
        return fail(error, QStringLiteral("message definition has no fields"));
    }

    QHash<QString, bool> names;
    for (const ProtocolField& field : definition.fields) {
        if (!isKnownFieldType(field.type) || field.nameEn.isEmpty() || field.startByte <= 0 ||
            field.endByte < field.startByte || field.declaredLength <= 0 || field.byteLength <= 0 ||
            !std::isfinite(field.lsb) || field.lsb <= 0.0 || names.contains(field.nameEn)) {
            return fail(error, QStringLiteral("message definition has an invalid field"));
        }
        names.insert(field.nameEn, true);
        if (field.type == FieldType::Bit) {
            if (field.startByte != field.endByte || field.byteLength != 1 ||
                field.declaredLength > 8 || field.bitLength != field.declaredLength) {
                return fail(error, QStringLiteral("message definition has an invalid BIT field"));
            }
            continue;
        }
        if (field.endByte - field.startByte + 1 != field.declaredLength ||
            field.byteLength != field.declaredLength) {
            return fail(error, QStringLiteral("message definition has inconsistent field length"));
        }
        const int fixedLength = fixedByteLength(field.type);
        if ((fixedLength != 0 && field.byteLength != fixedLength) ||
            (field.type == FieldType::Const && field.byteLength > 8)) {
            return fail(error, QStringLiteral("message definition has an invalid fixed-width field"));
        }
        if (field.type == FieldType::Const) {
            quint64 value = 0;
            if (!defaultAsUnsigned(field, &value) ||
                value > maxUnsignedForBits(field.byteLength * 8)) {
                return fail(error, QStringLiteral("message definition has an invalid CONST default"));
            }
        }
        if (field.type == FieldType::Reserved) {
            quint64 value = 0;
            if (!defaultAsUnsigned(field, &value) || value != 0) {
                return fail(error, QStringLiteral("message definition has a nonzero RESERVED default"));
            }
        }
    }

    QVector<ProtocolField> normalized = definition.fields;
    int payloadLength = 0;
    quint8 typeGroup = 0;
    quint8 subType = 0;
    const QString name = definition.name.isEmpty() ? QStringLiteral("message") : definition.name;
    if (!validateCommonFields(name, normalized, &payloadLength, &typeGroup, &subType, error) ||
        payloadLength != definition.payloadLength || typeGroup != definition.typeGroup ||
        subType != definition.subType ||
        !validateLayout(name, &normalized, payloadLength + kFrameHeaderLength + kFrameCrcLength, error)) {
        return false;
    }

    const ProtocolField& crc = normalized.constLast();
    if (crc.nameEn != QStringLiteral("crc") || crc.type != FieldType::U16 ||
        crc.startByte != payloadLength + 4 || crc.endByte != payloadLength + 5) {
        return fail(error, QStringLiteral("message definition has an invalid CRC field"));
    }
    for (int index = 0; index < normalized.size(); ++index) {
        if (normalized.at(index).type == FieldType::Bit &&
            (normalized.at(index).bitOffset != definition.fields.at(index).bitOffset ||
             normalized.at(index).bitLength != definition.fields.at(index).bitLength)) {
            return fail(error, QStringLiteral("message definition has inconsistent BIT offsets"));
        }
    }
    return true;
}

bool loadDefinition(const QFileInfo& fileInfo, MessageDefinition* definition, QString* error)
{
    const QString fileName = fileInfo.fileName();
    Direction direction = Direction::Request;
    if (fileName.endsWith(QStringLiteral("_request.csv"))) {
        direction = Direction::Request;
    } else if (fileName.endsWith(QStringLiteral("_response.csv"))) {
        direction = Direction::Response;
    } else {
        return fail(error, QStringLiteral("protocol filename must end in _request.csv or _response.csv: %1")
                               .arg(fileName));
    }

    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("cannot read protocol CSV: %1").arg(fileInfo.absoluteFilePath()));
    }
    QVector<CsvRow> rows;
    QString csvError;
    if (!parseCsv(file.readAll(), &rows, &csvError)) {
        return fail(error, QStringLiteral("%1: %2").arg(fileName, csvError));
    }
    if (rows.first().cells != expectedColumns()) {
        return fail(error,
                    QStringLiteral("%1: header must exactly equal %2")
                        .arg(fileName, expectedColumns().join(QStringLiteral(","))));
    }
    if (rows.size() == 1) {
        return fail(error, QStringLiteral("%1: CSV must contain at least one field").arg(fileName));
    }

    QVector<ProtocolField> fields;
    fields.reserve(rows.size() - 1);
    QHash<QString, bool> names;
    for (int rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
        ProtocolField field;
        if (!parseField(fileName, rows.at(rowIndex), &field, error)) {
            return false;
        }
        if (names.contains(field.nameEn)) {
            return fail(error, QStringLiteral("%1: duplicate name_en: %2").arg(fileName, field.nameEn));
        }
        names.insert(field.nameEn, true);
        fields.append(field);
    }

    int payloadLength = 0;
    quint8 typeGroup = 0;
    quint8 subType = 0;
    if (!validateCommonFields(fileName, fields, &payloadLength, &typeGroup, &subType, error)) {
        return false;
    }
    const int fullFrameLength = payloadLength + kFrameHeaderLength + kFrameCrcLength;
    if (!validateLayout(fileName, &fields, fullFrameLength, error)) {
        return false;
    }

    const ProtocolField& crc = fields.constLast();
    if (crc.nameEn != QStringLiteral("crc") || crc.type != FieldType::U16 ||
        crc.startByte != payloadLength + 4 || crc.endByte != payloadLength + 5) {
        return fail(error,
                    QStringLiteral("%1: crc must be the U16 field at frame tail B%2-%3")
                        .arg(fileName)
                        .arg(payloadLength + 4)
                        .arg(payloadLength + 5));
    }

    MessageDefinition loaded;
    loaded.name = fileName.left(fileName.size() - 4);
    loaded.direction = direction;
    loaded.sourcePath = fileInfo.absoluteFilePath();
    loaded.fields = fields;
    loaded.payloadLength = payloadLength;
    loaded.typeGroup = typeGroup;
    loaded.subType = subType;
    *definition = loaded;
    return true;
}

quint32 commandKey(quint8 typeGroup, quint8 subType, Direction direction)
{
    const quint32 directionBit = direction == Direction::Response ? 1U : 0U;
    return (directionBit << 16U) | (static_cast<quint32>(typeGroup) << 8U) |
        static_cast<quint32>(subType);
}

bool isProtectedPayloadField(const ProtocolField& field)
{
    return field.type == FieldType::Const || field.nameEn == QStringLiteral("type_group") ||
        field.nameEn == QStringLiteral("sub_type") || field.nameEn == QStringLiteral("seq");
}

bool fieldExpectedValue(const MessageDefinition& definition,
                        const ProtocolField& field,
                        quint16 sequence,
                        quint64* value)
{
    if (field.nameEn == QStringLiteral("seq")) {
        *value = sequence;
        return true;
    }
    if (field.nameEn == QStringLiteral("type_group")) {
        *value = definition.typeGroup;
        return true;
    }
    if (field.nameEn == QStringLiteral("sub_type")) {
        *value = definition.subType;
        return true;
    }
    return defaultAsUnsigned(field, value);
}

bool validateSuppliedValues(const MessageDefinition& definition,
                            const QVariantMap& values,
                            quint16 sequence,
                            QString* error)
{
    for (auto iterator = values.constBegin(); iterator != values.constEnd(); ++iterator) {
        const ProtocolField* field = definition.findField(iterator.key());
        if (field == nullptr || !field->isPayloadField(definition.payloadLength)) {
            return fail(error, QStringLiteral("%1: is not a valid payload field").arg(iterator.key()));
        }
        if (field->type == FieldType::Reserved) {
            quint64 value = 0;
            if (!variantToUnsignedInteger(iterator.value(), &value) || value != 0) {
                return fail(error, QStringLiteral("%1: RESERVED field must be zero").arg(iterator.key()));
            }
            continue;
        }
        if (!field->isValid && !isProtectedPayloadField(*field)) {
            return fail(error, QStringLiteral("%1: is_valid=0 field cannot be encoded").arg(iterator.key()));
        }
        if (isProtectedPayloadField(*field)) {
            quint64 supplied = 0;
            quint64 expected = 0;
            if (!variantToUnsignedInteger(iterator.value(), &supplied) ||
                !fieldExpectedValue(definition, *field, sequence, &expected) || supplied != expected) {
                return fail(error, QStringLiteral("%1: cannot override a protocol constant or sequence").arg(iterator.key()));
            }
        }
    }
    return true;
}

bool writeLittleEndian(QByteArray* bytes, int offset, int length, quint64 value, QString* error)
{
    if (offset < 0 || length <= 0 || length > 8 || offset + length > bytes->size()) {
        return fail(error, QStringLiteral("field write is out of bounds"));
    }
    for (int index = 0; index < length; ++index) {
        bytes->operator[](offset + index) = static_cast<char>((value >> (index * 8)) & 0xFFU);
    }
    return true;
}

bool readLittleEndian(const QByteArray& bytes, int offset, int length, quint64* value, QString* error)
{
    if (offset < 0 || length <= 0 || length > 8 || offset + length > bytes.size()) {
        return fail(error, QStringLiteral("field read is out of bounds"));
    }
    quint64 result = 0;
    for (int index = 0; index < length; ++index) {
        result |= static_cast<quint64>(static_cast<uchar>(bytes.at(offset + index))) << (index * 8);
    }
    *value = result;
    return true;
}

qint64 signExtend(quint64 raw, int bitCount)
{
    if (bitCount >= 64) {
        return static_cast<qint64>(raw);
    }
    const quint64 signBit = quint64(1) << (bitCount - 1);
    if ((raw & signBit) != 0U) {
        raw |= ~maxUnsignedForBits(bitCount);
    }
    return static_cast<qint64>(raw);
}

bool scaledIntegerRaw(const ProtocolField& field,
                      const QVariant& value,
                      bool signedValue,
                      int bitCount,
                      quint64* raw,
                      QString* error)
{
    double semantic = 0.0;
    if (!variantToFiniteDouble(value, &semantic)) {
        return fail(error, QStringLiteral("field %1 is not a valid number").arg(field.nameEn));
    }
    const double scaled = semantic / field.lsb;
    if (!std::isfinite(scaled)) {
        return fail(error, QStringLiteral("field %1 is not finite after scaling").arg(field.nameEn));
    }
    const double rounded = std::round(scaled);
    if (signedValue) {
        const double minimum = -std::ldexp(1.0, bitCount - 1);
        const double maximum = std::ldexp(1.0, bitCount - 1) - 1.0;
        if (rounded < minimum || rounded > maximum) {
            return fail(error, QStringLiteral("field %1 exceeds the signed integer range").arg(field.nameEn));
        }
        *raw = static_cast<quint64>(static_cast<qint64>(rounded)) & maxUnsignedForBits(bitCount);
    } else {
        const double maximum = static_cast<double>(maxUnsignedForBits(bitCount));
        if (rounded < 0.0 || rounded > maximum) {
            return fail(error, QStringLiteral("field %1 exceeds the unsigned integer range").arg(field.nameEn));
        }
        *raw = static_cast<quint64>(rounded);
    }
    return true;
}

bool encodeField(const MessageDefinition& definition,
                 const ProtocolField& field,
                 const QVariantMap& values,
                 quint16 sequence,
                 QByteArray* payload,
                 QString* error)
{
    const int offset = field.payloadOffset();
    if (field.type == FieldType::Reserved) {
        return true;
    }

    QVariant value;
    if (isProtectedPayloadField(field)) {
        quint64 expected = 0;
        if (!fieldExpectedValue(definition, field, sequence, &expected)) {
            return fail(error, QStringLiteral("field %1 lacks a protocol default").arg(field.nameEn));
        }
        value = QVariant::fromValue<qulonglong>(expected);
    } else if (!field.isValid) {
        value = field.defaultValue.isValid() ? field.defaultValue : QVariant(0);
    } else if (values.contains(field.nameEn)) {
        value = values.value(field.nameEn);
    } else {
        value = field.defaultValue.isValid() ? field.defaultValue : QVariant(0);
    }

    if (field.type == FieldType::Bit) {
        quint64 raw = 0;
        if (!scaledIntegerRaw(field, value, false, field.bitLength, &raw, error)) {
            return false;
        }
        if (offset < 0 || offset >= payload->size()) {
            return fail(error, QStringLiteral("field %1 has an out-of-range BIT offset").arg(field.nameEn));
        }
        const quint8 mask = static_cast<quint8>(maxUnsignedForBits(field.bitLength) << field.bitOffset);
        const quint8 previous = static_cast<uchar>(payload->at(offset));
        const quint8 next = static_cast<quint8>((previous & ~mask) |
                                                ((static_cast<quint8>(raw) << field.bitOffset) & mask));
        payload->operator[](offset) = static_cast<char>(next);
        return true;
    }

    if (field.type == FieldType::F32) {
        double semantic = 0.0;
        if (!variantToFiniteDouble(value, &semantic)) {
            return fail(error, QStringLiteral("field %1 is not a valid F32").arg(field.nameEn));
        }
        const double scaled = semantic / field.lsb;
        const float stored = static_cast<float>(scaled);
        if (!std::isfinite(scaled) || !std::isfinite(stored)) {
            return fail(error, QStringLiteral("field %1 F32 must be finite").arg(field.nameEn));
        }
        quint32 bits = 0;
        static_assert(sizeof(bits) == sizeof(stored), "F32 requires four bytes");
        std::memcpy(&bits, &stored, sizeof(bits));
        return writeLittleEndian(payload, offset, field.byteLength, bits, error);
    }

    quint64 raw = 0;
    if (field.type == FieldType::Const) {
        if (!fieldExpectedValue(definition, field, sequence, &raw)) {
            return fail(error, QStringLiteral("field %1 lacks a CONST default").arg(field.nameEn));
        }
    } else {
        const bool signedValue = field.type == FieldType::S16 || field.type == FieldType::S16F ||
            field.type == FieldType::S32F;
        if (!scaledIntegerRaw(field, value, signedValue, field.byteLength * 8, &raw, error)) {
            return false;
        }
    }
    return writeLittleEndian(payload, offset, field.byteLength, raw, error);
}

bool decodeField(const ProtocolField& field,
                 const QByteArray& payload,
                 QVariant* value,
                 QString* error)
{
    const int offset = field.payloadOffset();
    if (field.type == FieldType::Reserved) {
        if (offset < 0 || offset + field.byteLength > payload.size()) {
            return fail(error, QStringLiteral("field %1 has an out-of-range RESERVED offset").arg(field.nameEn));
        }
        for (int index = 0; index < field.byteLength; ++index) {
            if (payload.at(offset + index) != '\0') {
                return fail(error, QStringLiteral("field %1 RESERVED bytes must be zero").arg(field.nameEn));
            }
        }
        return true;
    }

    if (field.type == FieldType::Bit) {
        if (offset < 0 || offset >= payload.size()) {
            return fail(error, QStringLiteral("field %1 has an out-of-range BIT offset").arg(field.nameEn));
        }
        const quint8 raw = static_cast<uchar>(payload.at(offset));
        const quint64 extracted = (raw >> field.bitOffset) & maxUnsignedForBits(field.bitLength);
        *value = field.hasLsb
            ? QVariant(static_cast<double>(extracted) * field.lsb)
            : QVariant::fromValue<qulonglong>(extracted);
        return true;
    }

    quint64 raw = 0;
    if (!readLittleEndian(payload, offset, field.byteLength, &raw, error)) {
        return false;
    }
    if (field.type == FieldType::F32) {
        const quint32 bits = static_cast<quint32>(raw);
        float stored = 0.0F;
        std::memcpy(&stored, &bits, sizeof(stored));
        const double decoded = static_cast<double>(stored) * field.lsb;
        if (!std::isfinite(decoded)) {
            return fail(error, QStringLiteral("field %1 F32 must be finite").arg(field.nameEn));
        }
        *value = QVariant(decoded);
        return true;
    }
    if (field.type == FieldType::Const) {
        quint64 expected = 0;
        if (!defaultAsUnsigned(field, &expected) || raw != expected) {
            return fail(error, QStringLiteral("field %1 constant value does not match").arg(field.nameEn));
        }
        *value = QVariant::fromValue<qulonglong>(raw);
        return true;
    }

    const bool signedValue = field.type == FieldType::S16 || field.type == FieldType::S16F ||
        field.type == FieldType::S32F;
    if (signedValue) {
        const qint64 signedRaw = signExtend(raw, field.byteLength * 8);
        *value = field.type == FieldType::S16F || field.type == FieldType::S32F || field.hasLsb
            ? QVariant(static_cast<double>(signedRaw) * field.lsb)
            : QVariant::fromValue<qlonglong>(signedRaw);
    } else {
        *value = field.hasLsb
            ? QVariant(static_cast<double>(raw) * field.lsb)
            : QVariant::fromValue<qulonglong>(raw);
    }
    return true;
}

} // namespace

bool ProtocolField::isPayloadField(int payloadLength) const noexcept
{
    return startByte >= 4 && endByte <= payloadLength + 3;
}

int ProtocolField::payloadOffset() const noexcept
{
    return startByte - 4;
}

const ProtocolField* MessageDefinition::findField(const QString& name) const noexcept
{
    for (const ProtocolField& field : fields) {
        if (field.nameEn == name) {
            return &field;
        }
    }
    return nullptr;
}

bool ProtocolCatalog::loadFromDirectory(const QString& directory, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    m_directory.clear();
    m_messages.clear();
    m_nameIndex.clear();
    m_commandIndex.clear();

    const QDir catalogDirectory(directory);
    if (!catalogDirectory.exists()) {
        return fail(error, QStringLiteral("protocol CSV directory does not exist: %1").arg(directory));
    }
    const QFileInfoList files = catalogDirectory.entryInfoList(
        {QStringLiteral("*.csv")}, QDir::Files | QDir::Readable, QDir::Name);
    if (files.isEmpty()) {
        return fail(error, QStringLiteral("protocol CSV directory contains no CSV files: %1").arg(directory));
    }

    QVector<MessageDefinition> loadedMessages;
    QHash<QString, int> nameIndex;
    QHash<quint32, int> commandIndex;
    loadedMessages.reserve(files.size());
    for (const QFileInfo& file : files) {
        MessageDefinition definition;
        if (!loadDefinition(file, &definition, error)) {
            return false;
        }
        if (nameIndex.contains(definition.name)) {
            return fail(error, QStringLiteral("duplicate protocol name: %1").arg(definition.name));
        }
        const quint32 key = commandKey(definition.typeGroup, definition.subType, definition.direction);
        if (commandIndex.contains(key)) {
            return fail(error,
                        QStringLiteral("duplicate %1 command 0x%2/0x%3")
                            .arg(definition.direction == Direction::Request
                                     ? QStringLiteral("request")
                                     : QStringLiteral("response"))
                            .arg(definition.typeGroup, 2, 16, QLatin1Char('0'))
                            .arg(definition.subType, 2, 16, QLatin1Char('0')));
        }
        const int messageIndex = loadedMessages.size();
        nameIndex.insert(definition.name, messageIndex);
        commandIndex.insert(key, messageIndex);
        loadedMessages.append(definition);
    }

    m_directory = catalogDirectory.absolutePath();
    m_messages = loadedMessages;
    m_nameIndex = nameIndex;
    m_commandIndex = commandIndex;
    return true;
}

QString ProtocolCatalog::directory() const
{
    return m_directory;
}

int ProtocolCatalog::size() const noexcept
{
    return m_messages.size();
}

const QVector<MessageDefinition>& ProtocolCatalog::messages() const noexcept
{
    return m_messages;
}

const MessageDefinition* ProtocolCatalog::findByName(const QString& name) const noexcept
{
    const auto iterator = m_nameIndex.constFind(name);
    if (iterator == m_nameIndex.constEnd()) {
        return nullptr;
    }
    return &m_messages.at(iterator.value());
}

const MessageDefinition* ProtocolCatalog::findByCommand(quint8 typeGroup,
                                                         quint8 subType,
                                                         Direction direction) const noexcept
{
    const auto iterator = m_commandIndex.constFind(commandKey(typeGroup, subType, direction));
    if (iterator == m_commandIndex.constEnd()) {
        return nullptr;
    }
    return &m_messages.at(iterator.value());
}

quint16 crc16Xmodem(const QByteArray& data) noexcept
{
    quint16 crc = 0;
    for (const char value : data) {
        crc ^= static_cast<quint16>(static_cast<uchar>(value)) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0U
                ? static_cast<quint16>((crc << 1U) ^ 0x1021U)
                : static_cast<quint16>(crc << 1U);
        }
    }
    return crc;
}

bool encodePayload(const MessageDefinition& definition,
                   const QVariantMap& values,
                   quint16 sequence,
                   QByteArray* payload,
                   QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (payload == nullptr) {
        return fail(error, QStringLiteral("payload output must not be null"));
    }
    if (!validateDefinitionForCodec(definition, error)) {
        return false;
    }
    if (!validateSuppliedValues(definition, values, sequence, error)) {
        return false;
    }

    QByteArray encoded(definition.payloadLength, '\0');
    for (const ProtocolField& field : definition.fields) {
        if (!field.isPayloadField(definition.payloadLength)) {
            continue;
        }
        if (!encodeField(definition, field, values, sequence, &encoded, error)) {
            return false;
        }
    }
    *payload = encoded;
    return true;
}

bool decodePayload(const MessageDefinition& definition,
                   const QByteArray& payload,
                   QVariantMap* values,
                   QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (values == nullptr) {
        return fail(error, QStringLiteral("values output must not be null"));
    }
    if (!validateDefinitionForCodec(definition, error)) {
        return false;
    }
    if (payload.size() != definition.payloadLength) {
        return fail(error,
                    QStringLiteral("%1 payload length must be %2, actual %3")
                        .arg(definition.name)
                        .arg(definition.payloadLength)
                        .arg(payload.size()));
    }

    QVariantMap decoded;
    quint64 typeGroup = 0;
    quint64 subType = 0;
    bool hasTypeGroup = false;
    bool hasSubType = false;
    for (const ProtocolField& field : definition.fields) {
        if (!field.isPayloadField(definition.payloadLength)) {
            continue;
        }
        QVariant value;
        if (!decodeField(field, payload, &value, error)) {
            return false;
        }
        if (field.type == FieldType::Reserved) {
            continue;
        }
        if (field.nameEn == QStringLiteral("type_group")) {
            hasTypeGroup = variantToUnsignedInteger(value, &typeGroup);
        } else if (field.nameEn == QStringLiteral("sub_type")) {
            hasSubType = variantToUnsignedInteger(value, &subType);
        }
        if (field.isValid) {
            decoded.insert(field.nameEn, value);
        }
    }
    if (!hasTypeGroup || !hasSubType || typeGroup != definition.typeGroup ||
        subType != definition.subType) {
        return fail(error, QStringLiteral("%1 type_group/sub_type does not match").arg(definition.name));
    }
    *values = decoded;
    return true;
}

bool encodeFrame(const QByteArray& payload, QByteArray* frame, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (frame == nullptr) {
        return fail(error, QStringLiteral("frame output must not be null"));
    }
    if (payload.isEmpty() || payload.size() > 0xFF) {
        return fail(error, QStringLiteral("physical frame payload length must be in 1..255"));
    }

    QByteArray encoded;
    encoded.reserve(kFrameHeaderLength + payload.size() + kFrameCrcLength);
    encoded.append(static_cast<char>(kSync0));
    encoded.append(static_cast<char>(kSync1));
    encoded.append(static_cast<char>(payload.size()));
    encoded.append(payload);
    const quint16 crc = crc16Xmodem(encoded.mid(2));
    encoded.append(static_cast<char>(crc & 0xFFU));
    encoded.append(static_cast<char>((crc >> 8U) & 0xFFU));
    *frame = encoded;
    return true;
}

bool decodeFrame(const QByteArray& frame, QByteArray* payload, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (payload == nullptr) {
        return fail(error, QStringLiteral("payload output must not be null"));
    }
    if (frame.size() < kFrameHeaderLength + kFrameCrcLength) {
        return fail(error, QStringLiteral("physical frame is too short"));
    }
    if (static_cast<uchar>(frame.at(0)) != kSync0 || static_cast<uchar>(frame.at(1)) != kSync1) {
        return fail(error, QStringLiteral("physical frame sync must be 55 AA"));
    }
    const int length = static_cast<uchar>(frame.at(2));
    if (length == 0 || frame.size() != length + kFrameHeaderLength + kFrameCrcLength) {
        return fail(error, QStringLiteral("physical frame length byte does not match frame size"));
    }
    const quint16 received = static_cast<quint16>(static_cast<uchar>(frame.at(frame.size() - 2))) |
        (static_cast<quint16>(static_cast<uchar>(frame.at(frame.size() - 1))) << 8U);
    const quint16 expected = crc16Xmodem(frame.mid(2, length + 1));
    if (received != expected) {
        return fail(error,
                    QStringLiteral("CRC-16/XMODEM mismatch: received 0x%1, expected 0x%2")
                        .arg(received, 4, 16, QLatin1Char('0'))
                        .arg(expected, 4, 16, QLatin1Char('0')));
    }
    *payload = frame.mid(kFrameHeaderLength, length);
    return true;
}

} // namespace hwtest::algorithm::mbddf
