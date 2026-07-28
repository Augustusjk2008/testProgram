#include <gtest/gtest.h>

#include <algorithm/elec_health_status_executor.h>
#include <algorithm/mbddf_exchange_executor.h>
#include <algorithm/mbddf_transport.h>
#include <algorithm/system_status_executor.h>

#include <biz/biz_factory.h>
#include <biz/i_test_run_service.h>
#include <biz/test_config_manager.h>

#include <hal/hal_factory.h>
#include <hal/i_control_channel.h>
#include <hal/i_hal_device.h>
#include <hal/i_hal_service.h>

#include <QFileInfo>
#include <QDir>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QUdpSocket>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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

class AlgorithmLogCollector {
public:
    void append(const hwtest::logging::LogEvent& event)
    {
        if (event.source != QStringLiteral("algorithm")) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_event = event;
        m_hasEvent = true;
        m_condition.notify_all();
    }

    bool waitForEvent(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock,
                                    std::chrono::milliseconds(timeoutMs),
                                    [this] { return m_hasEvent; });
    }

    hwtest::logging::LogEvent event() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_event;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    hwtest::logging::LogEvent m_event;
    bool m_hasEvent = false;
};

class BlockingByteTransport final : public IByteTransport {
public:
    bool open(QString* error) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (error != nullptr) {
            error->clear();
        }
        m_open = true;
        m_openThread = std::this_thread::get_id();
        return true;
    }

    TransportResult transact(const QByteArray&, int) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_transactionStarted = true;
        m_condition.notify_all();
        m_condition.wait(lock, [this] { return m_released; });

        TransportResult result;
        result.errorCode = TransportResult::Error::Timeout;
        result.error = QStringLiteral("released blocking transaction");
        return result;
    }

    void close() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_open) {
            return;
        }
        m_open = false;
        m_closeThread = std::this_thread::get_id();
        m_closed = true;
        m_condition.notify_all();
    }

    bool waitForTransaction(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock,
                                    std::chrono::milliseconds(timeoutMs),
                                    [this] { return m_transactionStarted; });
    }

    bool waitForClose(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock,
                                    std::chrono::milliseconds(timeoutMs),
                                    [this] { return m_closed; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_condition.notify_all();
    }

    std::thread::id openThread() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_openThread;
    }

    std::thread::id closeThread() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closeThread;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::thread::id m_openThread;
    std::thread::id m_closeThread;
    bool m_open = false;
    bool m_transactionStarted = false;
    bool m_released = false;
    bool m_closed = false;
};

using hwtest::hal::HalResult;
using hwtest::hal::HalStatus;
using hwtest::hal::HalStatusCode;
using hwtest::hal::IControlChannel;
using hwtest::hal::IHalDevice;
using hwtest::hal::OperationOptions;
using hwtest::hal::ResourceId;

HalStatus failedHalStatus(HalStatusCode code, const QString& message)
{
    HalStatus status;
    status.code = code;
    status.error.code = code;
    status.error.message = message;
    return status;
}

class FakeControlChannel final : public IControlChannel {
public:
    struct ReadAction {
        HalStatus status;
        QByteArray bytes;
    };

    void enqueueBytes(const QByteArray& bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reads.push_back({HalStatus{}, bytes});
    }

    void enqueueFailure(const HalStatus& status)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reads.push_back({status, {}});
    }

    HalStatus openControl(const ResourceId& resourceId,
                          const OperationOptions& options) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_openCount;
        m_openResources.push_back(resourceId);
        m_lastOpenOptions = options;
        if (!m_openStatus.ok()) {
            return m_openStatus;
        }
        m_open = true;
        return HalStatus{};
    }

    HalStatus closeControl(const ResourceId& resourceId,
                           const OperationOptions& options) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_closeCount;
        m_closeResources.push_back(resourceId);
        m_lastCloseOptions = options;
        m_open = false;
        return m_closeStatus;
    }

    HalStatus writeControl(const ResourceId& resourceId,
                           const QByteArray& data,
                           const OperationOptions& options) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_open) {
            return failedHalStatus(HalStatusCode::InvalidState,
                                   QStringLiteral("control channel is not open"));
        }
        ++m_writeCount;
        m_writeResources.push_back(resourceId);
        m_writes.push_back(data);
        m_lastWriteOptions = options;
        return m_writeStatus;
    }

    HalResult<QByteArray> readControl(const ResourceId& resourceId,
                                      int maxBytes,
                                      const OperationOptions& options) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        HalResult<QByteArray> result;
        if (!m_open) {
            result.status = failedHalStatus(HalStatusCode::InvalidState,
                                            QStringLiteral("control channel is not open"));
            return result;
        }
        ++m_readCount;
        m_readResources.push_back(resourceId);
        m_lastReadMaxBytes = maxBytes;
        m_lastReadOptions = options;
        if (m_reads.empty()) {
            return result;
        }
        const ReadAction action = m_reads.front();
        m_reads.pop_front();
        result.status = action.status;
        result.value = action.bytes;
        return result;
    }

    int openCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_openCount;
    }

    int closeCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closeCount;
    }

    int writeCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_writeCount;
    }

    int readCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_readCount;
    }

    QByteArray writeAt(int index) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_writes.at(index);
    }

    ResourceId lastWriteResource() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_writeResources.isEmpty() ? ResourceId{} : m_writeResources.last();
    }

    int lastWriteTimeoutMs() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastWriteOptions.timeoutMs;
    }

    hwtest::hal::RequestId lastOpenRequestId() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastOpenOptions.requestId;
    }

    hwtest::hal::RequestId lastCloseRequestId() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastCloseOptions.requestId;
    }

    hwtest::hal::RequestId lastWriteRequestId() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastWriteOptions.requestId;
    }

    hwtest::hal::RequestId lastReadRequestId() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastReadOptions.requestId;
    }

private:
    mutable std::mutex m_mutex;
    std::deque<ReadAction> m_reads;
    QVector<ResourceId> m_openResources;
    QVector<ResourceId> m_closeResources;
    QVector<ResourceId> m_writeResources;
    QVector<ResourceId> m_readResources;
    QVector<QByteArray> m_writes;
    HalStatus m_openStatus;
    HalStatus m_closeStatus;
    HalStatus m_writeStatus;
    OperationOptions m_lastOpenOptions;
    OperationOptions m_lastCloseOptions;
    OperationOptions m_lastWriteOptions;
    OperationOptions m_lastReadOptions;
    int m_lastReadMaxBytes = 0;
    int m_openCount = 0;
    int m_closeCount = 0;
    int m_writeCount = 0;
    int m_readCount = 0;
    bool m_open = false;
};

