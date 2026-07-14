#pragma once

#include "biz_types.h"

#include <logging/log_types.h>

#include <QObject>

namespace hwtest::biz {

class HWTEST_BIZ_EXPORT ITestRunService : public QObject {
    Q_OBJECT

public:
    explicit ITestRunService(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    ~ITestRunService() override = default;

    virtual Status initialize() = 0;
    virtual Status shutdown() = 0;
    virtual Status loadConfiguration(const ConfigPath& configPath) = 0;
    virtual Result<TaskId> startTest(const QStringList& testItems = {},
                                     int priority = -1) = 0;
    virtual Status pauseTest() = 0;
    virtual Status resumeTest() = 0;
    virtual Status stopTest(int timeoutMs = 5000) = 0;
    virtual Status resetHardware() = 0;
    virtual Result<TestState> getCurrentState() const = 0;
    virtual Result<ReportPath> generateReport(const ReportOptions& options = {}) = 0;
    virtual Result<SystemResource> getResourceStatus() const = 0;

signals:
    void testProgress(const TaskId& taskId,
                      const TestItemId& testItemId,
                      int progress,
                      const QString& step);
    void stateChanged(const TaskId& taskId, TestState state);
    void resultProduced(const TaskId& taskId, const TestResult& result);
    void logProduced(const hwtest::logging::LogEvent& event);
    void hardwareError(const TaskId& taskId,
                       const TestItemId& testItemId,
                       ErrorCode code,
                       const QString& description);
};

} // namespace hwtest::biz
