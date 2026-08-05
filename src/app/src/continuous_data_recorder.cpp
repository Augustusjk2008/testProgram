#include "continuous_data_recorder.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QMetaType>
#include <QSaveFile>
#include <QStringList>
#include <QTimeZone>

#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace hwtest::app {
namespace {

ActionResult storageFailure(const QString& message)
{
    return ActionResult{false, QStringLiteral("data_storage"), message};
}

ActionResult parameterFailure(const QString& message)
{
    return ActionResult{false, QStringLiteral("ParameterRangeError"), message};
}

qint64 utcNowUs()
{
    return static_cast<qint64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

QString timestampText(qint64 timestampUs, const QString& dateFormat)
{
    const qint64 milliseconds = timestampUs / 1000;
    const int microseconds = static_cast<int>(timestampUs % 1000000);
    const QDateTime utc = QDateTime::fromMSecsSinceEpoch(milliseconds, Qt::UTC);
    const QTimeZone shanghai(QByteArrayLiteral("Asia/Shanghai"));
    const QDateTime local = shanghai.isValid()
        ? utc.toTimeZone(shanghai)
        : utc.toOffsetFromUtc(8 * 60 * 60);
    return local.toString(dateFormat) +
        QStringLiteral("%1").arg(microseconds, 6, 10, QLatin1Char('0'));
}

QString metadataText(QString text)
{
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text;
}

QString floatText(float value)
{
#if defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 11
    const QLocale locale = QLocale::c();
    for (int precision = 1;
         precision <= std::numeric_limits<float>::max_digits10;
         ++precision) {
        const QString candidate = locale.toString(static_cast<double>(value),
                                                  'g',
                                                  precision);
        bool parsedOk = false;
        const float parsed = locale.toFloat(candidate, &parsedOk);
        if (parsedOk &&
            (parsed == value || (std::isnan(parsed) && std::isnan(value)))) {
            return candidate;
        }
    }
    return locale.toString(static_cast<double>(value),
                           'g',
                           std::numeric_limits<float>::max_digits10);
#else
    char buffer[64];
    const auto converted = std::to_chars(buffer,
                                         buffer + sizeof(buffer),
                                         value,
                                         std::chars_format::general);
    if (converted.ec == std::errc{}) {
        return QString::fromLatin1(buffer,
                                   static_cast<int>(converted.ptr - buffer));
    }
    return QLocale::c().toString(
        static_cast<double>(value),
        'g',
        std::numeric_limits<float>::max_digits10);
#endif
}

QString scalarText(const QVariant& value)
{
    if (!value.isValid() || value.isNull()) {
        return QStringLiteral("NA");
    }
    switch (value.userType()) {
    case QMetaType::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::Short:
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
        return QString::number(value.toLongLong());
    case QMetaType::UChar:
    case QMetaType::UShort:
    case QMetaType::UInt:
    case QMetaType::ULong:
    case QMetaType::ULongLong:
        return QString::number(value.toULongLong());
    case QMetaType::Float:
        return floatText(value.toFloat());
    case QMetaType::Double:
        return QLocale::c().toString(value.toDouble(), 'g', 15);
    case QMetaType::QString:
        return metadataText(value.toString()).replace(QLatin1Char('\t'), QLatin1Char(' '));
    default:
        break;
    }

    const QJsonValue jsonValue = QJsonValue::fromVariant(value);
    QByteArray json;
    if (jsonValue.isObject()) {
        json = QJsonDocument(jsonValue.toObject()).toJson(QJsonDocument::Compact);
    } else if (jsonValue.isArray()) {
        json = QJsonDocument(jsonValue.toArray()).toJson(QJsonDocument::Compact);
    } else {
        return metadataText(value.toString()).replace(QLatin1Char('\t'), QLatin1Char(' '));
    }
    return metadataText(QString::fromUtf8(json)).replace(QLatin1Char('\t'), QLatin1Char(' '));
}

QString genericColumnName(const TestMeasurementDescriptor& measurement)
{
    QString unit = measurement.unit.trimmed();
    if (unit.isEmpty()) {
        return measurement.id;
    }
    unit.replace(QLatin1Char('/'), QLatin1Char('_'));
    unit.replace(QLatin1Char(' '), QLatin1Char('_'));
    unit.replace(QLatin1Char('%'), QStringLiteral("percent"));
    return QStringLiteral("%1_%2").arg(measurement.id, unit);
}

QString safeFileStem(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        value = QStringLiteral("ContinuousTest");
    }
    for (int index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (!character.isLetterOrNumber() && character != QLatin1Char('-') &&
            character != QLatin1Char('_')) {
            value[index] = QLatin1Char('_');
        }
    }
    return value;
}

QString dataFileProject(const TestDescriptor& descriptor)
{
    return descriptor.reportTitle.trimmed().isEmpty()
        ? descriptor.configId
        : descriptor.reportTitle;
}

ActionResult normalizeDataFileName(const QString& requested, QString* output)
{
    QString fileName = requested.trimmed();
    if (fileName.isEmpty()) {
        if (output != nullptr) *output = QString{};
        return {};
    }
    if (fileName.contains(QLatin1Char('/')) ||
        fileName.contains(QLatin1Char('\\'))) {
        return parameterFailure(
            QStringLiteral("Continuous data file name must not contain a path separator"));
    }
    if (fileName.endsWith(QLatin1Char('.')) ||
        fileName.endsWith(QLatin1Char(' '))) {
        return parameterFailure(
            QStringLiteral("Continuous data file name must not end with a dot or space"));
    }
    const QString invalidCharacters = QStringLiteral("<>:\"|?*");
    for (const QChar character : fileName) {
        if (character.unicode() < 0x20 || invalidCharacters.contains(character)) {
            return parameterFailure(
                QStringLiteral("Continuous data file name contains an invalid Windows character"));
        }
    }

    const QFileInfo info(fileName);
    const QString suffix = info.suffix();
    if (!suffix.isEmpty() &&
        suffix.compare(QStringLiteral("txt"), Qt::CaseInsensitive) != 0) {
        return parameterFailure(
            QStringLiteral("Continuous data file name must use the .txt extension"));
    }
    QString stem = suffix.isEmpty()
        ? fileName
        : fileName.left(fileName.size() - suffix.size() - 1);
    if (stem.isEmpty() || stem == QStringLiteral(".") || stem == QStringLiteral("..")) {
        return parameterFailure(QStringLiteral("Continuous data file name is empty"));
    }
    const QString reservedStem = stem.section(QLatin1Char('.'), 0, 0).toUpper();
    const bool numberedDevice =
        (reservedStem.startsWith(QStringLiteral("COM")) ||
         reservedStem.startsWith(QStringLiteral("LPT"))) &&
        reservedStem.size() == 4 && reservedStem.at(3) >= QLatin1Char('1') &&
        reservedStem.at(3) <= QLatin1Char('9');
    if (reservedStem == QStringLiteral("CON") ||
        reservedStem == QStringLiteral("PRN") ||
        reservedStem == QStringLiteral("AUX") ||
        reservedStem == QStringLiteral("NUL") ||
        reservedStem == QStringLiteral("CLOCK$") || numberedDevice) {
        return parameterFailure(
            QStringLiteral("Continuous data file name is reserved by Windows"));
    }
    if (stem.size() > 220) {
        return parameterFailure(QStringLiteral("Continuous data file name is too long"));
    }

    fileName = stem + QStringLiteral(".txt");
    if (output != nullptr) *output = fileName;
    return {};
}

bool isFullyQualifiedDataDirectory(const QString& requested)
{
    const QString path = QDir::fromNativeSeparators(requested.trimmed());
#ifdef Q_OS_WIN
    const auto hasDriveRoot = [](const QString& value) {
        return value.size() >= 3 && value.at(0).isLetter() &&
            value.at(1) == QLatin1Char(':') && value.at(2) == QLatin1Char('/');
    };
    const auto hasServerAndShare = [](const QString& value) {
        const QStringList components = value.split(
            QLatin1Char('/'), Qt::SkipEmptyParts);
        return components.size() >= 2;
    };

    if (path.startsWith(QStringLiteral("//?/UNC/"), Qt::CaseInsensitive)) {
        return hasServerAndShare(path.mid(8));
    }
    if (path.startsWith(QStringLiteral("//?/"))) {
        return hasDriveRoot(path.mid(4));
    }
    if (path.startsWith(QStringLiteral("//"))) {
        return hasServerAndShare(path.mid(2));
    }
    return hasDriveRoot(path);
#else
    return QDir::isAbsolutePath(path);
#endif
}

QString uniqueOutputPath(const QString& directory, const QString& fileName)
{
    const QString stem = fileName.left(fileName.size() - 4);
    for (int suffix = 0;; ++suffix) {
        const QString candidateName = suffix == 0
            ? fileName
            : QStringLiteral("%1_%2.txt").arg(stem).arg(suffix);
        const QString candidate = QDir(directory).absoluteFilePath(candidateName);
        if (!QFileInfo::exists(candidate) &&
            !QFileInfo::exists(candidate + QStringLiteral(".partial"))) {
            return candidate;
        }
    }
}

ActionResult reserveOutputPath(const QString& directory,
                               const QString& fileName,
                               const QString& ownPartialPath,
                               QString* outputPath)
{
    const QString stem = fileName.left(fileName.size() - 4);
    for (int suffix = 0;; ++suffix) {
        const QString candidateName = suffix == 0
            ? fileName
            : QStringLiteral("%1_%2.txt").arg(stem).arg(suffix);
        const QString candidate = QDir(directory).absoluteFilePath(candidateName);
        const QString candidatePartial = candidate + QStringLiteral(".partial");
        if (candidatePartial != ownPartialPath && QFileInfo::exists(candidatePartial)) {
            continue;
        }
        QFile reservation(candidate);
        if (reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            reservation.close();
            if (outputPath != nullptr) *outputPath = candidate;
            return {};
        }
        if (!QFileInfo::exists(candidate)) {
            return storageFailure(
                QStringLiteral("Cannot reserve continuous data file '%1': %2")
                    .arg(candidate, reservation.errorString()));
        }
    }
}

bool writeAll(QIODevice* device, const QByteArray& content)
{
    return device != nullptr && device->write(content) == content.size();
}

} // namespace

