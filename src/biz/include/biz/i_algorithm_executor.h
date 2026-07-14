#pragma once

#include "biz_types.h"

#include <logging/log_types.h>

namespace hwtest::biz {

class HWTEST_BIZ_EXPORT IRunControl {
public:
    virtual ~IRunControl() = default;
    // The reference is valid only for the duration of executeStep().
    virtual RunControl current() const = 0;
    virtual bool checkpoint() const = 0;
};

class HWTEST_BIZ_EXPORT IAlgorithmObserver {
public:
    virtual ~IAlgorithmObserver() = default;
    // Implementations must not retain this observer after executeStep() returns.
    virtual void onProgress(const StepId& stepId,
                            const TestItemId& testItemId,
                            int progress,
                            const QString& stage) = 0;
    virtual void onSample(const StepId& stepId, const RawSample& sample) = 0;
    virtual void onLog(const hwtest::logging::LogEvent& event) = 0;
};

class HWTEST_BIZ_EXPORT IAlgorithmExecutor {
public:
    virtual ~IAlgorithmExecutor() = default;

    virtual Status prepare(const TestPlan& plan,
                           const TestContext& context,
                           const QVariantMap& executionConfig) = 0;
    virtual Result<TestResult> executeStep(const TestStep& step,
                                           const IRunControl& control,
                                           IAlgorithmObserver& observer) = 0;
    // The active executeStep() must observe cancellation and return within
    // timeoutMs even when cleanup reports an error; the Status describes the
    // cleanup outcome, not permission to keep the call blocked.
    virtual Status requestStop(int timeoutMs) = 0;
    virtual Status reset() = 0;
    // Implementations must honor timeoutMs and must not retain BIZ callbacks.
    virtual Status shutdown(int timeoutMs) = 0;
};

} // namespace hwtest::biz
