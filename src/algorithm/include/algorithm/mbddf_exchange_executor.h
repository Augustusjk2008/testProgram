#pragma once

#include <algorithm/system_status_executor.h>

namespace hwtest::algorithm::mbddf {

// Reuses the generic CSV/transport exchange implementation for MB_DDF
// commands whose execution is one request followed by one response.
class MbdDfExchangeAlgorithmExecutor final : public SystemStatusAlgorithmExecutor {
public:
    MbdDfExchangeAlgorithmExecutor(std::unique_ptr<IByteTransport> transport,
                                   QString algorithmId,
                                   QString requestProfileId,
                                   QString responseProfileId,
                                   QString commandName)
        : SystemStatusAlgorithmExecutor(std::move(transport),
                                         std::move(algorithmId),
                                         std::move(requestProfileId),
                                         std::move(responseProfileId),
                                         std::move(commandName))
    {
    }
};

} // namespace hwtest::algorithm::mbddf
