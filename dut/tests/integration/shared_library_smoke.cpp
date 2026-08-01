#include "MB_DDF/DDS/DDSCore.h"

#include <cstring>
#include <semaphore.h>
#include <sys/mman.h>

namespace {

constexpr char kSharedMemoryName[] = "/MB_DDF_V2_SHM";
constexpr char kSemaphoreName[] = "/MB_DDF_V2_SHM_sem";

void cleanup_shared_state() {
    shm_unlink(kSharedMemoryName);
    sem_unlink(kSemaphoreName);
}

} // namespace

int main() {
    using MB_DDF::DDS::DDSCore;

    cleanup_shared_state();
    auto& dds = DDSCore::instance();
    if (!dds.initialize(16 * 1024 * 1024)) {
        cleanup_shared_state();
        return 10;
    }

    int result = 0;
    {
        constexpr char topic[] = "rt://abi/shared-library-smoke";
        constexpr char payload[] = "shared-library-v2";
        auto subscriber = dds.create_subscriber(topic, true);
        auto publisher = dds.create_publisher(topic, true);
        if (subscriber == nullptr || publisher == nullptr) {
            result = 11;
        } else if (dds.data_write(publisher, payload, sizeof(payload)) == 0) {
            result = 12;
        } else {
            char received[64] = {};
            const size_t size = dds.data_read(subscriber, received, sizeof(received));
            if (size != sizeof(payload) || std::memcmp(received, payload, sizeof(payload)) != 0) {
                result = 13;
            }
        }
    }

    dds.shutdown();
    cleanup_shared_state();
    return result;
}
