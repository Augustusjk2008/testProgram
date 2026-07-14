#include <gtest/gtest.h>

#include <biz/biz_factory.h>
#include <biz/i_test_run_service.h>
#include <biz/test_config_manager.h>

#include <logging/log_types.h>

#include <QTemporaryDir>

#include <memory>

#include "test_support.h"

namespace hwtest::biz {
namespace {

using ServiceHandle = std::unique_ptr<ITestRunService, void (*)(ITestRunService*)>;

ServiceHandle makeService(test::FakeAlgorithmExecutor& executor)
{
    return ServiceHandle(createTestRunService(&executor), destroyTestRunService);
}

ConfigPath saveConfiguration(TestConfigManager& manager,
                             QTemporaryDir& temporaryDirectory,
                             const TestConfig& config,
                             const QString& fileName)
{
    const ConfigPath path = temporaryDirectory.filePath(fileName);
    EXPECT_TRUE(manager.save(path, config).ok()) << path.toStdString();
    return path;
}

bool waitForState(ITestRunService* service, TestState expected, int timeoutMs = 3000)
{
    return test::waitUntil(
        [service, expected] {
            const Result<TestState> state = service->getCurrentState();
            return state.ok() && state.value == expected;
        },
        timeoutMs);
}

bool waitForTerminalState(ITestRunService* service, int timeoutMs = 3000)
{
    return test::waitUntil(
        [service] {
            const Result<TestState> state = service->getCurrentState();
            return state.ok() && (state.value == TestState::Finished ||
                                  state.value == TestState::Error);
        },
        timeoutMs);
}

const TestResult* findResult(const QVector<TestResult>& results, const StepId& stepId)
{
    for (const TestResult& result : results) {
        if (result.stepId == stepId) {
            return &result;
        }
    }
    return nullptr;
}

} // namespace

TEST(TestRunServiceTest, InitializesLoadsAndExecutesEnabledStepsInPlanOrder)
{
    test::ensureQtApplication();
    hwtest::logging::registerLogMetaTypes();
    registerBizMetaTypes();

    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    TestStep disabled = test::makeStep(QStringLiteral("STEP_DISABLED"),
                                       QStringLiteral("ITEM_DISABLED"),
                                       QStringLiteral("Disabled step"));
    disabled.enabled = false;
    config.steps.insert(1, disabled);

    TestConfigManager manager;
    const ConfigPath configurationPath =
        saveConfiguration(manager, temporaryDirectory, config, QStringLiteral("ordered.testcfg"));

    test::FakeAlgorithmExecutor executor;
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);

