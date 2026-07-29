#include "helm_analysis_capture.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <limits>

namespace hwtest::algorithm::mbddf {
namespace {

constexpr quint16 kCaptureFormatVersion = 1;
constexpr quint16 kRecordBytes = 124;
constexpr qint64 kFooterBytes = 64;
const QByteArray kHeaderMagic("HWTHPA01", 8);
const QByteArray kFooterMagic("HWTHPE01", 8);

AnalysisError failure(const QString& code, const QString& message)
{
    return {code, message};
}

void configure(QDataStream& stream)
{
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

bool writeBytes(QDataStream& stream, const QByteArray& bytes)
{
    return stream.writeRawData(bytes.constData(), bytes.size()) == bytes.size() &&
           stream.status() == QDataStream::Ok;
}

bool readBytes(QDataStream& stream, int size, QByteArray* bytes)
{
    if (bytes == nullptr) return false;
    bytes->resize(size);
    return stream.readRawData(bytes->data(), size) == size &&
           stream.status() == QDataStream::Ok;
}

QVariantMap captureMetadata(const AnalysisSessionSpec& spec,
                            const AnalysisIdentity& identity)
{
    QVariantMap metadata = spec.metadata;
    metadata.insert(QStringLiteral("taskId"), identity.taskId);
    metadata.insert(QStringLiteral("analysisGeneration"),
                    QVariant::fromValue<qulonglong>(identity.analysisGeneration));
    metadata.insert(QStringLiteral("algorithmId"), spec.algorithmId);
    metadata.insert(QStringLiteral("configId"), spec.configId);
    metadata.insert(QStringLiteral("schemaVersion"), spec.schemaVersion);
    metadata.insert(QStringLiteral("sourceStepId"), spec.sourceStepId);
    metadata.insert(QStringLiteral("effectiveRunParameters"), spec.effectiveRunParameters);
    return metadata;
}

bool finite(const QVariant& value, double* output)
{
    if (output == nullptr || !value.isValid() || value.isNull()) return false;
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number)) return false;
    *output = number;
    return true;
}

bool uintValue(const QVariantMap& values, const QString& name, quint32* output)
{
    if (output == nullptr || !values.contains(name)) return false;
    bool ok = false;
    const qulonglong value = values.value(name).toULongLong(&ok);
    if (!ok || value > std::numeric_limits<quint32>::max()) return false;
    *output = static_cast<quint32>(value);
    return true;
}

bool uint64Value(const QVariantMap& values, const QString& name, quint64* output)
{
    if (output == nullptr || !values.contains(name)) return false;
    bool ok = false;
    const qulonglong value = values.value(name).toULongLong(&ok);
    if (!ok) return false;
    *output = static_cast<quint64>(value);
    return true;
}

QByteArray encodeRecord(const HelmCaptureRecord& record)
{
    QByteArray bytes;
    bytes.reserve(kRecordBytes);
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    configure(stream);
    stream << record.acceptedSequence << record.streamElapsedUs;
    for (double value : record.command) stream << value;
    for (double value : record.feedback) stream << value;
    stream << record.status << record.errorCode << record.selfCheck << record.timeout
           << record.productSequence << record.serialA << record.serialB
           << record.ddsSequence << record.batchIndex << record.flags;
    return stream.status() == QDataStream::Ok ? bytes : QByteArray{};
}

bool decodeRecord(const QByteArray& bytes, HelmCaptureRecord* record)
{
    if (record == nullptr || bytes.size() != kRecordBytes) return false;
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return false;
    QDataStream stream(&buffer);
    configure(stream);
    stream >> record->acceptedSequence >> record->streamElapsedUs;
    for (double& value : record->command) stream >> value;
    for (double& value : record->feedback) stream >> value;
    stream >> record->status >> record->errorCode >> record->selfCheck >> record->timeout
           >> record->productSequence >> record->serialA >> record->serialB
           >> record->ddsSequence >> record->batchIndex >> record->flags;
    return stream.status() == QDataStream::Ok && buffer.pos() == bytes.size();
}

} // namespace

