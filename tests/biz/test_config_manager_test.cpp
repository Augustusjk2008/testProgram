#include <gtest/gtest.h>

#include <biz/biz_types.h>
#include <biz/test_config_manager.h>
#include <biz/test_plan_builder.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <type_traits>
#include <algorithm>

#include "test_support.h"

namespace hwtest::biz {
namespace {

void expectCriterionEqual(const Criterion& expected, const Criterion& actual)
{
    EXPECT_EQ(expected.metric, actual.metric);
    EXPECT_EQ(expected.op, actual.op);
    EXPECT_EQ(expected.ref, actual.ref);
    EXPECT_DOUBLE_EQ(expected.lo, actual.lo);
    EXPECT_DOUBLE_EQ(expected.hi, actual.hi);
    EXPECT_DOUBLE_EQ(expected.tol, actual.tol);
    EXPECT_EQ(expected.passIfMatched, actual.passIfMatched);
}

void expectStepEqual(const TestStep& expected, const TestStep& actual)
{
    EXPECT_EQ(expected.stepId, actual.stepId);
    EXPECT_EQ(expected.testItemId, actual.testItemId);
    EXPECT_EQ(expected.name, actual.name);
    EXPECT_EQ(expected.type, actual.type);
    EXPECT_EQ(expected.board, actual.board);
    EXPECT_EQ(expected.algorithmId, actual.algorithmId);
    EXPECT_EQ(expected.parameters, actual.parameters);
    EXPECT_EQ(expected.timeoutMs, actual.timeoutMs);
    EXPECT_EQ(expected.retryCount, actual.retryCount);
    EXPECT_EQ(expected.enabled, actual.enabled);
    EXPECT_EQ(expected.dependsOn, actual.dependsOn);
    ASSERT_EQ(expected.criteria.size(), actual.criteria.size());
    for (int index = 0; index < expected.criteria.size(); ++index) {
        expectCriterionEqual(expected.criteria.at(index), actual.criteria.at(index));
    }
}

void expectRequirementEqual(const HardwareRequirement& expected,
                            const HardwareRequirement& actual)
{
    EXPECT_EQ(expected.requirementId, actual.requirementId);
    EXPECT_EQ(expected.deviceId, actual.deviceId);
    EXPECT_EQ(expected.adapterId, actual.adapterId);
    EXPECT_EQ(expected.requiredModules, actual.requiredModules);
    EXPECT_EQ(expected.requiredResources, actual.requiredResources);
    EXPECT_EQ(expected.priority, actual.priority);
    EXPECT_EQ(expected.match, actual.match);
}

void expectProfileEqual(const ProtocolProfile& expected, const ProtocolProfile& actual)
{
    EXPECT_EQ(expected.id, actual.id);
    EXPECT_EQ(expected.busType, actual.busType);
    EXPECT_EQ(expected.payloadEncoding, actual.payloadEncoding);
    EXPECT_EQ(expected.frameFormat, actual.frameFormat);
    EXPECT_EQ(expected.timing, actual.timing);
    EXPECT_EQ(expected.responseRules, actual.responseRules);
    EXPECT_EQ(expected.fieldMapping, actual.fieldMapping);
}

void expectConfigEqual(const TestConfig& expected, const TestConfig& actual)
{
    EXPECT_EQ(expected.schemaVersion, actual.schemaVersion);
    EXPECT_EQ(expected.configId, actual.configId);
    EXPECT_EQ(expected.productModel, actual.productModel);
    EXPECT_EQ(expected.productName, actual.productName);
    EXPECT_EQ(expected.configVersion, actual.configVersion);

    ASSERT_EQ(expected.steps.size(), actual.steps.size());
    for (int index = 0; index < expected.steps.size(); ++index) {
        expectStepEqual(expected.steps.at(index), actual.steps.at(index));
    }

    ASSERT_EQ(expected.hardwareRequirements.size(), actual.hardwareRequirements.size());
    for (int index = 0; index < expected.hardwareRequirements.size(); ++index) {
        expectRequirementEqual(expected.hardwareRequirements.at(index),
                               actual.hardwareRequirements.at(index));
    }

    ASSERT_EQ(expected.protocolProfiles.size(), actual.protocolProfiles.size());
    for (int index = 0; index < expected.protocolProfiles.size(); ++index) {
        expectProfileEqual(expected.protocolProfiles.at(index), actual.protocolProfiles.at(index));
    }

    EXPECT_EQ(expected.executionConfig, actual.executionConfig);
    EXPECT_EQ(expected.safetyPolicy.outputLimits, actual.safetyPolicy.outputLimits);
    EXPECT_EQ(expected.safetyPolicy.safeState, actual.safetyPolicy.safeState);
    EXPECT_EQ(expected.safetyPolicy.enterSafeStateOnStop,
              actual.safetyPolicy.enterSafeStateOnStop);
    EXPECT_EQ(expected.safetyPolicy.enterSafeStateOnError,
              actual.safetyPolicy.enterSafeStateOnError);
    EXPECT_DOUBLE_EQ(expected.safetyPolicy.daMinVoltage, actual.safetyPolicy.daMinVoltage);
    EXPECT_DOUBLE_EQ(expected.safetyPolicy.daMaxVoltage, actual.safetyPolicy.daMaxVoltage);
    EXPECT_EQ(expected.safetyPolicy.doMinSwitchIntervalMs,
              actual.safetyPolicy.doMinSwitchIntervalMs);
    EXPECT_EQ(expected.safetyPolicy.canSendMaxHz, actual.safetyPolicy.canSendMaxHz);
    EXPECT_EQ(expected.safetyPolicy.resourceLockTimeoutMs,
              actual.safetyPolicy.resourceLockTimeoutMs);

    EXPECT_EQ(expected.runtimeConfig.parallelEnabled, actual.runtimeConfig.parallelEnabled);
    EXPECT_EQ(expected.runtimeConfig.maxParallel, actual.runtimeConfig.maxParallel);
    EXPECT_EQ(expected.runtimeConfig.defaultTimeoutMs, actual.runtimeConfig.defaultTimeoutMs);
    EXPECT_EQ(expected.runtimeConfig.defaultRetryCount, actual.runtimeConfig.defaultRetryCount);
    EXPECT_EQ(expected.runtimeConfig.retryIntervalMs, actual.runtimeConfig.retryIntervalMs);
    EXPECT_EQ(expected.runtimeConfig.taskPriorityDefault, actual.runtimeConfig.taskPriorityDefault);
    EXPECT_EQ(expected.runtimeConfig.pauseAutoReleaseMs,
              actual.runtimeConfig.pauseAutoReleaseMs);
    EXPECT_EQ(expected.runtimeConfig.stopOnFirstFailure,
              actual.runtimeConfig.stopOnFirstFailure);
    EXPECT_EQ(expected.runtimeConfig.allowResume, actual.runtimeConfig.allowResume);
    EXPECT_EQ(expected.runtimeConfig.reportDir, actual.runtimeConfig.reportDir);
    EXPECT_EQ(expected.runtimeConfig.logDir, actual.runtimeConfig.logDir);
    EXPECT_EQ(expected.runtimeConfig.logRotateBytes, actual.runtimeConfig.logRotateBytes);
    EXPECT_EQ(expected.runtimeConfig.logKeepFiles, actual.runtimeConfig.logKeepFiles);
    EXPECT_EQ(expected.runtimeConfig.tags, actual.runtimeConfig.tags);
    EXPECT_EQ(expected.reportFields, actual.reportFields);
}

} // namespace

static_assert(std::is_same_v<decltype(TestConfig{}.executionConfig), QVariantMap>);

TEST(TestConfigManagerTest, SavesAndLoadsEveryCurrentConfigurationField)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig expected = test::makeCompleteConfig();
    expected.steps[0].enabled = false;
    expected.safetyPolicy.enterSafeStateOnStop = false;
    expected.safetyPolicy.enterSafeStateOnError = false;
    expected.runtimeConfig.taskPriorityDefault = 3;
    expected.runtimeConfig.pauseAutoReleaseMs = 80;
    expected.steps[0].criteria[0].op = CmpOp::Equal;
    expected.steps[0].criteria[0].ref = QStringLiteral("READY");
    const ConfigPath path = temporaryDirectory.filePath(QStringLiteral("complete.testcfg"));
    TestConfigManager manager;

