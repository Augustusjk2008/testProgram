#include <biz/test_plan_builder.h>

#include <biz/test_config_manager.h>

#include <QHash>
#include <QSet>

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

template <typename T>
Result<T> failure(ErrorCode code, const QString& message)
{
    Result<T> result;
    result.status = makeStatus(code, message);
    return result;
}

} // namespace

Result<TestPlan> TestPlanBuilder::build(const TestConfig& config) const
{
    const TestConfigManager configManager;
    const Result<QVector<QString>> validation = configManager.validate(config);
    if (!validation.ok()) {
        return failure<TestPlan>(validation.status.code, validation.status.error.message);
    }

    TestPlan plan;
    plan.planId = config.configId.isEmpty()
        ? QStringLiteral("plan")
        : QStringLiteral("%1-plan").arg(config.configId);
    plan.configId = config.configId;
    plan.productModel = config.productModel;
    plan.configVersion = config.configVersion;
    plan.hardwareRequirements = config.hardwareRequirements;
    plan.protocolProfiles = config.protocolProfiles;
    plan.safetyPolicy = config.safetyPolicy;
    plan.runtimeConfig = config.runtimeConfig;

    QHash<StepId, int> stepIndexes;
    QHash<TestItemId, StepId> itemToStep;
    for (const TestStep& sourceStep : config.steps) {
        if (!sourceStep.enabled) {
            continue;
        }

        TestStep step = sourceStep;
        if (step.timeoutMs == 0) {
            step.timeoutMs = config.runtimeConfig.defaultTimeoutMs;
        }
        if (step.retryCount == -1) {
            step.retryCount = config.runtimeConfig.defaultRetryCount;
        }

        if (stepIndexes.contains(step.stepId)) {
            return failure<TestPlan>(ErrorCode::ConfigSchemaError,
                                     QStringLiteral("Duplicate step id '%1'").arg(step.stepId));
        }
        stepIndexes.insert(step.stepId, plan.steps.size());
        itemToStep.insert(step.testItemId, step.stepId);
        plan.steps.append(step);
    }

    for (TestStep& step : plan.steps) {
        QList<StepId> normalizedDependencies;
        for (const StepId& configuredDependency : step.dependsOn) {
            StepId dependency = configuredDependency;
            if (!stepIndexes.contains(dependency)) {
                dependency = itemToStep.value(configuredDependency);
            }
            if (dependency.isEmpty() || !stepIndexes.contains(dependency)) {
                return failure<TestPlan>(ErrorCode::ItemNotFound,
                                          QStringLiteral("Step '%1' depends on missing step '%2'")
                                              .arg(step.stepId, configuredDependency));
            }
            if (!normalizedDependencies.contains(dependency)) {
                normalizedDependencies.append(dependency);
            }
        }
        step.dependsOn = normalizedDependencies;
    }

    QHash<StepId, int> indegree;
    QHash<StepId, QList<StepId>> dependents;
    for (const TestStep& step : plan.steps) {
        indegree.insert(step.stepId, step.dependsOn.size());
        for (const StepId& dependency : step.dependsOn) {
            dependents[dependency].append(step.stepId);
        }
    }

    QVector<TestStep> ordered;
    ordered.reserve(plan.steps.size());
    QSet<StepId> emitted;
    while (ordered.size() < plan.steps.size()) {
        int candidateIndex = -1;
        for (int index = 0; index < plan.steps.size(); ++index) {
            const TestStep& candidate = plan.steps.at(index);
            if (!emitted.contains(candidate.stepId) && indegree.value(candidate.stepId) == 0) {
                candidateIndex = index;
                break;
            }
        }
        if (candidateIndex < 0) {
            return failure<TestPlan>(ErrorCode::DependencyCycle,
                                     QStringLiteral("Configuration contains a dependency cycle"));
        }

        const TestStep candidate = plan.steps.at(candidateIndex);
        emitted.insert(candidate.stepId);
        ordered.append(candidate);
        for (const StepId& dependent : dependents.value(candidate.stepId)) {
            indegree.insert(dependent, indegree.value(dependent) - 1);
        }
    }
    plan.steps = ordered;

    return Result<TestPlan>{Status{}, plan};
}

} // namespace hwtest::biz
