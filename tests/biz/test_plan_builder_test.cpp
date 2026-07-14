#include <gtest/gtest.h>

#include <biz/biz_types.h>
#include <biz/test_plan_builder.h>

#include "test_support.h"

namespace hwtest::biz {

TEST(TestPlanBuilderTest, AppliesDefaultsWhenStepUsesExplicitInheritMarkers)
{
    TestConfig config = test::makeCompleteConfig();
    config.runtimeConfig.defaultTimeoutMs = 2750;
    config.runtimeConfig.defaultRetryCount = 3;

    // RED contract: zero timeout and -1 retry are the explicit inheritance markers.
    config.steps[0].timeoutMs = 0;
    config.steps[0].retryCount = -1;

    TestPlanBuilder builder;
    const Result<TestPlan> plan = builder.build(config);

    ASSERT_TRUE(plan.ok()) << plan.status.error.message.toStdString();
    ASSERT_FALSE(plan.value.steps.isEmpty());
    EXPECT_EQ(plan.value.steps.at(0).timeoutMs, 2750);
    EXPECT_EQ(plan.value.steps.at(0).retryCount, 3);
}

TEST(TestPlanBuilderTest, FiltersDisabledStepsWithoutReorderingEnabledSteps)
{
    TestConfig config = test::makeCompleteConfig();
    TestStep disabled = test::makeStep(QStringLiteral("STEP_DISABLED"),
                                       QStringLiteral("ITEM_DISABLED"),
                                       QStringLiteral("Disabled step"));
    disabled.enabled = false;
    config.steps.insert(1, disabled);

    TestPlanBuilder builder;
    const Result<TestPlan> plan = builder.build(config);

    ASSERT_TRUE(plan.ok()) << plan.status.error.message.toStdString();
    ASSERT_EQ(plan.value.steps.size(), 2);
    EXPECT_EQ(plan.value.steps.at(0).stepId, QStringLiteral("STEP_A"));
    EXPECT_EQ(plan.value.steps.at(1).stepId, QStringLiteral("STEP_B"));
}

TEST(TestPlanBuilderTest, ProducesStableDependencyOrderForReverseOrderedDag)
{
    TestConfig config = test::makeCompleteConfig();
    const TestStep first = config.steps.at(0);
    const TestStep dependent = config.steps.at(1);
    config.steps = {dependent, first};

    TestPlanBuilder builder;
    const Result<TestPlan> plan = builder.build(config);

    ASSERT_TRUE(plan.ok()) << plan.status.error.message.toStdString();
    ASSERT_EQ(plan.value.steps.size(), 2);
    EXPECT_EQ(plan.value.steps.at(0).stepId, first.stepId);
    EXPECT_EQ(plan.value.steps.at(1).stepId, dependent.stepId);
}

TEST(TestPlanBuilderTest, MigratesUniqueLegacyTestItemDependenciesToStepIds)
{
    TestConfig config = test::makeCompleteConfig();
    config.steps[1].dependsOn = {config.steps[0].testItemId};

    TestPlanBuilder builder;
    const Result<TestPlan> plan = builder.build(config);

    ASSERT_TRUE(plan.ok()) << plan.status.error.message.toStdString();
    ASSERT_EQ(plan.value.steps.size(), 2);
    EXPECT_EQ(plan.value.steps.at(1).dependsOn,
              QList<StepId>{config.steps[0].stepId});
}

TEST(TestPlanBuilderTest, PreservesAlgorithmPreparationConfigurationInPlan)
{
    const TestConfig config = test::makeCompleteConfig();

    TestPlanBuilder builder;
    const Result<TestPlan> plan = builder.build(config);

    ASSERT_TRUE(plan.ok()) << plan.status.error.message.toStdString();
    ASSERT_EQ(plan.value.protocolProfiles.size(), config.protocolProfiles.size());
    EXPECT_EQ(plan.value.protocolProfiles.at(0).id, config.protocolProfiles.at(0).id);
    EXPECT_EQ(plan.value.safetyPolicy.outputLimits, config.safetyPolicy.outputLimits);
    EXPECT_EQ(plan.value.safetyPolicy.safeState, config.safetyPolicy.safeState);
}

TEST(TestPlanBuilderTest, RejectsDependenciesThatDoNotResolveToAPlanStep)
{
    TestConfig config = test::makeCompleteConfig();
    config.steps[0].dependsOn = {QStringLiteral("MISSING_STEP")};

    TestPlanBuilder builder;
    const Result<TestPlan> plan = builder.build(config);

    EXPECT_FALSE(plan.ok());
    EXPECT_EQ(plan.status.code, ErrorCode::ItemNotFound);
}

TEST(TestPlanBuilderTest, RejectsDependencyCyclesBeforeExecutionCanStart)
{
    TestConfig config = test::makeCompleteConfig();
    config.steps[0].dependsOn = {config.steps[1].stepId};
    config.steps[1].dependsOn = {config.steps[0].stepId};

    TestPlanBuilder builder;
    const Result<TestPlan> plan = builder.build(config);

    EXPECT_FALSE(plan.ok());
    EXPECT_EQ(plan.status.code, ErrorCode::DependencyCycle);
}

} // namespace hwtest::biz
