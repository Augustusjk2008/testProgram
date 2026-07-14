#include <biz/i_report_generator.h>

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QXmlStreamWriter>

namespace hwtest::biz {
namespace {

Status makeStatus(ErrorCode code, const QString& message)
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

Result<ReportPath> failure(ErrorCode code, const QString& message)
{
    Result<ReportPath> result;
    result.status = makeStatus(code, message);
    return result;
}

bool includesResult(const TestResult& result, const QList<QString>& filter)
{
    return filter.isEmpty() || filter.contains(result.stepId) ||
        filter.contains(result.testItemId) || filter.contains(result.algorithmId);
}

QVector<TestResult> filteredResults(const QVector<TestResult>& results, const QList<QString>& filter)
{
    QVector<TestResult> selected;
    for (const TestResult& result : results) {
        if (includesResult(result, filter)) {
            selected.append(result);
        }
    }
    return selected;
}

QString csvCell(const QString& value)
{
    QString escaped = value;
    if (!escaped.isEmpty() &&
        (escaped.startsWith(QLatin1Char('=')) || escaped.startsWith(QLatin1Char('+')) ||
         escaped.startsWith(QLatin1Char('-')) || escaped.startsWith(QLatin1Char('@')) ||
         escaped.startsWith(QLatin1Char('\t')) || escaped.startsWith(QLatin1Char('\r')))) {
        escaped.prepend(QLatin1Char('\''));
    }
    escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

QString safeFileStem(const QString& taskId)
{
    QString stem = taskId.trimmed();
    if (stem.isEmpty()) {
        return QStringLiteral("report");
    }
    for (int index = 0; index < stem.size(); ++index) {
        const QChar character = stem.at(index);
        if (!character.isLetterOrNumber() && character != QLatin1Char('-') &&
            character != QLatin1Char('_')) {
            stem[index] = QLatin1Char('_');
        }
    }
    return stem;
}

QByteArray makeHtml(const QVector<TestResult>& results, const ReportOptions& options)
{
    QString content;
    QTextStream stream(&content);
    stream << "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
           << options.title.toHtmlEscaped()
           << "</title></head><body><h1>" << options.title.toHtmlEscaped()
           << "</h1><table><thead><tr><th>Step</th><th>Test item</th><th>Algorithm</th><th>Verdict</th>"
              "<th>Error</th><th>Message</th><th>Attempts</th></tr></thead><tbody>";
    for (const TestResult& result : results) {
        stream << "<tr><td>" << result.stepId.toHtmlEscaped() << "</td><td>"
               << result.testItemId.toHtmlEscaped() << "</td><td>"
               << result.algorithmId.toHtmlEscaped() << "</td><td>"
               << testVerdictToString(result.verdict).toHtmlEscaped() << "</td><td>"
               << errorCodeToString(result.errorCode).toHtmlEscaped() << "</td><td>"
               << result.message.toHtmlEscaped() << "</td><td>" << result.attempts
               << "</td></tr>";
    }
    stream << "</tbody></table></body></html>";
    return content.toUtf8();
}

QByteArray makeCsv(const QVector<TestResult>& results)
{
    QString content;
    QTextStream stream(&content);
    stream << "stepId,testItemId,algorithmId,verdict,errorCode,message,attempts\n";
    for (const TestResult& result : results) {
        stream << csvCell(result.stepId) << ',' << csvCell(result.testItemId) << ','
               << csvCell(result.algorithmId) << ','
               << csvCell(testVerdictToString(result.verdict)) << ','
               << csvCell(errorCodeToString(result.errorCode)) << ',' << csvCell(result.message)
               << ',' << result.attempts << '\n';
    }
    return content.toUtf8();
}

QByteArray makeText(const QVector<TestResult>& results, const ReportOptions& options)
{
    QString content;
    QTextStream stream(&content);
    stream << options.title << '\n';
    stream << QString(options.title.size(), QLatin1Char('=')) << "\n\n";
    for (const TestResult& result : results) {
        stream << "Step: " << result.stepId << '\n';
        stream << "Test item: " << result.testItemId << '\n';
        stream << "Algorithm: " << result.algorithmId << '\n';
        stream << "Verdict: " << testVerdictToString(result.verdict) << '\n';
        stream << "Error: " << errorCodeToString(result.errorCode) << '\n';
        stream << "Message: " << result.message << '\n';
        stream << "Attempts: " << result.attempts << "\n\n";
    }
    return content.toUtf8();
}

QByteArray makeXml(const QVector<TestResult>& results, const ReportOptions& options)
{
    QByteArray content;
    QBuffer buffer(&content);
    buffer.open(QIODevice::WriteOnly);
    QXmlStreamWriter writer(&buffer);
    writer.setAutoFormatting(true);
    writer.writeStartDocument();
    writer.writeStartElement(QStringLiteral("report"));
    writer.writeTextElement(QStringLiteral("title"), options.title);
    writer.writeStartElement(QStringLiteral("results"));
    for (const TestResult& result : results) {
        writer.writeStartElement(QStringLiteral("result"));
        writer.writeAttribute(QStringLiteral("stepId"), result.stepId);
        writer.writeAttribute(QStringLiteral("testItemId"), result.testItemId);
        writer.writeAttribute(QStringLiteral("algorithmId"), result.algorithmId);
        writer.writeAttribute(QStringLiteral("verdict"), testVerdictToString(result.verdict));
        writer.writeAttribute(QStringLiteral("errorCode"), errorCodeToString(result.errorCode));
        writer.writeAttribute(QStringLiteral("attempts"), QString::number(result.attempts));
        writer.writeTextElement(QStringLiteral("message"), result.message);
        writer.writeStartElement(QStringLiteral("measurements"));
        for (const MeasurementRecord& measurement : result.measurements) {
            writer.writeStartElement(QStringLiteral("measurement"));
            writer.writeAttribute(QStringLiteral("name"), measurement.name);
            writer.writeAttribute(QStringLiteral("expected"), measurement.expected.toString());
            writer.writeAttribute(QStringLiteral("actual"), measurement.actual.toString());
            writer.writeAttribute(QStringLiteral("tolerance"), measurement.tolerance.toString());
            writer.writeAttribute(QStringLiteral("unit"), measurement.unit);
            writer.writeEndElement();
        }
        writer.writeEndElement();
        writer.writeEndElement();
    }
    writer.writeEndElement();
    writer.writeEndElement();
    writer.writeEndDocument();
    return content;
}

class ReportGenerator final : public IReportGenerator {
public:
    Result<ReportPath> createReport(const QVector<TestResult>& results,
                                    const ReportOptions& options) override
    {
        const int formatCount = static_cast<int>(options.html) + static_cast<int>(options.csv) +
            static_cast<int>(options.txt) + static_cast<int>(options.xml);
        if (formatCount != 1) {
            return failure(ErrorCode::ParameterRangeError,
                           QStringLiteral("Exactly one report format must be selected"));
        }

        const QString outputDirectory = options.outDir.trimmed().isEmpty()
            ? QDir::currentPath()
            : options.outDir;
        if (!QDir().mkpath(outputDirectory)) {
            return failure(ErrorCode::DiskFull,
                           QStringLiteral("Cannot create report directory '%1'").arg(outputDirectory));
        }

        QString extension;
        QByteArray content;
        const QVector<TestResult> selected = filteredResults(results, options.itemFilter);
        if (options.html) {
            extension = QStringLiteral("html");
            content = makeHtml(selected, options);
        } else if (options.csv) {
            extension = QStringLiteral("csv");
            content = makeCsv(selected);
        } else if (options.txt) {
            extension = QStringLiteral("txt");
            content = makeText(selected, options);
        } else {
            extension = QStringLiteral("xml");
            content = makeXml(selected, options);
        }

        const QString reportPath = QDir(outputDirectory).absoluteFilePath(
            QStringLiteral("%1.%2").arg(safeFileStem(options.taskId), extension));
        QSaveFile file(reportPath);
        if (!file.open(QIODevice::WriteOnly)) {
            return failure(ErrorCode::DiskFull,
                           QStringLiteral("Cannot write report '%1': %2")
                               .arg(reportPath, file.errorString()));
        }
        if (file.write(content) != content.size() || !file.commit()) {
            return failure(ErrorCode::DiskFull,
                           QStringLiteral("Cannot save report '%1': %2")
                               .arg(reportPath, file.errorString()));
        }
        return Result<ReportPath>{Status{}, QFileInfo(reportPath).absoluteFilePath()};
    }
};

} // namespace

IReportGenerator* createReportGeneratorImplementation()
{
    return new ReportGenerator;
}

} // namespace hwtest::biz
