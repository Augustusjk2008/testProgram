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

bool AdapterLoader::loadTaskApi(const HalAdapterHostApiV1& hostApi,
                                HalAdapterTaskApiV1* outApi)
{
    if (outApi == nullptr) return false;
    *outApi = HalAdapterTaskApiV1{};
    if (!isLoaded()) return false;
    const auto symbol = reinterpret_cast<HalAdapterGetTaskApiV1Fn>(
        m_library->resolve("hal_adapter_get_task_api_v1"));
    if (symbol == nullptr) return false;

    HalAdapterTaskApiV1 api {};
    if (symbol(&hostApi, &api) != 0 ||
        api.abiVersion != HAL_ADAPTER_TASK_ABI_VERSION ||
        api.structSize < static_cast<int>(sizeof(HalAdapterTaskApiV1)) ||
        api.createTask == nullptr || api.startTask == nullptr ||
        api.readTask == nullptr || api.writeTask == nullptr ||
        api.getTaskStatus == nullptr || api.stopTask == nullptr ||
        api.closeTask == nullptr) {
        return false;
    }
    *outApi = api;
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