    ASSERT_TRUE(manager.save(path, expected).ok());
    const Result<TestConfig> loaded = manager.load(path);

    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    expectConfigEqual(expected, loaded.value);
}

TEST(TestConfigManagerTest, MigratesLegacyExecutionKeyAndStringCriterionWithoutDataLoss)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfigManager manager;
    const ConfigPath legacyPath = temporaryDirectory.filePath(QStringLiteral("legacy.testcfg"));
    TestConfig expected = test::makeCompleteConfig();
    ASSERT_TRUE(manager.save(legacyPath, expected).ok());

    QFile legacyFile(legacyPath);
    ASSERT_TRUE(legacyFile.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(legacyFile.readAll()).object();
    legacyFile.close();

    root.insert(QStringLiteral("halConfig"), root.take(QStringLiteral("executionConfig")));
    QJsonArray steps = root.value(QStringLiteral("steps")).toArray();
    QJsonObject firstStep = steps.at(0).toObject();
    QJsonArray criteria = firstStep.value(QStringLiteral("criteria")).toArray();
    QJsonObject firstCriterion = criteria.at(0).toObject();
    firstCriterion.insert(QStringLiteral("op"), QStringLiteral("Equal"));
    firstCriterion.insert(QStringLiteral("ref"), QStringLiteral("READY"));
    criteria.replace(0, firstCriterion);
    firstStep.insert(QStringLiteral("criteria"), criteria);
    steps.replace(0, firstStep);
    root.insert(QStringLiteral("steps"), steps);

    ASSERT_TRUE(legacyFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GE(legacyFile.write(QJsonDocument(root).toJson()), 0);
    legacyFile.close();

    const Result<TestConfig> loaded = manager.load(legacyPath);
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    EXPECT_EQ(loaded.value.executionConfig, expected.executionConfig);
    ASSERT_FALSE(loaded.value.steps.at(0).criteria.isEmpty());
    EXPECT_EQ(loaded.value.steps.at(0).criteria.at(0).op, CmpOp::Equal);
    EXPECT_EQ(loaded.value.steps.at(0).criteria.at(0).ref, QStringLiteral("READY"));

    const ConfigPath migratedPath = temporaryDirectory.filePath(QStringLiteral("migrated.testcfg"));
    ASSERT_TRUE(manager.save(migratedPath, loaded.value).ok());
    QFile migratedFile(migratedPath);
    ASSERT_TRUE(migratedFile.open(QIODevice::ReadOnly));
    const QJsonObject migrated = QJsonDocument::fromJson(migratedFile.readAll()).object();
    EXPECT_TRUE(migrated.contains(QStringLiteral("executionConfig")));
    EXPECT_FALSE(migrated.contains(QStringLiteral("halConfig")));
}

TEST(TestConfigManagerTest, RejectsMissingRequiredIdentityStepsAndInvalidEnumBeforeSave)
{
    TestConfigManager manager;
    TestConfig invalid = test::makeCompleteConfig();
    invalid.schemaVersion.clear();
    invalid.configId.clear();
    invalid.productModel.clear();
    invalid.steps[0].stepId.clear();
    invalid.steps[0].criteria[0].op = static_cast<CmpOp>(999);

    const Result<QVector<QString>> validation = manager.validate(invalid);
    EXPECT_FALSE(validation.ok());
    EXPECT_EQ(validation.status.code, ErrorCode::ConfigSchemaError);
}

TEST(TestConfigManagerTest, RejectsUnknownJsonFieldsInsteadOfSilentlyDroppingThem)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const ConfigPath path = temporaryDirectory.filePath(QStringLiteral("unknown-field.testcfg"));
    TestConfigManager manager;
    ASSERT_TRUE(manager.save(path, test::makeCompleteConfig()).ok());

    QFile input(path);
    ASSERT_TRUE(input.open(QIODevice::ReadOnly));
    const QJsonDocument savedDocument = QJsonDocument::fromJson(input.readAll());
    ASSERT_TRUE(savedDocument.isObject());

    QJsonObject root = savedDocument.object();
    root.insert(QStringLiteral("unexpectedRootField"), true);
    input.close();

    ASSERT_TRUE(input.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GE(input.write(QJsonDocument(root).toJson()), 0);
    input.close();

    const Result<TestConfig> loaded = manager.load(path);
    EXPECT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.status.code, ErrorCode::ConfigSchemaError);
}

