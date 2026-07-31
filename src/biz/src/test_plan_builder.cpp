// 包含本模块头文件
#include <biz/test_plan_builder.h>

// 配置管理器（用于 validate）
#include <biz/test_config_manager.h>

// Qt 容器
#include <QHash>
#include <QSet>

namespace hwtest::biz {
namespace { // 匿名命名空间 - 内部辅助函数

// 构造带错误码和消息的 Status
Status makeStatus(ErrorCode code, const QString& message)
{
    Status status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

// 构造失败的 Result<T>
template <typename T>
Result<T> failure(ErrorCode code, const QString& message)
{
    Result<T> result;
    result.status = makeStatus(code, message);
    return result;
}

} // 匿名命名空间

// ========================================================================
// 从 TestConfig 构建可执行的 TestPlan
// 主要做三件事：
//   1. 验证配置
//   2. 过滤未启用的步骤，补全默认超时/重试值
//   3. 解析依赖关系，进行拓扑排序，检测循环依赖
// ========================================================================
Result<TestPlan> TestPlanBuilder::build(const TestConfig& config) const
{
    // ---- 1. 验证配置 ----
    const TestConfigManager configManager;
    const Result<QVector<QString>> validation = configManager.validate(config);
    if (!validation.ok()) {
        return failure<TestPlan>(validation.status.code, validation.status.error.message);
    }

    // ---- 2. 基本字段复制，生成计划ID ----
    TestPlan plan;
    plan.planId = config.configId.isEmpty()
        ? QStringLiteral("plan")
        : QStringLiteral("%1-plan").arg(config.configId); // 计划ID = 配置ID + "-plan"
    plan.configId              = config.configId;
    plan.productModel          = config.productModel;
    plan.configVersion         = config.configVersion;
    plan.hardwareRequirements  = config.hardwareRequirements;
    plan.protocolProfiles      = config.protocolProfiles;
    plan.safetyPolicy          = config.safetyPolicy;
    plan.runtimeConfig         = config.runtimeConfig;

    // ---- 3. 过滤并规范化步骤 ----
    QHash<StepId, int> stepIndexes;    // stepId → 在 plan.steps 中的下标
    QHash<TestItemId, StepId> itemToStep; // testItemId → stepId（用于依赖解析）

    for (const TestStep& sourceStep : config.steps) {
        // 跳过未启用的步骤
        if (!sourceStep.enabled) continue;

        TestStep step = sourceStep;

        // 超时为0 → 使用默认超时
        if (step.timeoutMs == 0) {
            step.timeoutMs = config.runtimeConfig.defaultTimeoutMs;
        }
        // 重试次数为-1（未设置）→ 使用默认重试次数
        if (step.retryCount == -1) {
            step.retryCount = config.runtimeConfig.defaultRetryCount;
        }

        // 检查 stepId 唯一性
        if (stepIndexes.contains(step.stepId)) {
            return failure<TestPlan>(ErrorCode::ConfigSchemaError,
                                     QStringLiteral("Duplicate step id '%1'").arg(step.stepId));
        }

        stepIndexes.insert(step.stepId, plan.steps.size());
        itemToStep.insert(step.testItemId, step.stepId);
        plan.steps.append(step);
    }

    // ---- 4. 解析依赖：支持 stepId 或 testItemId 两种引用方式 ----
    for (TestStep& step : plan.steps) {
        QList<StepId> normalizedDeps;
        for (const StepId& configuredDep : step.dependsOn) {
            StepId dependency = configuredDep;

            // 如果直接找不到 stepId，尝试当作 testItemId 查找
            if (!stepIndexes.contains(dependency)) {
                dependency = itemToStep.value(configuredDep);
            }

            // 最终找不到 → 报错
            if (dependency.isEmpty() || !stepIndexes.contains(dependency)) {
                return failure<TestPlan>(ErrorCode::ItemNotFound,
                    QStringLiteral("Step '%1' depends on missing step '%2'")
                        .arg(step.stepId, configuredDep));
            }

            // 去重
            if (!normalizedDeps.contains(dependency)) {
                normalizedDeps.append(dependency);
            }
        }
        step.dependsOn = normalizedDeps;
    }

    // ---- 5. 拓扑排序（Kahn 算法），检测循环依赖 ----
    // 入度表
    QHash<StepId, int> indegree;
    // 反向依赖表：被谁依赖
    QHash<StepId, QList<StepId>> dependents;
    for (const TestStep& step : plan.steps) {
        indegree.insert(step.stepId, step.dependsOn.size());
        for (const StepId& dep : step.dependsOn) {
            dependents[dep].append(step.stepId);
        }
    }

    QVector<TestStep> ordered;
    ordered.reserve(plan.steps.size());
    QSet<StepId> emitted; // 已输出的步骤ID

    while (ordered.size() < plan.steps.size()) {
        // 找一个入度为0且尚未输出的步骤
        int candidateIndex = -1;
        for (int i = 0; i < plan.steps.size(); ++i) {
            const TestStep& candidate = plan.steps.at(i);
            if (!emitted.contains(candidate.stepId) && indegree.value(candidate.stepId) == 0) {
                candidateIndex = i;
                break;
            }
        }

        // 找不到入度为0的步骤 → 存在循环依赖
        if (candidateIndex < 0) {
            return failure<TestPlan>(ErrorCode::DependencyCycle,
                                     QStringLiteral("Configuration contains a dependency cycle"));
        }

        const TestStep candidate = plan.steps.at(candidateIndex);
        emitted.insert(candidate.stepId);
        ordered.append(candidate);

        // 将该步骤的依赖者的入度减1
        for (const StepId& dependent : dependents.value(candidate.stepId)) {
            indegree.insert(dependent, indegree.value(dependent) - 1);
        }
    }

    // 用排序后的步骤替换
    plan.steps = ordered;

    return Result<TestPlan>{Status{}, plan};
}

} // namespace hwtest::biz