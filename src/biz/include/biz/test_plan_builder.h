#pragma once

#include "biz_types.h"

namespace hwtest::biz {

class HWTEST_BIZ_EXPORT TestPlanBuilder {
public:
    Result<TestPlan> build(const TestConfig& config) const;
};

} // namespace hwtest::biz
