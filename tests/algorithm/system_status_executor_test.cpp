#include <gtest/gtest.h>

#include <algorithm/mbddf_transport.h>
#include <algorithm/system_status_executor.h>

#include <biz/biz_factory.h>
#include <biz/i_test_run_service.h>
#include <biz/test_config_manager.h>

#include <QFileInfo>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace hwtest::algorithm::mbddf {
namespace {

QString catalogDirectory()
{
    const QString configured = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    return configured.isEmpty()
        ? QStringLiteral("H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv")
        : configured;
}

class ResultCollector {
public:
    void append(const hwtest::biz::TestResult& result)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = result;
        m_hasResult = true;
        m_condition.notify_all();
    }

    bool waitForResult(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock,
                                    std::chrono::milliseconds(timeoutMs),
                                    [this] { return m_hasResult; });
    }

    hwtest::biz::TestResult result() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_result;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    hwtest::biz::TestResult m_result;
    bool m_hasResult = false;
};

class StateCollector {
public:
    void append(hwtest::biz::TestState state)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = state;
        m_condition.notify_all();
    }

    bool waitForTerminal(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock,
                                    std::chrono::milliseconds(timeoutMs),
                                    [this] {
                                        return m_state == hwtest::biz::TestState::Finished ||
                                            m_state == hwtest::biz::TestState::Error;
                                    });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    hwtest::biz::TestState m_state = hwtest::biz::TestState::Uninitialized;
};

TEST(SystemStatusExecutorTest, ConfigBIZSimulatorAndGoldenFrameFormOneClosedLoop)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }

    const QString configPath =
        QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG);
    ASSERT_TRUE(QFileInfo(configPath).isFile()) << configPath.toStdString();
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    auto simulator = std::make_unique<SystemStatusSimulator>(&catalog);
    SystemStatusSimulator* simulatorPtr = simulator.get();
    simulator->setResponseValues({
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("cpu_usage"), 12.5},
        {QStringLiteral("mem_usage"), 25.0},
        {QStringLiteral("rk_temp"), 42.0},
        {QStringLiteral("k7_temp"), -5.0},
        {QStringLiteral("power_on_sec"), 99u},
    });

    SystemStatusAlgorithmExecutor executor(std::move(simulator));
    hwtest::biz::TestConfigManager configManager;
    const auto config = configManager.load(configPath);
    ASSERT_TRUE(config.ok()) << config.status.error.message.toStdString();

    std::unique_ptr<hwtest::biz::ITestRunService,
                    void (*)(hwtest::biz::ITestRunService*)>
        service(hwtest::biz::createTestRunService(&executor),
                &hwtest::biz::destroyTestRunService);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());

    ResultCollector results;
    StateCollector states;
    QObject::connect(service.get(),
                     &hwtest::biz::ITestRunService::resultProduced,
                     [&results](const hwtest::biz::TaskId&,
                                const hwtest::biz::TestResult& result) {
                         results.append(result);
                     });
    QObject::connect(service.get(),
                     &hwtest::biz::ITestRunService::stateChanged,
                     [&states](const hwtest::biz::TaskId&, hwtest::biz::TestState state) {
                         states.append(state);
                     });

    ASSERT_TRUE(service->loadConfiguration(configPath).ok());
    const auto started = service->startTest();
    ASSERT_TRUE(started.ok()) << started.status.error.message.toStdString();
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.stepId, QStringLiteral("SYSTEM_STATUS"));
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::Ok);
    EXPECT_EQ(result.rawData.value(QStringLiteral("requestFrameHex")).toString().toUpper(),
              QStringLiteral("55AA301101013412") + QString(86, QLatin1Char('0')) + QStringLiteral("AC1C"));
    EXPECT_EQ(simulatorPtr->transactionCount(), 1);
    ASSERT_EQ(simulatorPtr->lastRequest().left(5).toHex().toUpper(), QByteArray("55AA301101"));

    ASSERT_TRUE(service->shutdown().ok());
}

TEST(SystemStatusExecutorTest, BadCrcBecomesProtocolErrorThroughBIZ)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }

    const QString configPath = QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG);
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    auto simulator = std::make_unique<SystemStatusSimulator>(&catalog);
    SystemStatusSimulator* simulatorPtr = simulator.get();
    simulator->setFault(SystemStatusSimulator::Fault::BadCrc);
    SystemStatusAlgorithmExecutor executor(std::move(simulator));

    std::unique_ptr<hwtest::biz::ITestRunService,
                    void (*)(hwtest::biz::ITestRunService*)>
        service(hwtest::biz::createTestRunService(&executor),
                &hwtest::biz::destroyTestRunService);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    QObject::connect(service.get(),
                     &hwtest::biz::ITestRunService::resultProduced,
                     [&results](const hwtest::biz::TaskId&,
                                const hwtest::biz::TestResult& result) {
                         results.append(result);
                     });
    ASSERT_TRUE(service->loadConfiguration(configPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(results.waitForResult(3000));
    const auto result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Error);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::ProtocolParseError);
    EXPECT_EQ(simulatorPtr->transactionCount(), 1);
    ASSERT_TRUE(service->shutdown().ok());
}

TEST(SystemStatusExecutorTest, TimeoutIsTypedAndNullHardwareCannotOpen)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    const auto* request = catalog.findByName(QStringLiteral("system_status_request"));
    ASSERT_NE(request, nullptr);

    auto simulator = std::make_unique<SystemStatusSimulator>(&catalog);
    simulator->setFault(SystemStatusSimulator::Fault::Timeout);
    ASSERT_TRUE(simulator->open(&error)) << error.toStdString();
    QByteArray payload;
    ASSERT_TRUE(encodePayload(*request, QVariantMap{}, 1, &payload, &error))
        << error.toStdString();
    QByteArray frame;
    ASSERT_TRUE(encodeFrame(payload, &frame, &error)) << error.toStdString();
    const TransportResult result = simulator->transact(frame, 100);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, TransportResult::Error::Timeout);

    HalSerialTransport hardware(nullptr, QStringLiteral("SERIAL_A"));
    EXPECT_FALSE(hardware.open(&error));
    EXPECT_FALSE(error.isEmpty());
}

} // namespace
} // namespace hwtest::algorithm::mbddf