HelmAnalysisCapture::HelmAnalysisCapture(const AnalysisSessionSpec& spec,
                                         AnalysisError* error)
    : m_spec(spec)
    , m_file(spec.captureFilePath)
    , m_hash(new QCryptographicHash(QCryptographicHash::Sha256))
{
    if (error != nullptr) *error = {};
    if (m_spec.captureFilePath.isEmpty()) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_storage"),
                                               QStringLiteral("Capture file path is required"));
        return;
    }
    if (m_spec.sourceStepId != QStringLiteral("HELM_STREAM")) {
        if (error != nullptr) *error = failure(QStringLiteral("invalid_session_spec"),
                                               QStringLiteral("HELM analysis requires the HELM_STREAM source step"));
        return;
    }
    if (m_spec.maxCaptureBytes == 0 || m_spec.maxInputSamples == 0 ||
        m_spec.maxAnalysisDurationUs <= 0) {
        if (error != nullptr) *error = failure(QStringLiteral("invalid_session_spec"),
                                               QStringLiteral("Capture resource limits must be positive"));
        return;
    }
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = failure(QStringLiteral("analysis_storage"),
                             QStringLiteral("Cannot create analysis capture '%1': %2")
                                 .arg(m_spec.captureFilePath, m_file.errorString()));
        }
        return;
    }
    m_open = true;
}

HelmAnalysisCapture::~HelmAnalysisCapture()
{
    delete m_hash;
    if (m_file.isOpen()) m_file.close();
}

AnalysisError HelmAnalysisCapture::bindIdentity(const AnalysisIdentity& identity)
{
    if (!m_open) return failure(QStringLiteral("analysis_storage"),
                                QStringLiteral("Analysis capture is not open"));
    if (!identity.isValid()) return failure(QStringLiteral("invalid_identity"),
                                            QStringLiteral("Analysis task identity is required"));
    if (m_sealed) return failure(QStringLiteral("analysis_sealed"),
                                 QStringLiteral("Cannot bind a sealed capture"));
    if (m_bound) {
        if (m_identity.taskId == identity.taskId &&
            m_identity.analysisGeneration == identity.analysisGeneration) {
            return {};
        }
        return failure(QStringLiteral("identity_already_bound"),
                       QStringLiteral("Capture identity cannot be changed after binding"));
    }
    m_identity = identity;
    AnalysisError error;
    if (!writeHeader(&error)) return error;
    m_bound = true;
    return {};
}

AnalysisAcceptResult HelmAnalysisCapture::result(AnalysisAppendDisposition disposition,
                                                  const AnalysisError& error) const
{
    AnalysisAcceptResult outcome;
    outcome.disposition = disposition;
    outcome.error = error;
    outcome.message = error.message;
    outcome.acceptedSampleCount = m_acceptedCount;
    outcome.lateSampleCount = m_lateCount;
    outcome.accepted = disposition == AnalysisAppendDisposition::Accepted;
    outcome.late = disposition == AnalysisAppendDisposition::Late;
    return outcome;
}

HelmCaptureRecord HelmAnalysisCapture::decodeSample(const PostRunSample& sample,
                                                     bool* valid) const
{
    HelmCaptureRecord record;
    record.acceptedSequence = m_acceptedCount;
    record.streamElapsedUs = sample.streamElapsedUs;
    bool allFinite = sample.streamElapsedUs >= 0;
    for (int channel = 0; channel < 4; ++channel) {
        double command = std::numeric_limits<double>::quiet_NaN();
        double feedback = std::numeric_limits<double>::quiet_NaN();
        if (!finite(sample.values.value(QStringLiteral("ins[%1]").arg(channel)), &command)) {
            allFinite = false;
        }
        if (!finite(sample.values.value(QStringLiteral("fdb[%1]").arg(channel)), &feedback)) {
            allFinite = false;
        }
        record.command[static_cast<size_t>(channel)] = command;
        record.feedback[static_cast<size_t>(channel)] = feedback;
    }
    // Health fields are required because they decide whether a captured
    // channel is trustworthy.  Transport/product sequence values remain
    // optional diagnostics: they must never turn an otherwise valid sample
    // into an unavailable analysis result.
    bool diagnosticsValid = uintValue(sample.values, QStringLiteral("status"), &record.status) &&
        uintValue(sample.values, QStringLiteral("err_code"), &record.errorCode) &&
        uintValue(sample.values, QStringLiteral("timeout"), &record.timeout);
    if (sample.values.contains(QStringLiteral("self_check_or"))) {
        diagnosticsValid = uintValue(sample.values, QStringLiteral("self_check_or"),
                                      &record.selfCheck) && diagnosticsValid;
    } else {
        diagnosticsValid = uintValue(sample.values, QStringLiteral("self_check_or_timeout"),
                                      &record.selfCheck) && diagnosticsValid;
    }
    if (sample.values.contains(QStringLiteral("product_frame_sequence"))) {
        uintValue(sample.values, QStringLiteral("product_frame_sequence"),
                  &record.productSequence);
    } else {
        uintValue(sample.values, QStringLiteral("seq"), &record.productSequence);
    }
    uintValue(sample.values, QStringLiteral("serial_a"), &record.serialA);
    uintValue(sample.values, QStringLiteral("serial_b"), &record.serialB);
    uint64Value(sample.values, QStringLiteral("dds_timestamp_us"), &record.ddsSequence);
    uintValue(sample.values, QStringLiteral("batch_sample_index"), &record.batchIndex);
    allFinite = allFinite && diagnosticsValid;
    if (!allFinite) record.flags |= 0x1u;
    if (sample.values.value(QStringLiteral("product_frame_discontinuity")).toBool()) {
        record.flags |= 0x2u;
    }
    if (sample.values.value(QStringLiteral("serial_a_discontinuity")).toBool()) {
        record.flags |= 0x4u;
    }
    if (sample.values.value(QStringLiteral("serial_b_discontinuity")).toBool()) {
        record.flags |= 0x8u;
    }
    if (valid != nullptr) *valid = allFinite;
    return record;
}