    QVector<TestResult> producedResults;
    QVector<hwtest::logging::LogEvent> producedLogs;
    QObject::connect(service.get(),
                     &ITestRunService::resultProduced,
                     QCoreApplication::instance(),
                     [&producedResults](const TaskId&, const TestResult& result) {
                         producedResults.append(result);
                     },
                     Qt::QueuedConnection);
    QObject::connect(service.get(),
                     &ITestRunService::logProduced,
                     QCoreApplication::instance(),
                     [&producedLogs](const hwtest::logging::LogEvent& event) {
                         producedLogs.append(event);
                     },
                     Qt::QueuedConnection);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(configurationPath).ok());
    const Result<TaskId> task = service->startTest();
    ASSERT_TRUE(task.ok()) << task.status.error.message.toStdString();
    ASSERT_TRUE(waitForState(service.get(), TestState::Finished));

    const test::FakeAlgorithmExecutor::PrepareSnapshot snapshot = executor.prepareSnapshot();
    EXPECT_EQ(executor.prepareCalls(), 1);
    EXPECT_EQ(snapshot.context.runId, task.value);
    EXPECT_FALSE(snapshot.context.requestId.isEmpty());
    EXPECT_EQ(snapshot.executionConfig, config.executionConfig);
    ASSERT_EQ(snapshot.plan.protocolProfiles.size(), config.protocolProfiles.size());
    EXPECT_EQ(snapshot.plan.protocolProfiles.at(0).id, config.protocolProfiles.at(0).id);
    EXPECT_EQ(snapshot.plan.safetyPolicy.outputLimits, config.safetyPolicy.outputLimits);
    ASSERT_EQ(snapshot.plan.steps.size(), 2);
    EXPECT_EQ(snapshot.plan.steps.at(0).stepId, QStringLiteral("STEP_A"));
    EXPECT_EQ(snapshot.plan.steps.at(1).stepId, QStringLiteral("STEP_B"));

    const QVector<StepId> executionOrder = executor.executeOrder();
    ASSERT_EQ(executionOrder.size(), 2);
    EXPECT_EQ(executionOrder.at(0), QStringLiteral("STEP_A"));
    EXPECT_EQ(executionOrder.at(1), QStringLiteral("STEP_B"));

    ASSERT_TRUE(test::waitUntil([&producedResults] { return producedResults.size() == 2; }, 1000));
    ASSERT_TRUE(test::waitUntil([&producedLogs] { return !producedLogs.isEmpty(); }, 1000));
    for (const hwtest::logging::LogEvent& event : producedLogs) {
        EXPECT_EQ(event.requestId, snapshot.context.requestId);
    }

    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, ValidatesPriorityAndTreatsIdleStopAsIdempotent)
{
    test::ensureQtApplication();
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfigManager manager;
    const ConfigPath configurationPath =
        saveConfiguration(manager,
                          temporaryDirectory,
                          test::makeCompleteConfig(),
                          QStringLiteral("priority.testcfg"));
    test::FakeAlgorithmExecutor executor;
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);

    ASSERT_TRUE(service->initialize().ok());
    EXPECT_TRUE(service->stopTest().ok());
    ASSERT_TRUE(service->loadConfiguration(configurationPath).ok());
    EXPECT_EQ(service->startTest({}, -2).status.code, ErrorCode::ParameterRangeError);
    EXPECT_EQ(service->startTest({}, 0).status.code, ErrorCode::ParameterRangeError);
    EXPECT_EQ(service->startTest({}, 4).status.code, ErrorCode::ParameterRangeError);
    EXPECT_EQ(executor.prepareCalls(), 0);
    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, SkipsDependentStepWhenItsDependencyFails)
{
    test::ensureQtApplication();

    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    TestStep independent = test::makeStep(QStringLiteral("STEP_C"),
                                           QStringLiteral("ITEM_C"),
                                           QStringLiteral("Independent step"));
    config.steps.append(independent);

    test::FakeAlgorithmExecutor executor;
    executor.setScript(config.steps.at(0).stepId,
                       {Result<TestResult>{Status{},
                                           test::makeResult(config.steps.at(0),
                                                            TestVerdict::Fail,
                                                            ErrorCode::SampleFail,
                                                            QStringLiteral("criterion failed"))}});

    TestConfigManager manager;
    const ConfigPath configurationPath =
        saveConfiguration(manager, temporaryDirectory, config, QStringLiteral("dependency.testcfg"));
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);

    QVector<TestResult> producedResults;
    QObject::connect(service.get(),
                     &ITestRunService::resultProduced,
                     QCoreApplication::instance(),
                     [&producedResults](const TaskId&, const TestResult& result) {
                         producedResults.append(result);
                     },
                     Qt::QueuedConnection);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(configurationPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(waitForTerminalState(service.get()));
    ASSERT_TRUE(test::waitUntil([&producedResults] { return producedResults.size() == 3; }, 1000));

    const QVector<StepId> executionOrder = executor.executeOrder();
    ASSERT_EQ(executionOrder.size(), 2);
    EXPECT_EQ(executionOrder.at(0), QStringLiteral("STEP_A"));
    EXPECT_EQ(executionOrder.at(1), QStringLiteral("STEP_C"));

    const TestResult* skipped = findResult(producedResults, QStringLiteral("STEP_B"));
    ASSERT_NE(skipped, nullptr);
    EXPECT_EQ(skipped->verdict, TestVerdict::Skipped);
    EXPECT_EQ(skipped->skipReason, SkipReason::DependencyFailed);

    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, RetriesRetryableFailureAndPublishesActualAttemptCount)
{
    test::ensureQtApplication();

    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    config.steps = {config.steps.at(0)};
    config.steps[0].retryCount = 2;

    test::FakeAlgorithmExecutor executor;
    executor.setScript(config.steps.at(0).stepId,
                       {test::failedResult(config.steps.at(0),
                                           ErrorCode::BusTimeout,
                                           QStringLiteral("first attempt timed out")),
                        test::successfulResult(config.steps.at(0))});

    TestConfigManager manager;
    const ConfigPath configurationPath =
        saveConfiguration(manager, temporaryDirectory, config, QStringLiteral("retry.testcfg"));
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);

    QVector<TestResult> producedResults;
    QObject::connect(service.get(),
                     &ITestRunService::resultProduced,
                     QCoreApplication::instance(),
                     [&producedResults](const TaskId&, const TestResult& result) {
                         producedResults.append(result);
                     },
                     Qt::QueuedConnection);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(configurationPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(waitForState(service.get(), TestState::Finished));
    ASSERT_TRUE(test::waitUntil([&producedResults] { return producedResults.size() == 1; }, 1000));

    const QVector<StepId> executionOrder = executor.executeOrder();
    ASSERT_EQ(executionOrder.size(), 2);
    EXPECT_EQ(executionOrder.at(0), QStringLiteral("STEP_A"));
    EXPECT_EQ(executionOrder.at(1), QStringLiteral("STEP_A"));
    EXPECT_EQ(producedResults.at(0).verdict, TestVerdict::Pass);
    EXPECT_EQ(producedResults.at(0).attempts, 2);

    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, DoesNotRetryNonRetryableBusinessFailure)
{
    test::ensureQtApplication();
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    config.steps = {config.steps.at(0)};
    config.steps[0].retryCount = 2;

    test::FakeAlgorithmExecutor executor;
    executor.setScript(config.steps.at(0).stepId,
                       {test::failedResult(config.steps.at(0),
                                           ErrorCode::PermissionDenied,
                                           QStringLiteral("permission denied")),
                        test::successfulResult(config.steps.at(0))});
    TestConfigManager manager;
    const ConfigPath path = saveConfiguration(manager,
                                               temporaryDirectory,
                                               config,
                                               QStringLiteral("non-retryable.testcfg"));
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);
    QVector<TestResult> results;
    QObject::connect(service.get(),
                     &ITestRunService::resultProduced,
                     QCoreApplication::instance(),
                     [&results](const TaskId&, const TestResult& result) { results.append(result); },
                     Qt::QueuedConnection);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(path).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(waitForState(service.get(), TestState::Error));
    ASSERT_TRUE(test::waitUntil([&results] { return results.size() == 1; }, 1000));
    EXPECT_EQ(executor.executeCallCount(), 1);
    EXPECT_EQ(results.at(0).attempts, 1);
    EXPECT_EQ(results.at(0).errorCode, ErrorCode::PermissionDenied);
    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, NormalizesExecutorResultIdentityToTheScheduledStep)
{
    test::ensureQtApplication();
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    TestResult wrongIdentity = test::makeResult(config.steps.at(0));
    wrongIdentity.stepId = QStringLiteral("WRONG_STEP");
    wrongIdentity.testItemId = QStringLiteral("WRONG_ITEM");
    wrongIdentity.algorithmId = QStringLiteral("wrong.algorithm");
    test::FakeAlgorithmExecutor executor;
    executor.setScript(config.steps.at(0).stepId,
                       {Result<TestResult>{Status{}, wrongIdentity}});
    TestConfigManager manager;
    const ConfigPath path = saveConfiguration(manager,
                                               temporaryDirectory,
                                               config,
                                               QStringLiteral("identity.testcfg"));
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);
    QVector<TestResult> results;
    QObject::connect(service.get(),
                     &ITestRunService::resultProduced,
                     QCoreApplication::instance(),
                     [&results](const TaskId&, const TestResult& result) { results.append(result); },
                     Qt::QueuedConnection);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(path).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(waitForState(service.get(), TestState::Finished));
    ASSERT_TRUE(test::waitUntil([&results] { return results.size() == 2; }, 1000));
    EXPECT_EQ(results.at(0).stepId, config.steps.at(0).stepId);
    EXPECT_EQ(results.at(0).testItemId, config.steps.at(0).testItemId);
    EXPECT_EQ(results.at(0).algorithmId, config.steps.at(0).algorithmId);
    EXPECT_EQ(executor.executeOrder(),
              QVector<StepId>({config.steps.at(0).stepId, config.steps.at(1).stepId}));
    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, PauseResumeAndStopDriveTheExecutorRunControl)
{
    test::ensureQtApplication();

    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    config.steps = {config.steps.at(0)};

    test::FakeAlgorithmExecutor executor;
    executor.blockExecution();
    TestConfigManager manager;
    const ConfigPath configurationPath =
        saveConfiguration(manager, temporaryDirectory, config, QStringLiteral("control.testcfg"));
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(configurationPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(executor.waitForExecutionStart(2000));

    ASSERT_TRUE(service->pauseTest().ok());
    ASSERT_TRUE(waitForState(service.get(), TestState::Paused));
    ASSERT_TRUE(test::waitUntil([&executor] { return executor.sawControl(RunControl::Pause); }, 1000));

    ASSERT_TRUE(service->resumeTest().ok());
    ASSERT_TRUE(waitForState(service.get(), TestState::Running));

    ASSERT_TRUE(service->stopTest(321).ok());
    ASSERT_TRUE(waitForState(service.get(), TestState::Idle));
    const QVector<int> stopTimeouts = executor.requestStopTimeouts();
    ASSERT_EQ(stopTimeouts.size(), 1);
    EXPECT_EQ(stopTimeouts.at(0), 321);
    EXPECT_TRUE(executor.sawControl(RunControl::Stop));

    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, RejectsResetWhileExecutionIsActive)
{
    test::ensureQtApplication();
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    config.steps = {config.steps.at(0)};
    test::FakeAlgorithmExecutor executor;
    executor.blockExecution();
    TestConfigManager manager;
    const ConfigPath path = saveConfiguration(manager,
                                               temporaryDirectory,
                                               config,
                                               QStringLiteral("reset-active.testcfg"));
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(path).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(executor.waitForExecutionStart(2000));
    EXPECT_EQ(service->resetHardware().code, ErrorCode::ResourceBusy);
    EXPECT_EQ(executor.portCallSnapshot().resetCalls, 0);
    EXPECT_TRUE(service->stopTest(500).ok());
    ASSERT_TRUE(waitForState(service.get(), TestState::Idle));
    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, StopFailureDoesNotFabricateAnIdleState)
{
    test::ensureQtApplication();

    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    config.steps = {config.steps.at(0)};

    test::FakeAlgorithmExecutor executor;
    executor.blockExecution();
    executor.ignoreRunControl();
    executor.setRequestStopStatus(test::makeStatus(ErrorCode::ResourceTimeout,
                                                    QStringLiteral("stop timed out")));
    TestConfigManager manager;
    const ConfigPath configurationPath =
        saveConfiguration(manager, temporaryDirectory, config, QStringLiteral("stop-failure.testcfg"));
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(configurationPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(executor.waitForExecutionStart(2000));

    const Status stopStatus = service->stopTest(37);
    EXPECT_FALSE(stopStatus.ok());
    EXPECT_EQ(stopStatus.code, ErrorCode::ResourceTimeout);

    const Result<TestState> stateAfterFailedStop = service->getCurrentState();
    ASSERT_TRUE(stateAfterFailedStop.ok());
    EXPECT_NE(stateAfterFailedStop.value, TestState::Idle);

    executor.allowCompletion();
    EXPECT_TRUE(waitForTerminalState(service.get()));
    EXPECT_TRUE(service->shutdown().ok());
}

TEST(TestRunServiceTest, GeneratingReportDoesNotReenterTheAlgorithmExecutor)
{
    test::ensureQtApplication();

    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    TestConfig config = test::makeCompleteConfig();
    config.steps = {config.steps.at(0)};

    TestConfigManager manager;
    const ConfigPath configurationPath =
        saveConfiguration(manager, temporaryDirectory, config, QStringLiteral("report-readonly.testcfg"));
    test::FakeAlgorithmExecutor executor;
    ServiceHandle service = makeService(executor);
    ASSERT_NE(service, nullptr);

    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(configurationPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(waitForState(service.get(), TestState::Finished));

    const test::FakeAlgorithmExecutor::PortCallSnapshot callsBeforeReport =
        executor.portCallSnapshot();
    ReportOptions options;
    options.outDir = temporaryDirectory.filePath(QStringLiteral("reports"));
    options.csv = false;
    options.txt = false;
    options.xml = false;
    const Result<ReportPath> report = service->generateReport(options);

    ASSERT_TRUE(report.ok()) << report.status.error.message.toStdString();
    const test::FakeAlgorithmExecutor::PortCallSnapshot callsAfterReport =
        executor.portCallSnapshot();
    EXPECT_EQ(callsAfterReport.prepareCalls, callsBeforeReport.prepareCalls);
    EXPECT_EQ(callsAfterReport.executeCalls, callsBeforeReport.executeCalls);
    EXPECT_EQ(callsAfterReport.requestStopCalls, callsBeforeReport.requestStopCalls);
    EXPECT_EQ(callsAfterReport.resetCalls, callsBeforeReport.resetCalls);
    EXPECT_EQ(callsAfterReport.shutdownCalls, callsBeforeReport.shutdownCalls);

    EXPECT_TRUE(service->shutdown().ok());
}

} // namespace hwtest::biz
