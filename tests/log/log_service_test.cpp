#include "logging/log_service.h"

#include <QObject>
#include <QVector>

#include <gtest/gtest.h>

using namespace hwtest::logging;

namespace {

class RecordingSink final : public ILogSink {
public:
    void append(const LogEvent& event) override
    {
        events.push_back(event);
    }

    QVector<LogEvent> events;
};

LogEvent makeEvent(const QString& level, const QString& message)
{
    LogEvent event;
    event.level = level;
    event.message = message;
    return event;
}

} // namespace

TEST(LogServiceTest, AppendNormalizesTimestampLevelRecentAndSignal)
{
    LogService service;
    service.setRecentLimit(2);

    QVector<LogEvent> emitted;
    QObject::connect(&service,
                     &ILogService::logAppended,
                     &service,
                     [&emitted](const LogEvent& event) {
                         emitted.push_back(event);
                     });

    LogEvent first;
    first.message = QStringLiteral("first");
    service.append(first);
    service.append(makeEvent(QStringLiteral("debug"), QStringLiteral("second")));
    service.append(makeEvent(QStringLiteral("WARN"), QStringLiteral("third")));

    ASSERT_EQ(emitted.size(), 3);
    EXPECT_GT(emitted.first().timestampUs, 0);
    EXPECT_EQ(emitted.first().level, QStringLiteral("INFO"));

    const QVector<LogEvent> recent = service.recent(10);
    ASSERT_EQ(recent.size(), 2);
    EXPECT_EQ(recent.at(0).message, QStringLiteral("second"));
    EXPECT_EQ(recent.at(0).level, QStringLiteral("DEBUG"));
    EXPECT_EQ(recent.at(1).message, QStringLiteral("third"));
}

TEST(LogServiceTest, FiltersBelowMinimumLevelBeforeSinksAndSignals)
{
    LogService service;
    service.setMinimumLevel(LogLevel::Warn);

    RecordingSink sink;
    service.addSink(&sink);

    QVector<LogEvent> emitted;
    QObject::connect(&service,
                     &ILogService::logAppended,
                     &service,
                     [&emitted](const LogEvent& event) {
                         emitted.push_back(event);
                     });

    service.append(makeEvent(QStringLiteral("INFO"), QStringLiteral("ignored")));
    service.append(makeEvent(QStringLiteral("ERROR"), QStringLiteral("kept")));

    ASSERT_EQ(emitted.size(), 1);
    ASSERT_EQ(sink.events.size(), 1);
    EXPECT_EQ(emitted.first().message, QStringLiteral("kept"));
    EXPECT_EQ(sink.events.first().message, QStringLiteral("kept"));

    const QVector<LogEvent> recent = service.recent(10);
    ASSERT_EQ(recent.size(), 1);
    EXPECT_EQ(recent.first().message, QStringLiteral("kept"));
}

TEST(LogServiceTest, OffMinimumLevelDisablesAllEvents)
{
    LogService service;
    service.setMinimumLevel(LogLevel::Off);

    RecordingSink sink;
    service.addSink(&sink);
    service.append(makeEvent(QStringLiteral("FATAL"), QStringLiteral("ignored")));

    EXPECT_TRUE(service.recent(10).isEmpty());
    EXPECT_TRUE(sink.events.isEmpty());
}