class FakeControlDevice final : public IHalDevice {
public:
    explicit FakeControlDevice(IControlChannel* channel)
        : m_channel(channel)
    {
    }

    hwtest::hal::DeviceDescriptor descriptor() const override
    {
        return {};
    }

    hwtest::hal::DeviceCapabilities capabilities() const override
    {
        return {};
    }

    hwtest::hal::IAnalogIo* analogIo() override
    {
        return nullptr;
    }

    hwtest::hal::IDigitalIo* digitalIo() override
    {
        return nullptr;
    }

    hwtest::hal::ISerialBus* serialBus() override
    {
        return nullptr;
    }

    hwtest::hal::ICanFdBus* canFdBus() override
    {
        return nullptr;
    }

    IControlChannel* controlChannel() override
    {
        return m_channel;
    }

private:
    IControlChannel* m_channel = nullptr;
};

QByteArray framedBytes(const QByteArray& payload, const QByteArray& crc = QByteArray::fromHex("A1B2"))
{
    QByteArray frame = QByteArray::fromHex("55AA");
    frame.append(static_cast<char>(payload.size()));
    frame.append(payload);
    frame.append(crc);
    return frame;
}

QByteArray expectedSystemStatusRequest()
{
    QByteArray frame = QByteArray::fromHex("55AA301101013412");
    frame.append(43, '\0');
    frame.append(QByteArray::fromHex("AC1C"));
    return frame;
}

bool makeSystemStatusResponseFrame(const ProtocolCatalog& catalog,
                                   quint16 sequence,
                                   QByteArray* frame,
                                   QString* error)
{
    const MessageDefinition* response =
        catalog.findByName(QStringLiteral("system_status_response"));
    if (response == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("system_status_response is missing");
        }
        return false;
    }

    const QVariantMap values = {
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("cpu_usage"), 12.5},
        {QStringLiteral("mem_usage"), 25.0},
        {QStringLiteral("rk_temp"), 42.0},
        {QStringLiteral("k7_temp"), -5.0},
        {QStringLiteral("power_on_sec"), 99u},
    };
    QByteArray payload;
    if (!encodePayload(*response, values, sequence, &payload, error)) {
        return false;
    }
    return encodeFrame(payload, frame, error);
}

bool makeStaleSystemStatusErrorFrame(const ProtocolCatalog& catalog,
                                     quint16 staleSequence,
                                     QByteArray* frame,
                                     QString* error)
{
    const MessageDefinition* response =
        catalog.findByName(QStringLiteral("error_response"));
    if (response == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("error_response is missing");
        }
        return false;
    }

    const QVariantMap values = {
        {QStringLiteral("orig_tg"), 0x01},
        {QStringLiteral("orig_st"), 0x01},
        {QStringLiteral("orig_seq"), staleSequence},
        {QStringLiteral("err_code"), 0x0102},
        {QStringLiteral("detail"), 0},
    };
    QByteArray payload;
    if (!encodePayload(*response, values, staleSequence, &payload, error)) {
        return false;
    }
    return encodeFrame(payload, frame, error);
}

QVariantMap udpHalConfiguration(quint16 peerPort)
{
    QVariantMap device;
    device.insert(QStringLiteral("alias"), QStringLiteral("mbddf_dut"));
    device.insert(QStringLiteral("vendor"), QStringLiteral("Qt"));
    device.insert(QStringLiteral("model"), QStringLiteral("UdpControl"));

    QVariantMap properties;
    properties.insert(QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1"));
    properties.insert(QStringLiteral("remotePort"), static_cast<int>(peerPort));
    properties.insert(QStringLiteral("localAddress"), QStringLiteral("127.0.0.1"));
    properties.insert(QStringLiteral("localPort"), 0);

    QVariantMap controlResource;
    controlResource.insert(QStringLiteral("device"), QStringLiteral("mbddf_dut"));
    controlResource.insert(QStringLiteral("module"), QStringLiteral("control"));
    controlResource.insert(QStringLiteral("direction"), QStringLiteral("bidirectional"));
    controlResource.insert(QStringLiteral("physicalIndex"), 0);
    controlResource.insert(QStringLiteral("providerId"), QStringLiteral("qt.udp"));
    controlResource.insert(QStringLiteral("properties"), properties);

    QVariantMap resources;
    resources.insert(QStringLiteral("CONTROL_UDP"), controlResource);

    QVariantMap hardware;
    hardware.insert(QStringLiteral("devices"), QVariantList{device});
    hardware.insert(QStringLiteral("resources"), resources);

    QVariantMap configuration;
    configuration.insert(QStringLiteral("hardware"), hardware);
    return configuration;
}

bool receiveDatagram(QUdpSocket& peer,
                     QByteArray* datagram,
                     QHostAddress* sender,
                     quint16* senderPort,
                     int timeoutMs)
{
    if (!peer.hasPendingDatagrams() && !peer.waitForReadyRead(timeoutMs)) {
        return false;
    }
    const qint64 size = peer.pendingDatagramSize();
    if (size < 0) {
        return false;
    }
    datagram->resize(static_cast<int>(size));
    return peer.readDatagram(datagram->data(), datagram->size(), sender, senderPort) == size;
}

using RunServiceHandle = std::unique_ptr<hwtest::biz::ITestRunService,
                                         void (*)(hwtest::biz::ITestRunService*)>;
using HalServiceHandle = std::unique_ptr<hwtest::hal::IHalService,
                                         void (*)(hwtest::hal::IHalService*)>;

RunServiceHandle makeRunService(SystemStatusAlgorithmExecutor* executor)
{
    return RunServiceHandle(hwtest::biz::createTestRunService(executor),
                            &hwtest::biz::destroyTestRunService);
}

void connectCollectors(hwtest::biz::ITestRunService* service,
                       ResultCollector* results,
                       StateCollector* states)
{
    QObject::connect(service,
                     &hwtest::biz::ITestRunService::resultProduced,
                     [results](const hwtest::biz::TaskId&, const hwtest::biz::TestResult& result) {
                         results->append(result);
                     });
    QObject::connect(service,
                     &hwtest::biz::ITestRunService::stateChanged,
                     [states](const hwtest::biz::TaskId&, hwtest::biz::TestState state) {
                         states->append(state);
                     });
}

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
    AlgorithmLogCollector logs;
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
    QObject::connect(service.get(),
                     &hwtest::biz::ITestRunService::logProduced,
                     [&logs](const hwtest::logging::LogEvent& event) {
                         logs.append(event);
                     });

    ASSERT_TRUE(service->loadConfiguration(configPath).ok());
    const auto started = service->startTest();
    ASSERT_TRUE(started.ok()) << started.status.error.message.toStdString();
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));
    ASSERT_TRUE(logs.waitForEvent(3000));

    const auto result = results.result();
    EXPECT_EQ(result.stepId, QStringLiteral("SYSTEM_STATUS"));
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::Ok);
    EXPECT_EQ(result.rawData.value(QStringLiteral("requestFrameHex")).toString().toUpper(),
              QStringLiteral("55AA301101013412") + QString(86, QLatin1Char('0')) + QStringLiteral("AC1C"));
    const hwtest::logging::LogEvent log = logs.event();
    EXPECT_FALSE(log.context.contains(QStringLiteral("requestFrameHex")));
    EXPECT_FALSE(log.context.contains(QStringLiteral("responseFrameHex")));
    EXPECT_EQ(log.context.value(QStringLiteral("requestFrameLength")).toInt(), 53);
    EXPECT_EQ(log.context.value(QStringLiteral("responseFrameLength")).toInt(), 53);
    EXPECT_EQ(log.context.value(QStringLiteral("requestFrameSha256")).toString().size(), 64);
    EXPECT_EQ(log.context.value(QStringLiteral("responseFrameSha256")).toString().size(), 64);
    EXPECT_LE(log.context.value(QStringLiteral("requestFrameHexPreview")).toString().size(), 32);
    EXPECT_LE(log.context.value(QStringLiteral("responseFrameHexPreview")).toString().size(), 32);
    EXPECT_EQ(simulatorPtr->transactionCount(), 1);
    ASSERT_EQ(simulatorPtr->lastRequest().left(5).toHex().toUpper(), QByteArray("55AA301101"));

    ASSERT_TRUE(service->shutdown().ok());
}

