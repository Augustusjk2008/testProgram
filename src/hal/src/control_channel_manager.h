#pragma once

#include "resource_mapper.h"

#include <functional>
#include <map>
#include <memory>

namespace hwtest::hal {

class ControlIoProvider;

class ControlChannelManager {
public:
    using ProviderFactory = std::function<std::unique_ptr<ControlIoProvider>(const ResourceBinding&)>;

    ControlChannelManager();
    explicit ControlChannelManager(ProviderFactory providerFactory);
    ~ControlChannelManager();

    HalStatus open(const ResourceBinding& binding,
                   const OperationOptions& options);
    HalStatus close(const ResourceBinding& binding,
                    const OperationOptions& options);
    HalStatus write(const ResourceBinding& binding,
                    const QByteArray& data,
                    const OperationOptions& options);
    HalResult<QByteArray> read(const ResourceBinding& binding,
                               int maxBytes,
                               const OperationOptions& options);
    HalStatus closeAll(const OperationOptions& options);

private:
    struct OpenControlSession {
        ResourceBinding binding;
        std::unique_ptr<ControlIoProvider> provider;
    };

    HalStatus ensureOpenFor(const ResourceBinding& binding,
                            const QString& operation) const;
    ResourceId openSerialPortOwner(const ResourceBinding& binding) const;
    static HalStatus withBindingContext(HalStatus status,
                                        const ResourceBinding& binding,
                                        const QString& fallbackOperation);

    ProviderFactory m_providerFactory;
    std::map<ResourceId, OpenControlSession> m_openSessions;
};

} // namespace hwtest::hal