ContinuousDataRecorder::ContinuousDataRecorder(Clock clock,
                                               OutputFileFactory outputFileFactory)
    : m_clock(std::move(clock)),
      m_outputFileFactory(std::move(outputFileFactory))
{
    if (!m_clock) m_clock = utcNowUs;
    if (!m_outputFileFactory) {
        m_outputFileFactory = [](const QString& path) {
            return std::make_unique<QSaveFile>(path);
        };
    }
}

ActionResult ContinuousDataRecorder::validateDestinationOverrides(
    const QString& dataDirectory,
    const QString& dataFileName,
    QString* normalizedFileName)
{
    const QString requestedDirectory = dataDirectory.trimmed();
    if (!requestedDirectory.isEmpty() &&
        !isFullyQualifiedDataDirectory(requestedDirectory)) {
        return parameterFailure(
            QStringLiteral("Continuous data directory must be an absolute path"));
    }
    return normalizeDataFileName(dataFileName, normalizedFileName);
}

ContinuousDataRecorder::~ContinuousDataRecorder()
{
    if (m_partialFile.isOpen()) {
        m_partialFile.flush();
        m_partialFile.close();
    }
}

ActionResult ContinuousDataRecorder::begin(const QString& directory,
                                           const TestDescriptor& descriptor,
                                           const QString& runMode,
                                           int intervalMs,
                                           quint64 maxCycles,
                                           const QString& dataDirectory,
                                           const QString& dataFileName)
{
    if (m_active) {
        return storageFailure(QStringLiteral("A continuous data recording is already active"));
    }
    resetState();

    const QString requestedDirectory = dataDirectory.trimmed();
    QString normalizedFileName;
    const ActionResult validated = validateDestinationOverrides(
        requestedDirectory, dataFileName, &normalizedFileName);
    if (!validated.ok) return validated;

    const QString normalizedDirectory = QDir(requestedDirectory.isEmpty()
                                                  ? directory
                                                  : requestedDirectory)
                                            .absolutePath();
    if (!QDir().mkpath(normalizedDirectory)) {
        return storageFailure(
            QStringLiteral("Cannot create continuous data directory '%1'")
                .arg(normalizedDirectory));
    }

    m_descriptor = descriptor;
    m_runMode = runMode;
    m_outputDirectory = normalizedDirectory;
    m_requestedFileName = normalizedFileName;
    m_intervalMs = intervalMs;
    m_maxCycles = maxCycles;
    m_startedAtUs = m_clock();
    m_electricalHealthFormat =
        descriptor.algorithmId == QStringLiteral("mbddf.elec_health_status");

    if (m_electricalHealthFormat) {
        const QVector<QPair<QString, QString>> fields{
            {QStringLiteral("c_volt"), QStringLiteral("c_volt_V")},
            {QStringLiteral("b_volt"), QStringLiteral("b_volt_V")},
            {QStringLiteral("external_vol"), QStringLiteral("external_vol_V")},
            {QStringLiteral("core_vol"), QStringLiteral("core_vol_V")},
            {QStringLiteral("assist_vol"), QStringLiteral("assist_vol_V")},
            {QStringLiteral("v28_5"), QStringLiteral("v28_5_V")},
            {QStringLiteral("js_5V"), QStringLiteral("js_5V_V")},
            {QStringLiteral("dyt_5V"), QStringLiteral("dyt_5V_V")},
            {QStringLiteral("power_24V"), QStringLiteral("power_24V_V")},
            {QStringLiteral("value_YX"), QStringLiteral("value_YX_V")},
        };
        for (const auto& field : fields) {
            m_columns.push_back(Column{field.first, field.second});
        }
    } else {
        for (const TestMeasurementDescriptor& measurement : descriptor.measurements) {
            if (measurement.id == QStringLiteral("status") ||
                measurement.id == QStringLiteral("err_code")) {
                continue;
            }
            m_columns.push_back(Column{measurement.id,
                                       genericColumnName(measurement)});
        }
    }

    const QString prefix = safeFileStem(dataFileProject(descriptor));
    const QString timestamp = timestampText(
        m_startedAtUs, QStringLiteral("yyyyMMdd_HHmmss_"));
    QString openError;
    bool partialOpened = false;
    for (int attempt = 0; attempt < 1000 && !partialOpened; ++attempt) {
        if (!m_requestedFileName.isEmpty()) {
            m_outputPath = uniqueOutputPath(normalizedDirectory, m_requestedFileName);
            m_partialPath = m_outputPath + QStringLiteral(".partial");
        } else {
            const QString partialName = attempt == 0
                ? QStringLiteral(".%1_%2.partial").arg(prefix, timestamp)
                : QStringLiteral(".%1_%2_%3.partial")
                      .arg(prefix, timestamp)
                      .arg(attempt);
            m_partialPath = QDir(normalizedDirectory).absoluteFilePath(partialName);
        }
        m_partialFile.setFileName(m_partialPath);
        partialOpened = m_partialFile.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        if (!partialOpened) {
            openError = m_partialFile.errorString();
            if (!QFileInfo::exists(m_partialPath)) break;
        }
    }
    if (!partialOpened) {
        const QString failedPartialPath = m_partialPath;
        resetState();
        return storageFailure(
            QStringLiteral("Cannot open continuous data file '%1': %2")
                .arg(failedPartialPath, openError));
    }
    m_active = true;

    QStringList header{
        QStringLiteral("report_index"),
        QStringLiteral("sample_time_us"),
        QStringLiteral("seq"),
        QStringLiteral("response_status"),
        QStringLiteral("err_code"),
    };
    for (const Column& column : m_columns) {
        header.push_back(column.header);
    }
    if (m_electricalHealthFormat) {
        header.push_back(QStringLiteral("activate_bits"));
        header.push_back(QStringLiteral("bc_activate_good"));
    }
    const ActionResult written = writePartial(
        (header.join(QLatin1Char('\t')) + QLatin1Char('\n')).toUtf8(), true);
    if (!written.ok) {
        cancel();
        return written;
    }
    return {};
}

