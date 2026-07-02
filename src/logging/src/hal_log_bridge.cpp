#include "logging/hal_log_bridge.h"

#include "hal/i_hal_service.h"

#include <QObject>

namespace hwtest::logging {

namespace {

void insertIfNotEmpty(QVariantMap* context, const QString& key, const QString& value)
{
    if (!value.isEmpty()) {
        context->insert(key, value);
    }
}

} // namespace

LogEvent fromHalLogEvent(const hwtest::hal::HalLogEvent& event)
{
    LogEvent result;
    result.timestampUs = event.timestampUs;
    result.level = event.level;
    result.source = event.source.isEmpty() ? QStringLiteral("hal") : event.source;
    result.category = event.category;
    result.message = event.message;
    result.requestId = event.requestId;
    result.durationMs = event.durationMs;
    result.status = event.status;
    result.adapterCode = event.adapterCode;

    QVariantMap context = event.context;
    insertIfNotEmpty(&context, QStringLiteral("requestId"), result.requestId);
    if (result.durationMs >= 0) {
        context.insert(QStringLiteral("durationMs"), result.durationMs);
    }
    insertIfNotEmpty(&context, QStringLiteral("status"), result.status);
    insertIfNotEmpty(&context, QStringLiteral("adapterCode"), result.adapterCode);
    insertIfNotEmpty(&context, QStringLiteral("deviceId"), event.deviceId);
    insertIfNotEmpty(&context, QStringLiteral("resourceId"), event.resourceId);
    insertIfNotEmpty(&context, QStringLiteral("operation"), event.operation);
    result.context = context;
    return result;
}

QMetaObject::Connection connectHalLogs(hwtest::hal::IHalService* halService,
                                       ILogService* logService,
                                       Qt::ConnectionType connectionType)
{
    if (halService == nullptr || logService == nullptr) {
        return {};
    }

    return QObject::connect(halService,
                            &hwtest::hal::IHalService::logProduced,
                            logService,
                            [logService](const hwtest::hal::HalLogEvent& event) {
                                logService->append(fromHalLogEvent(event));
                            },
                            connectionType);
}

} // namespace hwtest::logging
