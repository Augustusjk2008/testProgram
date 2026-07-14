#pragma once

#include "biz_types.h"

namespace hwtest::biz {

class HWTEST_BIZ_EXPORT TestConfigManager {
public:
    Result<TestConfig> load(const ConfigPath& filePath) const;
    Status save(const ConfigPath& filePath, const TestConfig& config) const;
    Result<QVector<QString>> validate(const TestConfig& config) const;
};

} // namespace hwtest::biz
