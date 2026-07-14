#pragma once

#include "biz_global.h"

#include <QObject>

namespace hwtest::biz {

class IAlgorithmExecutor;
class IReportGenerator;
class ITestRunService;

// The executor is non-owning and must outlive the returned service.
HWTEST_BIZ_EXPORT ITestRunService* createTestRunService(IAlgorithmExecutor* executor,
                                                         QObject* parent = nullptr);
HWTEST_BIZ_EXPORT void destroyTestRunService(ITestRunService* service);

HWTEST_BIZ_EXPORT IReportGenerator* createReportGenerator();
HWTEST_BIZ_EXPORT void destroyReportGenerator(IReportGenerator* generator);

} // namespace hwtest::biz