AnalysisAcceptResult HelmAnalysisCapture::append(const AnalysisIdentity& identity,
                                                  const PostRunSample& sample)
{
    if (!m_bound || identity.taskId != m_identity.taskId ||
        identity.analysisGeneration != m_identity.analysisGeneration) {
        return result(AnalysisAppendDisposition::IgnoredIdentity,
                      failure(QStringLiteral("stale_analysis_identity"),
                              QStringLiteral("Sample does not match the active analysis identity")));
    }
    if (!m_spec.sourceStepId.isEmpty() && sample.sourceId != m_spec.sourceStepId) {
        return result(AnalysisAppendDisposition::IgnoredSource,
                      failure(QStringLiteral("unexpected_analysis_source"),
                              QStringLiteral("Sample source does not match the analysis session")));
    }
    if (m_sealed) {
        ++m_lateCount;
        return result(AnalysisAppendDisposition::Late);
    }
    if (m_resourceLimit) {
        return result(AnalysisAppendDisposition::ResourceLimit,
                      failure(QStringLiteral("analysis_resource_limit"),
                              QStringLiteral("Analysis input limit was already reached")));
    }
    bool valid = false;
    HelmCaptureRecord record = decodeSample(sample, &valid);
    if (!valid || (m_acceptedCount > 0 && sample.streamElapsedUs <= m_lastElapsedUs)) {
        ++m_invalidInputCount;
        record.flags |= 0x1u;
        if (m_acceptedCount > 0 && sample.streamElapsedUs <= m_lastElapsedUs) {
            m_timeInvalid = true;
            record.flags |= 0x10u;
        }
    }
    if (!m_hasFirstElapsed && sample.streamElapsedUs >= 0) {
        m_hasFirstElapsed = true;
        m_firstElapsedUs = sample.streamElapsedUs;
    }
    if (m_hasFirstElapsed && sample.streamElapsedUs >= 0 &&
        sample.streamElapsedUs - m_firstElapsedUs > m_spec.maxAnalysisDurationUs) {
        m_resourceLimit = true;
        return result(AnalysisAppendDisposition::ResourceLimit,
                      failure(QStringLiteral("analysis_resource_limit"),
                              QStringLiteral("Analysis duration limit was reached")));
    }
    if (m_acceptedCount >= m_spec.maxInputSamples ||
        m_bytesWritten + kRecordBytes + kFooterBytes > m_spec.maxCaptureBytes) {
        m_resourceLimit = true;
        return result(AnalysisAppendDisposition::ResourceLimit,
                      failure(QStringLiteral("analysis_resource_limit"),
                              QStringLiteral("Analysis input resource limit was reached")));
    }
    AnalysisError error;
    if (!writeRecord(record, &error)) return result(AnalysisAppendDisposition::Rejected, error);
    ++m_acceptedCount;
    if (sample.streamElapsedUs >= 0) m_lastElapsedUs = sample.streamElapsedUs;
    return result(AnalysisAppendDisposition::Accepted);
}

