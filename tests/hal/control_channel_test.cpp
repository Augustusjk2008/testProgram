#include "hal/hal_factory.h"
#include "hal/i_control_channel.h"
#include "hal/i_hal_device.h"
#include "hal/i_hal_service.h"

#include "test_support.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QUdpSocket>

#include <memory>

using namespace hwtest::hal;

namespace {

const ResourceId kControlResourceId = QStringLiteral("CONTROL_CHANNEL");

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
