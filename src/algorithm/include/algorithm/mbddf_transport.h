#pragma once

#include <algorithm/mbddf_protocol.h>

#include <hal/hal_types.h>

#include <QByteArray>
#include <QVariantMap>

#include <functional>

namespace hwtest::hal {
class IHalDevice;
}

namespace hwtest::algorithm::mbddf {

struct TransportResult {
    enum class Error {
        None,
        Timeout,
        Io,
    };

    bool ok = false;
    Error errorCode = Error::None;
    QByteArray frame;
    QString error;
};

class IByteTransport {
public:
    virtual ~IByteTransport() = default;

    // The simulator transports ignore options. A real transport may use this
    // hook to apply the execution configuration before opening the device.
    virtual bool configure(const QVariantMap& options, QString* error)
    {
        Q_UNUSED(options)
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    virtual bool open(QString* error) = 0;
    virtual TransportResult transact(const QByteArray& frame, int timeoutMs) = 0;
    virtual void close() = 0;
};

class ScriptedByteTransport final : public IByteTransport {
public:
    using Handler = std::function<TransportResult(const QByteArray& frame, int timeoutMs)>;

    explicit ScriptedByteTransport(Handler handler);

    bool open(QString* error) override;
    TransportResult transact(const QByteArray& frame, int timeoutMs) override;
    void close() override;

    bool isOpen() const noexcept;
    int transactionCount() const noexcept;
    QByteArray lastRequest() const;

private:
    Handler m_handler;
    bool m_open = false;
    int m_transactionCount = 0;
    QByteArray m_lastRequest;
};

class SystemStatusSimulator final : public IByteTransport {
public:
    enum class Fault {
        None,
        Timeout,
        BadCrc,
        InvalidResponse
    };

    explicit SystemStatusSimulator(const ProtocolCatalog* catalog);

    bool open(QString* error) override;
    TransportResult transact(const QByteArray& frame, int timeoutMs) override;
    void close() override;

    void setResponseValues(const QVariantMap& values);
    void setFault(Fault fault);
    bool isOpen() const noexcept;
    int transactionCount() const noexcept;
    QByteArray lastRequest() const;

private:
    const ProtocolCatalog* m_catalog = nullptr;
    QVariantMap m_responseValues;
    Fault m_fault = Fault::None;
    bool m_open = false;
    int m_transactionCount = 0;
    QByteArray m_lastRequest;
};

// Product control bridge. The selected HAL resource decides whether the
// underlying Provider is Qt SerialPort or Qt UDP; this layer only assembles
// MB_DDF physical frames from raw byte chunks.
class HalControlTransport final : public IByteTransport {
public:
    HalControlTransport(hwtest::hal::IHalDevice* device,
                        hwtest::hal::ResourceId resourceId);

    bool configure(const QVariantMap& options, QString* error) override;
    bool open(QString* error) override;
    TransportResult transact(const QByteArray& frame, int timeoutMs) override;
    void close() override;

private:
    bool takeBufferedFrame(QByteArray* frame);

    hwtest::hal::IHalDevice* m_device = nullptr;
    hwtest::hal::ResourceId m_resourceId;
    QByteArray m_receiveBuffer;
    int m_openTimeoutMs = 1000;
    int m_readChunkBytes = 260;
    bool m_open = false;
};

// Optional real-device bridge. It only exposes a byte transaction to the
// algorithm layer; device discovery and ownership remain outside this class.
// The two-argument constructor uses the fixed MB_DDF 614400/8E1 settings.
class HalSerialTransport final : public IByteTransport {
public:
    HalSerialTransport(hwtest::hal::IHalDevice* device,
                       hwtest::hal::ResourceId resourceId);
    HalSerialTransport(hwtest::hal::IHalDevice* device,
                       hwtest::hal::ResourceId resourceId,
                       hwtest::hal::SerialConfig serialConfig);

    bool configure(const QVariantMap& options, QString* error) override;
    bool open(QString* error) override;
    TransportResult transact(const QByteArray& frame, int timeoutMs) override;
    void close() override;

private:
    hwtest::hal::IHalDevice* m_device = nullptr;
    hwtest::hal::ResourceId m_resourceId;
    hwtest::hal::SerialConfig m_serialConfig;
    bool m_open = false;
};

} // namespace hwtest::algorithm::mbddf
