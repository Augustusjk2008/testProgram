#include <algorithm/elec_health_status_executor.h>

namespace hwtest::algorithm::mbddf {

ElecHealthStatusAlgorithmExecutor::ElecHealthStatusAlgorithmExecutor(
    std::unique_ptr<IByteTransport> transport)
    : SystemStatusAlgorithmExecutor(std::move(transport),
                                    QStringLiteral("mbddf.elec_health_status"),
                                    QStringLiteral("elec_health_status_request"),
                                    QStringLiteral("elec_health_status_response"),
                                    QStringLiteral("ELEC_HEALTH_STATUS"))
{
}

} // namespace hwtest::algorithm::mbddf
