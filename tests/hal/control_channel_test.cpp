#include "hal/hal_factory.h"
#include "hal/i_control_channel.h"
#include "hal/i_hal_device.h"
#include "hal/i_hal_service.h"

#include "control_channel_manager.h"
#include "control_io_provider.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QThread>
#include <QUdpSocket>

#include <memory>

using namespace hwtest::hal;

namespace {

const ResourceId kControlResourceId = QStringLiteral("CONTROL_CHANNEL");
const ResourceId kControlResourceA = QStringLiteral("CONTROL_CHANNEL_A");
const ResourceId kControlResourceB = QStringLiteral("CONTROL_CHANNEL_B");

QCoreApplication& ensureQtApplication()
{
    if (QCoreApplication* existing = QCoreApplication::instance()) {
        return *existing;
    }

    static int argc = 1;
    static char argument[] = "hwtest_hal_tests";
    static char* argv[] = {argument, nullptr};
    static QCoreApplication application(argc, argv);
    return application;
}

QVariantMap controlHalConfig(const QString& providerId,
                             const QVariantMap& properties,
                             bool includeProviderId = true)
{
    QVariantMap config = testsupport::defaultHalConfig();
    QVariantMap hardware = config.value(QStringLiteral("hardware")).toMap();
    QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    QVariantMap control = testsupport::makeResource(QStringLiteral("main_daq"),
                                                     QStringLiteral("control"),
                                                     QStringLiteral("bidirectional"),
                                                     7);
    if (includeProviderId) {
        control.insert(QStringLiteral("providerId"), providerId);
    }
    control.insert(QStringLiteral("properties"), properties);
    resources.insert(kControlResourceId, control);
    hardware.insert(QStringLiteral("resources"), resources);
    config.insert(QStringLiteral("hardware"), hardware);
    return config;
}

void addControlResource(QVariantMap* config,
                        const ResourceId& resourceId,
                        const QString& providerId,
                        const QVariantMap& properties,
                        int physicalIndex)
{
    QVariantMap hardware = config->value(QStringLiteral("hardware")).toMap();
    QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    QVariantMap control = testsupport::makeResource(QStringLiteral("main_daq"),
                                                     QStringLiteral("control"),
                                                     QStringLiteral("bidirectional"),
                                                     physicalIndex);
    control.insert(QStringLiteral("providerId"), providerId);
    control.insert(QStringLiteral("properties"), properties);
    resources.insert(resourceId, control);
    hardware.insert(QStringLiteral("resources"), resources);
    config->insert(QStringLiteral("hardware"), hardware);
}

QVariantMap dualUdpControlHalConfig(const QVariantMap& firstProperties,
                                    const QVariantMap& secondProperties)
{
    QVariantMap config = testsupport::defaultHalConfig();
    addControlResource(&config,
                       kControlResourceA,
                       QStringLiteral("qt.udp"),
                       firstProperties,
                       7);
    addControlResource(&config,
                       kControlResourceB,
                       QStringLiteral("qt.udp"),
                       secondProperties,
                       8);
    return config;
}

quint16 reserveLoopbackUdpPort()
{
    QUdpSocket reservation;
    if (!reservation.bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        return 0;
    }
    return reservation.localPort();
}

struct ControlProviderCall {
    QString operation;
    ResourceId resourceId;
    QThread* thread = nullptr;
};

struct ControlledProviderEvents {
    QVector<ControlProviderCall> calls;
    QVector<ResourceId> closeOrder;
};

class ControlledControlProvider final : public ControlIoProvider {
public:
    ControlledControlProvider(ResourceId resourceId,
                              ControlledProviderEvents* events,
                              HalStatusCode closeStatus = HalStatusCode::Ok)
        : m_resourceId(std::move(resourceId))
        , m_events(events)
        , m_closeStatus(closeStatus)
    {
    }

    HalStatus open(const QVariantMap& properties, const OperationOptions& options) override
    {
        Q_UNUSED(properties)
        Q_UNUSED(options)
        record(QStringLiteral("open"));
        return {};
    }

