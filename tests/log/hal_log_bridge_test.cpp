#include "logging/hal_log_bridge.h"
#include "logging/log_service.h"

#include "hal/i_hal_service.h"

#include <QObject>

#include <gtest/gtest.h>

using namespace hwtest::hal;
using namespace hwtest::logging;

namespace {

class FakeHalService final : public IHalService {
    Q_OBJECT

public:
    using IHalService::IHalService;

    HalStatus initialize(const QVariantMap& halConfig) override
    {
        Q_UNUSED(halConfig)
        return {};
    }

    HalStatus shutdown() override
    {
        return {};
    }

    HalResult<QVector<DeviceDescriptor>> scanDevices(const OperationOptions& options) override
    {
        Q_UNUSED(options)
        return {};
    }

    HalResult<DeviceCapabilities> queryCapabilities(const DeviceId& deviceId,
                                                    const OperationOptions& options) override
    {
        Q_UNUSED(deviceId)
        Q_UNUSED(options)
        return {};
    }

    HalResult<SessionId> openDevice(const DeviceId& deviceId,
                                    const OperationOptions& options) override
    {
        Q_UNUSED(deviceId)
        Q_UNUSED(options)
        return {};
    }

    HalStatus closeDevice(const SessionId& sessionId,
                          const OperationOptions& options) override
    {
        Q_UNUSED(sessionId)
        Q_UNUSED(options)
        return {};
    }

    HalStatus resetDevice(const SessionId& sessionId,
                          const OperationOptions& options) override
    {
        Q_UNUSED(sessionId)
        Q_UNUSED(options)
        return {};
    }

    HalStatus healthCheck(const SessionId& sessionId,
                          const OperationOptions& options) override
    {
        Q_UNUSED(sessionId)
        Q_UNUSED(options)
        return {};
    }

    HalResult<IHalDevice*> device(const SessionId& sessionId) override
    {
        Q_UNUSED(sessionId)
        return {};
    }

    void produce(const HalLogEvent& event)
    {
        emit logProduced(event);
    }
};

} // namespace

TEST(HalLogBridgeTest, MapsHalFieldsToMainLogEventAndContext)
{
    HalLogEvent halEvent;
    halEvent.timestampUs = 456789;
    halEvent.level = QStringLiteral("ERROR");
    halEvent.category = QStringLiteral("hal.analog.readAd");
    halEvent.message = QStringLiteral("read failed");
    halEvent.requestId = QStringLiteral("req-7");
    halEvent.durationMs = 23;
    halEvent.status = QStringLiteral("IoError");
    halEvent.adapterCode = QStringLiteral("E_IO");
    halEvent.deviceId = QStringLiteral("main_daq");
    halEvent.resourceId = QStringLiteral("AD_MAIN_0");
    halEvent.operation = QStringLiteral("readAd");
    halEvent.context.insert(QStringLiteral("slot"), 3);

    const LogEvent mapped = fromHalLogEvent(halEvent);

    EXPECT_EQ(mapped.timestampUs, 456789);
    EXPECT_EQ(mapped.level, QStringLiteral("ERROR"));
    EXPECT_EQ(mapped.source, QStringLiteral("hal"));
    EXPECT_EQ(mapped.requestId, QStringLiteral("req-7"));
    EXPECT_EQ(mapped.durationMs, 23);
    EXPECT_EQ(mapped.status, QStringLiteral("IoError"));
    EXPECT_EQ(mapped.adapterCode, QStringLiteral("E_IO"));
    EXPECT_EQ(mapped.context.value(QStringLiteral("slot")).toInt(), 3);
    EXPECT_EQ(mapped.context.value(QStringLiteral("requestId")).toString(), QStringLiteral("req-7"));
    EXPECT_EQ(mapped.context.value(QStringLiteral("durationMs")).toLongLong(), 23);
    EXPECT_EQ(mapped.context.value(QStringLiteral("status")).toString(), QStringLiteral("IoError"));
    EXPECT_EQ(mapped.context.value(QStringLiteral("adapterCode")).toString(), QStringLiteral("E_IO"));
    EXPECT_EQ(mapped.context.value(QStringLiteral("deviceId")).toString(), QStringLiteral("main_daq"));
    EXPECT_EQ(mapped.context.value(QStringLiteral("resourceId")).toString(), QStringLiteral("AD_MAIN_0"));
    EXPECT_EQ(mapped.context.value(QStringLiteral("operation")).toString(), QStringLiteral("readAd"));
}

TEST(HalLogBridgeTest, ConnectsHalSignalToLogServiceAppend)
{
    FakeHalService halService;
    LogService logService;

    const QMetaObject::Connection connection = connectHalLogs(&halService, &logService);
    ASSERT_TRUE(static_cast<bool>(connection));

    HalLogEvent halEvent;
    halEvent.level = QStringLiteral("INFO");
    halEvent.source = QStringLiteral("adapter");
    halEvent.message = QStringLiteral("adapter message");
    halEvent.requestId = QStringLiteral("req-8");
    halEvent.deviceId = QStringLiteral("main_daq");
    halService.produce(halEvent);

    const QVector<LogEvent> recent = logService.recent(1);
    ASSERT_EQ(recent.size(), 1);
    EXPECT_EQ(recent.first().source, QStringLiteral("adapter"));
    EXPECT_EQ(recent.first().message, QStringLiteral("adapter message"));
    EXPECT_EQ(recent.first().context.value(QStringLiteral("deviceId")).toString(),
              QStringLiteral("main_daq"));
}

#include "hal_log_bridge_test.moc"