TEST(SystemStatusExecutorTest, ExplicitDebugTagIncludesFullFramesInAlgorithmLog)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    auto simulator = std::make_unique<SystemStatusSimulator>(&catalog);
    simulator->setResponseValues({
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
    });
    SystemStatusAlgorithmExecutor executor(std::move(simulator));

    hwtest::biz::TestConfigManager configManager;
    const auto loaded = configManager.load(QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    hwtest::biz::TestConfig debugConfig = loaded.value;
    debugConfig.runtimeConfig.tags.insert(QStringLiteral("logFullFrames"), true);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString configPath = directory.filePath(QStringLiteral("debug-frames.testcfg"));
    ASSERT_TRUE(configManager.save(configPath, debugConfig).ok());

    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    AlgorithmLogCollector logs;
    QObject::connect(service.get(),
                     &hwtest::biz::ITestRunService::logProduced,
                     [&logs](const hwtest::logging::LogEvent& event) {
                         logs.append(event);
                     });

    ASSERT_TRUE(service->loadConfiguration(configPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(logs.waitForEvent(3000));

    const QVariantMap context = logs.event().context;
    EXPECT_FALSE(context.value(QStringLiteral("requestFrameHex")).toString().isEmpty());
    EXPECT_FALSE(context.value(QStringLiteral("responseFrameHex")).toString().isEmpty());
    EXPECT_TRUE(service->shutdown().ok());
}

TEST(ElecHealthStatusExecutorTest, ConfigBIZScriptedTransportProducesReadOnlyExchange)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }

    const QString configPath = QStringLiteral(HWTEST_MBDDF_ELEC_HEALTH_CONFIG);
    ASSERT_TRUE(QFileInfo(configPath).isFile()) << configPath.toStdString();
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    const MessageDefinition* responseDefinition =
        catalog.findByName(QStringLiteral("elec_health_status_response"));
    ASSERT_NE(responseDefinition, nullptr);

    QByteArray capturedRequest;
    auto transport = std::make_unique<ScriptedByteTransport>(
        [&](const QByteArray& request, int) {
            capturedRequest = request;
            QByteArray requestPayload;
            QString transportError;
            if (!decodeFrame(request, &requestPayload, &transportError)) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       transportError};
            }
            const MessageDefinition* requestDefinition =
                catalog.findByCommand(static_cast<quint8>(requestPayload.at(1)),
                                      static_cast<quint8>(requestPayload.at(2)),
                                      Direction::Request);
            QVariantMap requestValues;
            if (requestDefinition == nullptr ||
                !decodePayload(*requestDefinition,
                               requestPayload,
                               &requestValues,
                               &transportError)) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       transportError.isEmpty()
                                           ? QStringLiteral("request definition is missing")
                                           : transportError};
            }

            const QVariantMap responseValues{
                {QStringLiteral("status"), 0},
                {QStringLiteral("err_code"), 0},
                {QStringLiteral("c_volt"), 28.51},
                {QStringLiteral("b_volt"), 27.90},
                {QStringLiteral("activate_bits"), 1},
                {QStringLiteral("external_vol"), 3.30},
                {QStringLiteral("core_vol"), 1.00},
                {QStringLiteral("assist_vol"), 1.80},
                {QStringLiteral("v28_5"), 28.50},
                {QStringLiteral("js_5V"), 5.01},
                {QStringLiteral("dyt_5V"), 4.99},
                {QStringLiteral("power_24V"), 24.00},
                {QStringLiteral("value_YX"), 5.045},
            };
            QByteArray responsePayload;
            if (!encodePayload(*responseDefinition,
                               responseValues,
                               static_cast<quint16>(requestValues.value(
                                   QStringLiteral("seq")).toUInt()),
                               &responsePayload,
                               &transportError)) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       transportError};
            }
            QByteArray responseFrame;
            if (!encodeFrame(responsePayload, &responseFrame, &transportError)) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       transportError};
            }
            return TransportResult{true, TransportResult::Error::None, responseFrame, {}};
        });
    ScriptedByteTransport* transportPtr = transport.get();
    ElecHealthStatusAlgorithmExecutor executor(std::move(transport));

    std::unique_ptr<hwtest::biz::ITestRunService,
                    void (*)(hwtest::biz::ITestRunService*)>
        service(hwtest::biz::createTestRunService(&executor),
                &hwtest::biz::destroyTestRunService);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());

    ResultCollector results;
    StateCollector states;
    QVector<hwtest::biz::RawSample> samples;
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
    QObject::connect(service.get(),
                     &hwtest::biz::ITestRunService::sampleProduced,
                     [&samples](const hwtest::biz::TaskId&,
                                const hwtest::biz::StepId&,
                                const hwtest::biz::RawSample& sample) {
                         samples.push_back(sample);
                     });

    ASSERT_TRUE(service->loadConfiguration(configPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.stepId, QStringLiteral("ELEC_HEALTH_STATUS"));
    EXPECT_EQ(result.algorithmId, QStringLiteral("mbddf.elec_health_status"));
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::Ok);
    EXPECT_NEAR(result.rawData.value(QStringLiteral("responseValues"))
                    .toMap()
                    .value(QStringLiteral("c_volt"))
                    .toDouble(),
                28.51,
                1e-6);
    EXPECT_NEAR(result.rawData.value(QStringLiteral("responseValues"))
                    .toMap()
                    .value(QStringLiteral("value_YX"))
                    .toDouble(),
                5.045,
                0.0025);
    ASSERT_EQ(samples.size(), 1);
    EXPECT_EQ(samples.first().channelId, QStringLiteral("ELEC_HEALTH_STATUS"));
    EXPECT_EQ(transportPtr->transactionCount(), 1);

    QByteArray requestPayload;
    ASSERT_TRUE(decodeFrame(capturedRequest, &requestPayload, &error))
        << error.toStdString();
    EXPECT_EQ(static_cast<quint8>(requestPayload.at(1)), 0x05u);
    EXPECT_EQ(static_cast<quint8>(requestPayload.at(2)), 0x01u);
    const MessageDefinition* requestDefinition =
        catalog.findByName(QStringLiteral("elec_health_status_request"));
    ASSERT_NE(requestDefinition, nullptr);
    QVariantMap requestValues;
    ASSERT_TRUE(decodePayload(*requestDefinition,
                              requestPayload,
                              &requestValues,
                              &error))
        << error.toStdString();
    EXPECT_EQ(requestValues.value(QStringLiteral("seq")).toUInt(), 0x1234u);
    EXPECT_EQ(requestPayload.mid(5), QByteArray(43, '\0'));

    ASSERT_TRUE(service->shutdown().ok());
}

