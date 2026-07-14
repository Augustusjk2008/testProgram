#include <biz/biz_factory.h>

#include <biz/i_report_generator.h>
#include <biz/i_test_run_service.h>

namespace hwtest::biz {

IReportGenerator* createReportGeneratorImplementation();
ITestRunService* createTestRunServiceImplementation(IAlgorithmExecutor* executor, QObject* parent);

ITestRunService* createTestRunService(IAlgorithmExecutor* executor, QObject* parent)
{
    return createTestRunServiceImplementation(executor, parent);
}

void destroyTestRunService(ITestRunService* service)
{
    delete service;
}

IReportGenerator* createReportGenerator()
{
    return createReportGeneratorImplementation();
}

void destroyReportGenerator(IReportGenerator* generator)
{
    delete generator;
}

} // namespace hwtest::biz