TEST(TestConfigManagerTest, RejectsIllegalStepTimeoutAndRetryValues)
{
    TestConfig invalid = test::makeCompleteConfig();
    invalid.steps[0].timeoutMs = -1;
    invalid.steps[0].retryCount = -2;

    TestConfigManager manager;
    const Result<QVector<QString>> validation = manager.validate(invalid);

    EXPECT_FALSE(validation.ok());
    EXPECT_EQ(validation.status.code, ErrorCode::ParameterRangeError);
}

TEST(TestConfigManagerTest, RejectsPersistedIllegalStepTimeoutAndRetryValuesDuringLoad)
{
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const ConfigPath path = temporaryDirectory.filePath(QStringLiteral("invalid-step.testcfg"));
    TestConfigManager manager;
    ASSERT_TRUE(manager.save(path, test::makeCompleteConfig()).ok());

    QFile input(path);
    ASSERT_TRUE(input.open(QIODevice::ReadOnly));
    QJsonDocument savedDocument = QJsonDocument::fromJson(input.readAll());
    ASSERT_TRUE(savedDocument.isObject());

    QJsonObject root = savedDocument.object();
    QJsonArray steps = root.value(QStringLiteral("steps")).toArray();
    ASSERT_FALSE(steps.isEmpty());
    QJsonObject firstStep = steps.at(0).toObject();
    firstStep.insert(QStringLiteral("timeoutMs"), -1);
    firstStep.insert(QStringLiteral("retryCount"), -2);
    steps.replace(0, firstStep);
    root.insert(QStringLiteral("steps"), steps);
    input.close();

    ASSERT_TRUE(input.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GE(input.write(QJsonDocument(root).toJson()), 0);
    input.close();

    const Result<TestConfig> loaded = manager.load(path);
    EXPECT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.status.code, ErrorCode::ParameterRangeError);
}

TEST(TestConfigManagerTest, ImportedColleagueSampleLoadsAndBuildsThroughMigrationBoundary)
{
    const ConfigPath samplePath = QString::fromUtf8(HWTEST_BIZ_ATTACHMENT_SAMPLE_PATH);
    if (!QFileInfo::exists(samplePath)) {
        GTEST_SKIP() << "Imported attachment fixture is not present";
    }

    TestConfigManager manager;
    const Result<TestConfig> loaded = manager.load(samplePath);
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    EXPECT_FALSE(loaded.value.executionConfig.isEmpty());

    TestPlanBuilder builder;
    const Result<TestPlan> plan = builder.build(loaded.value);
    ASSERT_TRUE(plan.ok()) << plan.status.error.message.toStdString();
    ASSERT_EQ(plan.value.steps.size(), 4);
    for (const TestStep& step : plan.value.steps) {
        for (const StepId& dependency : step.dependsOn) {
            EXPECT_TRUE(std::any_of(plan.value.steps.cbegin(),
                                    plan.value.steps.cend(),
                                    [&dependency](const TestStep& candidate) {
                                        return candidate.stepId == dependency;
                                    }));
        }
    }
}

} // namespace hwtest::biz
