#pragma once

#include "biz_types.h"

#include <QByteArray>

namespace hwtest::biz {

class HWTEST_BIZ_EXPORT TestConfigManager {
public:
    Result<TestConfig> load(const ConfigPath& filePath) const;
    Result<TestConfig> parse(const QByteArray& contents,
                             const QString& sourceName = {}) const;
    Status save(const ConfigPath& filePath, const TestConfig& config) const;
    Result<QVector<QString>> validate(const TestConfig& config) const;
};

} // namespace hwtest::biz
