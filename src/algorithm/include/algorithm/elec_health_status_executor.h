#pragma once

#include <algorithm/system_status_executor.h>

namespace hwtest::algorithm::mbddf {

class ElecHealthStatusAlgorithmExecutor final : public SystemStatusAlgorithmExecutor {
public:
    explicit ElecHealthStatusAlgorithmExecutor(std::unique_ptr<IByteTransport> transport);
};

} // namespace hwtest::algorithm::mbddf
