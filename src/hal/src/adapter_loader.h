#pragma once

#include "hal/hal_adapter_abi.h"

#include <QString>

class QLibrary;

namespace hwtest::hal {

class AdapterLoader {
public:
    AdapterLoader();
    ~AdapterLoader();

    bool load(const QString& libraryPath,
              const HalAdapterHostApiV1& hostApi,
              HalAdapterApiV1* outApi);
    void unload();

    bool isLoaded() const;
    QString errorString() const;
    QString libraryPath() const;

private:
    QLibrary* m_library = nullptr;
    QString m_errorString;
    QString m_libraryPath;
    HalAdapterApiV1 m_api {};
};

} // namespace hwtest::hal