    HalStatus close(const OperationOptions& options) override
    {
        Q_UNUSED(options)
        record(QStringLiteral("close"));
        m_events->closeOrder.push_back(m_resourceId);
        if (m_closeStatus == HalStatusCode::Ok) {
            return {};
        }

        HalStatus status;
        status.code = m_closeStatus;
        status.error.code = m_closeStatus;
        status.error.operation = QStringLiteral("fixture.control.close");
        status.error.message = QStringLiteral("Controlled close failure");
        return status;
    }

    HalStatus write(const QByteArray& data, const OperationOptions& options) override
    {
        Q_UNUSED(data)
        Q_UNUSED(options)
        record(QStringLiteral("write"));
        return {};
    }

    HalResult<QByteArray> read(int maxBytes, const OperationOptions& options) override
    {
        Q_UNUSED(maxBytes)
        Q_UNUSED(options)
        record(QStringLiteral("read"));
        HalResult<QByteArray> result;
        result.value = m_resourceId.toUtf8();
        return result;
    }

private:
    void record(const QString& operation)
    {
        m_events->calls.push_back({operation, m_resourceId, QThread::currentThread()});
    }

    ResourceId m_resourceId;
    ControlledProviderEvents* m_events = nullptr;
    HalStatusCode m_closeStatus = HalStatusCode::Ok;
};

ResourceBinding managerControlBinding(const ResourceId& resourceId,
                                      const QString& providerId,
                                      const QVariantMap& properties = {})
{
    ResourceBinding binding;
    binding.resourceId = resourceId;
    binding.deviceId = QStringLiteral("manager-test-device");
    binding.module = QStringLiteral("control");
    binding.direction = QStringLiteral("bidirectional");
    binding.physicalIndex = 0;
    binding.providerId = providerId;
    binding.properties = properties;
    return binding;
}

class ControlChannelTest : public ::testing::Test {
protected:
    void initialize(const QVariantMap& config)
    {
        m_service.reset(createHalService());
        ASSERT_NE(m_service, nullptr);
        ASSERT_TRUE(m_service->initialize(config).ok());

        const HalResult<SessionId> session =
            m_service->openDevice(QStringLiteral("main_daq"), OperationOptions{});
        ASSERT_TRUE(session.ok());
        m_sessionId = session.value;

        const HalResult<IHalDevice*> device = m_service->device(m_sessionId);
        ASSERT_TRUE(device.ok());
        ASSERT_NE(device.value, nullptr);
        m_channel = device.value->controlChannel();
        ASSERT_NE(m_channel, nullptr);
    }

    void TearDown() override
    {
        if (m_service != nullptr && !m_sessionId.isEmpty()) {
            m_service->closeDevice(m_sessionId, OperationOptions{});
        }
        if (m_service != nullptr) {
            m_service->shutdown();
        }
    }

