#include "hal/hal_factory.h"

#include "hal_service.h"

namespace hwtest::hal {

IHalService* createHalService(QObject* parent)
{
    return new HalService(parent);
}

void destroyHalService(IHalService* service)
{
    delete service;
}

} // namespace hwtest::hal
