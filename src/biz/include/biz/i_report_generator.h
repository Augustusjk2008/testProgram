#pragma once

#include "biz_types.h"

namespace hwtest::biz {

class HWTEST_BIZ_EXPORT IReportGenerator {
public:
    virtual ~IReportGenerator() = default;

    virtual Result<ReportPath> createReport(const QVector<TestResult>& results,
                                            const ReportOptions& options) = 0;
};

} // namespace hwtest::biz