    std::unique_ptr<IHalService> m_service;
    SessionId m_sessionId;
    IControlChannel* m_channel = nullptr;
};

TEST_F(ControlChannelTest, MissingProviderIdIsInvalidArgument)
{
    initialize(controlHalConfig({}, {}, false));

    EXPECT_EQ(m_channel->openControl(kControlResourceId, OperationOptions{}).code,
              HalStatusCode::InvalidArgument);
}

TEST_F(ControlChannelTest, UnknownProviderReportsProviderId)
{
    initialize(controlHalConfig(QStringLiteral("unknown.provider"), {}));

    const HalStatus status =
        m_channel->openControl(kControlResourceId, OperationOptions{});
    EXPECT_EQ(status.code, HalStatusCode::NotSupported);
    EXPECT_EQ(status.error.detail.value(QStringLiteral("providerId")).toString(),
              QStringLiteral("unknown.provider"));
}

TEST_F(ControlChannelTest, SerialProviderRequiresPortNameBeforeOpeningDevice)
{
    const QVariantMap properties = {
        {QStringLiteral("baudRate"), 614400},
        {QStringLiteral("dataBits"), 8},
        {QStringLiteral("parity"), QStringLiteral("Even")},
        {QStringLiteral("stopBits"), 1},
        {QStringLiteral("flowControl"), QStringLiteral("None")},
    };
    initialize(controlHalConfig(QStringLiteral("qt.serial"), properties));

    EXPECT_EQ(m_channel->openControl(kControlResourceId, OperationOptions{}).code,
              HalStatusCode::InvalidArgument);
}

TEST_F(ControlChannelTest, UdpProviderRequiresRemoteEndpoint)
{
    initialize(controlHalConfig(QStringLiteral("qt.udp"), {}));

    EXPECT_EQ(m_channel->openControl(kControlResourceId, OperationOptions{}).code,
              HalStatusCode::InvalidArgument);
}

TEST_F(ControlChannelTest, UdpLoopbackRoundTripsExactRawBytes)
{
    ensureQtApplication();
    QUdpSocket peer;
    QUdpSocket unexpectedPeer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    ASSERT_TRUE(unexpectedPeer.bind(QHostAddress(QHostAddress::LocalHost), 0));

    const QVariantMap properties = {
        {QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("remotePort"), static_cast<int>(peer.localPort())},
    };
    initialize(controlHalConfig(QStringLiteral("qt.udp"), properties));

    ASSERT_TRUE(m_channel->openControl(kControlResourceId, OperationOptions{}).ok());
    const QByteArray request = QByteArray::fromHex("55AA03112233");
    const HalStatus writeStatus =
        m_channel->writeControl(kControlResourceId, request, OperationOptions{});
    ASSERT_TRUE(writeStatus.ok()) << writeStatus.error.message.toStdString()
                                  << ", code=" << static_cast<int>(writeStatus.code);

    ASSERT_TRUE(peer.waitForReadyRead(1000));
    const qint64 requestSize = peer.pendingDatagramSize();
    ASSERT_EQ(requestSize, request.size());
    QByteArray received(static_cast<int>(requestSize), Qt::Uninitialized);
    QHostAddress senderAddress;
    quint16 senderPort = 0;
    ASSERT_EQ(peer.readDatagram(received.data(), received.size(), &senderAddress, &senderPort), requestSize);
    EXPECT_EQ(received, request);

    const QByteArray response = QByteArray::fromHex("55AA03445566");
    ASSERT_EQ(unexpectedPeer.writeDatagram(response, senderAddress, senderPort), response.size());
    OperationOptions unexpectedReadOptions;
    unexpectedReadOptions.timeoutMs = 75;
    EXPECT_EQ(m_channel->readControl(kControlResourceId, 64, unexpectedReadOptions).status.code,
              HalStatusCode::Timeout);

    ASSERT_EQ(peer.writeDatagram(response, senderAddress, senderPort), response.size());

    OperationOptions readOptions;
    readOptions.timeoutMs = 1000;
    const HalResult<QByteArray> read =
        m_channel->readControl(kControlResourceId, 64, readOptions);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value, response);
}

