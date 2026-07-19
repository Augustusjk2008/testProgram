#pragma once

#include "control_io_provider.h"

#include <QHostAddress>

#include <memory>

class QUdpSocket;

namespace hwtest::hal {

class QtUdpControlProvider final : public ControlIoProvider {
public:
    QtUdpControlProvider();
    ~QtUdpControlProvider() override;

    HalStatus open(const QVariantMap& properties,
                   const OperationOptions& options) override;
    HalStatus close(const OperationOptions& options) override;
    HalStatus write(const QByteArray& data,
                    const OperationOptions& options) override;
    HalResult<QByteArray> read(int maxBytes,
                               const OperationOptions& options) override;

private:
    std::unique_ptr<QUdpSocket> m_socket;
    QHostAddress m_remoteAddress;
    quint16 m_remotePort = 0;
};

} // namespace hwtest::hal
