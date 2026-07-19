#pragma once

#include "control_io_provider.h"

#include <memory>

class QSerialPort;

namespace hwtest::hal {

class QtSerialControlProvider final : public ControlIoProvider {
public:
    QtSerialControlProvider();
    ~QtSerialControlProvider() override;

    HalStatus open(const QVariantMap& properties,
                   const OperationOptions& options) override;
    HalStatus close(const OperationOptions& options) override;
    HalStatus write(const QByteArray& data,
                    const OperationOptions& options) override;
    HalResult<QByteArray> read(int maxBytes,
                               const OperationOptions& options) override;

private:
    std::unique_ptr<QSerialPort> m_port;
};

} // namespace hwtest::hal
