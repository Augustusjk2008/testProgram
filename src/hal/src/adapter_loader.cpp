#include "adapter_loader.h"

#include <QLibrary>

namespace hwtest::hal {

AdapterLoader::AdapterLoader() = default;

AdapterLoader::~AdapterLoader()
{
    unload();
}

bool AdapterLoader::load(const QString& libraryPath,
                         const HalAdapterHostApiV1& hostApi,
                         HalAdapterApiV1* outApi)
{
    unload();
    m_errorString.clear();
    if (libraryPath.isEmpty() || outApi == nullptr) {
        m_errorString = QStringLiteral("Invalid adapter library path or output pointer");
        return false;
    }

    m_library = new QLibrary(libraryPath);
    if (!m_library->load()) {
        m_errorString = m_library->errorString();
        unload();
        return false;
    }

    const auto symbol = reinterpret_cast<HalAdapterGetApiV1Fn>(m_library->resolve("hal_adapter_get_api_v1"));
    if (symbol == nullptr) {
        m_errorString = QStringLiteral("Missing symbol hal_adapter_get_api_v1");
        unload();
        return false;
    }

    HalAdapterApiV1 api {};
    if (symbol(&hostApi, &api) != 0) {
        m_errorString = QStringLiteral("hal_adapter_get_api_v1 returned failure");
        unload();
        return false;
    }

    if (api.abiVersion != HAL_ADAPTER_ABI_VERSION || api.structSize < static_cast<int>(sizeof(HalAdapterApiV1))) {
        m_errorString = QStringLiteral("Adapter ABI version mismatch");
        unload();
        return false;
    }

    m_api = api;
    *outApi = m_api;
    m_libraryPath = libraryPath;
    return true;
}

void AdapterLoader::unload()
{
    if (m_library != nullptr) {
        if (m_library->isLoaded()) {
            m_library->unload();
        }
        delete m_library;
        m_library = nullptr;
    }
    m_libraryPath.clear();
    m_api = HalAdapterApiV1{};
}

bool AdapterLoader::isLoaded() const
{
    return m_library != nullptr && m_library->isLoaded();
}

QString AdapterLoader::errorString() const
{
    return m_errorString;
}

QString AdapterLoader::libraryPath() const
{
    return m_libraryPath;
}

} // namespace hwtest::hal