bool HelmAnalysisCapture::writeHeader(AnalysisError* error)
{
    const QVariantMap metadata = captureMetadata(m_spec, m_identity);
    const QByteArray encodedMetadata = QJsonDocument(QJsonObject::fromVariantMap(metadata))
                                           .toJson(QJsonDocument::Compact);
    if (encodedMetadata.size() > std::numeric_limits<quint32>::max()) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_storage"),
                                               QStringLiteral("Analysis capture metadata is too large"));
        return false;
    }
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    configure(stream);
    if (!writeBytes(stream, kHeaderMagic)) return false;
    stream << kCaptureFormatVersion << kRecordBytes
           << static_cast<quint32>(encodedMetadata.size());
    if (!writeBytes(stream, encodedMetadata) || stream.status() != QDataStream::Ok) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_storage"),
                                               QStringLiteral("Cannot encode analysis capture header"));
        return false;
    }
    if (static_cast<quint64>(header.size()) + kFooterBytes > m_spec.maxCaptureBytes) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_resource_limit"),
                                               QStringLiteral("Analysis capture metadata exceeds the resource limit"));
        return false;
    }
    if (
        m_file.write(header) != header.size()) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_storage"),
                                               QStringLiteral("Cannot write analysis capture header"));
        return false;
    }
    m_headerBytes = header;
    m_bytesWritten = static_cast<quint64>(header.size());
    m_hash->addData(header);
    return true;
}

bool HelmAnalysisCapture::writeRecord(const HelmCaptureRecord& record, AnalysisError* error)
{
    const QByteArray encoded = encodeRecord(record);
    if (encoded.size() != kRecordBytes || m_file.write(encoded) != encoded.size()) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_storage"),
                                               QStringLiteral("Cannot write analysis capture record"));
        return false;
    }
    m_hash->addData(encoded);
    m_bytesWritten += static_cast<quint64>(encoded.size());
    return true;
}

bool HelmAnalysisCapture::writeFooter(AnalysisError* error)
{
    m_normalizedHash = m_hash->result();
    QByteArray footer;
    QDataStream stream(&footer, QIODevice::WriteOnly);
    configure(stream);
    const quint64 lateAtSeal = m_lateCount;
    if (!writeBytes(stream, kFooterMagic)) return false;
    stream << m_acceptedCount << lateAtSeal << m_invalidInputCount;
    if (!writeBytes(stream, m_normalizedHash) || footer.size() != kFooterBytes ||
        m_file.write(footer) != footer.size() || !m_file.flush()) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_storage"),
                                               QStringLiteral("Cannot finalize analysis capture"));
        return false;
    }
    m_bytesWritten += static_cast<quint64>(footer.size());
    m_file.close();
    return true;
}

AnalysisInputSeal HelmAnalysisCapture::seal(const AnalysisTermination& termination)
{
    if (m_sealed) return m_seal;
    m_sealed = true;
    m_seal.identity = m_identity;
    m_seal.algorithmId = m_spec.algorithmId;
    m_seal.configId = m_spec.configId;
    m_seal.schemaVersion = m_spec.schemaVersion;
    m_seal.effectiveRunParameters = m_spec.effectiveRunParameters;
    m_seal.metadata = m_spec.metadata;
    m_seal.captureFilePath = m_spec.captureFilePath;
    m_seal.captureFormatVersion = kCaptureFormatVersion;
    m_seal.captureRecordBytes = kRecordBytes;
    m_seal.acceptedSampleCount = m_acceptedCount;
    m_seal.lateSampleCount = m_lateCount;
    m_seal.resourceLimitReached = m_resourceLimit;
    m_seal.termination = termination;
    m_seal.metadata.insert(QStringLiteral("invalidInputCount"),
                           QVariant::fromValue<qulonglong>(m_invalidInputCount));
    m_seal.metadata.insert(QStringLiteral("strictTimeValid"), !m_timeInvalid);
    if (!m_bound) {
        m_seal.error = failure(QStringLiteral("invalid_identity"),
                               QStringLiteral("Capture was sealed before binding an identity"));
        m_seal.message = m_seal.error.message;
        if (m_file.isOpen()) m_file.close();
        return m_seal;
    }
    AnalysisError error;
    if (!writeFooter(&error)) {
        m_seal.error = error;
        m_seal.message = error.message;
        return m_seal;
    }
    m_seal.normalizedInputSha256 = m_normalizedHash;
    m_seal.valid = true;
    return m_seal;
}

