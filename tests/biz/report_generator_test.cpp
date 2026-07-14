#include <gtest/gtest.h>

#include <biz/biz_factory.h>
#include <biz/i_report_generator.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <memory>

namespace hwtest::biz {
namespace {

using ReportGeneratorHandle = std::unique_ptr<IReportGenerator, void (*)(IReportGenerator*)>;

QVector<TestResult> sampleResults()
{
    TestResult result;
    result.stepId = QStringLiteral("STEP_001");
    result.testItemId = QStringLiteral("ITEM_001");
    result.algorithmId = QStringLiteral("algorithm.report");
    result.verdict = TestVerdict::Pass;
    result.message = QStringLiteral("reportable result");
    result.attempts = 1;

    MeasurementRecord record;
    record.name = QStringLiteral("value");
    record.expected = 5.0;
    record.actual = 5.0;
    record.tolerance = 0.1;
    record.unit = QStringLiteral("V");
    result.measurements.append(record);
    return {result};
}

ReportOptions optionsForSingleFormat(const QString& outputDirectory,
                                     bool html,
                                     bool csv,
                                     bool txt,
                                     bool xml)
{
    ReportOptions options;
    options.outDir = outputDirectory;
    options.title = QStringLiteral("BIZ report contract");
    options.taskId = QStringLiteral("task-report");
    options.html = html;
    options.csv = csv;
    options.txt = txt;
    options.xml = xml;
    return options;
}

void expectBasicReport(const ReportOptions& options,
                       const QString& expectedSuffix,
                       const QString& requiredMarker = {})
{
    ReportGeneratorHandle generator(createReportGenerator(), destroyReportGenerator);
    ASSERT_NE(generator, nullptr);

    const Result<ReportPath> created = generator->createReport(sampleResults(), options);
    ASSERT_TRUE(created.ok()) << created.status.error.message.toStdString();

    const QFileInfo reportInfo(created.value);
    EXPECT_TRUE(reportInfo.exists()) << created.value.toStdString();
    EXPECT_EQ(reportInfo.suffix().toLower(), expectedSuffix);

    QFile reportFile(created.value);
    ASSERT_TRUE(reportFile.open(QIODevice::ReadOnly)) << created.value.toStdString();
    const QString content = QString::fromUtf8(reportFile.readAll());
    EXPECT_FALSE(content.trimmed().isEmpty());
    EXPECT_TRUE(content.contains(QStringLiteral("STEP_001")));
    if (!requiredMarker.isEmpty()) {
        EXPECT_TRUE(content.contains(requiredMarker, Qt::CaseInsensitive))
            << content.toStdString();
    }

    if (expectedSuffix == QStringLiteral("xml")) {
        QXmlStreamReader reader(content);
        QString rootName;
        while (!reader.atEnd()) {
            reader.readNext();
            if (rootName.isEmpty() && reader.isStartElement()) {
                rootName = reader.name().toString();
            }
        }
        EXPECT_FALSE(reader.hasError()) << reader.errorString().toStdString();
        EXPECT_FALSE(rootName.isEmpty());
    }
}

} // namespace

TEST(ReportGeneratorTest, CreatesBasicHtmlOutput)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    expectBasicReport(optionsForSingleFormat(temporaryDirectory.path(), true, false, false, false),
                      QStringLiteral("html"),
                      QStringLiteral("<html"));
}

TEST(ReportGeneratorTest, CreatesBasicCsvOutput)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    expectBasicReport(optionsForSingleFormat(temporaryDirectory.path(), false, true, false, false),
                      QStringLiteral("csv"),
                      QStringLiteral(","));
}

TEST(ReportGeneratorTest, CreatesBasicTextOutput)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    expectBasicReport(optionsForSingleFormat(temporaryDirectory.path(), false, false, true, false),
                      QStringLiteral("txt"),
                      QStringLiteral("BIZ report contract"));
}

TEST(ReportGeneratorTest, CreatesBasicXmlOutput)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    expectBasicReport(optionsForSingleFormat(temporaryDirectory.path(), false, false, false, true),
                      QStringLiteral("xml"),
                      QStringLiteral("<"));
}

TEST(ReportGeneratorTest, DefaultOptionsSelectExactlyOneHtmlReport)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    ReportOptions options;
    options.outDir = temporaryDirectory.path();
    options.taskId = QStringLiteral("default-report");
    expectBasicReport(options, QStringLiteral("html"), QStringLiteral("<html"));
}

TEST(ReportGeneratorTest, RejectsZeroOrMultipleSelectedFormats)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    ReportGeneratorHandle generator(createReportGenerator(), destroyReportGenerator);
    ASSERT_NE(generator, nullptr);

    ReportOptions none = optionsForSingleFormat(temporaryDirectory.path(), false, false, false, false);
    EXPECT_EQ(generator->createReport(sampleResults(), none).status.code,
              ErrorCode::ParameterRangeError);

    ReportOptions multiple = optionsForSingleFormat(temporaryDirectory.path(), true, true, false, false);
    EXPECT_EQ(generator->createReport(sampleResults(), multiple).status.code,
              ErrorCode::ParameterRangeError);
}

TEST(ReportGeneratorTest, FiltersByBusinessTestItemId)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    ReportGeneratorHandle generator(createReportGenerator(), destroyReportGenerator);
    ASSERT_NE(generator, nullptr);

    QVector<TestResult> results = sampleResults();
    TestResult excluded = results.at(0);
    excluded.stepId = QStringLiteral("STEP_002");
    excluded.testItemId = QStringLiteral("ITEM_002");
    results.append(excluded);

    ReportOptions options = optionsForSingleFormat(temporaryDirectory.path(), false, true, false, false);
    options.itemFilter = {QStringLiteral("ITEM_001")};
    const Result<ReportPath> created = generator->createReport(results, options);
    ASSERT_TRUE(created.ok()) << created.status.error.message.toStdString();
    QFile file(created.value);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(file.readAll());
    EXPECT_TRUE(content.contains(QStringLiteral("STEP_001")));
    EXPECT_FALSE(content.contains(QStringLiteral("STEP_002")));
}

} // namespace hwtest::biz