TEST(DiReadExecutorTest, ProducesOneBitmapSampleWithMatchingSequenceAndCrc)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    const MessageDefinition* responseDefinition =
        catalog.findByName(QStringLiteral("di_read_response"));
    ASSERT_NE(responseDefinition, nullptr);

    QByteArray capturedRequest;
    auto transport = std::make_unique<ScriptedByteTransport>(
        [&](const QByteArray& request, int) {
            capturedRequest = request;
            QByteArray payload;
            QString exchangeError;
            if (!decodeFrame(request, &payload, &exchangeError)) {
                return TransportResult{false, TransportResult::Error::Io, {}, exchangeError};
            }
            const MessageDefinition* requestDefinition = catalog.findByName(
                QStringLiteral("di_read_request"));
            QVariantMap requestValues;
            if (requestDefinition == nullptr ||
                !decodePayload(*requestDefinition, payload, &requestValues, &exchangeError)) {
                return TransportResult{false, TransportResult::Error::Io, {}, exchangeError};
            }
            QByteArray responsePayload;
            if (!encodePayload(*responseDefinition,
                               QVariantMap{{QStringLiteral("status"), 0},
                                           {QStringLiteral("err_code"), 0},
                                           {QStringLiteral("di_state[0]"), 0xA581u},
                                           {QStringLiteral("di_state[1]"), 0u}},
                               static_cast<quint16>(requestValues.value(
                                   QStringLiteral("seq")).toUInt()),
                               &responsePayload,
                               &exchangeError)) {
                return TransportResult{false, TransportResult::Error::Io, {}, exchangeError};
            }
            QByteArray responseFrame;
            if (!encodeFrame(responsePayload, &responseFrame, &exchangeError)) {
                return TransportResult{false, TransportResult::Error::Io, {}, exchangeError};
            }
            return TransportResult{true, TransportResult::Error::None, responseFrame, {}};
        });
    MbdDfExchangeAlgorithmExecutor executor(
        std::move(transport),
        QStringLiteral("mbddf.di_read"),
        QStringLiteral("di_read_request"),
        QStringLiteral("di_read_response"),
        QStringLiteral("DI_READ"));
    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    QVector<hwtest::biz::RawSample> samples;
    connectCollectors(service.get(), &results, &states);
    QObject::connect(service.get(),
                     &hwtest::biz::ITestRunService::sampleProduced,
                     [&samples](const hwtest::biz::TaskId&,
                                const hwtest::biz::StepId&,
                                const hwtest::biz::RawSample& sample) {
                         samples.push_back(sample);
                     });

    ASSERT_TRUE(service->loadConfiguration(QStringLiteral(HWTEST_MBDDF_DI_CONFIG)).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));
    const hwtest::biz::TestResult result = results.result();
    EXPECT_EQ(result.algorithmId, QStringLiteral("mbddf.di_read"));
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.rawData.value(QStringLiteral("responseValues")).toMap()
                  .value(QStringLiteral("di_state[0]")).toUInt(),
              0xA581u);
    ASSERT_EQ(samples.size(), 1);
    EXPECT_EQ(samples.first().channelId, QStringLiteral("DI_READ"));

    QByteArray requestPayload;
    ASSERT_TRUE(decodeFrame(capturedRequest, &requestPayload, &error));
    EXPECT_EQ(static_cast<quint8>(requestPayload.at(1)), 0x04u);
    EXPECT_EQ(static_cast<quint8>(requestPayload.at(2)), 0x01u);
    const MessageDefinition* requestDefinition = catalog.findByName(
        QStringLiteral("di_read_request"));
    ASSERT_NE(requestDefinition, nullptr);
    QVariantMap requestValues;
    ASSERT_TRUE(decodePayload(*requestDefinition,
                              requestPayload,
                              &requestValues,
                              &error));
    EXPECT_EQ(requestValues.value(QStringLiteral("seq")).toUInt(), 0x1234u);
    ASSERT_TRUE(service->shutdown().ok());
}

TEST(SystemStatusExecutorTest, RejectsDeviceManagedStreamingBeforeOpeningTransport)
{
    ProtocolCatalog catalog;
    auto simulator = std::make_unique<SystemStatusSimulator>(&catalog);
    SystemStatusSimulator* simulatorPtr = simulator.get();
    SystemStatusAlgorithmExecutor executor(std::move(simulator));

    hwtest::biz::TestContext context;
    context.tags.insert(QStringLiteral("runMode"), QStringLiteral("device_stream"));
    const hwtest::biz::Status prepared =
        executor.prepare(hwtest::biz::TestPlan{}, context, QVariantMap{});

    EXPECT_FALSE(prepared.ok());
    EXPECT_EQ(prepared.code, hwtest::biz::ErrorCode::CapabilityUnsupported);
    EXPECT_TRUE(prepared.error.message.contains(QStringLiteral("SYSTEM_STATUS")));
    EXPECT_EQ(simulatorPtr->transactionCount(), 0);
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

TEST(HalControlTransportTest, ReassemblesFrameSplitAcrossControlReads)
{
    const QByteArray response = framedBytes(QByteArray::fromHex("102030"));
    const QByteArray request = QByteArray::fromHex("55AA0100A1B2");
    FakeControlChannel channel;
    channel.enqueueBytes(response.left(4));
    channel.enqueueBytes(response.mid(4));
    FakeControlDevice device(&channel);
    HalControlTransport transport(&device, QStringLiteral("CONTROL_A"));

    QString error;
    ASSERT_TRUE(transport.open(&error)) << error.toStdString();
    const TransportResult result = transport.transact(request, 75);

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.frame, response);
    EXPECT_EQ(channel.openCount(), 1);
    EXPECT_EQ(channel.writeCount(), 1);
    EXPECT_EQ(channel.readCount(), 2);
    EXPECT_EQ(channel.writeAt(0), request);
    EXPECT_EQ(channel.lastWriteResource(), QStringLiteral("CONTROL_A"));
    EXPECT_EQ(channel.lastWriteTimeoutMs(), 75);

    transport.close();
    EXPECT_EQ(channel.closeCount(), 1);
}

