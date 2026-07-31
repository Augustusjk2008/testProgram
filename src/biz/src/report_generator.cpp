#include <biz/i_report_generator.h>

// Qt 文件与流处理
#include <QBuffer>          // 内存缓冲区（用于 XML 构建）
#include <QDir>             // 目录操作
#include <QFileInfo>        // 文件路径信息
#include <QSaveFile>        // 原子保存（先写临时文件再替换）
#include <QTextStream>      // 文本流（HTML/CSV/纯文本）
#include <QXmlStreamWriter> // XML 流式写入

namespace hwtest::biz {
namespace { // 匿名命名空间 - 内部实现

// ---- 辅助函数 ----

// 构造 Status
Status makeStatus(ErrorCode code, const QString& message)
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

// 构造失败的 Result<ReportPath>
Result<ReportPath> failure(ErrorCode code, const QString& message)
{
    Result<ReportPath> result;
    result.status = makeStatus(code, message);
    return result;
}

// 判断某条结果是否在过滤器内
bool includesResult(const TestResult& result, const QList<QString>& filter)
{
    return filter.isEmpty() ||
           filter.contains(result.stepId) ||
           filter.contains(result.testItemId) ||
           filter.contains(result.algorithmId);
}

// 按过滤器筛选结果
QVector<TestResult> filteredResults(const QVector<TestResult>& results,
                                    const QList<QString>& filter)
{
    QVector<TestResult> selected;
    for (const TestResult& r : results) {
        if (includesResult(r, filter))
            selected.append(r);
    }
    return selected;
}

// CSV 单元格转义：防公式注入，处理双引号
QString csvCell(const QString& value)
{
    QString escaped = value;
    // 防止 CSV 注入：以 = + - @ \t \r 开头时前面加单引号
    if (!escaped.isEmpty() &&
        (escaped.startsWith(QLatin1Char('=')) ||
         escaped.startsWith(QLatin1Char('+')) ||
         escaped.startsWith(QLatin1Char('-')) ||
         escaped.startsWith(QLatin1Char('@')) ||
         escaped.startsWith(QLatin1Char('\t')) ||
         escaped.startsWith(QLatin1Char('\r')))) {
        escaped.prepend(QLatin1Char('\''));
    }
    // 双引号转义为两个双引号
    escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    // 用双引号包裹整个单元格
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

// 将 taskId 转为安全的文件名主干（替换非法字符）
QString safeFileStem(const QString& taskId)
{
    QString stem = taskId.trimmed();
    if (stem.isEmpty()) return QStringLiteral("report");
    for (int i = 0; i < stem.size(); ++i) {
        const QChar ch = stem.at(i);
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_')) {
            stem[i] = QLatin1Char('_');
        }
    }
    return stem;
}

// ---- 各格式生成函数 ----

// HTML 报告
QByteArray makeHtml(const QVector<TestResult>& results, const ReportOptions& options)
{
    QString content;
    QTextStream stream(&content);
    stream << "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
           << options.title.toHtmlEscaped()
           << "</title></head><body><h1>" << options.title.toHtmlEscaped()
           << "</h1><table><thead><tr>"
              "<th>Step</th><th>Test item</th><th>Algorithm</th>"
              "<th>Verdict</th><th>Error</th><th>Message</th><th>Attempts</th>"
              "</tr></thead><tbody>";
    for (const TestResult& r : results) {
        stream << "<tr><td>" << r.stepId.toHtmlEscaped()
               << "</td><td>" << r.testItemId.toHtmlEscaped()
               << "</td><td>" << r.algorithmId.toHtmlEscaped()
               << "</td><td>" << testVerdictToString(r.verdict).toHtmlEscaped()
               << "</td><td>" << errorCodeToString(r.errorCode).toHtmlEscaped()
               << "</td><td>" << r.message.toHtmlEscaped()
               << "</td><td>" << r.attempts << "</td></tr>";
    }
    stream << "</tbody></table></body></html>";
    return content.toUtf8();
}

// CSV 报告
QByteArray makeCsv(const QVector<TestResult>& results)
{
    QString content;
    QTextStream stream(&content);
    // 表头
    stream << "stepId,testItemId,algorithmId,verdict,errorCode,message,attempts\n";
    for (const TestResult& r : results) {
        stream << csvCell(r.stepId) << ','
               << csvCell(r.testItemId) << ','
               << csvCell(r.algorithmId) << ','
               << csvCell(testVerdictToString(r.verdict)) << ','
               << csvCell(errorCodeToString(r.errorCode)) << ','
               << csvCell(r.message) << ','
               << r.attempts << '\n';
    }
    return content.toUtf8();
}

// 纯文本报告
QByteArray makeText(const QVector<TestResult>& results, const ReportOptions& options)
{
    QString content;
    QTextStream stream(&content);
    stream << options.title << '\n';
    stream << QString(options.title.size(), QLatin1Char('=')) << "\n\n"; // 标题下划线
    for (const TestResult& r : results) {
        stream << "Step: " << r.stepId << '\n'
               << "Test item: " << r.testItemId << '\n'
               << "Algorithm: " << r.algorithmId << '\n'
               << "Verdict: " << testVerdictToString(r.verdict) << '\n'
               << "Error: " << errorCodeToString(r.errorCode) << '\n'
               << "Message: " << r.message << '\n'
               << "Attempts: " << r.attempts << "\n\n";
    }
    return content.toUtf8();
}

// XML 报告（含测量数据）
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
    for (const TestResult& r : results) {
        writer.writeStartElement(QStringLiteral("result"));
        writer.writeAttribute(QStringLiteral("stepId"), r.stepId);
        writer.writeAttribute(QStringLiteral("testItemId"), r.testItemId);
        writer.writeAttribute(QStringLiteral("algorithmId"), r.algorithmId);
        writer.writeAttribute(QStringLiteral("verdict"), testVerdictToString(r.verdict));
        writer.writeAttribute(QStringLiteral("errorCode"), errorCodeToString(r.errorCode));
        writer.writeAttribute(QStringLiteral("attempts"), QString::number(r.attempts));
        writer.writeTextElement(QStringLiteral("message"), r.message);
        // 测量记录
        writer.writeStartElement(QStringLiteral("measurements"));
        for (const MeasurementRecord& m : r.measurements) {
            writer.writeStartElement(QStringLiteral("measurement"));
            writer.writeAttribute(QStringLiteral("name"), m.name);
            writer.writeAttribute(QStringLiteral("expected"), m.expected.toString());
            writer.writeAttribute(QStringLiteral("actual"), m.actual.toString());
            writer.writeAttribute(QStringLiteral("tolerance"), m.tolerance.toString());
            writer.writeAttribute(QStringLiteral("unit"), m.unit);
            writer.writeEndElement(); // measurement
        }
        writer.writeEndElement(); // measurements
        writer.writeEndElement(); // result
    }
    writer.writeEndElement(); // results
    writer.writeEndElement(); // report
    writer.writeEndDocument();
    return content;
}

// ========================================================================
// ReportGenerator - 报告生成器具体实现
// ========================================================================
class ReportGenerator final : public IReportGenerator {
public:
    Result<ReportPath> createReport(const QVector<TestResult>& results,
                                    const ReportOptions& options) override
    {
        // 必须恰好选择一种格式
        const int formatCount = static_cast<int>(options.html) +
                                static_cast<int>(options.csv) +
                                static_cast<int>(options.txt) +
                                static_cast<int>(options.xml);
        if (formatCount != 1) {
            return failure(ErrorCode::ParameterRangeError,
                           QStringLiteral("Exactly one report format must be selected"));
        }

        // 确定输出目录
        const QString outputDir = options.outDir.trimmed().isEmpty()
            ? QDir::currentPath() : options.outDir;
        if (!QDir().mkpath(outputDir)) {
            return failure(ErrorCode::DiskFull,
                           QStringLiteral("Cannot create report directory '%1'").arg(outputDir));
        }

        // 按过滤器筛选结果
        const QVector<TestResult> selected = filteredResults(results, options.itemFilter);

        // 根据选择的格式生成内容
        QString extension;
        QByteArray content;
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

        // 构建报告文件路径
        const QString reportPath = QDir(outputDir).absoluteFilePath(
            QStringLiteral("%1.%2").arg(safeFileStem(options.taskId), extension));

        // 原子写入
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

} // 匿名命名空间

// 工厂函数
IReportGenerator* createReportGeneratorImplementation()
{
    return new ReportGenerator;
}

} // namespace hwtest::biz