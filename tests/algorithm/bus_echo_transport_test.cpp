#include <gtest/gtest.h>

#include <algorithm/bus_echo_transport.h>
#include <algorithm/mbddf_protocol.h>

#include <hal/i_control_channel.h>

#include <QFileInfo>

#include <deque>
#include <memory>
#include <utility>

namespace hwtest::algorithm::mbddf {
namespace {

QString catalogDirectory()
{
    const QString configured = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    return configured.isEmpty()
        ? QStringLiteral(HWTEST_MBDDF_PROTOCOL_CATALOG_DIR)
        : configured;
}

QByteArray echoRequest(const ProtocolCatalog& catalog, int linkId)
{
    const MessageDefinition* definition = catalog.findByName(
        QStringLiteral("bus_echo_test_request"));
    EXPECT_NE(definition, nullptr);
    if (definition == nullptr) return {};
    QVariantMap values{
        {QStringLiteral("link_id"), linkId},
        {QStringLiteral("data[0]"), 0x4D},
        {QStringLiteral("data[1]"), 0x42},
        {QStringLiteral("data[2]"), 0x31},
    };
    QString error;
    QByteArray payload;
    EXPECT_TRUE(encodePayload(*definition, values, 0x1234, &payload, &error))
        << error.toStdString();
    QByteArray frame;
    EXPECT_TRUE(encodeFrame(payload, &frame, &error)) << error.toStdString();
    return frame;
}

QByteArray requestData(const QByteArray& frame)
{
    QByteArray payload;
    QString error;
    EXPECT_TRUE(decodeFrame(frame, &payload, &error)) << error.toStdString();
    return payload.mid(6, 114);
}

class SplitControlTransport final : public IByteTransport {
public:
    SplitControlTransport(QByteArray response, QStringList* order)
        : m_response(std::move(response)), m_order(order)
    {
    }

    bool configure(const QVariantMap&, QString* error) override
    {
        if (error != nullptr) error->clear();
        return true;
    }

    bool open(QString* error) override
    {
        m_open = true;
        if (error != nullptr) error->clear();
        return true;
    }

    TransportResult transact(const QByteArray&, int) override
    {
        return failure(QStringLiteral("split I/O required"), TransportResult::Error::Io);
    }

    TransportResult writeFrame(const QByteArray& frame, int) override
    {
        if (m_order != nullptr) m_order->push_back(QStringLiteral("control-write"));
        m_request = frame;
        TransportResult result;
        result.ok = m_open;
        if (!result.ok) result.error = QStringLiteral("closed");
        return result;
    }

    TransportResult readFrame(int) override
    {
        if (m_order != nullptr) m_order->push_back(QStringLiteral("control-read"));
        TransportResult result;
        result.ok = m_open;
        result.frame = m_response;
        if (!result.ok) result.error = QStringLiteral("closed");
        return result;
    }

    void close() override { m_open = false; }

    QByteArray request() const { return m_request; }

private:
    static TransportResult failure(const QString& message, TransportResult::Error code)
    {
        TransportResult result;
        result.errorCode = code;
        result.error = message;
        return result;
    }

    QByteArray m_response;
    QStringList* m_order = nullptr;
    QByteArray m_request;
    bool m_open = false;
};

class RawControlChannel final : public hwtest::hal::IControlChannel {
public:
    explicit RawControlChannel(QStringList* order) : m_order(order) {}

    hwtest::hal::HalStatus openControl(
        const hwtest::hal::ResourceId& resourceId,
        const hwtest::hal::OperationOptions&) override
    {
        m_openResource = resourceId;
        return {};
    }

    hwtest::hal::HalStatus closeControl(
        const hwtest::hal::ResourceId& resourceId,
        const hwtest::hal::OperationOptions&) override
    {
        if (m_openResource == resourceId) m_openResource.clear();
        return {};
    }

    hwtest::hal::HalStatus writeControl(
        const hwtest::hal::ResourceId& resourceId,
        const QByteArray& data,
        const hwtest::hal::OperationOptions&) override
    {
        if (m_order != nullptr) m_order->push_back(QStringLiteral("raw-write"));
        if (resourceId != m_openResource) return status(hwtest::hal::HalStatusCode::InvalidState);
        m_written = data;
        return {};
    }

    hwtest::hal::HalResult<QByteArray> readControl(
        const hwtest::hal::ResourceId& resourceId,
        int maxBytes,
        const hwtest::hal::OperationOptions&) override
    {
        if (m_order != nullptr) m_order->push_back(QStringLiteral("raw-read"));
        if (resourceId != m_openResource) {
            return {status(hwtest::hal::HalStatusCode::InvalidState), {}};
        }
        if (m_reads.empty()) {
            return {status(hwtest::hal::HalStatusCode::Timeout), {}};
        }
        QByteArray next = std::move(m_reads.front());
        m_reads.pop_front();
        if (next.size() > maxBytes) {
            m_reads.push_front(next.mid(maxBytes));
            next.truncate(maxBytes);
        }
        return {{}, next};
    }

    void queue(QByteArray bytes) { m_reads.push_back(std::move(bytes)); }
    QByteArray written() const { return m_written; }
    QString openResource() const { return m_openResource; }

private:
    static hwtest::hal::HalStatus status(hwtest::hal::HalStatusCode code)
    {
        hwtest::hal::HalStatus result;
        result.code = code;
        result.error.code = code;
        result.error.message = QStringLiteral("injected raw channel status");
        return result;
    }