TEST(HalControlTransportTest, SupportsSeparateWriteAndReadForDeviceStreaming)
{
    const QByteArray firstResponse = framedBytes(QByteArray::fromHex("0102"));
    const QByteArray secondResponse = framedBytes(QByteArray::fromHex("030405"));
    const QByteArray request = QByteArray::fromHex("55AA0100A1B2");
    FakeControlChannel channel;
    channel.enqueueBytes(firstResponse + secondResponse);
    FakeControlDevice device(&channel);
    HalControlTransport transport(&device, QStringLiteral("CONTROL_A"));

    QString error;
    ASSERT_TRUE(transport.open(&error)) << error.toStdString();
    const TransportResult written = transport.writeFrame(request, 75);
    const TransportResult first = transport.readFrame(75);
    const TransportResult second = transport.readFrame(75);

    ASSERT_TRUE(written.ok) << written.error.toStdString();
    ASSERT_TRUE(first.ok) << first.error.toStdString();
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    EXPECT_TRUE(written.frame.isEmpty());
    EXPECT_EQ(first.frame, firstResponse);
    EXPECT_EQ(second.frame, secondResponse);
    EXPECT_EQ(channel.writeCount(), 1);
    EXPECT_EQ(channel.readCount(), 1);
    EXPECT_EQ(channel.writeAt(0), request);
}

TEST(HalControlTransportTest, ReturnsFirstConcatenatedFrameAndBuffersSecond)
{
    const QByteArray firstResponse = framedBytes(QByteArray::fromHex("01"));
    const QByteArray secondResponse = framedBytes(QByteArray::fromHex("0203"));
    FakeControlChannel channel;
    channel.enqueueBytes(firstResponse + secondResponse);
    FakeControlDevice device(&channel);
    HalControlTransport transport(&device, QStringLiteral("CONTROL_A"));

    QString error;
    ASSERT_TRUE(transport.open(&error)) << error.toStdString();
    const TransportResult first = transport.transact(QByteArray::fromHex("55AA01AABBCC"), 75);
    const TransportResult second = transport.transact(QByteArray::fromHex("55AA01DDEEFF"), 75);

    ASSERT_TRUE(first.ok) << first.error.toStdString();
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    EXPECT_EQ(first.frame, firstResponse);
    EXPECT_EQ(second.frame, secondResponse);
    EXPECT_EQ(channel.writeCount(), 2);
    EXPECT_EQ(channel.readCount(), 1);
}

TEST(HalControlTransportTest, DiscardsLeadingNoiseBeforeFrameSync)
{
    const QByteArray response = framedBytes(QByteArray::fromHex("112233"));
    FakeControlChannel channel;
    channel.enqueueBytes(QByteArray::fromHex("00017FFF") + response);
    FakeControlDevice device(&channel);
    HalControlTransport transport(&device, QStringLiteral("CONTROL_A"));

    QString error;
    ASSERT_TRUE(transport.open(&error)) << error.toStdString();
    const TransportResult result = transport.transact(QByteArray::fromHex("55AA0142BEEF"), 75);

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.frame, response);
    EXPECT_EQ(channel.readCount(), 1);
}

TEST(HalControlTransportTest, MapsControlReadTimeoutToTransportTimeout)
{
    FakeControlChannel channel;
    channel.enqueueFailure(failedHalStatus(HalStatusCode::Timeout,
                                           QStringLiteral("scripted control timeout")));
    FakeControlDevice device(&channel);
    HalControlTransport transport(&device, QStringLiteral("CONTROL_A"));

    QString error;
    ASSERT_TRUE(transport.open(&error)) << error.toStdString();
    const TransportResult result = transport.transact(QByteArray::fromHex("55AA0100A1B2"), 75);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, TransportResult::Error::Timeout);
    EXPECT_EQ(channel.writeCount(), 1);
    EXPECT_EQ(channel.readCount(), 1);
}

TEST(SystemStatusExecutorTest, HalControlTransportOpensOnceForRetryingRun)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    QByteArray response;
    ASSERT_TRUE(makeSystemStatusResponseFrame(catalog, 0x1234, &response, &error))
        << error.toStdString();

    FakeControlChannel channel;
    channel.enqueueFailure(failedHalStatus(HalStatusCode::Timeout,
                                           QStringLiteral("first attempt times out")));
    channel.enqueueBytes(response);
    FakeControlDevice device(&channel);
    auto transport = std::make_unique<HalControlTransport>(&device, QStringLiteral("CONTROL_RETRY"));
    SystemStatusAlgorithmExecutor executor(std::move(transport));

    hwtest::biz::TestConfigManager configManager;
    const auto loaded = configManager.load(QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG));
    ASSERT_TRUE(loaded.ok()) << loaded.status.error.message.toStdString();
    hwtest::biz::TestConfig retryConfig = loaded.value;
    ASSERT_EQ(retryConfig.steps.size(), 1);
    retryConfig.steps[0].retryCount = 1;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString retryConfigPath = directory.filePath(QStringLiteral("retry-control.testcfg"));
    ASSERT_TRUE(configManager.save(retryConfigPath, retryConfig).ok());

    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    connectCollectors(service.get(), &results, &states);

    ASSERT_TRUE(service->loadConfiguration(retryConfigPath).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::Ok);
    EXPECT_EQ(result.attempts, 2);
    EXPECT_EQ(channel.openCount(), 1);
    EXPECT_EQ(channel.closeCount(), 1);
    EXPECT_EQ(channel.writeCount(), 2);
    EXPECT_EQ(channel.writeAt(0), expectedSystemStatusRequest());
    EXPECT_EQ(channel.writeAt(1), expectedSystemStatusRequest());
    const hwtest::hal::RequestId requestId = channel.lastOpenRequestId();
    EXPECT_FALSE(requestId.isEmpty());
    EXPECT_EQ(channel.lastWriteRequestId(), requestId);
    EXPECT_EQ(channel.lastReadRequestId(), requestId);
    EXPECT_EQ(channel.lastCloseRequestId(), requestId);

    ASSERT_TRUE(service->shutdown().ok());
}