void ContinuousDataRecorder::setTaskId(const QString& taskId)
{
    m_taskId = taskId;
}

ActionResult ContinuousDataRecorder::append(const ApplicationSample& sample)
{
    if (!m_active || (!m_taskId.isEmpty() && sample.taskId != m_taskId)) {
        return {};
    }
    if (!m_writeError.isEmpty()) {
        return storageFailure(m_writeError);
    }

    ++m_sampleCount;
    const int responseStatus = sample.values.value(QStringLiteral("status")).toInt();
    const quint16 errorCode = static_cast<quint16>(
        sample.values.value(QStringLiteral("err_code")).toUInt());
    const bool failed = responseStatus != 0 || errorCode != 0;
    const bool suppressMeasurements = failed &&
        m_descriptor.algorithmId != QStringLiteral("mbddf.helm_stream");
    const quint16 sequence = static_cast<quint16>(
        sample.values.value(QStringLiteral("seq")).toUInt());
    const qint64 sampleTimeUs = sample.streamElapsedUs >= 0
        ? sample.streamElapsedUs
        : qMax<qint64>(0, sample.timestampUs - m_startedAtUs);

    QStringList row{
        QString::number(m_sampleCount),
        QString::number(sampleTimeUs),
        QString::number(sequence),
        QString::number(responseStatus),
        QStringLiteral("0x") +
            QString::number(errorCode, 16)
                .toUpper()
                .rightJustified(4, QLatin1Char('0')),
    };
    for (const Column& column : m_columns) {
        row.push_back(suppressMeasurements
                          ? QStringLiteral("NA")
                          : scalarText(sample.values.value(column.valueKey)));
    }
    if (m_electricalHealthFormat) {
        if (suppressMeasurements ||
            !sample.values.contains(QStringLiteral("activate_bits"))) {
            row.push_back(QStringLiteral("NA"));
            row.push_back(QStringLiteral("NA"));
        } else {
            const quint8 bits = static_cast<quint8>(
                sample.values.value(QStringLiteral("activate_bits")).toUInt());
            row.push_back(QStringLiteral("0x") +
                          QString::number(bits, 16)
                              .toUpper()
                              .rightJustified(2, QLatin1Char('0')));
            row.push_back(QString::number(bits & 1));
        }
    }
    return writePartial((row.join(QLatin1Char('\t')) + QLatin1Char('\n')).toUtf8(),
                        true);
}

