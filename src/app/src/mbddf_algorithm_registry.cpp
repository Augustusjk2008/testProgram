#include "mbddf_algorithm_registry.h"

#include <algorithm/board_test_executor.h>
#include <algorithm/bus_echo_transport.h>
#include <algorithm/elec_health_status_executor.h>
#include <algorithm/dh_ignite_stream_executor.h>
#include <algorithm/imu_stream_executor.h>
#include <algorithm/helm_stream_executor.h>
#include <algorithm/mbddf_exchange_executor.h>
#include <algorithm/mbddf_transport.h>
#include <algorithm/serial_test_executor.h>
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
        {QStringLiteral("mbddf.serial_test"),
         QStringLiteral("bus_loop_test_request"),
         QStringLiteral("bus_loop_test_response"),
         QStringLiteral("SERIAL_TEST")},
        {QStringLiteral("mbddf.bus_loop"),
         QStringLiteral("bus_loop_test_request"),
         QStringLiteral("bus_loop_test_response"),
         QStringLiteral("BUS_LOOP_TEST")},
        {QStringLiteral("mbddf.bus_echo"),
         QStringLiteral("bus_echo_test_request"),
         QStringLiteral("bus_echo_test_response"),
         QStringLiteral("BUS_ECHO_TEST")},
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
        {QStringLiteral("mbddf.dh_ignite_stream"),
         QStringLiteral("dh_control_request"),
         QStringLiteral("dh_control_response"),
         QStringLiteral("DH_IGNITE_STREAM")},
        {QStringLiteral("mbddf.timer_jitter"),
         QStringLiteral("timer_jitter_start_request"),
         QStringLiteral("timer_jitter_start_response"),
         QStringLiteral("TIMER_JITTER_START")},
        {QStringLiteral("mbddf.di_read"),
         QStringLiteral("di_read_request"),
         QStringLiteral("di_read_response"),
         QStringLiteral("DI_READ")},
        {QStringLiteral("mbddf.do_write"),
         QStringLiteral("do_write_request"),
         QStringLiteral("do_write_response"),
         QStringLiteral("DO_WRITE")},
        {QStringLiteral("mbddf.helm_board_test"),
         QStringLiteral("helm_board_test_request"),
         QStringLiteral("helm_board_test_response"),
         QStringLiteral("HELM_BOARD_TEST")},
        {QStringLiteral("mbddf.imu_stream"),
         QStringLiteral("imu_stream_start_request"),
         QStringLiteral("imu_stream_feedback_response"),
         QStringLiteral("IMU_STREAM")},
        {QStringLiteral("mbddf.helm_stream"),
         QStringLiteral("helm_start_request"),
         QStringLiteral("helm_feedback_response"),
         QStringLiteral("HELM_STREAM"),
         QStringLiteral("mbddf.helm.performance"),
         QStringLiteral("1")},
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
    std::unique_ptr<hwtest::algorithm::mbddf::IByteTransport> transport,
    hwtest::hal::IControlChannel* auxiliaryControlChannel,
    const QString& controlResourceId,
    hwtest::algorithm::mbddf::IBoardTestFixture* boardTestFixture)
{
    using namespace hwtest::algorithm::mbddf;
    if (algorithmId == QStringLiteral("mbddf.system_status")) {
        return std::make_unique<SystemStatusAlgorithmExecutor>(std::move(transport));
    }
    if (algorithmId == QStringLiteral("mbddf.elec_health_status")) {
        return std::make_unique<ElecHealthStatusAlgorithmExecutor>(std::move(transport));
    }
    if (algorithmId == QStringLiteral("mbddf.imu_stream")) {
        return std::make_unique<ImuStreamAlgorithmExecutor>(std::move(transport));
    }
    if (algorithmId == QStringLiteral("mbddf.dh_ignite_stream")) {
        return std::make_unique<DhIgniteStreamAlgorithmExecutor>(
            std::move(transport));
    }
    if (algorithmId == QStringLiteral("mbddf.helm_stream")) {
        return std::make_unique<HelmStreamAlgorithmExecutor>(std::move(transport));
    }
    if (algorithmId == QStringLiteral("mbddf.do_write")) {
        return std::make_unique<DoWriteAlgorithmExecutor>(
            std::move(transport), boardTestFixture);
    }
    if (algorithmId == QStringLiteral("mbddf.helm_board_test")) {
        return std::make_unique<HelmBoardTestAlgorithmExecutor>(
            std::move(transport), boardTestFixture);
    }
    if (algorithmId == QStringLiteral("mbddf.serial_test")) {
        return std::make_unique<SerialTestAlgorithmExecutor>(
            std::move(transport), auxiliaryControlChannel, controlResourceId);
    }
    if (algorithmId == QStringLiteral("mbddf.bus_echo")) {
        transport = std::make_unique<BusEchoTransport>(
            std::move(transport), auxiliaryControlChannel, controlResourceId);
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