TEST(SystemStatusExecutorTest, RetransmitsOnceForStaleBankErrorWithoutReopeningControl)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    QByteArray staleError;
    QByteArray response;
    ASSERT_TRUE(makeStaleSystemStatusErrorFrame(catalog, 0x1030, &staleError, &error))
        << error.toStdString();
    ASSERT_TRUE(makeSystemStatusResponseFrame(catalog, 0x1234, &response, &error))
        << error.toStdString();

    FakeControlChannel channel;
    channel.enqueueBytes(staleError);
    channel.enqueueBytes(response);
    FakeControlDevice device(&channel);
    auto transport = std::make_unique<HalControlTransport>(&device,
                                                           QStringLiteral("CONTROL_RETRY"));
    SystemStatusAlgorithmExecutor executor(std::move(transport));

    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    connectCollectors(service.get(), &results, &states);

    ASSERT_TRUE(service->loadConfiguration(
        QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG)).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::Ok);
    EXPECT_EQ(result.attempts, 1);
    EXPECT_EQ(channel.openCount(), 1);
    EXPECT_EQ(channel.closeCount(), 1);
    ASSERT_EQ(channel.writeCount(), 2);
    EXPECT_EQ(channel.writeAt(0), expectedSystemStatusRequest());
    EXPECT_EQ(channel.writeAt(1), expectedSystemStatusRequest());

    ASSERT_TRUE(service->shutdown().ok());
}

TEST(SystemStatusExecutorTest, DoesNotRetransmitCurrentRequestError)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    QByteArray response;
    ASSERT_TRUE(makeStaleSystemStatusErrorFrame(catalog, 0x1234, &response, &error))
        << error.toStdString();

    FakeControlChannel channel;
    channel.enqueueBytes(response);
    FakeControlDevice device(&channel);
    auto transport = std::make_unique<HalControlTransport>(&device,
                                                           QStringLiteral("CONTROL_ERROR"));
    SystemStatusAlgorithmExecutor executor(std::move(transport));

    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    connectCollectors(service.get(), &results, &states);

    ASSERT_TRUE(service->loadConfiguration(
        QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG)).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Error);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::ProtocolParseError);
    EXPECT_EQ(channel.openCount(), 1);
    EXPECT_EQ(channel.closeCount(), 1);
    EXPECT_EQ(channel.writeCount(), 1);

    ASSERT_TRUE(service->shutdown().ok());
}

TEST(SystemStatusExecutorTest, StopWaitsForRunScopedTransportCloseOnWorkerThread)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    auto transport = std::make_unique<BlockingByteTransport>();
    BlockingByteTransport* transportPtr = transport.get();
    SystemStatusAlgorithmExecutor executor(std::move(transport));
    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ASSERT_TRUE(service->loadConfiguration(
        QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG)).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(transportPtr->waitForTransaction(3000));

    std::atomic_bool stopReturned{false};
    hwtest::biz::Status stopStatus;
    std::thread stopper([&] {
        stopStatus = service->stopTest(1000);
        stopReturned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(stopReturned.load());

    transportPtr->release();
    stopper.join();
    EXPECT_TRUE(stopStatus.ok());
    ASSERT_TRUE(transportPtr->waitForClose(3000));
    EXPECT_EQ(transportPtr->closeThread(), transportPtr->openThread());

    ASSERT_TRUE(service->shutdown().ok());
}

TEST(MbdDfExchangeExecutorTest, SendsConfiguredTimerCleanupAfterStatistics)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    QVector<QString> requests;
    auto transport = std::make_unique<ScriptedByteTransport>(
        [&](const QByteArray& request, int) {
            QByteArray requestPayload;
            QString transportError;
            if (!decodeFrame(request, &requestPayload, &transportError) ||
                requestPayload.size() < 3) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       transportError.isEmpty()
                                           ? QStringLiteral("invalid request frame")
                                           : transportError};
            }
            const MessageDefinition* requestDefinition = catalog.findByCommand(
                static_cast<quint8>(requestPayload.at(1)),
                static_cast<quint8>(requestPayload.at(2)),
                Direction::Request);
            if (requestDefinition == nullptr) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       QStringLiteral("request definition is missing")};
            }
            requests.push_back(requestDefinition->name);

            QVariantMap requestValues;
            if (!decodePayload(*requestDefinition,
                               requestPayload,
                               &requestValues,
                               &transportError)) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       transportError};
            }
            const QString responseName = requestDefinition->name ==
                    QStringLiteral("timer_jitter_start_request")
                ? QStringLiteral("timer_jitter_start_response")
                : QStringLiteral("timer_jitter_stop_response");
            const MessageDefinition* responseDefinition = catalog.findByName(responseName);
            if (responseDefinition == nullptr) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       QStringLiteral("response definition is missing")};
            }
            QVariantMap responseValues{
                {QStringLiteral("status"), 0},
                {QStringLiteral("err_code"), 0},
            };
            if (responseName == QStringLiteral("timer_jitter_start_response")) {
                for (int index = 0; index < 8; ++index) {
                    responseValues.insert(QStringLiteral("buckets[%1]").arg(index), index + 1);
                }
                responseValues.insert(QStringLiteral("avg_jitter"), 1.25);
                responseValues.insert(QStringLiteral("max_jitter"), 4.5);
            }
            QByteArray responsePayload;
            if (!encodePayload(*responseDefinition,
                               responseValues,
                               static_cast<quint16>(requestValues.value(
                                   QStringLiteral("seq")).toUInt()),
                               &responsePayload,
                               &transportError)) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       transportError};
            }
            QByteArray responseFrame;
            if (!encodeFrame(responsePayload, &responseFrame, &transportError)) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       transportError};
            }
            return TransportResult{true, TransportResult::Error::None, responseFrame, {}};
        });
    ScriptedByteTransport* transportPtr = transport.get();
    MbdDfExchangeAlgorithmExecutor executor(
        std::move(transport),
        QStringLiteral("mbddf.timer_jitter"),
        QStringLiteral("timer_jitter_start_request"),
        QStringLiteral("timer_jitter_start_response"),
        QStringLiteral("TIMER_JITTER_START"));

    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    connectCollectors(service.get(), &results, &states);
    ASSERT_TRUE(service->loadConfiguration(
                    QStringLiteral(HWTEST_MBDDF_TIMER_JITTER_CONFIG)).ok());
    ASSERT_TRUE(service->startTest().ok());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::Ok);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests.at(0), QStringLiteral("timer_jitter_start_request"));
    EXPECT_EQ(requests.at(1), QStringLiteral("timer_jitter_stop_request"));
    EXPECT_EQ(transportPtr->transactionCount(), 2);
    EXPECT_FALSE(result.rawData.value(QStringLiteral("followUpRequestFrameHex"))
                     .toString()
                     .isEmpty());
    EXPECT_EQ(result.rawData.value(QStringLiteral("followUpResponseValues"))
                  .toMap()
                  .value(QStringLiteral("status"))
                  .toInt(),
              0);

    ASSERT_TRUE(service->shutdown().ok());
}

