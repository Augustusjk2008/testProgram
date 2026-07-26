#pragma once

#include "hardware_adapter.h"
#include "resource_mapper.h"

#include <QHash>

#include <memory>

namespace hwtest::hal {

class AdapterRouter {
public:
    AdapterRouter() = default;
    ~AdapterRouter();

    HalStatus configure(const QVariantMap& halConfig,
                        const ResourceMapper& mapper);
    HalResult<HardwareAdapter*> acquire(const AdapterId& adapterId);
    HalStatus release(const AdapterId& adapterId);
    HalStatus shutdown();

private:
    struct Entry {
        QVariantMap config;
        std::shared_ptr<HardwareAdapter> backend;
        int leases = 0;
    };

    QVariantMap configForAdapter(const AdapterId& adapterId,
                                 const ResourceMapper& mapper) const;
    std::shared_ptr<HardwareAdapter> createBackend(const AdapterId& adapterId,
                                                   const QVariantMap& config) const;

    QVariantMap m_halConfig;
    QHash<AdapterId, Entry> m_entries;
};

} // namespace hwtest::hal
