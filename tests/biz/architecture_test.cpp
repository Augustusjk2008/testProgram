#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>

namespace {

struct ForbiddenRule {
    QString name;
    QRegularExpression expression;
};

QVector<ForbiddenRule> forbiddenRules()
{
    return {
        {QStringLiteral("hal include path"),
         QRegularExpression(QStringLiteral(R"((?:^|[^A-Za-z0-9_])hal[\\/])"),
                            QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("hwtest::hal namespace"),
         QRegularExpression(QStringLiteral(R"(\bhwtest\s*::\s*hal\b)"))},
        {QStringLiteral("IHal type"),
         QRegularExpression(QStringLiteral(R"(\bIHal[A-Za-z0-9_]*)"))},
        {QStringLiteral("HalStatus type"),
         QRegularExpression(QStringLiteral(R"(\bHalStatus[A-Za-z0-9_]*)"))},
        {QStringLiteral("OperationOptions type"),
         QRegularExpression(QStringLiteral(R"(\bOperationOptions\b)"))},
        {QStringLiteral("hwtest_hal target"),
         QRegularExpression(QStringLiteral(R"(\bhwtest_hal\b)"))},
        {QStringLiteral("socket dependency"),
         QRegularExpression(QStringLiteral("socket"), QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("MeasurementBase or MeasurementFactory"),
         QRegularExpression(QStringLiteral(R"(\bMeasurement(?:Base|Factory)[A-Za-z0-9_]*)"))},
    };
}

int lineNumberAt(const QString& text, qsizetype characterOffset)
{
    return text.left(characterOffset).count(QLatin1Char('\n')) + 1;
}

QStringList scanTree(const QString& root, const QStringList& excludedFileNames = {})
{
    const QVector<ForbiddenRule> rules = forbiddenRules();
    QStringList violations;
    QDirIterator iterator(root,
                          QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (excludedFileNames.contains(QFileInfo(path).fileName())) {
            continue;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            violations.append(QStringLiteral("%1: unreadable file").arg(QDir::cleanPath(path)));
            continue;
        }
        const QString text = QString::fromUtf8(file.readAll());
        for (const ForbiddenRule& rule : rules) {
            QRegularExpressionMatchIterator matches = rule.expression.globalMatch(text);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                violations.append(QStringLiteral("%1:%2: %3")
                                      .arg(QDir::cleanPath(path))
                                      .arg(lineNumberAt(text, match.capturedStart()))
                                      .arg(rule.name));
            }
        }
    }
    return violations;
}

} // namespace

TEST(BizArchitectureTest, SourceTreeDoesNotReachForbiddenLayers)
{
    const QString sourceRoot = QString::fromUtf8(HWTEST_BIZ_SOURCE_DIR);
    ASSERT_TRUE(QDir(sourceRoot).exists()) << sourceRoot.toStdString();

    const QVector<ForbiddenRule> rules = forbiddenRules();
    QStringList violations;
    int scannedFiles = 0;

    QDirIterator iterator(sourceRoot,
                           QDir::Files | QDir::NoDotAndDotDot,
                           QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        ++scannedFiles;
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
        const QString text = QString::fromUtf8(file.readAll());

        for (const ForbiddenRule& rule : rules) {
            QRegularExpressionMatchIterator matches = rule.expression.globalMatch(text);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                violations.append(QStringLiteral("%1:%2: %3")
                                      .arg(QDir::cleanPath(path))
                                      .arg(lineNumberAt(text, match.capturedStart()))
                                      .arg(rule.name));
            }
        }
    }

    ASSERT_GT(scannedFiles, 0) << sourceRoot.toStdString();
    EXPECT_TRUE(violations.isEmpty())
        << "BIZ source boundary violations:\n"
        << violations.join(QLatin1Char('\n')).toStdString();
}

TEST(BizArchitectureTest, MeasurementRecordRemainsAnAllowedPureResultName)
{
    const QString allowedName = QStringLiteral("MeasurementRecord");
    const QRegularExpression forbiddenMeasurementType(
        QStringLiteral(R"(\bMeasurement(?:Base|Factory)[A-Za-z0-9_]*)"));

    EXPECT_FALSE(forbiddenMeasurementType.match(allowedName).hasMatch());
}

TEST(BizArchitectureTest, TestTreeDoesNotReachForbiddenLayers)
{
    const QString testRoot = QString::fromUtf8(HWTEST_BIZ_TEST_SOURCE_DIR);
    ASSERT_TRUE(QDir(testRoot).exists()) << testRoot.toStdString();

    const QStringList violations = scanTree(
        testRoot,
        {QStringLiteral("architecture_test.cpp")});
    EXPECT_TRUE(violations.isEmpty())
        << "BIZ test boundary violations:\n"
        << violations.join(QLatin1Char('\n')).toStdString();
}
