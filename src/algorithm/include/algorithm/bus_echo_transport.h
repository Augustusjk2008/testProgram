#pragma once

#include <algorithm/mbddf_transport.h>

#include <hal/i_control_channel.h>

#include <QMap>

#include <memory>

namespace hwtest::algorithm::mbddf {

// Coordinates BUS_ECHO across the MB_DDF control channel and the selected
// physical COM link. The DUT sends the request payload on the physical link;
// the PC reads and validates its complete physical frame, echoes that frame
// unchanged, and then collects the control response.
class BusEchoTransport final : public IByteTransport {
public:
    BusEchoTransport(std::unique_ptr<IByteTransport> controlTransport,
                     hwtest::hal::IControlChannel* rawChannel,
                     hwtest::hal::ResourceId controlResourceId);
    ~BusEchoTransport() override;

    void setRequestId(const hwtest::hal::RequestId& requestId) override;
    bool configure(const QVariantMap& options, QString* error) override;
    bool open(QString* error) override;
    TransportResult transact(const QByteArray& frame, int timeoutMs) override;
    void close() override;

private:
    std::unique_ptr<IByteTransport> m_controlTransport;
    hwtest::hal::IControlChannel* m_rawChannel = nullptr;
    hwtest::hal::ResourceId m_controlResourceId;
    hwtest::hal::ResourceId m_rawResourceId;
    hwtest::hal::RequestId m_requestId;
    QMap<int, hwtest::hal::ResourceId> m_resourceByLink;
    int m_payloadBytes = 114;
    int m_openTimeoutMs = 1000;
    bool m_open = false;
    bool m_rawOpen = false;
};

} // namespace hwtest::algorithm::mbddf
