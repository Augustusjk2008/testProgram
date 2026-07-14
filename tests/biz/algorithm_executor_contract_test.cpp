#include <gtest/gtest.h>

#include <biz/i_algorithm_executor.h>

#include <QObject>

#include <type_traits>

namespace hwtest::biz {
namespace {

using PrepareSignature = Status (IAlgorithmExecutor::*)(const TestPlan&,
                                                         const TestContext&,
                                                         const QVariantMap&);
using ExecuteStepSignature = Result<TestResult> (IAlgorithmExecutor::*)(const TestStep&,
                                                                          const IRunControl&,
                                                                          IAlgorithmObserver&);
using RequestStopSignature = Status (IAlgorithmExecutor::*)(int);
using ResetSignature = Status (IAlgorithmExecutor::*)();
using ShutdownSignature = Status (IAlgorithmExecutor::*)(int);
using CurrentSignature = RunControl (IRunControl::*)() const;
using CheckpointSignature = bool (IRunControl::*)() const;
using ProgressSignature = void (IAlgorithmObserver::*)(const StepId&,
                                                        const TestItemId&,
                                                        int,
                                                        const QString&);
using SampleSignature = void (IAlgorithmObserver::*)(const StepId&, const RawSample&);
using LogSignature = void (IAlgorithmObserver::*)(const hwtest::logging::LogEvent&);

static_assert(!std::is_base_of_v<QObject, IAlgorithmExecutor>);
static_assert(std::is_same_v<decltype(&IAlgorithmExecutor::prepare), PrepareSignature>);
static_assert(std::is_same_v<decltype(&IAlgorithmExecutor::executeStep), ExecuteStepSignature>);
static_assert(std::is_same_v<decltype(&IAlgorithmExecutor::requestStop), RequestStopSignature>);
static_assert(std::is_same_v<decltype(&IAlgorithmExecutor::reset), ResetSignature>);
static_assert(std::is_same_v<decltype(&IAlgorithmExecutor::shutdown), ShutdownSignature>);
static_assert(std::is_same_v<decltype(&IRunControl::current), CurrentSignature>);
static_assert(std::is_same_v<decltype(&IRunControl::checkpoint), CheckpointSignature>);
static_assert(std::is_same_v<decltype(&IAlgorithmObserver::onProgress), ProgressSignature>);
static_assert(std::is_same_v<decltype(&IAlgorithmObserver::onSample), SampleSignature>);
static_assert(std::is_same_v<decltype(&IAlgorithmObserver::onLog), LogSignature>);

} // namespace

TEST(AlgorithmExecutorContractTest, RemainsAPureCppBoundaryWithTheRequiredControlApi)
{
    EXPECT_FALSE((std::is_base_of_v<QObject, IAlgorithmExecutor>));
}

} // namespace hwtest::biz