bool readHelmAnalysisCapture(const AnalysisInputSeal& seal,
                             HelmCaptureData* capture,
                             AnalysisError* error)
{
    if (capture == nullptr) {
        if (error != nullptr) *error = failure(QStringLiteral("invalid_input"),
                                               QStringLiteral("Capture output is null"));
        return false;
    }
    *capture = {};
    QFile file(seal.captureFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_storage"),
                                               QStringLiteral("Cannot open sealed analysis capture"));
        return false;
    }
    QDataStream stream(&file);
    configure(stream);
    QByteArray magic;
    quint16 formatVersion = 0;
    quint16 recordBytes = 0;
    quint32 metadataBytes = 0;
    const bool headerMagicRead = readBytes(stream, 8, &magic);
    if (headerMagicRead) stream >> formatVersion >> recordBytes >> metadataBytes;
    if (!headerMagicRead || magic != kHeaderMagic ||
        stream.status() != QDataStream::Ok ||
        formatVersion != kCaptureFormatVersion || recordBytes != kRecordBytes) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                               QStringLiteral("Invalid analysis capture header"));
        return false;
    }
    if (seal.captureFormatVersion != formatVersion || seal.captureRecordBytes != recordBytes) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                               QStringLiteral("Analysis seal does not match capture format"));
        return false;
    }
    const qint64 remainingAfterMetadata = file.size() - file.pos() - kFooterBytes;
    if (metadataBytes > static_cast<quint32>(std::numeric_limits<int>::max()) ||
        remainingAfterMetadata < 0 ||
        static_cast<quint64>(metadataBytes) > static_cast<quint64>(remainingAfterMetadata)) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                               QStringLiteral("Analysis capture metadata length is invalid"));
        return false;
    }
    QByteArray metadataBytesRaw;
    if (!readBytes(stream, static_cast<int>(metadataBytes), &metadataBytesRaw)) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                               QStringLiteral("Truncated analysis capture metadata"));
        return false;
    }
    const QJsonDocument metadataDocument = QJsonDocument::fromJson(metadataBytesRaw);
    if (!metadataDocument.isObject()) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                               QStringLiteral("Invalid analysis capture metadata"));
        return false;
    }
    capture->metadata = metadataDocument.object().toVariantMap();
    const QVariantMap& metadata = capture->metadata;
    const bool matchingIdentity = metadata.value(QStringLiteral("taskId")).toString() ==
            seal.identity.taskId &&
        metadata.value(QStringLiteral("analysisGeneration")).toULongLong() ==
            seal.identity.analysisGeneration &&
        metadata.value(QStringLiteral("algorithmId")).toString() == seal.algorithmId &&
        metadata.value(QStringLiteral("configId")).toString() == seal.configId &&
        metadata.value(QStringLiteral("schemaVersion")).toString() == seal.schemaVersion &&
        metadata.value(QStringLiteral("sourceStepId")).toString() == QStringLiteral("HELM_STREAM");
    if (!matchingIdentity) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                               QStringLiteral("Analysis seal identity does not match capture metadata"));
        return false;
    }
    const qint64 recordsStart = file.pos();
    const qint64 footerStart = file.size() - kFooterBytes;
    if (footerStart < recordsStart || (footerStart - recordsStart) % recordBytes != 0) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                               QStringLiteral("Invalid analysis capture record region"));
        return false;
    }
    const quint64 count = static_cast<quint64>((footerStart - recordsStart) / recordBytes);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    file.seek(0);
    hash.addData(file.read(recordsStart));
    file.seek(recordsStart);
    capture->records.reserve(static_cast<int>(qMin<quint64>(count,
        static_cast<quint64>(std::numeric_limits<int>::max()))));
    for (quint64 index = 0; index < count; ++index) {
        const QByteArray recordBytesRaw = file.read(recordBytes);
        HelmCaptureRecord record;
        if (recordBytesRaw.size() != recordBytes || !decodeRecord(recordBytesRaw, &record)) {
            if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                                   QStringLiteral("Invalid analysis capture record"));
            return false;
        }
        hash.addData(recordBytesRaw);
        capture->records.push_back(record);
    }
    file.seek(footerStart);
    QDataStream footerStream(&file);
    configure(footerStream);
    QByteArray footerMagic;
    quint64 accepted = 0;
    quint64 late = 0;
    quint64 invalid = 0;
    QByteArray storedHash;
    const bool footerMagicRead = readBytes(footerStream, 8, &footerMagic);
    if (footerMagicRead) footerStream >> accepted >> late >> invalid;
    if (!footerMagicRead || footerMagic != kFooterMagic ||
        footerStream.status() != QDataStream::Ok || !readBytes(footerStream, 32, &storedHash) ||
        accepted != count || accepted != seal.acceptedSampleCount ||
        storedHash != hash.result() || storedHash != seal.normalizedInputSha256) {
        if (error != nullptr) *error = failure(QStringLiteral("analysis_capture_format"),
                                               QStringLiteral("Analysis capture footer verification failed"));
        return false;
    }
    capture->acceptedSampleCount = accepted;
    capture->lateSampleCount = late;
    capture->invalidInputCount = invalid;
    capture->normalizedInputSha256 = storedHash;
    return true;
}

} // namespace hwtest::algorithm::mbddf
