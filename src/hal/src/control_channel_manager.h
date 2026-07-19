#pragma once

#include "resource_mapper.h"

#include <memory>

namespace hwtest::hal {

class ControlIoProvider;

class ControlChannelManager {
public:
    ControlChannelManager();
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
    HalStatus ensureOpenFor(const ResourceBinding& binding,
                            const QString& operation) const;
    static HalStatus withBindingContext(HalStatus status,
                                        const ResourceBinding& binding,
                                        const QString& fallbackOperation);

    std::unique_ptr<ControlIoProvider> m_provider;
    ResourceBinding m_openBinding;
};

} // namespace hwtest::hal