ActionResult ContinuousDataRecorder::finish(const QString& finalStatus,
                                            const QString& finalDetail)
{
    if (!m_active) {
        return {};
    }
    m_active = false;
    if (m_partialFile.isOpen()) {
        if (!m_partialFile.flush() && m_writeError.isEmpty()) {
            m_writeError = QStringLiteral("Cannot flush partial continuous data file '%1': %2")
                               .arg(m_partialPath, m_partialFile.errorString());
        }
        m_partialFile.close();
    }
    if (!m_writeError.isEmpty()) {
        m_outputPath = m_partialPath;
        return storageFailure(m_writeError);
    }

    QFile rows(m_partialPath);
    if (!rows.open(QIODevice::ReadOnly)) {
        m_outputPath = m_partialPath;
        return storageFailure(
            QStringLiteral("Cannot reopen partial continuous data file '%1': %2")
                .arg(m_partialPath, rows.errorString()));
    }

    const qint64 finishedAtUs = m_clock();
    const QString fileName = m_requestedFileName.isEmpty()
        ? QStringLiteral("%1_%2-%3.txt")
              .arg(safeFileStem(dataFileProject(m_descriptor)),
                   timestampText(m_startedAtUs,
                                 QStringLiteral("yyyyMMdd_HHmmss_")),
                   timestampText(finishedAtUs,
                                 QStringLiteral("yyyyMMdd_HHmmss_")))
        : m_requestedFileName;
    QString reservedOutputPath;
    const ActionResult reserved = reserveOutputPath(
        m_outputDirectory, fileName, m_partialPath, &reservedOutputPath);
    if (!reserved.ok) {
        m_outputPath = m_partialPath;
        return reserved;
    }
    m_outputPath = reservedOutputPath;
    m_outputReservationActive = true;

    std::unique_ptr<QSaveFile> output = m_outputFileFactory(m_outputPath);
    if (!output || !output->open(QIODevice::WriteOnly)) {
        const QString failedPath = m_outputPath;
        const QString error = output == nullptr
            ? QStringLiteral("Output file factory returned null")
            : output->errorString();
        releaseOutputReservation();
        m_outputPath = m_partialPath;
        return storageFailure(
            QStringLiteral("Cannot create continuous data file '%1': %2")
                .arg(failedPath, error));
    }

    const QString title = m_electricalHealthFormat
        ? QStringLiteral("电气健康连续采集数据")
        : QStringLiteral("%1连续采集数据").arg(m_descriptor.title);
    QString metadata;
    metadata += QStringLiteral("# %1\n").arg(metadataText(title));
    metadata += QStringLiteral("# started_at=%1\n")
                    .arg(timestampText(m_startedAtUs,
                                       QStringLiteral("yyyy-MM-dd HH:mm:ss.")) +
                         QStringLiteral("+08:00"));
    metadata += QStringLiteral("# finished_at=%1\n")
                    .arg(timestampText(finishedAtUs,
                                       QStringLiteral("yyyy-MM-dd HH:mm:ss.")) +
                         QStringLiteral("+08:00"));
    metadata += QStringLiteral("# final_status=%1\n").arg(metadataText(finalStatus));
    metadata += QStringLiteral("# final_detail=%1\n").arg(metadataText(finalDetail));
    metadata += QStringLiteral("# sample_count=%1\n").arg(m_sampleCount);
    metadata += QStringLiteral("# run_mode=%1\n").arg(metadataText(m_runMode));
    metadata += m_runMode == QStringLiteral("pc_periodic")
        ? QStringLiteral("# repeat_delay_ms=%1\n").arg(m_intervalMs)
        : QStringLiteral("# repeat_delay_ms=NA\n");
    metadata += QStringLiteral("# config_id=%1\n")
                    .arg(metadataText(m_descriptor.configId));
    metadata += QStringLiteral("# algorithm_id=%1\n")
                    .arg(metadataText(m_descriptor.algorithmId));
    metadata += m_runMode == QStringLiteral("pc_periodic")
        ? QStringLiteral("# max_cycles=%1\n\n").arg(m_maxCycles)
        : QStringLiteral("# max_cycles=NA\n\n");

    bool ok = writeAll(output.get(), QByteArray::fromHex("EFBBBF")) &&
        writeAll(output.get(), metadata.toUtf8());
    while (ok && !rows.atEnd()) {
        const QByteArray chunk = rows.read(64 * 1024);
        if (chunk.isEmpty() && rows.error() != QFile::NoError) {
            ok = false;
            break;
        }
        ok = writeAll(output.get(), chunk);
    }
    rows.close();
    if (!ok || !output->commit()) {
        const QString finalPath = m_outputPath;
        releaseOutputReservation();
        m_outputPath = m_partialPath;
        return storageFailure(
            QStringLiteral("Cannot finalize continuous data file '%1': %2")
                .arg(finalPath, output->errorString()));
    }
    m_outputReservationActive = false;

    QFile::remove(m_partialPath);
    return {};
}