TEST_F(ControlChannelTest, DistinctUdpControlResourcesRemainIndependent)
{
    ensureQtApplication();
    QUdpSocket firstPeer;
    QUdpSocket secondPeer;
    ASSERT_TRUE(firstPeer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    ASSERT_TRUE(secondPeer.bind(QHostAddress(QHostAddress::LocalHost), 0));

    const QVariantMap firstProperties = {
        {QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("remotePort"), static_cast<int>(firstPeer.localPort())},
    };
    const QVariantMap secondProperties = {
        {QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("remotePort"), static_cast<int>(secondPeer.localPort())},
    };
    initialize(dualUdpControlHalConfig(firstProperties, secondProperties));

    ASSERT_TRUE(m_channel->openControl(kControlResourceA, OperationOptions{}).ok());
    ASSERT_TRUE(m_channel->openControl(kControlResourceB, OperationOptions{}).ok());
    EXPECT_EQ(m_channel->openControl(kControlResourceB, OperationOptions{}).code,
              HalStatusCode::Busy);

    const QByteArray firstRequest = QByteArray::fromHex("01020304");
    const QByteArray secondRequest = QByteArray::fromHex("A1B2C3D4");
    ASSERT_TRUE(m_channel->writeControl(kControlResourceA, firstRequest, OperationOptions{}).ok());
    ASSERT_TRUE(m_channel->writeControl(kControlResourceB, secondRequest, OperationOptions{}).ok());

    ASSERT_TRUE(firstPeer.waitForReadyRead(1000));
    const qint64 firstRequestSize = firstPeer.pendingDatagramSize();
    ASSERT_EQ(firstRequestSize, firstRequest.size());
    QByteArray firstReceived(static_cast<int>(firstRequestSize), Qt::Uninitialized);
    QHostAddress firstSender;
    quint16 firstSenderPort = 0;
    ASSERT_EQ(firstPeer.readDatagram(firstReceived.data(),
                                     firstReceived.size(),
                                     &firstSender,
                                     &firstSenderPort),
              firstRequestSize);
    EXPECT_EQ(firstReceived, firstRequest);

    ASSERT_TRUE(secondPeer.waitForReadyRead(1000));
    const qint64 secondRequestSize = secondPeer.pendingDatagramSize();
    ASSERT_EQ(secondRequestSize, secondRequest.size());
    QByteArray secondReceived(static_cast<int>(secondRequestSize), Qt::Uninitialized);
    QHostAddress secondSender;
    quint16 secondSenderPort = 0;
    ASSERT_EQ(secondPeer.readDatagram(secondReceived.data(),
                                      secondReceived.size(),
                                      &secondSender,
                                      &secondSenderPort),
              secondRequestSize);
    EXPECT_EQ(secondReceived, secondRequest);

    const QByteArray firstResponse = QByteArray::fromHex("11121314");
    const QByteArray secondResponse = QByteArray::fromHex("D1D2D3D4");
    ASSERT_EQ(firstPeer.writeDatagram(firstResponse, firstSender, firstSenderPort),
              firstResponse.size());
    ASSERT_EQ(secondPeer.writeDatagram(secondResponse, secondSender, secondSenderPort),
              secondResponse.size());

    OperationOptions readOptions;
    readOptions.timeoutMs = 1000;
    const HalResult<QByteArray> firstRead =
        m_channel->readControl(kControlResourceA, 64, readOptions);
    const HalResult<QByteArray> secondRead =
        m_channel->readControl(kControlResourceB, 64, readOptions);
    ASSERT_TRUE(firstRead.ok());
    ASSERT_TRUE(secondRead.ok());
    EXPECT_EQ(firstRead.value, firstResponse);
    EXPECT_EQ(secondRead.value, secondResponse);

    ASSERT_TRUE(m_channel->closeControl(kControlResourceA, OperationOptions{}).ok());
    EXPECT_EQ(m_channel->writeControl(kControlResourceA,
                                      QByteArrayLiteral("closed"),
                                      OperationOptions{}).code,
              HalStatusCode::InvalidState);

    const QByteArray survivingRequest = QByteArray::fromHex("E1E2E3E4");
    ASSERT_TRUE(m_channel->writeControl(kControlResourceB,
                                        survivingRequest,
                                        OperationOptions{}).ok());
    ASSERT_TRUE(secondPeer.waitForReadyRead(1000));
    const qint64 survivingRequestSize = secondPeer.pendingDatagramSize();
    ASSERT_EQ(survivingRequestSize, survivingRequest.size());
    QByteArray survivingReceived(static_cast<int>(survivingRequestSize), Qt::Uninitialized);
    ASSERT_EQ(secondPeer.readDatagram(survivingReceived.data(), survivingReceived.size()),
              survivingRequestSize);
    EXPECT_EQ(survivingReceived, survivingRequest);
}

TEST_F(ControlChannelTest, ClosingDeviceClosesEachOpenUdpControlResource)
{
    ensureQtApplication();
    QUdpSocket firstPeer;
    QUdpSocket secondPeer;
    ASSERT_TRUE(firstPeer.bind(QHostAddress(QHostAddress::LocalHost), 0));
    ASSERT_TRUE(secondPeer.bind(QHostAddress(QHostAddress::LocalHost), 0));

    const quint16 firstLocalPort = reserveLoopbackUdpPort();
    const quint16 secondLocalPort = reserveLoopbackUdpPort();
    ASSERT_NE(firstLocalPort, 0);
    ASSERT_NE(secondLocalPort, 0);
    ASSERT_NE(firstLocalPort, secondLocalPort);

    const QVariantMap firstProperties = {
        {QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("remotePort"), static_cast<int>(firstPeer.localPort())},
        {QStringLiteral("localAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("localPort"), static_cast<int>(firstLocalPort)},
    };
    const QVariantMap secondProperties = {
        {QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("remotePort"), static_cast<int>(secondPeer.localPort())},
        {QStringLiteral("localAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("localPort"), static_cast<int>(secondLocalPort)},
    };
    initialize(dualUdpControlHalConfig(firstProperties, secondProperties));

    ASSERT_TRUE(m_channel->openControl(kControlResourceA, OperationOptions{}).ok());
    ASSERT_TRUE(m_channel->openControl(kControlResourceB, OperationOptions{}).ok());

    ASSERT_TRUE(m_service->closeDevice(m_sessionId, OperationOptions{}).ok());
    m_sessionId.clear();

    QUdpSocket firstRebind;
    QUdpSocket secondRebind;
    EXPECT_TRUE(firstRebind.bind(QHostAddress(QHostAddress::LocalHost), firstLocalPort));
    EXPECT_TRUE(secondRebind.bind(QHostAddress(QHostAddress::LocalHost), secondLocalPort));
}

TEST(ControlChannelManagerTest, CloseAllClosesEverySessionInResourceIdOrderAndPreservesFirstFailure)
{
    ControlledProviderEvents events;
    ControlChannelManager manager(
        [&events](const ResourceBinding& binding) -> std::unique_ptr<ControlIoProvider> {
            HalStatusCode closeStatus = HalStatusCode::Ok;
            if (binding.resourceId == kControlResourceA) {
                closeStatus = HalStatusCode::IoError;
            } else if (binding.resourceId == kControlResourceB) {
                closeStatus = HalStatusCode::Timeout;
            }
            return std::make_unique<ControlledControlProvider>(binding.resourceId,
                                                                &events,
                                                                closeStatus);
        });

    const ResourceBinding first = managerControlBinding(kControlResourceA, QStringLiteral("qt.udp"));
    const ResourceBinding second = managerControlBinding(kControlResourceB, QStringLiteral("qt.udp"));
    const ResourceBinding third = managerControlBinding(QStringLiteral("CONTROL_CHANNEL_C"),
                                                         QStringLiteral("qt.udp"));

    ASSERT_TRUE(manager.open(third, OperationOptions{}).ok());
    ASSERT_TRUE(manager.open(second, OperationOptions{}).ok());
    ASSERT_TRUE(manager.open(first, OperationOptions{}).ok());

    const HalStatus closeStatus = manager.closeAll(OperationOptions{});
    EXPECT_EQ(closeStatus.code, HalStatusCode::IoError);
    EXPECT_EQ(closeStatus.error.resourceId, kControlResourceA);
    const QVector<ResourceId> expectedCloseOrder = {
        kControlResourceA,
        kControlResourceB,
        QStringLiteral("CONTROL_CHANNEL_C"),
    };
    ASSERT_EQ(events.closeOrder.size(), expectedCloseOrder.size());
    for (int index = 0; index < expectedCloseOrder.size(); ++index) {
        EXPECT_EQ(events.closeOrder.at(index), expectedCloseOrder.at(index));
    }
    EXPECT_EQ(manager.write(first, QByteArrayLiteral("A"), OperationOptions{}).code,
              HalStatusCode::InvalidState);
    EXPECT_EQ(manager.write(second, QByteArrayLiteral("B"), OperationOptions{}).code,
              HalStatusCode::InvalidState);
    EXPECT_EQ(manager.write(third, QByteArrayLiteral("C"), OperationOptions{}).code,
              HalStatusCode::InvalidState);
}

TEST(ControlChannelManagerTest, ProviderOperationsRunInTheCallingThread)
{
    ControlledProviderEvents events;
    ControlChannelManager manager(
        [&events](const ResourceBinding& binding) -> std::unique_ptr<ControlIoProvider> {
            return std::make_unique<ControlledControlProvider>(binding.resourceId, &events);
        });
    const ResourceBinding binding = managerControlBinding(kControlResourceA, QStringLiteral("qt.udp"));
    QThread* const callingThread = QThread::currentThread();

    ASSERT_TRUE(manager.open(binding, OperationOptions{}).ok());
    ASSERT_TRUE(manager.write(binding, QByteArrayLiteral("data"), OperationOptions{}).ok());
    const HalResult<QByteArray> read = manager.read(binding, 64, OperationOptions{});
    ASSERT_TRUE(read.ok());
    ASSERT_TRUE(manager.close(binding, OperationOptions{}).ok());

    ASSERT_EQ(events.calls.size(), 4);
    for (const ControlProviderCall& call : events.calls) {
        EXPECT_EQ(call.thread, callingThread) << call.operation.toStdString();
    }
}

TEST(ControlChannelManagerTest, RejectsDuplicateQtSerialPortBeforeOpeningSecondProvider)
{
    ControlledProviderEvents events;
    ControlChannelManager manager(
        [&events](const ResourceBinding& binding) -> std::unique_ptr<ControlIoProvider> {
            return std::make_unique<ControlledControlProvider>(binding.resourceId, &events);
        });
    const ResourceBinding first = managerControlBinding(
        QStringLiteral("CONTROL_SERIAL_A"),
        QStringLiteral("qt.serial"),
        {{QStringLiteral("portName"), QStringLiteral(" COM_TEST ")}});
#ifdef Q_OS_WIN
    const QString duplicatePortName = QStringLiteral("com_test");
#else
    const QString duplicatePortName = QStringLiteral("COM_TEST");
#endif
    const ResourceBinding second = managerControlBinding(
        QStringLiteral("CONTROL_SERIAL_B"),
        QStringLiteral("qt.serial"),
        {{QStringLiteral("portName"), duplicatePortName}});

    ASSERT_TRUE(manager.open(first, OperationOptions{}).ok());
    const HalStatus duplicateStatus = manager.open(second, OperationOptions{});
    EXPECT_EQ(duplicateStatus.code, HalStatusCode::Busy);
    EXPECT_EQ(duplicateStatus.error.resourceId, second.resourceId);
    EXPECT_EQ(duplicateStatus.error.detail.value(QStringLiteral("portName")).toString(),
              duplicatePortName);
    EXPECT_EQ(duplicateStatus.error.detail.value(QStringLiteral("conflictingResourceId")).toString(),
              first.resourceId);
    EXPECT_EQ(events.calls.size(), 1);
    EXPECT_EQ(events.calls.front().resourceId, first.resourceId);
    ASSERT_TRUE(manager.closeAll(OperationOptions{}).ok());
}

TEST_F(ControlChannelTest, UdpReadWithoutResponseTimesOut)
{
    ensureQtApplication();
    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));

    const QVariantMap properties = {
        {QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("remotePort"), static_cast<int>(peer.localPort())},
    };
    initialize(controlHalConfig(QStringLiteral("qt.udp"), properties));

    ASSERT_TRUE(m_channel->openControl(kControlResourceId, OperationOptions{}).ok());
    OperationOptions readOptions;
    readOptions.timeoutMs = 75;
    const HalResult<QByteArray> read =
        m_channel->readControl(kControlResourceId, 64, readOptions);
    EXPECT_EQ(read.status.code, HalStatusCode::Timeout)
        << read.status.error.message.toStdString()
        << ", code=" << static_cast<int>(read.status.code);
}

TEST_F(ControlChannelTest, ReadAfterCloseIsInvalidState)
{
    ensureQtApplication();
    QUdpSocket peer;
    ASSERT_TRUE(peer.bind(QHostAddress(QHostAddress::LocalHost), 0));

    const QVariantMap properties = {
        {QStringLiteral("remoteAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("remotePort"), static_cast<int>(peer.localPort())},
    };
    initialize(controlHalConfig(QStringLiteral("qt.udp"), properties));

    ASSERT_TRUE(m_channel->openControl(kControlResourceId, OperationOptions{}).ok());
    ASSERT_TRUE(m_channel->closeControl(kControlResourceId, OperationOptions{}).ok());
    EXPECT_EQ(m_channel->readControl(kControlResourceId, 64, OperationOptions{}).status.code,
              HalStatusCode::InvalidState);
}

} // namespace
