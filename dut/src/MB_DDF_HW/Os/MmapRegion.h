#pragma once

#include "MB_DDF_HW/Core/Result.h"

#include <cstddef>
#include <sys/types.h>

namespace MB_DDF::HW::Os {

class MmapRegion {
public:
    MmapRegion() noexcept = default;
    ~MmapRegion();

    MmapRegion(const MmapRegion&) = delete;
    MmapRegion& operator=(const MmapRegion&) = delete;

    MmapRegion(MmapRegion&& other) noexcept;
    MmapRegion& operator=(MmapRegion&& other) noexcept;

    static Result<MmapRegion> map(int fd, size_t length, off_t offset);

    bool valid() const noexcept;
    void* data() noexcept;
    const void* data() const noexcept;
    size_t size() const noexcept;
    void reset() noexcept;

private:
    MmapRegion(void* data, size_t size) noexcept;

    void* data_{nullptr};
    size_t size_{0};
};

} // namespace MB_DDF::HW::Os
