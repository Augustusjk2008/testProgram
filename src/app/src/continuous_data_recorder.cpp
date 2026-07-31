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

#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>

namespace hwtest::app {
namespace {

ActionResult storageFailure(const QString& message)
{
    return ActionResult{false, QStringLiteral("data_storage"), message};
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
    return QDateTime::fromMSecsSinceEpoch(milliseconds, Qt::UTC).toString(dateFormat) +
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

bool writeAll(QIODevice* device, const QByteArray& content)
{
    return device != nullptr && device->write(content) == content.size();
}

} // namespace

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
                                           quint64 maxCycles)
{
    if (m_active) {
        return storageFailure(QStringLiteral("A continuous data recording is already active"));
    }
    resetState();

    const QString normalizedDirectory = QDir(directory).absolutePath();
    if (!QDir().mkpath(normalizedDirectory)) {
        return storageFailure(
            QStringLiteral("Cannot create continuous data directory '%1'")
                .arg(normalizedDirectory));
    }

    m_descriptor = descriptor;
    m_runMode = runMode;
    m_intervalMs = intervalMs;
    m_maxCycles = maxCycles;
    m_startedAtUs = utcNowUs();
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

    const QString prefix = m_electricalHealthFormat
        ? QStringLiteral("ElectricalHealth")
        : safeFileStem(descriptor.configId);
    const QString timestamp = timestampText(m_startedAtUs,
                                            QStringLiteral("yyyyMMdd_HHmmss_"));
    QString fileName = QStringLiteral("%1_data_%2.txt").arg(prefix, timestamp);
    QString finalPath = QDir(normalizedDirectory).absoluteFilePath(fileName);
    for (int suffix = 1;
         QFileInfo::exists(finalPath) || QFileInfo::exists(finalPath + QStringLiteral(".partial"));
         ++suffix) {
        fileName = QStringLiteral("%1_data_%2_%3.txt").arg(prefix, timestamp).arg(suffix);
        finalPath = QDir(normalizedDirectory).absoluteFilePath(fileName);
    }

    m_outputPath = finalPath;
    m_partialPath = finalPath + QStringLiteral(".partial");
    m_partialFile.setFileName(m_partialPath);
    if (!m_partialFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString error = m_partialFile.errorString();
        resetState();
        return storageFailure(
            QStringLiteral("Cannot open continuous data file '%1': %2")
                .arg(m_partialPath, error));
    }

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
    m_active = true;
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
        row.push_back(failed ? QStringLiteral("NA")
                             : scalarText(sample.values.value(column.valueKey)));
    }
    if (m_electricalHealthFormat) {
        if (failed || !sample.values.contains(QStringLiteral("activate_bits"))) {
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

    QSaveFile output(m_outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        m_outputPath = m_partialPath;
        return storageFailure(
            QStringLiteral("Cannot create continuous data file '%1': %2")
                .arg(output.fileName(), output.errorString()));
    }

    const qint64 finishedAtUs = utcNowUs();
    const QString title = m_electricalHealthFormat
        ? QStringLiteral("电气健康连续采集数据")
        : QStringLiteral("%1连续采集数据").arg(m_descriptor.title);
    QString metadata;
    metadata += QStringLiteral("# %1\n").arg(metadataText(title));
    metadata += QStringLiteral("# started_at=%1\n")
                    .arg(timestampText(m_startedAtUs,
                                       QStringLiteral("yyyy-MM-dd HH:mm:ss.")));
    metadata += QStringLiteral("# finished_at=%1\n")
                    .arg(timestampText(finishedAtUs,
                                       QStringLiteral("yyyy-MM-dd HH:mm:ss.")));
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

    bool ok = writeAll(&output, QByteArray::fromHex("EFBBBF")) &&
        writeAll(&output, metadata.toUtf8());
    while (ok && !rows.atEnd()) {
        const QByteArray chunk = rows.read(64 * 1024);
        if (chunk.isEmpty() && rows.error() != QFile::NoError) {
            ok = false;
            break;
        }
        ok = writeAll(&output, chunk);
    }
    rows.close();
    if (!ok || !output.commit()) {
        const QString finalPath = m_outputPath;
        m_outputPath = m_partialPath;
        return storageFailure(
            QStringLiteral("Cannot finalize continuous data file '%1': %2")
                .arg(finalPath, output.errorString()));
    }

    QFile::remove(m_partialPath);
    return {};
}

void ContinuousDataRecorder::cancel()
{
    if (m_partialFile.isOpen()) {
        m_partialFile.close();
    }
    if (!m_partialPath.isEmpty()) {
        QFile::remove(m_partialPath);
    }
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
        return storageFailure(m_writeError);
    }
    return {};
}

void ContinuousDataRecorder::resetState()
{
    m_partialFile.setFileName(QString{});
    m_descriptor = {};
    m_columns.clear();
    m_taskId.clear();
    m_runMode.clear();
    m_outputPath.clear();
    m_partialPath.clear();
    m_writeError.clear();
    m_startedAtUs = 0;
    m_sampleCount = 0;
    m_maxCycles = 0;
    m_intervalMs = 0;
    m_active = false;
    m_electricalHealthFormat = false;
}

} // namespace hwtest::app