TEST(MbdDfExchangeExecutorTest, AllAddedConfigsPrepareAgainstTheCurrentCatalog)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    const QDir configDirectory(QFileInfo(
        QStringLiteral(HWTEST_MBDDF_TIMER_JITTER_CONFIG)).absolutePath());
    struct Case {
        QString fileName;
        QString algorithmId;
        QString requestProfileId;
        QString responseProfileId;
        QString commandName;
    };
    const QVector<Case> cases{
        {QStringLiteral("mbddf_memperf.testcfg.json"),
         QStringLiteral("mbddf.memperf"),
         QStringLiteral("memperf_test_request"),
         QStringLiteral("memperf_test_response"),
         QStringLiteral("MEMPERF_TEST")},
        {QStringLiteral("mbddf_spi_flash.testcfg.json"),
         QStringLiteral("mbddf.spi_flash"),
         QStringLiteral("spi_flash_test_request"),
         QStringLiteral("spi_flash_test_response"),
         QStringLiteral("SPI_FLASH_TEST")},
        {QStringLiteral("mbddf_dh_pulse_config.testcfg.json"),
         QStringLiteral("mbddf.dh_pulse_config"),
         QStringLiteral("dh_pulse_config_request"),
         QStringLiteral("dh_pulse_config_response"),
         QStringLiteral("DH_PULSE_CONFIG")},
        {QStringLiteral("mbddf_timer_jitter.testcfg.json"),
         QStringLiteral("mbddf.timer_jitter"),
         QStringLiteral("timer_jitter_start_request"),
         QStringLiteral("timer_jitter_start_response"),
         QStringLiteral("TIMER_JITTER_START")},
        {QStringLiteral("mbddf_di.testcfg.json"),
         QStringLiteral("mbddf.di_read"),
         QStringLiteral("di_read_request"),
         QStringLiteral("di_read_response"),
         QStringLiteral("DI_READ")},
    };

    hwtest::biz::TestConfigManager configManager;
    for (const Case& testCase : cases) {
        const QString configPath = configDirectory.filePath(testCase.fileName);
        const auto loaded = configManager.load(configPath);
        ASSERT_TRUE(loaded.ok()) << configPath.toStdString() << ": "
                                 << loaded.status.error.message.toStdString();
        ASSERT_EQ(loaded.value.steps.size(), 1);
        EXPECT_EQ(loaded.value.steps.first().algorithmId, testCase.algorithmId);

        auto transport = std::make_unique<ScriptedByteTransport>(
            [](const QByteArray&, int) {
                return TransportResult{false,
                                       TransportResult::Error::Io,
                                       {},
                                       QStringLiteral("prepare-only transport")};
            });
        MbdDfExchangeAlgorithmExecutor executor(
            std::move(transport),
            testCase.algorithmId,
            testCase.requestProfileId,
            testCase.responseProfileId,
            testCase.commandName);
        const hwtest::biz::Status prepared = executor.prepare(
            hwtest::biz::TestPlan{}, hwtest::biz::TestContext{}, loaded.value.executionConfig);
        EXPECT_TRUE(prepared.ok()) << testCase.algorithmId.toStdString() << ": "
                                   << prepared.error.message.toStdString();
    }
}

TEST(MbdDfExchangeExecutorTest, RuntimeParametersOverrideDhConfiguredRequestValues)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: "
                     << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();
    const MessageDefinition* responseDefinition =
        catalog.findByName(QStringLiteral("dh_pulse_config_response"));
    ASSERT_NE(responseDefinition, nullptr);

    QVariantMap capturedValues;
    auto transport = std::make_unique<ScriptedByteTransport>(
        [&](const QByteArray& requestFrame, int) {
            QString transportError;
            QByteArray requestPayload;
            if (!decodeFrame(requestFrame, &requestPayload, &transportError)) {
                return TransportResult{false, TransportResult::Error::Io, {},
                                       transportError};
            }
            const MessageDefinition* requestDefinition =
                catalog.findByName(QStringLiteral("dh_pulse_config_request"));
            if (requestDefinition == nullptr ||
                !decodePayload(*requestDefinition,
                               requestPayload,
                               &capturedValues,
                               &transportError)) {
                return TransportResult{false, TransportResult::Error::Io, {},
                                       transportError};
            }

            QVariantMap responseValues{{QStringLiteral("status"), 0},
                                       {QStringLiteral("err_code"), 0}};
            for (int channel = 0; channel < 23; ++channel) {
                responseValues.insert(
                    QStringLiteral("pulse_width_readback[%1]").arg(channel),
                    capturedValues.value(
                        QStringLiteral("pulse_width[%1]").arg(channel)));
            }
            QByteArray responsePayload;
            if (!encodePayload(*responseDefinition,
                               responseValues,
                               static_cast<quint16>(capturedValues.value(
                                   QStringLiteral("seq")).toUInt()),
                               &responsePayload,
                               &transportError)) {
                return TransportResult{false, TransportResult::Error::Io, {},
                                       transportError};
            }
            QByteArray responseFrame;
            if (!encodeFrame(responsePayload, &responseFrame, &transportError)) {
                return TransportResult{false, TransportResult::Error::Io, {},
                                       transportError};
            }
            return TransportResult{true, TransportResult::Error::None,
                                   responseFrame, {}};
        });
    MbdDfExchangeAlgorithmExecutor executor(
        std::move(transport),
        QStringLiteral("mbddf.dh_pulse_config"),
        QStringLiteral("dh_pulse_config_request"),
        QStringLiteral("dh_pulse_config_response"),
        QStringLiteral("DH_PULSE_CONFIG"));
    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    connectCollectors(service.get(), &results, &states);

    const QString configPath = QDir(QFileInfo(
        QStringLiteral(HWTEST_MBDDF_TIMER_JITTER_CONFIG)).absolutePath())
                                   .filePath(QStringLiteral(
                                       "mbddf_dh_pulse_config.testcfg.json"));
    ASSERT_TRUE(service->loadConfiguration(configPath).ok());
    hwtest::biz::RunOptions options;
    options.parameters.insert(QStringLiteral("pulse_width[0]"), 123);
    options.parameters.insert(QStringLiteral("pulse_width[22]"), 456);
    ASSERT_TRUE(service->startTestWithOptions(options).ok());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    EXPECT_EQ(capturedValues.value(QStringLiteral("pulse_width[0]")).toInt(),
              123);
    EXPECT_EQ(capturedValues.value(QStringLiteral("pulse_width[1]")).toInt(),
              63);
    EXPECT_EQ(capturedValues.value(QStringLiteral("pulse_width[22]")).toInt(),
              456);
    const hwtest::biz::TestResult result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::Ok);
    EXPECT_TRUE(service->shutdown().ok());
}

