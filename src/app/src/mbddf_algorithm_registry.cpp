#include "mbddf_algorithm_registry.h"

#include <algorithm/elec_health_status_executor.h>
#include <algorithm/mbddf_exchange_executor.h>
#include <algorithm/mbddf_transport.h>
#include <algorithm/system_status_executor.h>

#include <biz/i_algorithm_executor.h>

namespace hwtest::app {

const QVector<MbdDfAlgorithmRegistration>& mbddfAlgorithmRegistry()
{
    static const QVector<MbdDfAlgorithmRegistration> registry{
        {QStringLiteral("mbddf.system_status"),
         QStringLiteral("system_status_request"),
         QStringLiteral("system_status_response"),
         QStringLiteral("SYSTEM_STATUS")},
        {QStringLiteral("mbddf.elec_health_status"),
         QStringLiteral("elec_health_status_request"),
         QStringLiteral("elec_health_status_response"),
         QStringLiteral("ELEC_HEALTH_STATUS")},
        {QStringLiteral("mbddf.memperf"),
         QStringLiteral("memperf_test_request"),
         QStringLiteral("memperf_test_response"),
         QStringLiteral("MEMPERF_TEST")},
        {QStringLiteral("mbddf.spi_flash"),
         QStringLiteral("spi_flash_test_request"),
         QStringLiteral("spi_flash_test_response"),
         QStringLiteral("SPI_FLASH_TEST")},
        {QStringLiteral("mbddf.dh_pulse_config"),
         QStringLiteral("dh_pulse_config_request"),
         QStringLiteral("dh_pulse_config_response"),
         QStringLiteral("DH_PULSE_CONFIG")},
        {QStringLiteral("mbddf.timer_jitter"),
         QStringLiteral("timer_jitter_start_request"),
         QStringLiteral("timer_jitter_start_response"),
         QStringLiteral("TIMER_JITTER_START")},
        {QStringLiteral("mbddf.di_read"),
         QStringLiteral("di_read_request"),
         QStringLiteral("di_read_response"),
         QStringLiteral("DI_READ")},
    };
    return registry;
}

const MbdDfAlgorithmRegistration* findMbdDfAlgorithm(const QString& algorithmId)
{
    const auto& registry = mbddfAlgorithmRegistry();
    const auto it = std::find_if(registry.cbegin(), registry.cend(),
                                 [&algorithmId](const auto& item) {
                                     return item.algorithmId == algorithmId;
                                 });
    return it == registry.cend() ? nullptr : &*it;
}

bool isSupportedMbdDfAlgorithm(const QString& algorithmId)
{
    return findMbdDfAlgorithm(algorithmId) != nullptr;
}

std::unique_ptr<hwtest::biz::IAlgorithmExecutor> createMbdDfExecutor(
    const QString& algorithmId,
    std::unique_ptr<hwtest::algorithm::mbddf::IByteTransport> transport)
{
    using namespace hwtest::algorithm::mbddf;
    if (algorithmId == QStringLiteral("mbddf.system_status")) {
        return std::make_unique<SystemStatusAlgorithmExecutor>(std::move(transport));
    }
    if (algorithmId == QStringLiteral("mbddf.elec_health_status")) {
        return std::make_unique<ElecHealthStatusAlgorithmExecutor>(std::move(transport));
    }
    const MbdDfAlgorithmRegistration* registration = findMbdDfAlgorithm(algorithmId);
    if (registration == nullptr) return {};
    return std::make_unique<MbdDfExchangeAlgorithmExecutor>(
        std::move(transport),
        registration->algorithmId,
        registration->requestProfileId,
        registration->responseProfileId,
        registration->commandName);
}

} // namespace hwtest::app