    QStringList* m_order = nullptr;
    QString m_openResource;
    std::deque<QByteArray> m_reads;
    QByteArray m_written;
};

QVariantMap options()
{
    return {
        {QStringLiteral("busEcho"), QVariantMap{
             {QStringLiteral("payloadBytes"), 114},
             {QStringLiteral("resourceByLink"), QVariantMap{
                  {QStringLiteral("0"), QStringLiteral("BUS_ECHO_COM1")},
                  {QStringLiteral("1"), QStringLiteral("BUS_ECHO_COM2")},
                  {QStringLiteral("3"), QStringLiteral("BUS_ECHO_COM4")},
              }},
         }},
    };
}

TEST(BusEchoTransportTest, ReadsFragmentedPayloadAndWritesItBackBeforeControlResponse)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error))
        << error.toStdString();
    const QByteArray request = echoRequest(catalog, 1);
    const QByteArray expected = requestData(request);
    QStringList order;
    auto control = std::make_unique<SplitControlTransport>(
        QByteArrayLiteral("control-response"), &order);
    RawControlChannel raw(&order);
    raw.queue(expected.left(1));
    raw.queue(expected.mid(1, 17));
    raw.queue(expected.mid(18));

    BusEchoTransport transport(std::move(control), &raw,
                               QStringLiteral("CONTROL_SERIAL"));
    ASSERT_TRUE(transport.configure(options(), &error)) << error.toStdString();
    ASSERT_TRUE(transport.open(&error)) << error.toStdString();
    const TransportResult result = transport.transact(request, 5000);

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.frame, QByteArrayLiteral("control-response"));
    EXPECT_EQ(raw.openResource(), QStringLiteral("BUS_ECHO_COM2"));
    EXPECT_EQ(raw.written(), expected);
    ASSERT_GE(order.size(), 6);
    EXPECT_EQ(order.front(), QStringLiteral("control-write"));
    EXPECT_EQ(order.at(order.size() - 2), QStringLiteral("raw-write"));
    EXPECT_EQ(order.back(), QStringLiteral("control-read"));
}

TEST(BusEchoTransportTest, DrainsControlResponseButFailsOnPayloadMismatch)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    const QByteArray request = echoRequest(catalog, 0);
    QByteArray corrupted = requestData(request);
    corrupted[57] = static_cast<char>(static_cast<uchar>(corrupted.at(57)) ^ 0x01u);
    QStringList order;
    auto control = std::make_unique<SplitControlTransport>(
        QByteArrayLiteral("control-response"), &order);
    RawControlChannel raw(&order);
    raw.queue(corrupted);
    BusEchoTransport transport(std::move(control), &raw,
                               QStringLiteral("CONTROL_SERIAL"));
    ASSERT_TRUE(transport.configure(options(), &error));
    ASSERT_TRUE(transport.open(&error));

    const TransportResult result = transport.transact(request, 5000);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, TransportResult::Error::Io);
    EXPECT_TRUE(result.error.contains(QStringLiteral("mismatch"), Qt::CaseInsensitive));
    EXPECT_EQ(raw.written(), corrupted);
    EXPECT_EQ(order.back(), QStringLiteral("control-read"));
}

TEST(BusEchoTransportTest, RejectsControlPortAndUnsupportedLinksBeforeWriting)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    QStringList order;
    auto control = std::make_unique<SplitControlTransport>(QByteArray{}, &order);
    RawControlChannel raw(&order);
    BusEchoTransport transport(std::move(control), &raw,
                               QStringLiteral("CONTROL_SERIAL"));
    QVariantMap invalidOptions = options();
    QVariantMap busEcho = invalidOptions.value(QStringLiteral("busEcho")).toMap();
    QVariantMap mapping = busEcho.value(QStringLiteral("resourceByLink")).toMap();
    mapping.insert(QStringLiteral("1"), QStringLiteral("CONTROL_SERIAL"));
    busEcho.insert(QStringLiteral("resourceByLink"), mapping);
    invalidOptions.insert(QStringLiteral("busEcho"), busEcho);
    EXPECT_FALSE(transport.configure(invalidOptions, &error));

    ASSERT_TRUE(transport.configure(options(), &error));
    ASSERT_TRUE(transport.open(&error));
    const TransportResult result = transport.transact(echoRequest(catalog, 2), 5000);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(order.isEmpty());
}

TEST(BusEchoTransportTest, TimesOutAfterPartialRawPayloadWithoutEchoing)
{
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(catalogDirectory(), &error));
    const QByteArray request = echoRequest(catalog, 3);
    QStringList order;
    auto control = std::make_unique<SplitControlTransport>(QByteArray{}, &order);
    RawControlChannel raw(&order);
    raw.queue(requestData(request).left(113));
    BusEchoTransport transport(std::move(control), &raw,
                               QStringLiteral("CONTROL_SERIAL"));
    ASSERT_TRUE(transport.configure(options(), &error));
    ASSERT_TRUE(transport.open(&error));

    const TransportResult result = transport.transact(request, 1);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, TransportResult::Error::Timeout);
    EXPECT_TRUE(raw.written().isEmpty());
}

} // namespace
} // namespace hwtest::algorithm::mbddf