TEST(SystemStatusUdpIntegrationTest, UdpPeerCompletesSystemStatusThroughHalAndBIZ)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    HalServiceHandle hal(hwtest::hal::createHalService(), &hwtest::hal::destroyHalService);
    ASSERT_NE(hal, nullptr);
    ASSERT_TRUE(hal->initialize(udpHalConfiguration(peer.localPort())).ok());
    const auto session = hal->openDevice(QStringLiteral("mbddf_dut"), OperationOptions{});
    ASSERT_TRUE(session.ok());
    const auto device = hal->device(session.value);
    ASSERT_TRUE(device.ok());

    auto transport = std::make_unique<HalControlTransport>(device.value,
                                                           QStringLiteral("CONTROL_UDP"));
    SystemStatusAlgorithmExecutor executor(std::move(transport));
    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    connectCollectors(service.get(), &results, &states);
    ASSERT_TRUE(service->loadConfiguration(QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG)).ok());
    ASSERT_TRUE(service->startTest().ok());

    QByteArray request;
    QHostAddress sender;
    quint16 senderPort = 0;
    ASSERT_TRUE(receiveDatagram(peer, &request, &sender, &senderPort, 3000));
    EXPECT_EQ(request, expectedSystemStatusRequest());

    QByteArray response;
    ASSERT_TRUE(makeSystemStatusResponseFrame(catalog, 0x1234, &response, &error))
        << error.toStdString();
    ASSERT_EQ(peer.writeDatagram(response, sender, senderPort), response.size());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Pass);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::Ok);
    EXPECT_NEAR(result.rawData.value(QStringLiteral("responseValues"))
                    .toMap()
                    .value(QStringLiteral("cpu_usage"))
                    .toDouble(),
                12.5,
                1e-6);

    ASSERT_TRUE(service->shutdown().ok());
    ASSERT_TRUE(hal->closeDevice(session.value, OperationOptions{}).ok());
    ASSERT_TRUE(hal->shutdown().ok());
}

TEST(SystemStatusUdpIntegrationTest, NoResponseBecomesBusTimeoutThroughBIZ)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    HalServiceHandle hal(hwtest::hal::createHalService(), &hwtest::hal::destroyHalService);
    ASSERT_NE(hal, nullptr);
    ASSERT_TRUE(hal->initialize(udpHalConfiguration(peer.localPort())).ok());
    const auto session = hal->openDevice(QStringLiteral("mbddf_dut"), OperationOptions{});
    ASSERT_TRUE(session.ok());
    const auto device = hal->device(session.value);
    ASSERT_TRUE(device.ok());

    auto transport = std::make_unique<HalControlTransport>(device.value,
                                                           QStringLiteral("CONTROL_UDP"));
    SystemStatusAlgorithmExecutor executor(std::move(transport));
    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    connectCollectors(service.get(), &results, &states);
    ASSERT_TRUE(service->loadConfiguration(QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG)).ok());
    ASSERT_TRUE(service->startTest().ok());

    QByteArray request;
    QHostAddress sender;
    quint16 senderPort = 0;
    ASSERT_TRUE(receiveDatagram(peer, &request, &sender, &senderPort, 3000));
    EXPECT_EQ(request, expectedSystemStatusRequest());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Error);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::BusTimeout);

    ASSERT_TRUE(service->shutdown().ok());
    ASSERT_TRUE(hal->closeDevice(session.value, OperationOptions{}).ok());
    ASSERT_TRUE(hal->shutdown().ok());
}

TEST(SystemStatusUdpIntegrationTest, BadCrcDatagramBecomesProtocolErrorThroughBIZ)
{
    const QString assets = catalogDirectory();
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present: " << assets.toStdString();
    }
    qputenv("MB_DDF_PROTOCOL_CSV_DIR", assets.toUtf8());

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(assets, &error)) << error.toStdString();

    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    HalServiceHandle hal(hwtest::hal::createHalService(), &hwtest::hal::destroyHalService);
    ASSERT_NE(hal, nullptr);
    ASSERT_TRUE(hal->initialize(udpHalConfiguration(peer.localPort())).ok());
    const auto session = hal->openDevice(QStringLiteral("mbddf_dut"), OperationOptions{});
    ASSERT_TRUE(session.ok());
    const auto device = hal->device(session.value);
    ASSERT_TRUE(device.ok());

    auto transport = std::make_unique<HalControlTransport>(device.value,
                                                           QStringLiteral("CONTROL_UDP"));
    SystemStatusAlgorithmExecutor executor(std::move(transport));
    RunServiceHandle service = makeRunService(&executor);
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->initialize().ok());
    ResultCollector results;
    StateCollector states;
    connectCollectors(service.get(), &results, &states);
    ASSERT_TRUE(service->loadConfiguration(QStringLiteral(HWTEST_MBDDF_SYSTEM_STATUS_CONFIG)).ok());
    ASSERT_TRUE(service->startTest().ok());

    QByteArray request;
    QHostAddress sender;
    quint16 senderPort = 0;
    ASSERT_TRUE(receiveDatagram(peer, &request, &sender, &senderPort, 3000));
    EXPECT_EQ(request, expectedSystemStatusRequest());

    QByteArray response;
    ASSERT_TRUE(makeSystemStatusResponseFrame(catalog, 0x1234, &response, &error))
        << error.toStdString();
    ASSERT_FALSE(response.isEmpty());
    response[response.size() - 1] = static_cast<char>(response.at(response.size() - 1) ^ 0xFF);
    ASSERT_EQ(peer.writeDatagram(response, sender, senderPort), response.size());
    ASSERT_TRUE(results.waitForResult(3000));
    ASSERT_TRUE(states.waitForTerminal(3000));

    const auto result = results.result();
    EXPECT_EQ(result.verdict, hwtest::biz::TestVerdict::Error);
    EXPECT_EQ(result.errorCode, hwtest::biz::ErrorCode::ProtocolParseError);

    ASSERT_TRUE(service->shutdown().ok());
    ASSERT_TRUE(hal->closeDevice(session.value, OperationOptions{}).ok());
    ASSERT_TRUE(hal->shutdown().ok());
}

} // namespace
} // namespace hwtest::algorithm::mbddf