void ContinuousDataRecorder::cancel()
{
    const bool discardWorkingFile = m_active;
    if (m_partialFile.isOpen()) {
        m_partialFile.close();
    }
    if (discardWorkingFile && !m_partialPath.isEmpty()) {
        QFile::remove(m_partialPath);
    }
    if (discardWorkingFile) releaseOutputReservation();
    resetState();
}

bool ContinuousDataRecorder::active() const
{
    return m_active;
}

QString ContinuousDataRecorder::outputPath() const
{
    return m_outputPath;
}

ActionResult ContinuousDataRecorder::writePartial(const QByteArray& bytes, bool flush)
{
    if (!m_partialFile.isOpen() || !writeAll(&m_partialFile, bytes) ||
        (flush && !m_partialFile.flush())) {
        m_writeError = QStringLiteral("Cannot write continuous data file '%1': %2")
                           .arg(m_partialPath, m_partialFile.errorString());
        releaseOutputReservation();
        m_outputPath = m_partialPath;
        return storageFailure(m_writeError);
    }
    return {};
}

void ContinuousDataRecorder::releaseOutputReservation()
{
    if (!m_outputReservationActive) return;
    const QFileInfo reservation(m_outputPath);
    if (reservation.isFile() && reservation.size() == 0) {
        QFile::remove(m_outputPath);
    }
    m_outputReservationActive = false;
}

void ContinuousDataRecorder::resetState()
{
    m_partialFile.setFileName(QString{});
    m_descriptor = {};
    m_columns.clear();
    m_taskId.clear();
    m_runMode.clear();
    m_outputDirectory.clear();
    m_requestedFileName.clear();
    m_outputPath.clear();
    m_partialPath.clear();
    m_writeError.clear();
    m_outputReservationActive = false;
    m_startedAtUs = 0;
    m_sampleCount = 0;
    m_maxCycles = 0;
    m_intervalMs = 0;
    m_active = false;
    m_electricalHealthFormat = false;
}

} // namespace hwtest::app
