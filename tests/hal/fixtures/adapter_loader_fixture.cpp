#include "hal/hal_adapter_abi.h"

extern "C" int HAL_ADAPTER_CALL hal_adapter_get_api_v1(const HalAdapterHostApiV1* host,
                                                       HalAdapterApiV1* outApi)
{
    if (host == nullptr || outApi == nullptr) {
        return -1;
    }

    *outApi = HalAdapterApiV1{};
    outApi->abiVersion = HAL_ADAPTER_ABI_VERSION;
    outApi->structSize = static_cast<int>(sizeof(HalAdapterApiV1));
    return 0;
}
